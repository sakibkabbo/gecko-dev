/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=8 sts=2 et sw=2 tw=80: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "VRChild.h"
#include "VRProcessParent.h"
#include "gfxConfig.h"

#include "mozilla/gfx/gfxVars.h"
#include "mozilla/SystemGroup.h"
#include "mozilla/VsyncDispatcher.h"

namespace mozilla {
namespace gfx {

class OpenVRControllerManifestManager {
  NS_INLINE_DECL_THREADSAFE_REFCOUNTING(OpenVRControllerManifestManager)
 public:
  explicit OpenVRControllerManifestManager() = default;

  void SetOpenVRControllerActionPath(const nsCString& aPath) {
    mAction = aPath;
  }

  void SetOpenVRControllerManifestPath(OpenVRControllerType aType,
                                       const nsCString& aPath) {
    mManifest.Put(static_cast<uint32_t>(aType), aPath);
  }

  bool GetActionPath(nsCString* aPath) {
    if (!mAction.IsEmpty()) {
      *aPath = mAction;
      return true;
    }
    return false;
  }

  bool GetManifestPath(OpenVRControllerType aType, nsCString* aPath) {
    return mManifest.Get(static_cast<uint32_t>(aType), aPath);
  }

 private:
  ~OpenVRControllerManifestManager() {
    if (!mAction.IsEmpty() && remove(mAction.BeginReading()) != 0) {
      MOZ_ASSERT(false, "Delete controller action file failed.");
    }
    mAction = "";

    for (auto iter = mManifest.Iter(); !iter.Done(); iter.Next()) {
      nsCString path(iter.Data());
      if (!path.IsEmpty() && remove(path.BeginReading()) != 0) {
        MOZ_ASSERT(false, "Delete controller manifest file failed.");
      }
    }
    mManifest.Clear();
  }

  nsCString mAction;
  nsDataHashtable<nsUint32HashKey, nsCString> mManifest;
  DISALLOW_COPY_AND_ASSIGN(OpenVRControllerManifestManager);
};

StaticRefPtr<OpenVRControllerManifestManager> sOpenVRControllerManifestManager;

VRChild::VRChild(VRProcessParent* aHost) : mHost(aHost) {
  MOZ_ASSERT(XRE_IsParentProcess());
}

void VRChild::ActorDestroy(ActorDestroyReason aWhy) {
  gfxVars::RemoveReceiver(this);
  mHost->OnChannelClosed();
}

void VRChild::Init() {
  // Build a list of prefs the VR process will need. Note that because we
  // limit the VR process to prefs contained in gfxPrefs, we can simplify
  // the message in two ways: one, we only need to send its index in gfxPrefs
  // rather than its name, and two, we only need to send prefs that don't
  // have their default value.
  // Todo: Consider to make our own vrPrefs that we are interested in VR
  // process.
  nsTArray<GfxPrefSetting> prefs;
  for (auto pref : gfxPrefs::all()) {
    if (pref->HasDefaultValue()) {
      continue;
    }

    GfxPrefValue value;
    pref->GetCachedValue(&value);
    prefs.AppendElement(GfxPrefSetting(pref->Index(), value));
  }
  nsTArray<GfxVarUpdate> updates = gfxVars::FetchNonDefaultVars();

  DevicePrefs devicePrefs;
  devicePrefs.hwCompositing() = gfxConfig::GetValue(Feature::HW_COMPOSITING);
  devicePrefs.d3d11Compositing() =
      gfxConfig::GetValue(Feature::D3D11_COMPOSITING);
  devicePrefs.oglCompositing() =
      gfxConfig::GetValue(Feature::OPENGL_COMPOSITING);
  devicePrefs.advancedLayers() = gfxConfig::GetValue(Feature::ADVANCED_LAYERS);
  devicePrefs.useD2D1() = gfxConfig::GetValue(Feature::DIRECT2D);

  SendInit(prefs, updates, devicePrefs);

  if (!sOpenVRControllerManifestManager) {
    sOpenVRControllerManifestManager = new OpenVRControllerManifestManager();
    NS_DispatchToMainThread(NS_NewRunnableFunction(
        "ClearOnShutdown OpenVRControllerManifestManager",
        []() { ClearOnShutdown(&sOpenVRControllerManifestManager); }));
  }

  nsCString output;
  if (sOpenVRControllerManifestManager->GetActionPath(&output)) {
    SendOpenVRControllerActionPathToVR(output);
  }
  if (sOpenVRControllerManifestManager->GetManifestPath(
          OpenVRControllerType::Vive, &output)) {
    SendOpenVRControllerManifestPathToVR(OpenVRControllerType::Vive, output);
  }
  if (sOpenVRControllerManifestManager->GetManifestPath(
          OpenVRControllerType::WMR, &output)) {
    SendOpenVRControllerManifestPathToVR(OpenVRControllerType::WMR, output);
  }
  if (sOpenVRControllerManifestManager->GetManifestPath(
          OpenVRControllerType::Knuckles, &output)) {
    SendOpenVRControllerManifestPathToVR(OpenVRControllerType::Knuckles,
                                         output);
  }
  gfxVars::AddReceiver(this);
}

mozilla::ipc::IPCResult VRChild::RecvOpenVRControllerActionPathToParent(
    const nsCString& aPath) {
  sOpenVRControllerManifestManager->SetOpenVRControllerActionPath(aPath);
  return IPC_OK();
}

mozilla::ipc::IPCResult VRChild::RecvOpenVRControllerManifestPathToParent(
    const OpenVRControllerType& aType, const nsCString& aPath) {
  sOpenVRControllerManifestManager->SetOpenVRControllerManifestPath(aType,
                                                                    aPath);
  return IPC_OK();
}

void VRChild::OnVarChanged(const GfxVarUpdate& aVar) { SendUpdateVar(aVar); }

class DeferredDeleteVRChild : public Runnable {
 public:
  explicit DeferredDeleteVRChild(UniquePtr<VRChild>&& aChild)
      : Runnable("gfx::DeferredDeleteVRChild"), mChild(std::move(aChild)) {}

  NS_IMETHODIMP Run() override { return NS_OK; }

 private:
  UniquePtr<VRChild> mChild;
};

/* static */ void VRChild::Destroy(UniquePtr<VRChild>&& aChild) {
  NS_DispatchToMainThread(new DeferredDeleteVRChild(std::move(aChild)));
}

}  // namespace gfx
}  // namespace mozilla