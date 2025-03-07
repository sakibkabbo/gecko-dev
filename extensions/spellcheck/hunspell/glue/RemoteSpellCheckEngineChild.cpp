/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "mozilla/UniquePtr.h"
#include "RemoteSpellCheckEngineChild.h"

namespace mozilla {

RemoteSpellcheckEngineChild::RemoteSpellcheckEngineChild(
    mozSpellChecker* aOwner)
    : mOwner(aOwner) {}

RemoteSpellcheckEngineChild::~RemoteSpellcheckEngineChild() {
  // null out the owner's SpellcheckEngineChild to prevent state corruption
  // during shutdown
  mOwner->DeleteRemoteEngine();
}

RefPtr<GenericPromise>
RemoteSpellcheckEngineChild::SetCurrentDictionaryFromList(
    const nsTArray<nsString>& aList) {
  RefPtr<mozSpellChecker> spellChecker = mOwner;

  return SendSetDictionaryFromList(aList)->Then(
      GetMainThreadSerialEventTarget(), __func__,
      [spellChecker](Tuple<bool, nsString>&& aParam) {
        if (!Get<0>(aParam)) {
          spellChecker->mCurrentDictionary.Truncate();
          return GenericPromise::CreateAndReject(NS_ERROR_NOT_AVAILABLE,
                                                 __func__);
        }
        spellChecker->mCurrentDictionary = std::move(Get<1>(aParam));
        return GenericPromise::CreateAndResolve(true, __func__);
      },
      [spellChecker](ResponseRejectReason&& aReason) {
        spellChecker->mCurrentDictionary.Truncate();
        return GenericPromise::CreateAndReject(NS_ERROR_NOT_AVAILABLE,
                                               __func__);
      });
}

}  // namespace mozilla
