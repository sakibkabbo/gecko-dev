/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=8 sts=2 et sw=2 tw=80: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this file,
 * You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "mozilla/dom/U2F.h"
#include "mozilla/dom/WebCryptoCommon.h"
#include "mozilla/ipc/PBackgroundChild.h"
#include "mozilla/ipc/BackgroundChild.h"
#include "mozilla/dom/WebAuthnTransactionChild.h"
#include "mozilla/dom/WebAuthnUtil.h"
#include "nsContentUtils.h"
#include "nsIEffectiveTLDService.h"
#include "nsNetUtil.h"
#include "nsURLParsers.h"

using namespace mozilla::ipc;

// Forward decl because of nsHTMLDocument.h's complex dependency on
// /layout/style
class nsHTMLDocument {
 public:
  bool IsRegistrableDomainSuffixOfOrEqualTo(const nsAString& aHostSuffixString,
                                            const nsACString& aOrigHost);
};

namespace mozilla {
namespace dom {

static mozilla::LazyLogModule gU2FLog("u2fmanager");

NS_NAMED_LITERAL_STRING(kFinishEnrollment, "navigator.id.finishEnrollment");
NS_NAMED_LITERAL_STRING(kGetAssertion, "navigator.id.getAssertion");

// Bug #1436078 - Permit Google Accounts. Remove in Bug #1436085 in Jan 2023.
NS_NAMED_LITERAL_STRING(kGoogleAccountsAppId1,
                        "https://www.gstatic.com/securitykey/origins.json");
NS_NAMED_LITERAL_STRING(
    kGoogleAccountsAppId2,
    "https://www.gstatic.com/securitykey/a/google.com/origins.json");

NS_INTERFACE_MAP_BEGIN_CYCLE_COLLECTION(U2F)
  NS_WRAPPERCACHE_INTERFACE_MAP_ENTRY
  NS_INTERFACE_MAP_ENTRY(nsISupports)
  NS_INTERFACE_MAP_ENTRY(nsIDOMEventListener)
NS_INTERFACE_MAP_END

NS_IMPL_CYCLE_COLLECTING_ADDREF(U2F)
NS_IMPL_CYCLE_COLLECTING_RELEASE(U2F)

NS_IMPL_CYCLE_COLLECTION_WRAPPERCACHE(U2F, mParent)

/***********************************************************************
 * Utility Functions
 **********************************************************************/

static ErrorCode ConvertNSResultToErrorCode(const nsresult& aError) {
  if (aError == NS_ERROR_DOM_TIMEOUT_ERR) {
    return ErrorCode::TIMEOUT;
  }
  /* Emitted by U2F{Soft,HID}TokenManager when we really mean ineligible */
  if (aError == NS_ERROR_DOM_INVALID_STATE_ERR) {
    return ErrorCode::DEVICE_INELIGIBLE;
  }
  return ErrorCode::OTHER_ERROR;
}

static uint32_t AdjustedTimeoutMillis(
    const Optional<Nullable<int32_t>>& opt_aSeconds) {
  uint32_t adjustedTimeoutMillis = 30000u;
  if (opt_aSeconds.WasPassed() && !opt_aSeconds.Value().IsNull()) {
    adjustedTimeoutMillis = opt_aSeconds.Value().Value() * 1000u;
    adjustedTimeoutMillis = std::max(15000u, adjustedTimeoutMillis);
    adjustedTimeoutMillis = std::min(120000u, adjustedTimeoutMillis);
  }
  return adjustedTimeoutMillis;
}

static nsresult AssembleClientData(const nsAString& aOrigin,
                                   const nsAString& aTyp,
                                   const nsAString& aChallenge,
                                   /* out */ nsString& aClientData) {
  MOZ_ASSERT(NS_IsMainThread());
  U2FClientData clientDataObject;
  clientDataObject.mTyp.Construct(aTyp);  // "Typ" from the U2F specification
  clientDataObject.mChallenge.Construct(aChallenge);
  clientDataObject.mOrigin.Construct(aOrigin);

  if (NS_WARN_IF(!clientDataObject.ToJSON(aClientData))) {
    return NS_ERROR_FAILURE;
  }

  return NS_OK;
}

static void RegisteredKeysToScopedCredentialList(
    const nsAString& aAppId, const nsTArray<RegisteredKey>& aKeys,
    nsTArray<WebAuthnScopedCredential>& aList) {
  for (const RegisteredKey& key : aKeys) {
    // Check for required attributes
    if (!key.mVersion.WasPassed() || !key.mKeyHandle.WasPassed() ||
        key.mVersion.Value() != kRequiredU2FVersion) {
      continue;
    }

    // If this key's mAppId doesn't match the invocation, we can't handle it.
    if (key.mAppId.WasPassed() && !key.mAppId.Value().Equals(aAppId)) {
      continue;
    }

    CryptoBuffer keyHandle;
    nsresult rv = keyHandle.FromJwkBase64(key.mKeyHandle.Value());
    if (NS_WARN_IF(NS_FAILED(rv))) {
      continue;
    }

    WebAuthnScopedCredential c;
    c.id() = keyHandle;
    aList.AppendElement(c);
  }
}

/***********************************************************************
 * U2F JavaScript API Implementation
 **********************************************************************/

U2F::~U2F() {
  MOZ_ASSERT(NS_IsMainThread());

  if (mTransaction.isSome()) {
    RejectTransaction(NS_ERROR_ABORT);
  }

  if (mChild) {
    RefPtr<WebAuthnTransactionChild> c;
    mChild.swap(c);
    c->Disconnect();
  }
}

void U2F::Init(ErrorResult& aRv) {
  MOZ_ASSERT(mParent);

  nsCOMPtr<Document> doc = mParent->GetDoc();
  MOZ_ASSERT(doc);
  if (!doc) {
    aRv.Throw(NS_ERROR_FAILURE);
    return;
  }

  nsIPrincipal* principal = doc->NodePrincipal();
  aRv = nsContentUtils::GetUTFOrigin(principal, mOrigin);
  if (NS_WARN_IF(aRv.Failed())) {
    return;
  }

  if (NS_WARN_IF(mOrigin.IsEmpty())) {
    aRv.Throw(NS_ERROR_FAILURE);
    return;
  }
}

/* virtual */ JSObject* U2F::WrapObject(JSContext* aCx,
                                        JS::Handle<JSObject*> aGivenProto) {
  return U2F_Binding::Wrap(aCx, this, aGivenProto);
}

template <typename T, typename C>
void U2F::ExecuteCallback(T& aResp, nsMainThreadPtrHandle<C>& aCb) {
  MOZ_ASSERT(NS_IsMainThread());
  MOZ_ASSERT(aCb);

  // Assert that mTransaction was cleared before before we were called to allow
  // reentrancy from microtask checkpoints.
  MOZ_ASSERT(mTransaction.isNothing());

  ErrorResult error;
  aCb->Call(aResp, error);
  NS_WARNING_ASSERTION(!error.Failed(), "dom::U2F::Promise callback failed");
  error.SuppressException();  // Useful exceptions already emitted
}

void U2F::Register(const nsAString& aAppId,
                   const Sequence<RegisterRequest>& aRegisterRequests,
                   const Sequence<RegisteredKey>& aRegisteredKeys,
                   U2FRegisterCallback& aCallback,
                   const Optional<Nullable<int32_t>>& opt_aTimeoutSeconds,
                   ErrorResult& aRv) {
  MOZ_ASSERT(NS_IsMainThread());

  if (mTransaction.isSome()) {
    CancelTransaction(NS_ERROR_ABORT);
  }

  nsMainThreadPtrHandle<U2FRegisterCallback> callback(
      new nsMainThreadPtrHolder<U2FRegisterCallback>("U2F::Register::callback",
                                                     &aCallback));

  // Ensure we have a callback.
  if (NS_WARN_IF(!callback)) {
    return;
  }

  // Evaluate the AppID
  nsString adjustedAppId(aAppId);
  if (!EvaluateAppID(mParent, mOrigin, U2FOperation::Register, adjustedAppId)) {
    RegisterResponse response;
    response.mErrorCode.Construct(
        static_cast<uint32_t>(ErrorCode::BAD_REQUEST));
    ExecuteCallback(response, callback);
    return;
  }

  nsAutoString clientDataJSON;

  // Pick the first valid RegisterRequest; we can only work with one.
  CryptoBuffer challenge;
  for (const RegisterRequest& req : aRegisterRequests) {
    if (!req.mChallenge.WasPassed() || !req.mVersion.WasPassed() ||
        req.mVersion.Value() != kRequiredU2FVersion) {
      continue;
    }
    if (!challenge.Assign(NS_ConvertUTF16toUTF8(req.mChallenge.Value()))) {
      continue;
    }

    nsresult rv = AssembleClientData(mOrigin, kFinishEnrollment,
                                     req.mChallenge.Value(), clientDataJSON);
    if (NS_WARN_IF(NS_FAILED(rv))) {
      continue;
    }
  }

  // Did we not get a valid RegisterRequest? Abort.
  if (clientDataJSON.IsEmpty()) {
    RegisterResponse response;
    response.mErrorCode.Construct(
        static_cast<uint32_t>(ErrorCode::BAD_REQUEST));
    ExecuteCallback(response, callback);
    return;
  }

  // Build the exclusion list, if any
  nsTArray<WebAuthnScopedCredential> excludeList;
  RegisteredKeysToScopedCredentialList(adjustedAppId, aRegisteredKeys,
                                       excludeList);

  if (!MaybeCreateBackgroundActor()) {
    RegisterResponse response;
    response.mErrorCode.Construct(
        static_cast<uint32_t>(ErrorCode::OTHER_ERROR));
    ExecuteCallback(response, callback);
    return;
  }

  ListenForVisibilityEvents();

  NS_ConvertUTF16toUTF8 clientData(clientDataJSON);
  uint32_t adjustedTimeoutMillis = AdjustedTimeoutMillis(opt_aTimeoutSeconds);

  WebAuthnMakeCredentialInfo info(mOrigin, adjustedAppId, challenge, clientData,
                                  adjustedTimeoutMillis, excludeList,
                                  null_t() /* no extra info for U2F */);

  MOZ_ASSERT(mTransaction.isNothing());
  mTransaction = Some(U2FTransaction(AsVariant(callback)));
  mChild->SendRequestRegister(mTransaction.ref().mId, info);
}

void U2F::FinishMakeCredential(const uint64_t& aTransactionId,
                               const WebAuthnMakeCredentialResult& aResult) {
  MOZ_ASSERT(NS_IsMainThread());

  // Check for a valid transaction.
  if (mTransaction.isNothing() || mTransaction.ref().mId != aTransactionId) {
    return;
  }

  if (NS_WARN_IF(!mTransaction.ref().HasRegisterCallback())) {
    RejectTransaction(NS_ERROR_ABORT);
    return;
  }

  // A CTAP2 response.
  if (aResult.RegistrationData().Length() == 0) {
    RejectTransaction(NS_ERROR_ABORT);
    return;
  }

  CryptoBuffer clientDataBuf;
  if (NS_WARN_IF(!clientDataBuf.Assign(aResult.ClientDataJSON()))) {
    RejectTransaction(NS_ERROR_ABORT);
    return;
  }

  CryptoBuffer regBuf;
  if (NS_WARN_IF(!regBuf.Assign(aResult.RegistrationData()))) {
    RejectTransaction(NS_ERROR_ABORT);
    return;
  }

  nsString clientDataBase64;
  nsString registrationDataBase64;
  nsresult rvClientData = clientDataBuf.ToJwkBase64(clientDataBase64);
  nsresult rvRegistrationData = regBuf.ToJwkBase64(registrationDataBase64);

  if (NS_WARN_IF(NS_FAILED(rvClientData)) ||
      NS_WARN_IF(NS_FAILED(rvRegistrationData))) {
    RejectTransaction(NS_ERROR_ABORT);
    return;
  }

  // Assemble a response object to return
  RegisterResponse response;
  response.mVersion.Construct(kRequiredU2FVersion);
  response.mClientData.Construct(clientDataBase64);
  response.mRegistrationData.Construct(registrationDataBase64);
  response.mErrorCode.Construct(static_cast<uint32_t>(ErrorCode::OK));

  // Keep the callback pointer alive.
  nsMainThreadPtrHandle<U2FRegisterCallback> callback(
      mTransaction.ref().GetRegisterCallback());

  ClearTransaction();
  ExecuteCallback(response, callback);
}

void U2F::Sign(const nsAString& aAppId, const nsAString& aChallenge,
               const Sequence<RegisteredKey>& aRegisteredKeys,
               U2FSignCallback& aCallback,
               const Optional<Nullable<int32_t>>& opt_aTimeoutSeconds,
               ErrorResult& aRv) {
  MOZ_ASSERT(NS_IsMainThread());

  if (mTransaction.isSome()) {
    CancelTransaction(NS_ERROR_ABORT);
  }

  nsMainThreadPtrHandle<U2FSignCallback> callback(
      new nsMainThreadPtrHolder<U2FSignCallback>("U2F::Sign::callback",
                                                 &aCallback));

  // Ensure we have a callback.
  if (NS_WARN_IF(!callback)) {
    return;
  }

  // Evaluate the AppID
  nsString adjustedAppId(aAppId);
  if (!EvaluateAppID(mParent, mOrigin, U2FOperation::Sign, adjustedAppId)) {
    SignResponse response;
    response.mErrorCode.Construct(
        static_cast<uint32_t>(ErrorCode::BAD_REQUEST));
    ExecuteCallback(response, callback);
    return;
  }

  // Produce the AppParam from the current AppID
  nsCString cAppId = NS_ConvertUTF16toUTF8(adjustedAppId);

  nsAutoString clientDataJSON;
  nsresult rv =
      AssembleClientData(mOrigin, kGetAssertion, aChallenge, clientDataJSON);
  if (NS_WARN_IF(NS_FAILED(rv))) {
    SignResponse response;
    response.mErrorCode.Construct(
        static_cast<uint32_t>(ErrorCode::BAD_REQUEST));
    ExecuteCallback(response, callback);
    return;
  }

  CryptoBuffer challenge;
  if (!challenge.Assign(NS_ConvertUTF16toUTF8(aChallenge))) {
    SignResponse response;
    response.mErrorCode.Construct(
        static_cast<uint32_t>(ErrorCode::OTHER_ERROR));
    ExecuteCallback(response, callback);
    return;
  }

  // Build the key list, if any
  nsTArray<WebAuthnScopedCredential> permittedList;
  RegisteredKeysToScopedCredentialList(adjustedAppId, aRegisteredKeys,
                                       permittedList);

  if (!MaybeCreateBackgroundActor()) {
    SignResponse response;
    response.mErrorCode.Construct(
        static_cast<uint32_t>(ErrorCode::OTHER_ERROR));
    ExecuteCallback(response, callback);
    return;
  }

  ListenForVisibilityEvents();

  // Always blank for U2F
  nsTArray<WebAuthnExtension> extensions;

  NS_ConvertUTF16toUTF8 clientData(clientDataJSON);
  uint32_t adjustedTimeoutMillis = AdjustedTimeoutMillis(opt_aTimeoutSeconds);

  WebAuthnGetAssertionInfo info(mOrigin, adjustedAppId, challenge, clientData,
                                adjustedTimeoutMillis, permittedList,
                                null_t() /* no extra info for U2F */);

  MOZ_ASSERT(mTransaction.isNothing());
  mTransaction = Some(U2FTransaction(AsVariant(callback)));
  mChild->SendRequestSign(mTransaction.ref().mId, info);
}

void U2F::FinishGetAssertion(const uint64_t& aTransactionId,
                             const WebAuthnGetAssertionResult& aResult) {
  MOZ_ASSERT(NS_IsMainThread());

  // Check for a valid transaction.
  if (mTransaction.isNothing() || mTransaction.ref().mId != aTransactionId) {
    return;
  }

  if (NS_WARN_IF(!mTransaction.ref().HasSignCallback())) {
    RejectTransaction(NS_ERROR_ABORT);
    return;
  }

  // A CTAP2 response.
  if (aResult.SignatureData().Length() == 0) {
    RejectTransaction(NS_ERROR_ABORT);
    return;
  }

  CryptoBuffer clientDataBuf;
  if (NS_WARN_IF(!clientDataBuf.Assign(aResult.ClientDataJSON()))) {
    RejectTransaction(NS_ERROR_ABORT);
    return;
  }

  CryptoBuffer credBuf;
  if (NS_WARN_IF(!credBuf.Assign(aResult.KeyHandle()))) {
    RejectTransaction(NS_ERROR_ABORT);
    return;
  }

  CryptoBuffer sigBuf;
  if (NS_WARN_IF(!sigBuf.Assign(aResult.SignatureData()))) {
    RejectTransaction(NS_ERROR_ABORT);
    return;
  }

  // Assemble a response object to return
  nsString clientDataBase64;
  nsString signatureDataBase64;
  nsString keyHandleBase64;
  nsresult rvClientData = clientDataBuf.ToJwkBase64(clientDataBase64);
  nsresult rvSignatureData = sigBuf.ToJwkBase64(signatureDataBase64);
  nsresult rvKeyHandle = credBuf.ToJwkBase64(keyHandleBase64);
  if (NS_WARN_IF(NS_FAILED(rvClientData)) ||
      NS_WARN_IF(NS_FAILED(rvSignatureData) ||
                 NS_WARN_IF(NS_FAILED(rvKeyHandle)))) {
    RejectTransaction(NS_ERROR_ABORT);
    return;
  }

  SignResponse response;
  response.mKeyHandle.Construct(keyHandleBase64);
  response.mClientData.Construct(clientDataBase64);
  response.mSignatureData.Construct(signatureDataBase64);
  response.mErrorCode.Construct(static_cast<uint32_t>(ErrorCode::OK));

  // Keep the callback pointer alive.
  nsMainThreadPtrHandle<U2FSignCallback> callback(
      mTransaction.ref().GetSignCallback());

  ClearTransaction();
  ExecuteCallback(response, callback);
}

void U2F::ClearTransaction() {
  if (!NS_WARN_IF(mTransaction.isNothing())) {
    StopListeningForVisibilityEvents();
  }

  mTransaction.reset();
}

void U2F::RejectTransaction(const nsresult& aError) {
  if (NS_WARN_IF(mTransaction.isNothing())) {
    return;
  }

  StopListeningForVisibilityEvents();

  // Clear out mTransaction before calling ExecuteCallback() below to allow
  // reentrancy from microtask checkpoints.
  Maybe<U2FTransaction> maybeTransaction(std::move(mTransaction));
  MOZ_ASSERT(mTransaction.isNothing() && maybeTransaction.isSome());

  U2FTransaction& transaction = maybeTransaction.ref();
  ErrorCode code = ConvertNSResultToErrorCode(aError);

  if (transaction.HasRegisterCallback()) {
    RegisterResponse response;
    response.mErrorCode.Construct(static_cast<uint32_t>(code));
    ExecuteCallback(response, transaction.GetRegisterCallback());
  }

  if (transaction.HasSignCallback()) {
    SignResponse response;
    response.mErrorCode.Construct(static_cast<uint32_t>(code));
    ExecuteCallback(response, transaction.GetSignCallback());
  }
}

void U2F::CancelTransaction(const nsresult& aError) {
  if (!NS_WARN_IF(!mChild || mTransaction.isNothing())) {
    mChild->SendRequestCancel(mTransaction.ref().mId);
  }

  RejectTransaction(aError);
}

void U2F::RequestAborted(const uint64_t& aTransactionId,
                         const nsresult& aError) {
  MOZ_ASSERT(NS_IsMainThread());

  if (mTransaction.isSome() && mTransaction.ref().mId == aTransactionId) {
    RejectTransaction(aError);
  }
}

}  // namespace dom
}  // namespace mozilla
