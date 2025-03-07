/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=8 sts=2 et sw=2 tw=80: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef MOZILLA_SVGANIMATEDPRESERVEASPECTRATIO_H__
#define MOZILLA_SVGANIMATEDPRESERVEASPECTRATIO_H__

#include "nsCycleCollectionParticipant.h"
#include "nsError.h"
#include "nsISMILAttr.h"
#include "SVGElement.h"
#include "SVGPreserveAspectRatio.h"
#include "mozilla/Attributes.h"
#include "mozilla/UniquePtr.h"

class nsSMILValue;

namespace mozilla {
namespace dom {
class DOMSVGAnimatedPreserveAspectRatio;
class SVGAnimationElement;
}  // namespace dom

class SVGAnimatedPreserveAspectRatio final {
 public:
  void Init() {
    mBaseVal.mAlign =
        dom::SVGPreserveAspectRatio_Binding::SVG_PRESERVEASPECTRATIO_XMIDYMID;
    mBaseVal.mMeetOrSlice =
        dom::SVGPreserveAspectRatio_Binding::SVG_MEETORSLICE_MEET;
    mAnimVal = mBaseVal;
    mIsAnimated = false;
    mIsBaseSet = false;
  }

  nsresult SetBaseValueString(const nsAString& aValue,
                              dom::SVGElement* aSVGElement, bool aDoSetAttr);
  void GetBaseValueString(nsAString& aValue) const;

  void SetBaseValue(const SVGPreserveAspectRatio& aValue,
                    dom::SVGElement* aSVGElement);
  nsresult SetBaseAlign(uint16_t aAlign, dom::SVGElement* aSVGElement) {
    if (aAlign < SVG_ALIGN_MIN_VALID || aAlign > SVG_ALIGN_MAX_VALID) {
      return NS_ERROR_FAILURE;
    }
    SetBaseValue(SVGPreserveAspectRatio(aAlign, mBaseVal.GetMeetOrSlice()),
                 aSVGElement);
    return NS_OK;
  }
  nsresult SetBaseMeetOrSlice(uint16_t aMeetOrSlice,
                              dom::SVGElement* aSVGElement) {
    if (aMeetOrSlice < SVG_MEETORSLICE_MIN_VALID ||
        aMeetOrSlice > SVG_MEETORSLICE_MAX_VALID) {
      return NS_ERROR_FAILURE;
    }
    SetBaseValue(SVGPreserveAspectRatio(mBaseVal.GetAlign(), aMeetOrSlice),
                 aSVGElement);
    return NS_OK;
  }
  void SetAnimValue(uint64_t aPackedValue, dom::SVGElement* aSVGElement);

  const SVGPreserveAspectRatio& GetBaseValue() const { return mBaseVal; }
  const SVGPreserveAspectRatio& GetAnimValue() const { return mAnimVal; }
  bool IsAnimated() const { return mIsAnimated; }
  bool IsExplicitlySet() const { return mIsAnimated || mIsBaseSet; }

  already_AddRefed<mozilla::dom::DOMSVGAnimatedPreserveAspectRatio>
  ToDOMAnimatedPreserveAspectRatio(dom::SVGElement* aSVGElement);
  UniquePtr<nsISMILAttr> ToSMILAttr(dom::SVGElement* aSVGElement);

 private:
  SVGPreserveAspectRatio mAnimVal;
  SVGPreserveAspectRatio mBaseVal;
  bool mIsAnimated;
  bool mIsBaseSet;

 public:
  struct SMILPreserveAspectRatio final : public nsISMILAttr {
   public:
    SMILPreserveAspectRatio(SVGAnimatedPreserveAspectRatio* aVal,
                            dom::SVGElement* aSVGElement)
        : mVal(aVal), mSVGElement(aSVGElement) {}

    // These will stay alive because a nsISMILAttr only lives as long
    // as the Compositing step, and DOM elements don't get a chance to
    // die during that.
    SVGAnimatedPreserveAspectRatio* mVal;
    dom::SVGElement* mSVGElement;

    // nsISMILAttr methods
    virtual nsresult ValueFromString(
        const nsAString& aStr, const dom::SVGAnimationElement* aSrcElement,
        nsSMILValue& aValue, bool& aPreventCachingOfSandwich) const override;
    virtual nsSMILValue GetBaseValue() const override;
    virtual void ClearAnimValue() override;
    virtual nsresult SetAnimValue(const nsSMILValue& aValue) override;
  };
};

namespace dom {
class DOMSVGAnimatedPreserveAspectRatio final : public nsISupports,
                                                public nsWrapperCache {
  ~DOMSVGAnimatedPreserveAspectRatio();

  NS_DECL_CYCLE_COLLECTING_ISUPPORTS
  NS_DECL_CYCLE_COLLECTION_SCRIPT_HOLDER_CLASS(
      DOMSVGAnimatedPreserveAspectRatio)

  DOMSVGAnimatedPreserveAspectRatio(SVGAnimatedPreserveAspectRatio* aVal,
                                    dom::SVGElement* aSVGElement)
      : mVal(aVal), mSVGElement(aSVGElement) {}

  // WebIDL
  dom::SVGElement* GetParentObject() const { return mSVGElement; }
  virtual JSObject* WrapObject(JSContext* aCx,
                               JS::Handle<JSObject*> aGivenProto) override;

  // These aren't weak refs because new objects are returned each time
  already_AddRefed<DOMSVGPreserveAspectRatio> BaseVal();
  already_AddRefed<DOMSVGPreserveAspectRatio> AnimVal();

 protected:
  // kept alive because it belongs to content:
  SVGAnimatedPreserveAspectRatio* mVal;
  RefPtr<dom::SVGElement> mSVGElement;
};

}  // namespace dom
}  // namespace mozilla

#endif  // MOZILLA_SVGANIMATEDPRESERVEASPECTRATIO_H__
