/* -*- Mode: C++; tab-width: 2; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "WinIMEHandler.h"

#include "IMMHandler.h"
#include "mozilla/Preferences.h"
#include "mozilla/TextEvents.h"
#include "mozilla/WindowsVersion.h"
#include "nsWindowDefs.h"
#include "WinTextEventDispatcherListener.h"

#ifdef NS_ENABLE_TSF
#include "TSFTextStore.h"
#endif  // #ifdef NS_ENABLE_TSF

#include "nsLookAndFeel.h"
#include "nsWindow.h"
#include "WinUtils.h"
#include "nsIWindowsRegKey.h"
#include "nsIWindowsUIUtils.h"

#ifdef ACCESSIBILITY
#include "nsAccessibilityService.h"
#endif  // #ifdef ACCESSIBILITY

#include "shellapi.h"
#include "shlobj.h"
#include "powrprof.h"
#include "setupapi.h"
#include "cfgmgr32.h"

const char* kOskPathPrefName = "ui.osk.on_screen_keyboard_path";
const char* kOskEnabled = "ui.osk.enabled";
const char* kOskDetectPhysicalKeyboard = "ui.osk.detect_physical_keyboard";
const char* kOskRequireWin10 = "ui.osk.require_win10";
const char* kOskDebugReason = "ui.osk.debug.keyboardDisplayReason";

namespace mozilla {
namespace widget {

/******************************************************************************
 * IMEHandler
 ******************************************************************************/

nsWindow* IMEHandler::sFocusedWindow = nullptr;
InputContextAction::Cause IMEHandler::sLastContextActionCause =
    InputContextAction::CAUSE_UNKNOWN;
bool IMEHandler::sMaybeEditable = false;
bool IMEHandler::sForceDisableCurrentIMM_IME = false;
bool IMEHandler::sPluginHasFocus = false;
bool IMEHandler::sNativeCaretIsCreated = false;
bool IMEHandler::sHasNativeCaretBeenRequested = false;

#ifdef NS_ENABLE_TSF
bool IMEHandler::sIsInTSFMode = false;
bool IMEHandler::sIsIMMEnabled = true;
bool IMEHandler::sAssociateIMCOnlyWhenIMM_IMEActive = false;
decltype(SetInputScopes)* IMEHandler::sSetInputScopes = nullptr;
#endif  // #ifdef NS_ENABLE_TSF

static POWER_PLATFORM_ROLE sPowerPlatformRole = PlatformRoleUnspecified;
static bool sDeterminedPowerPlatformRole = false;

// static
void IMEHandler::Initialize() {
#ifdef NS_ENABLE_TSF
  TSFTextStore::Initialize();
  sIsInTSFMode = TSFTextStore::IsInTSFMode();
  sIsIMMEnabled =
      !sIsInTSFMode || Preferences::GetBool("intl.tsf.support_imm", true);
  sAssociateIMCOnlyWhenIMM_IMEActive =
      sIsIMMEnabled &&
      Preferences::GetBool("intl.tsf.associate_imc_only_when_imm_ime_is_active",
                           false);
  if (!sIsInTSFMode) {
    // When full TSFTextStore is not available, try to use SetInputScopes API
    // to enable at least InputScope. Use GET_MODULE_HANDLE_EX_FLAG_PIN to
    // ensure that msctf.dll will not be unloaded.
    HMODULE module = nullptr;
    if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_PIN, L"msctf.dll",
                           &module)) {
      sSetInputScopes = reinterpret_cast<decltype(SetInputScopes)*>(
          GetProcAddress(module, "SetInputScopes"));
    }
  }
#endif  // #ifdef NS_ENABLE_TSF

  IMMHandler::Initialize();

  sForceDisableCurrentIMM_IME = IMMHandler::IsActiveIMEInBlockList();
}

// static
void IMEHandler::Terminate() {
#ifdef NS_ENABLE_TSF
  if (sIsInTSFMode) {
    TSFTextStore::Terminate();
    sIsInTSFMode = false;
  }
#endif  // #ifdef NS_ENABLE_TSF

  IMMHandler::Terminate();
  WinTextEventDispatcherListener::Shutdown();
}

// static
void* IMEHandler::GetNativeData(nsWindow* aWindow, uint32_t aDataType) {
  if (aDataType == NS_RAW_NATIVE_IME_CONTEXT) {
#ifdef NS_ENABLE_TSF
    if (IsTSFAvailable()) {
      return TSFTextStore::GetThreadManager();
    }
#endif  // #ifdef NS_ENABLE_TSF
    IMEContext context(aWindow);
    if (context.IsValid()) {
      return context.get();
    }
    // If IMC isn't associated with the window, IME is disabled on the window
    // now.  In such case, we should return default IMC instead.
    const IMEContext& defaultIMC = aWindow->DefaultIMC();
    if (defaultIMC.IsValid()) {
      return defaultIMC.get();
    }
    // If there is no default IMC, we should return the pointer to the window
    // since if we return nullptr, IMEStateManager cannot manage composition
    // with TextComposition instance.  This is possible if no IME is installed,
    // but composition may occur with dead key sequence.
    return aWindow;
  }

#ifdef NS_ENABLE_TSF
  void* result = TSFTextStore::GetNativeData(aDataType);
  if (!result || !(*(static_cast<void**>(result)))) {
    return nullptr;
  }
  // XXX During the TSF module test, sIsInTSFMode must be true.  After that,
  //     the value should be restored but currently, there is no way for that.
  //     When the TSF test is enabled again, we need to fix this.  Perhaps,
  //     sending a message can fix this.
  sIsInTSFMode = true;
  return result;
#else   // #ifdef NS_ENABLE_TSF
  return nullptr;
#endif  // #ifdef NS_ENABLE_TSF #else
}

// static
bool IMEHandler::ProcessRawKeyMessage(const MSG& aMsg) {
#ifdef NS_ENABLE_TSF
  if (IsTSFAvailable()) {
    return TSFTextStore::ProcessRawKeyMessage(aMsg);
  }
#endif           // #ifdef NS_ENABLE_TSF
  return false;  // noting to do in IMM mode.
}

// static
bool IMEHandler::ProcessMessage(nsWindow* aWindow, UINT aMessage,
                                WPARAM& aWParam, LPARAM& aLParam,
                                MSGResult& aResult) {
  if (aMessage == MOZ_WM_DISMISS_ONSCREEN_KEYBOARD) {
    if (!sFocusedWindow) {
      DismissOnScreenKeyboard();
    }
    return true;
  }

  // If we're putting native caret over our caret, Windows dispatches
  // EVENT_OBJECT_LOCATIONCHANGE event on other applications which hook
  // the event with ::SetWinEventHook() and handles WM_GETOBJECT for
  // OBJID_CARET (this is request of caret from such applications) instead
  // of us.  If a11y module is active, it observes every our caret change
  // and put native caret over it automatically.  However, if other
  // applications require only caret information, activating a11y module is
  // overwork and such applications may requires carets only in editors.
  // Therefore, if it'd be possible, IMEHandler should put native caret over
  // our caret, but there is a problem.  Some versions of ATOK (Japanese TIP)
  // refer native caret and if there is, the behavior is worse than the
  // behavior without native caret.  Therefore, we shouldn't put native caret
  // as far as possible.
  if (!sHasNativeCaretBeenRequested && aMessage == WM_GETOBJECT &&
      static_cast<DWORD>(aLParam) == OBJID_CARET) {
    // So, when we receive first WM_GETOBJECT for OBJID_CARET, let's start to
    // create native caret for such applications.
    sHasNativeCaretBeenRequested = true;
    // If an editable element has focus, we can put native caret now.
    // XXX Should we avoid doing this if there is composition?
    MaybeCreateNativeCaret(aWindow);
  }

#ifdef NS_ENABLE_TSF
  if (IsTSFAvailable()) {
    TSFTextStore::ProcessMessage(aWindow, aMessage, aWParam, aLParam, aResult);
    if (aResult.mConsumed) {
      return true;
    }
    // If we don't support IMM in TSF mode, we don't use IMMHandler.
    if (!sIsIMMEnabled) {
      return false;
    }
    // IME isn't implemented with IMM, IMMHandler shouldn't handle any
    // messages.
    if (!IsIMMActive()) {
      return false;
    }
  }
#endif  // #ifdef NS_ENABLE_TSF

  bool keepGoing =
      IMMHandler::ProcessMessage(aWindow, aMessage, aWParam, aLParam, aResult);

  // If user changes active IME to an IME which is listed in our block list,
  // we should disassociate IMC from the window for preventing the IME to work
  // and crash.
  if (aMessage == WM_INPUTLANGCHANGE) {
    bool disableIME = IMMHandler::IsActiveIMEInBlockList();
    if (disableIME != sForceDisableCurrentIMM_IME) {
      bool enable =
          !disableIME && WinUtils::IsIMEEnabled(aWindow->InputContextRef());
      AssociateIMEContext(aWindow, enable);
      sForceDisableCurrentIMM_IME = disableIME;
    }
  }

  return keepGoing;
}

// static
bool IMEHandler::IsA11yHandlingNativeCaret() {
#ifndef ACCESSIBILITY
  return false;
#else   // #ifndef ACCESSIBILITY
  // Let's assume that when there is the service, it handles native caret.
  return GetAccService() != nullptr;
#endif  // #ifndef ACCESSIBILITY #else
}

#ifdef NS_ENABLE_TSF
// static
bool IMEHandler::IsIMMActive() { return TSFTextStore::IsIMM_IMEActive(); }

#endif  // #ifdef NS_ENABLE_TSF

// static
bool IMEHandler::IsComposing() {
#ifdef NS_ENABLE_TSF
  if (IsTSFAvailable()) {
    return TSFTextStore::IsComposing() || IMMHandler::IsComposing();
  }
#endif  // #ifdef NS_ENABLE_TSF

  return IMMHandler::IsComposing();
}

// static
bool IMEHandler::IsComposingOn(nsWindow* aWindow) {
#ifdef NS_ENABLE_TSF
  if (IsTSFAvailable()) {
    return TSFTextStore::IsComposingOn(aWindow) ||
           IMMHandler::IsComposingOn(aWindow);
  }
#endif  // #ifdef NS_ENABLE_TSF

  return IMMHandler::IsComposingOn(aWindow);
}

// static
nsresult IMEHandler::NotifyIME(nsWindow* aWindow,
                               const IMENotification& aIMENotification) {
#ifdef NS_ENABLE_TSF
  if (IsTSFAvailable()) {
    switch (aIMENotification.mMessage) {
      case NOTIFY_IME_OF_SELECTION_CHANGE: {
        nsresult rv = TSFTextStore::OnSelectionChange(aIMENotification);
        // If IMM IME is active, we need to notify IMMHandler of updating
        // composition change.  It will adjust candidate window position or
        // composition window position.
        bool isIMMActive = IsIMMActive();
        if (isIMMActive) {
          IMMHandler::OnUpdateComposition(aWindow);
        }
        IMMHandler::OnSelectionChange(aWindow, aIMENotification, isIMMActive);
        return rv;
      }
      case NOTIFY_IME_OF_COMPOSITION_EVENT_HANDLED:
        // If IMM IME is active, we need to notify IMMHandler of updating
        // composition change.  It will adjust candidate window position or
        // composition window position.
        if (IsIMMActive()) {
          IMMHandler::OnUpdateComposition(aWindow);
        } else {
          TSFTextStore::OnUpdateComposition();
        }
        return NS_OK;
      case NOTIFY_IME_OF_TEXT_CHANGE:
        return TSFTextStore::OnTextChange(aIMENotification);
      case NOTIFY_IME_OF_FOCUS: {
        sFocusedWindow = aWindow;
        IMMHandler::OnFocusChange(true, aWindow);
        nsresult rv = TSFTextStore::OnFocusChange(true, aWindow,
                                                  aWindow->GetInputContext());
        MaybeCreateNativeCaret(aWindow);
        IMEHandler::MaybeShowOnScreenKeyboard();
        return rv;
      }
      case NOTIFY_IME_OF_BLUR:
        sFocusedWindow = nullptr;
        IMEHandler::MaybeDismissOnScreenKeyboard(aWindow);
        IMMHandler::OnFocusChange(false, aWindow);
        return TSFTextStore::OnFocusChange(false, aWindow,
                                           aWindow->GetInputContext());
      case NOTIFY_IME_OF_MOUSE_BUTTON_EVENT:
        // If IMM IME is active, we should send a mouse button event via IMM.
        if (IsIMMActive()) {
          return IMMHandler::OnMouseButtonEvent(aWindow, aIMENotification);
        }
        return TSFTextStore::OnMouseButtonEvent(aIMENotification);
      case REQUEST_TO_COMMIT_COMPOSITION:
        if (TSFTextStore::IsComposingOn(aWindow)) {
          TSFTextStore::CommitComposition(false);
        } else if (IsIMMActive()) {
          IMMHandler::CommitComposition(aWindow);
        }
        return NS_OK;
      case REQUEST_TO_CANCEL_COMPOSITION:
        if (TSFTextStore::IsComposingOn(aWindow)) {
          TSFTextStore::CommitComposition(true);
        } else if (IsIMMActive()) {
          IMMHandler::CancelComposition(aWindow);
        }
        return NS_OK;
      case NOTIFY_IME_OF_POSITION_CHANGE:
        return TSFTextStore::OnLayoutChange();
      default:
        return NS_ERROR_NOT_IMPLEMENTED;
    }
  }
#endif  // NS_ENABLE_TSF

  switch (aIMENotification.mMessage) {
    case REQUEST_TO_COMMIT_COMPOSITION:
      IMMHandler::CommitComposition(aWindow);
      return NS_OK;
    case REQUEST_TO_CANCEL_COMPOSITION:
      IMMHandler::CancelComposition(aWindow);
      return NS_OK;
    case NOTIFY_IME_OF_POSITION_CHANGE:
    case NOTIFY_IME_OF_COMPOSITION_EVENT_HANDLED:
      IMMHandler::OnUpdateComposition(aWindow);
      return NS_OK;
    case NOTIFY_IME_OF_SELECTION_CHANGE:
      IMMHandler::OnSelectionChange(aWindow, aIMENotification, true);
      // IMMHandler::OnSelectionChange() cannot work without its singleton
      // instance.  Therefore, IMEHandler needs to create native caret instead
      // if it's necessary.
      MaybeCreateNativeCaret(aWindow);
      return NS_OK;
    case NOTIFY_IME_OF_MOUSE_BUTTON_EVENT:
      return IMMHandler::OnMouseButtonEvent(aWindow, aIMENotification);
    case NOTIFY_IME_OF_FOCUS:
      sFocusedWindow = aWindow;
      IMMHandler::OnFocusChange(true, aWindow);
      IMEHandler::MaybeShowOnScreenKeyboard();
      MaybeCreateNativeCaret(aWindow);
      return NS_OK;
    case NOTIFY_IME_OF_BLUR:
      sFocusedWindow = nullptr;
      IMEHandler::MaybeDismissOnScreenKeyboard(aWindow);
      IMMHandler::OnFocusChange(false, aWindow);
#ifdef NS_ENABLE_TSF
      // If a plugin gets focus while TSF has focus, we need to notify TSF of
      // the blur.
      if (TSFTextStore::ThinksHavingFocus()) {
        return TSFTextStore::OnFocusChange(false, aWindow,
                                           aWindow->GetInputContext());
      }
#endif  // NS_ENABLE_TSF
      return NS_OK;
    default:
      return NS_ERROR_NOT_IMPLEMENTED;
  }
}

// static
IMENotificationRequests IMEHandler::GetIMENotificationRequests() {
  // While a plugin has focus, neither TSFTextStore nor IMMHandler needs
  // notifications.
  if (sPluginHasFocus) {
    return IMENotificationRequests();
  }

#ifdef NS_ENABLE_TSF
  if (IsTSFAvailable()) {
    if (!sIsIMMEnabled) {
      return TSFTextStore::GetIMENotificationRequests();
    }
    // Even if TSF is available, the active IME may be an IMM-IME.
    // Unfortunately, changing the result of GetIMENotificationRequests() while
    // an editor has focus isn't supported by IMEContentObserver nor
    // ContentCacheInParent.  Therefore, we need to request whole notifications
    // which are necessary either IMMHandler or TSFTextStore.
    return IMMHandler::GetIMENotificationRequests() |
           TSFTextStore::GetIMENotificationRequests();
  }
#endif  // NS_ENABLE_TSF

  return IMMHandler::GetIMENotificationRequests();
}

// static
TextEventDispatcherListener*
IMEHandler::GetNativeTextEventDispatcherListener() {
  return WinTextEventDispatcherListener::GetInstance();
}

// static
bool IMEHandler::GetOpenState(nsWindow* aWindow) {
#ifdef NS_ENABLE_TSF
  if (IsTSFAvailable() && !IsIMMActive()) {
    return TSFTextStore::GetIMEOpenState();
  }
#endif  // NS_ENABLE_TSF

  IMEContext context(aWindow);
  return context.GetOpenState();
}

// static
void IMEHandler::OnDestroyWindow(nsWindow* aWindow) {
  // When focus is in remote process, but the window is being destroyed, we
  // need to clean up TSFTextStore here since NOTIFY_IME_OF_BLUR won't reach
  // here because TabParent already lost the reference to the nsWindow when
  // it receives from the remote process.
  if (sFocusedWindow == aWindow) {
    MOZ_ASSERT(aWindow->GetInputContext().IsOriginContentProcess(),
               "input context of focused widget should've been set by a remote "
               "process "
               "if IME focus isn't cleared before destroying the widget");
    NotifyIME(aWindow, IMENotification(NOTIFY_IME_OF_BLUR));
  }

#ifdef NS_ENABLE_TSF
  // We need to do nothing here for TSF. Just restore the default context
  // if it's been disassociated.
  if (!sIsInTSFMode) {
    // MSDN says we need to set IS_DEFAULT to avoid memory leak when we use
    // SetInputScopes API. Use an empty string to do this.
    SetInputScopeForIMM32(aWindow, EmptyString(), EmptyString());
  }
#endif  // #ifdef NS_ENABLE_TSF
  AssociateIMEContext(aWindow, true);
}

#ifdef NS_ENABLE_TSF
// static
bool IMEHandler::NeedsToAssociateIMC() {
  return !sForceDisableCurrentIMM_IME &&
         (!sAssociateIMCOnlyWhenIMM_IMEActive || !IsIMMActive());
}
#endif  // #ifdef NS_ENABLE_TSF

// static
void IMEHandler::SetInputContext(nsWindow* aWindow, InputContext& aInputContext,
                                 const InputContextAction& aAction) {
  sLastContextActionCause = aAction.mCause;
  // FYI: If there is no composition, this call will do nothing.
  NotifyIME(aWindow, IMENotification(REQUEST_TO_COMMIT_COMPOSITION));

  const InputContext& oldInputContext = aWindow->GetInputContext();

  // Assume that SetInputContext() is called only when aWindow has focus.
  sPluginHasFocus = (aInputContext.mIMEState.mEnabled == IMEState::PLUGIN);

  if (aAction.UserMightRequestOpenVKB()) {
    IMEHandler::MaybeShowOnScreenKeyboard();
  }

  bool enable = WinUtils::IsIMEEnabled(aInputContext);
  bool adjustOpenState = (enable && aInputContext.mIMEState.mOpen !=
                                        IMEState::DONT_CHANGE_OPEN_STATE);
  bool open =
      (adjustOpenState && aInputContext.mIMEState.mOpen == IMEState::OPEN);

#ifdef NS_ENABLE_TSF
  // Note that even while a plugin has focus, we need to notify TSF of that.
  if (sIsInTSFMode) {
    TSFTextStore::SetInputContext(aWindow, aInputContext, aAction);
    if (IsTSFAvailable()) {
      if (sIsIMMEnabled) {
        // Associate IMC with aWindow only when it's necessary.
        AssociateIMEContext(aWindow, enable && NeedsToAssociateIMC());
      } else if (oldInputContext.mIMEState.mEnabled == IMEState::PLUGIN) {
        // Disassociate the IME context from the window when plugin loses focus
        // in pure TSF mode.
        AssociateIMEContext(aWindow, false);
      }
      if (adjustOpenState) {
        TSFTextStore::SetIMEOpenState(open);
      }
      return;
    }
  } else {
    // Set at least InputScope even when TextStore is not available.
    SetInputScopeForIMM32(aWindow, aInputContext.mHTMLInputType,
                          aInputContext.mHTMLInputInputmode);
  }
#endif  // #ifdef NS_ENABLE_TSF

  AssociateIMEContext(aWindow, enable);

  IMEContext context(aWindow);
  if (adjustOpenState) {
    context.SetOpenState(open);
  }
}

// static
void IMEHandler::AssociateIMEContext(nsWindowBase* aWindowBase, bool aEnable) {
  IMEContext context(aWindowBase);
  if (aEnable) {
    context.AssociateDefaultContext();
    return;
  }
  // Don't disassociate the context after the window is destroyed.
  if (aWindowBase->Destroyed()) {
    return;
  }
  context.Disassociate();
}

// static
void IMEHandler::InitInputContext(nsWindow* aWindow,
                                  InputContext& aInputContext) {
  MOZ_ASSERT(aWindow);
  MOZ_ASSERT(aWindow->GetWindowHandle(),
             "IMEHandler::SetInputContext() requires non-nullptr HWND");

  static bool sInitialized = false;
  if (!sInitialized) {
    sInitialized = true;
    // Some TIPs like QQ Input (Simplified Chinese) may need normal window
    // (i.e., windows except message window) when initializing themselves.
    // Therefore, we need to initialize TSF/IMM modules after first normal
    // window is created.  InitInputContext() should be called immediately
    // after creating each normal window, so, here is a good place to
    // initialize these modules.
    Initialize();
  }

  // For a11y, the default enabled state should be 'enabled'.
  aInputContext.mIMEState.mEnabled = IMEState::ENABLED;

#ifdef NS_ENABLE_TSF
  if (sIsInTSFMode) {
    TSFTextStore::SetInputContext(
        aWindow, aInputContext,
        InputContextAction(InputContextAction::CAUSE_UNKNOWN,
                           InputContextAction::WIDGET_CREATED));
    // IME context isn't necessary in pure TSF mode.
    if (!sIsIMMEnabled) {
      AssociateIMEContext(aWindow, false);
    }
    return;
  }
#endif  // #ifdef NS_ENABLE_TSF

#ifdef DEBUG
  // NOTE: IMC may be null if IMM module isn't installed.
  IMEContext context(aWindow);
  MOZ_ASSERT(context.IsValid() || !CurrentKeyboardLayoutHasIME());
#endif  // #ifdef DEBUG
}

#ifdef DEBUG
// static
bool IMEHandler::CurrentKeyboardLayoutHasIME() {
#ifdef NS_ENABLE_TSF
  if (sIsInTSFMode) {
    return TSFTextStore::CurrentKeyboardLayoutHasIME();
  }
#endif  // #ifdef NS_ENABLE_TSF

  return IMMHandler::IsIMEAvailable();
}
#endif  // #ifdef DEBUG

// static
void IMEHandler::OnKeyboardLayoutChanged() {
  // Be aware, this method won't be called until TSFStaticSink starts to
  // observe active TIP change.  If you need to be notified of this, you
  // need to create TSFStaticSink::Observe() or something and call it
  // TSFStaticSink::EnsureInitActiveTIPKeyboard() forcibly.

  if (!sIsIMMEnabled || !IsTSFAvailable()) {
    return;
  }

  // We don't need to do anything when sAssociateIMCOnlyWhenIMM_IMEActive is
  // false because IMContext won't be associated/disassociated when changing
  // active keyboard layout/IME.
  if (!sAssociateIMCOnlyWhenIMM_IMEActive) {
    return;
  }

  // If there is no TSFTextStore which has focus, i.e., no editor has focus,
  // nothing to do here.
  nsWindowBase* windowBase = TSFTextStore::GetEnabledWindowBase();
  if (!windowBase) {
    return;
  }

  // If IME isn't available, nothing to do here.
  InputContext inputContext = windowBase->GetInputContext();
  if (!WinUtils::IsIMEEnabled(inputContext)) {
    return;
  }

  // Associate or Disassociate IMC if it's necessary.
  // Note that this does nothing if the window has already associated with or
  // disassociated from the window.
  AssociateIMEContext(windowBase, NeedsToAssociateIMC());
}

// static
void IMEHandler::SetInputScopeForIMM32(nsWindow* aWindow,
                                       const nsAString& aHTMLInputType,
                                       const nsAString& aHTMLInputInputmode) {
  if (sIsInTSFMode || !sSetInputScopes || aWindow->Destroyed()) {
    return;
  }
  UINT arraySize = 0;
  const InputScope* scopes = nullptr;
  // http://www.whatwg.org/specs/web-apps/current-work/multipage/the-input-element.html
  if (aHTMLInputType.IsEmpty() || aHTMLInputType.EqualsLiteral("text")) {
    if (aHTMLInputInputmode.EqualsLiteral("url")) {
      static const InputScope inputScopes[] = {IS_URL};
      scopes = &inputScopes[0];
      arraySize = ArrayLength(inputScopes);
    } else if (aHTMLInputInputmode.EqualsLiteral("mozAwesomebar")) {
      // Even if Awesomebar has focus, user may not input URL directly.
      // However, on-screen keyboard for URL should be shown because it has
      // some useful additional keys like ".com" and they are not hindrances
      // even when inputting non-URL text, e.g., words to search something in
      // the web.  On the other hand, a lot of Microsoft's IMEs and Google
      // Japanese Input make their open state "closed" automatically if we
      // notify them of URL as the input scope.  However, this is very annoying
      // for the users when they try to input some words to search the web or
      // bookmark/history items.  Therefore, if they are active, we need to
      // notify them of the default input scope for avoiding this issue.
      // FYI: We cannot check active TIP without TSF.  Therefore, if it's
      //      not in TSF mode, this will check only if active IMM-IME is Google
      //      Japanese Input.  Google Japanese Input is a TIP of TSF basically.
      //      However, if the OS is Win7 or it's installed on Win7 but has not
      //      been updated yet even after the OS is upgraded to Win8 or later,
      //      it's installed as IMM-IME.
      if (TSFTextStore::ShouldSetInputScopeOfURLBarToDefault()) {
        static const InputScope inputScopes[] = {IS_DEFAULT};
        scopes = &inputScopes[0];
        arraySize = ArrayLength(inputScopes);
      } else {
        static const InputScope inputScopes[] = {IS_URL};
        scopes = &inputScopes[0];
        arraySize = ArrayLength(inputScopes);
      }
    } else if (aHTMLInputInputmode.EqualsLiteral("email")) {
      static const InputScope inputScopes[] = {IS_EMAIL_SMTPEMAILADDRESS};
      scopes = &inputScopes[0];
      arraySize = ArrayLength(inputScopes);
    } else if (aHTMLInputInputmode.EqualsLiteral("tel")) {
      static const InputScope inputScopes[] = {
          IS_TELEPHONE_LOCALNUMBER, IS_TELEPHONE_FULLTELEPHONENUMBER};
      scopes = &inputScopes[0];
      arraySize = ArrayLength(inputScopes);
    } else if (aHTMLInputInputmode.EqualsLiteral("numeric")) {
      static const InputScope inputScopes[] = {IS_NUMBER};
      scopes = &inputScopes[0];
      arraySize = ArrayLength(inputScopes);
    } else {
      static const InputScope inputScopes[] = {IS_DEFAULT};
      scopes = &inputScopes[0];
      arraySize = ArrayLength(inputScopes);
    }
  } else if (aHTMLInputType.EqualsLiteral("url")) {
    static const InputScope inputScopes[] = {IS_URL};
    scopes = &inputScopes[0];
    arraySize = ArrayLength(inputScopes);
  } else if (aHTMLInputType.EqualsLiteral("search")) {
    static const InputScope inputScopes[] = {IS_SEARCH};
    scopes = &inputScopes[0];
    arraySize = ArrayLength(inputScopes);
  } else if (aHTMLInputType.EqualsLiteral("email")) {
    static const InputScope inputScopes[] = {IS_EMAIL_SMTPEMAILADDRESS};
    scopes = &inputScopes[0];
    arraySize = ArrayLength(inputScopes);
  } else if (aHTMLInputType.EqualsLiteral("password")) {
    static const InputScope inputScopes[] = {IS_PASSWORD};
    scopes = &inputScopes[0];
    arraySize = ArrayLength(inputScopes);
  } else if (aHTMLInputType.EqualsLiteral("datetime") ||
             aHTMLInputType.EqualsLiteral("datetime-local")) {
    static const InputScope inputScopes[] = {IS_DATE_FULLDATE,
                                             IS_TIME_FULLTIME};
    scopes = &inputScopes[0];
    arraySize = ArrayLength(inputScopes);
  } else if (aHTMLInputType.EqualsLiteral("date") ||
             aHTMLInputType.EqualsLiteral("month") ||
             aHTMLInputType.EqualsLiteral("week")) {
    static const InputScope inputScopes[] = {IS_DATE_FULLDATE};
    scopes = &inputScopes[0];
    arraySize = ArrayLength(inputScopes);
  } else if (aHTMLInputType.EqualsLiteral("time")) {
    static const InputScope inputScopes[] = {IS_TIME_FULLTIME};
    scopes = &inputScopes[0];
    arraySize = ArrayLength(inputScopes);
  } else if (aHTMLInputType.EqualsLiteral("tel")) {
    static const InputScope inputScopes[] = {IS_TELEPHONE_FULLTELEPHONENUMBER,
                                             IS_TELEPHONE_LOCALNUMBER};
    scopes = &inputScopes[0];
    arraySize = ArrayLength(inputScopes);
  } else if (aHTMLInputType.EqualsLiteral("number")) {
    static const InputScope inputScopes[] = {IS_NUMBER};
    scopes = &inputScopes[0];
    arraySize = ArrayLength(inputScopes);
  }
  if (scopes && arraySize > 0) {
    sSetInputScopes(aWindow->GetWindowHandle(), scopes, arraySize, nullptr, 0,
                    nullptr, nullptr);
  }
}

// static
void IMEHandler::MaybeShowOnScreenKeyboard() {
  if (sPluginHasFocus || !IsWin8OrLater() ||
      !Preferences::GetBool(kOskEnabled, true) || GetOnScreenKeyboardWindow() ||
      !IMEHandler::NeedOnScreenKeyboard()) {
    return;
  }

  // On Windows 10 we require tablet mode, unless the user has set the relevant
  // Windows setting to enable the on-screen keyboard in desktop mode.
  // We might be disabled specifically on Win8(.1), so we check that afterwards.
  if (IsWin10OrLater()) {
    if (!IsInTabletMode() && !AutoInvokeOnScreenKeyboardInDesktopMode()) {
      return;
    }
  } else if (Preferences::GetBool(kOskRequireWin10, true)) {
    return;
  }

  IMEHandler::ShowOnScreenKeyboard();
}

// static
void IMEHandler::MaybeDismissOnScreenKeyboard(nsWindow* aWindow) {
  if (sPluginHasFocus || !IsWin8OrLater()) {
    return;
  }

  ::PostMessage(aWindow->GetWindowHandle(), MOZ_WM_DISMISS_ONSCREEN_KEYBOARD, 0,
                0);
}

// static
bool IMEHandler::WStringStartsWithCaseInsensitive(const std::wstring& aHaystack,
                                                  const std::wstring& aNeedle) {
  std::wstring lowerCaseHaystack(aHaystack);
  std::wstring lowerCaseNeedle(aNeedle);
  std::transform(lowerCaseHaystack.begin(), lowerCaseHaystack.end(),
                 lowerCaseHaystack.begin(), ::tolower);
  std::transform(lowerCaseNeedle.begin(), lowerCaseNeedle.end(),
                 lowerCaseNeedle.begin(), ::tolower);
  return wcsstr(lowerCaseHaystack.c_str(), lowerCaseNeedle.c_str()) ==
         lowerCaseHaystack.c_str();
}

// Returns false if a physical keyboard is detected on Windows 8 and up,
// or there is some other reason why an onscreen keyboard is not necessary.
// Returns true if no keyboard is found and this device looks like it needs
// an on-screen keyboard for text input.
// static
bool IMEHandler::NeedOnScreenKeyboard() {
  // This function is only supported for Windows 8 and up.
  if (!IsWin8OrLater()) {
    Preferences::SetString(kOskDebugReason, L"IKPOS: Requires Win8+.");
    return false;
  }

  if (!Preferences::GetBool(kOskDetectPhysicalKeyboard, true)) {
    Preferences::SetString(kOskDebugReason, L"IKPOS: Detection disabled.");
    return true;
  }

  // If the last focus cause was not user-initiated (ie a result of code
  // setting focus to an element) then don't auto-show a keyboard. This
  // avoids cases where the keyboard would pop up "just" because e.g. a
  // web page chooses to focus a search field on the page, even when that
  // really isn't what the user is trying to do at that moment.
  if (!InputContextAction::IsHandlingUserInput(sLastContextActionCause)) {
    return false;
  }

  // This function should be only invoked for machines with touch screens.
  if ((::GetSystemMetrics(SM_DIGITIZER) & NID_INTEGRATED_TOUCH) !=
      NID_INTEGRATED_TOUCH) {
    Preferences::SetString(kOskDebugReason, L"IKPOS: Touch screen not found.");
    return false;
  }

  // If the device is docked, the user is treating the device as a PC.
  if (::GetSystemMetrics(SM_SYSTEMDOCKED) != 0) {
    Preferences::SetString(kOskDebugReason, L"IKPOS: System docked.");
    return false;
  }

  // To determine whether a keyboard is present on the device, we do the
  // following:-
  // 1. If the platform role is that of a mobile or slate device, check the
  //    system metric SM_CONVERTIBLESLATEMODE to see if it is being used
  //    in slate mode. If it is, also check that the last input was a touch.
  //    If all of this is true, then we should show the on-screen keyboard.

  // 2. If step 1 didn't determine we should show the keyboard, we check if
  //    this device has keyboards attached to it.

  // Check if the device is being used as a laptop or a tablet. This can be
  // checked by first checking the role of the device and then the
  // corresponding system metric (SM_CONVERTIBLESLATEMODE). If it is being
  // used as a tablet then we want the OSK to show up.
  if (!sDeterminedPowerPlatformRole) {
    sDeterminedPowerPlatformRole = true;
    sPowerPlatformRole = WinUtils::GetPowerPlatformRole();
  }

  // If this a mobile or slate (tablet) device, check if it is in slate mode.
  // If the last input was touch, ignore whether or not a keyboard is present.
  if ((sPowerPlatformRole == PlatformRoleMobile ||
       sPowerPlatformRole == PlatformRoleSlate) &&
      ::GetSystemMetrics(SM_CONVERTIBLESLATEMODE) == 0 &&
      sLastContextActionCause == InputContextAction::CAUSE_TOUCH) {
    Preferences::SetString(
        kOskDebugReason,
        L"IKPOS: Mobile/Slate Platform role, in slate mode with touch event.");
    return true;
  }

  return !IMEHandler::IsKeyboardPresentOnSlate();
}

// Uses the Setup APIs to enumerate the attached keyboards and returns true
// if the keyboard count is 1 or more. While this will work in most cases
// it won't work if there are devices which expose keyboard interfaces which
// are attached to the machine.
// Based on IsKeyboardPresentOnSlate() in Chromium's base/win/win_util.cc.
// static
bool IMEHandler::IsKeyboardPresentOnSlate() {
  const GUID KEYBOARD_CLASS_GUID = {
      0x4D36E96B,
      0xE325,
      0x11CE,
      {0xBF, 0xC1, 0x08, 0x00, 0x2B, 0xE1, 0x03, 0x18}};

  // Query for all the keyboard devices.
  HDEVINFO device_info = ::SetupDiGetClassDevs(&KEYBOARD_CLASS_GUID, nullptr,
                                               nullptr, DIGCF_PRESENT);
  if (device_info == INVALID_HANDLE_VALUE) {
    Preferences::SetString(kOskDebugReason, L"IKPOS: No keyboard info.");
    return false;
  }

  // Enumerate all keyboards and look for ACPI\PNP and HID\VID devices. If
  // the count is more than 1 we assume that a keyboard is present. This is
  // under the assumption that there will always be one keyboard device.
  for (DWORD i = 0;; ++i) {
    SP_DEVINFO_DATA device_info_data = {0};
    device_info_data.cbSize = sizeof(device_info_data);
    if (!::SetupDiEnumDeviceInfo(device_info, i, &device_info_data)) {
      break;
    }

    // Get the device ID.
    wchar_t device_id[MAX_DEVICE_ID_LEN];
    CONFIGRET status = ::CM_Get_Device_ID(device_info_data.DevInst, device_id,
                                          MAX_DEVICE_ID_LEN, 0);
    if (status == CR_SUCCESS) {
      static const std::wstring BT_HID_DEVICE = L"HID\\{00001124";
      static const std::wstring BT_HOGP_DEVICE = L"HID\\{00001812";
      // To reduce the scope of the hack we only look for ACPI and HID\\VID
      // prefixes in the keyboard device ids.
      if (IMEHandler::WStringStartsWithCaseInsensitive(device_id, L"ACPI") ||
          IMEHandler::WStringStartsWithCaseInsensitive(device_id,
                                                       L"HID\\VID") ||
          IMEHandler::WStringStartsWithCaseInsensitive(device_id,
                                                       BT_HID_DEVICE) ||
          IMEHandler::WStringStartsWithCaseInsensitive(device_id,
                                                       BT_HOGP_DEVICE)) {
        // The heuristic we are using is to check the count of keyboards and
        // return true if the API's report one or more keyboards. Please note
        // that this will break for non keyboard devices which expose a
        // keyboard PDO.
        Preferences::SetString(kOskDebugReason,
                               L"IKPOS: Keyboard presence confirmed.");
        return true;
      }
    }
  }
  Preferences::SetString(kOskDebugReason,
                         L"IKPOS: Lack of keyboard confirmed.");
  return false;
}

// static
bool IMEHandler::IsInTabletMode() {
  nsCOMPtr<nsIWindowsUIUtils> uiUtils(
      do_GetService("@mozilla.org/windows-ui-utils;1"));
  if (NS_WARN_IF(!uiUtils)) {
    Preferences::SetString(kOskDebugReason,
                           L"IITM: nsIWindowsUIUtils not available.");
    return false;
  }
  bool isInTabletMode = false;
  uiUtils->GetInTabletMode(&isInTabletMode);
  if (isInTabletMode) {
    Preferences::SetString(kOskDebugReason, L"IITM: GetInTabletMode=true.");
  } else {
    Preferences::SetString(kOskDebugReason, L"IITM: GetInTabletMode=false.");
  }
  return isInTabletMode;
}

// static
bool IMEHandler::AutoInvokeOnScreenKeyboardInDesktopMode() {
  nsresult rv;
  nsCOMPtr<nsIWindowsRegKey> regKey(
      do_CreateInstance("@mozilla.org/windows-registry-key;1", &rv));
  if (NS_WARN_IF(NS_FAILED(rv))) {
    Preferences::SetString(kOskDebugReason,
                           L"AIOSKIDM: "
                           L"nsIWindowsRegKey not available");
    return false;
  }
  rv = regKey->Open(nsIWindowsRegKey::ROOT_KEY_CURRENT_USER,
                    NS_LITERAL_STRING("SOFTWARE\\Microsoft\\TabletTip\\1.7"),
                    nsIWindowsRegKey::ACCESS_QUERY_VALUE);
  if (NS_FAILED(rv)) {
    Preferences::SetString(kOskDebugReason,
                           L"AIOSKIDM: failed opening regkey.");
    return false;
  }
  // EnableDesktopModeAutoInvoke is an opt-in option from the Windows
  // Settings to "Automatically show the touch keyboard in windowed apps
  // when there's no keyboard attached to your device." If the user has
  // opted-in to this behavior, the tablet-mode requirement is skipped.
  uint32_t value;
  rv = regKey->ReadIntValue(NS_LITERAL_STRING("EnableDesktopModeAutoInvoke"),
                            &value);
  if (NS_FAILED(rv)) {
    Preferences::SetString(kOskDebugReason,
                           L"AIOSKIDM: failed reading value of regkey.");
    return false;
  }
  if (!!value) {
    Preferences::SetString(kOskDebugReason, L"AIOSKIDM: regkey value=true.");
  } else {
    Preferences::SetString(kOskDebugReason, L"AIOSKIDM: regkey value=false.");
  }
  return !!value;
}

// Based on DisplayVirtualKeyboard() in Chromium's base/win/win_util.cc.
// static
void IMEHandler::ShowOnScreenKeyboard() {
  nsAutoString cachedPath;
  nsresult result = Preferences::GetString(kOskPathPrefName, cachedPath);
  if (NS_FAILED(result) || cachedPath.IsEmpty()) {
    wchar_t path[MAX_PATH];
    // The path to TabTip.exe is defined at the following registry key.
    // This is pulled out of the 64-bit registry hive directly.
    const wchar_t kRegKeyName[] =
        L"Software\\Classes\\CLSID\\"
        L"{054AAE20-4BEA-4347-8A35-64A533254A9D}\\LocalServer32";
    if (!WinUtils::GetRegistryKey(HKEY_LOCAL_MACHINE, kRegKeyName, nullptr,
                                  path, sizeof path)) {
      return;
    }

    std::wstring wstrpath(path);
    // The path provided by the registry will often contain
    // %CommonProgramFiles%, which will need to be replaced if it is present.
    size_t commonProgramFilesOffset = wstrpath.find(L"%CommonProgramFiles%");
    if (commonProgramFilesOffset != std::wstring::npos) {
      // The path read from the registry contains the %CommonProgramFiles%
      // environment variable prefix. On 64 bit Windows the
      // SHGetKnownFolderPath function returns the common program files path
      // with the X86 suffix for the FOLDERID_ProgramFilesCommon value.
      // To get the correct path to TabTip.exe we first read the environment
      // variable CommonProgramW6432 which points to the desired common
      // files path. Failing that we fallback to the SHGetKnownFolderPath API.

      // We then replace the %CommonProgramFiles% value with the actual common
      // files path found in the process.
      std::wstring commonProgramFilesPath;
      std::vector<wchar_t> commonProgramFilesPathW6432;
      DWORD bufferSize =
          ::GetEnvironmentVariableW(L"CommonProgramW6432", nullptr, 0);
      if (bufferSize) {
        commonProgramFilesPathW6432.resize(bufferSize);
        ::GetEnvironmentVariableW(L"CommonProgramW6432",
                                  commonProgramFilesPathW6432.data(),
                                  bufferSize);
        commonProgramFilesPath =
            std::wstring(commonProgramFilesPathW6432.data());
      } else {
        PWSTR path = nullptr;
        HRESULT hres = SHGetKnownFolderPath(FOLDERID_ProgramFilesCommon, 0,
                                            nullptr, &path);
        if (FAILED(hres) || !path) {
          return;
        }
        commonProgramFilesPath =
            static_cast<const wchar_t*>(nsDependentString(path).get());
        ::CoTaskMemFree(path);
      }
      wstrpath.replace(commonProgramFilesOffset,
                       wcslen(L"%CommonProgramFiles%"), commonProgramFilesPath);
    }

    cachedPath.Assign(wstrpath.data());
    Preferences::SetString(kOskPathPrefName, cachedPath);
  }

  const char16_t* cachedPathPtr;
  cachedPath.GetData(&cachedPathPtr);
  ShellExecuteW(nullptr, L"", char16ptr_t(cachedPathPtr), nullptr, nullptr,
                SW_SHOW);
}

// Based on DismissVirtualKeyboard() in Chromium's base/win/win_util.cc.
// static
void IMEHandler::DismissOnScreenKeyboard() {
  // Dismiss the virtual keyboard if it's open
  HWND osk = GetOnScreenKeyboardWindow();
  if (osk) {
    ::PostMessage(osk, WM_SYSCOMMAND, SC_CLOSE, 0);
  }
}

// static
HWND IMEHandler::GetOnScreenKeyboardWindow() {
  const wchar_t kOSKClassName[] = L"IPTip_Main_Window";
  HWND osk = ::FindWindowW(kOSKClassName, nullptr);
  if (::IsWindow(osk) && ::IsWindowEnabled(osk) && ::IsWindowVisible(osk)) {
    return osk;
  }
  return nullptr;
}

// static
void IMEHandler::SetCandidateWindow(nsWindow* aWindow, CANDIDATEFORM* aForm) {
  if (!sPluginHasFocus) {
    return;
  }

  IMMHandler::SetCandidateWindow(aWindow, aForm);
}

// static
void IMEHandler::DefaultProcOfPluginEvent(nsWindow* aWindow,
                                          const NPEvent* aPluginEvent) {
  if (!sPluginHasFocus) {
    return;
  }
  IMMHandler::DefaultProcOfPluginEvent(aWindow, aPluginEvent);
}

bool IMEHandler::MaybeCreateNativeCaret(nsWindow* aWindow) {
  MOZ_ASSERT(aWindow);

  if (IsA11yHandlingNativeCaret()) {
    return false;
  }

  if (!sHasNativeCaretBeenRequested) {
    // If we have not received WM_GETOBJECT for OBJID_CARET, there may be new
    // application which requires our caret information.  For kicking its
    // window event proc, we should fire a window event here.
    // (If there is such application, sHasNativeCaretBeenRequested will be set
    // to true later.)
    // FYI: If we create native caret and move its position, native caret
    //      causes EVENT_OBJECT_LOCATIONCHANGE event with OBJID_CARET and
    //      OBJID_CLIENT.
    ::NotifyWinEvent(EVENT_OBJECT_LOCATIONCHANGE, aWindow->GetWindowHandle(),
                     OBJID_CARET, OBJID_CLIENT);
    return false;
  }

  MaybeDestroyNativeCaret();

  // If focused content is not text editable, we don't support caret
  // caret information without a11y module.
  if (!aWindow->GetInputContext().mIMEState.IsEditable()) {
    return false;
  }

  WidgetQueryContentEvent queryCaretRect(true, eQueryCaretRect, aWindow);
  aWindow->InitEvent(queryCaretRect);

  WidgetQueryContentEvent::Options options;
  options.mRelativeToInsertionPoint = true;
  queryCaretRect.InitForQueryCaretRect(0, options);

  aWindow->DispatchWindowEvent(&queryCaretRect);
  if (NS_WARN_IF(!queryCaretRect.mSucceeded)) {
    return false;
  }

  return CreateNativeCaret(aWindow, queryCaretRect.mReply.mRect);
}

bool IMEHandler::CreateNativeCaret(nsWindow* aWindow,
                                   const LayoutDeviceIntRect& aCaretRect) {
  MOZ_ASSERT(aWindow);

  MOZ_ASSERT(!IsA11yHandlingNativeCaret());

  sNativeCaretIsCreated =
      ::CreateCaret(aWindow->GetWindowHandle(), nullptr, aCaretRect.Width(),
                    aCaretRect.Height());
  if (!sNativeCaretIsCreated) {
    return false;
  }
  nsWindow* toplevelWindow = aWindow->GetTopLevelWindow(false);
  if (NS_WARN_IF(!toplevelWindow)) {
    MaybeDestroyNativeCaret();
    return false;
  }

  LayoutDeviceIntPoint caretPosition(aCaretRect.TopLeft());
  if (toplevelWindow != aWindow) {
    caretPosition += toplevelWindow->WidgetToScreenOffset();
    caretPosition -= aWindow->WidgetToScreenOffset();
  }

  ::SetCaretPos(caretPosition.x, caretPosition.y);
  return true;
}

void IMEHandler::MaybeDestroyNativeCaret() {
  if (!sNativeCaretIsCreated) {
    return;
  }
  ::DestroyCaret();
  sNativeCaretIsCreated = false;
}

}  // namespace widget
}  // namespace mozilla
