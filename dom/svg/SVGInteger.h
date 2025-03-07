/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=8 sts=2 et sw=2 tw=80: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef __NS_SVGINTEGER_H__
#define __NS_SVGINTEGER_H__

#include "nsCycleCollectionParticipant.h"
#include "nsError.h"
#include "SVGAnimatedInteger.h"
#include "nsISMILAttr.h"
#include "SVGElement.h"
#include "mozilla/Attributes.h"
#include "mozilla/UniquePtr.h"

class nsSMILValue;

namespace mozilla {
namespace dom {
class SVGAnimationElement;
}  // namespace dom

class SVGInteger {
 public:
  typedef mozilla::dom::SVGElement SVGElement;

  void Init(uint8_t aAttrEnum = 0xff, int32_t aValue = 0) {
    mAnimVal = mBaseVal = aValue;
    mAttrEnum = aAttrEnum;
    mIsAnimated = false;
    mIsBaseSet = false;
  }

  nsresult SetBaseValueString(const nsAString& aValue, SVGElement* aSVGElement);
  void GetBaseValueString(nsAString& aValue);

  void SetBaseValue(int32_t aValue, SVGElement* aSVGElement);
  int32_t GetBaseValue() const { return mBaseVal; }

  void SetAnimValue(int aValue, SVGElement* aSVGElement);
  int GetAnimValue() const { return mAnimVal; }

  // Returns true if the animated value of this integer has been explicitly
  // set (either by animation, or by taking on the base value which has been
  // explicitly set by markup or a DOM call), false otherwise.
  // If this returns false, the animated value is still valid, that is,
  // usable, and represents the default base value of the attribute.
  bool IsExplicitlySet() const { return mIsAnimated || mIsBaseSet; }

  already_AddRefed<mozilla::dom::SVGAnimatedInteger> ToDOMAnimatedInteger(
      SVGElement* aSVGElement);
  mozilla::UniquePtr<nsISMILAttr> ToSMILAttr(SVGElement* aSVGElement);

 private:
  int32_t mAnimVal;
  int32_t mBaseVal;
  uint8_t mAttrEnum;  // element specified tracking for attribute
  bool mIsAnimated;
  bool mIsBaseSet;

 public:
  struct DOMAnimatedInteger final : public mozilla::dom::SVGAnimatedInteger {
    DOMAnimatedInteger(SVGInteger* aVal, SVGElement* aSVGElement)
        : mozilla::dom::SVGAnimatedInteger(aSVGElement), mVal(aVal) {}
    virtual ~DOMAnimatedInteger();

    SVGInteger* mVal;  // kept alive because it belongs to content

    virtual int32_t BaseVal() override { return mVal->GetBaseValue(); }
    virtual void SetBaseVal(int32_t aValue) override {
      mVal->SetBaseValue(aValue, mSVGElement);
    }

    // Script may have modified animation parameters or timeline -- DOM getters
    // need to flush any resample requests to reflect these modifications.
    virtual int32_t AnimVal() override {
      mSVGElement->FlushAnimations();
      return mVal->GetAnimValue();
    }
  };

  struct SMILInteger : public nsISMILAttr {
   public:
    SMILInteger(SVGInteger* aVal, SVGElement* aSVGElement)
        : mVal(aVal), mSVGElement(aSVGElement) {}

    // These will stay alive because a nsISMILAttr only lives as long
    // as the Compositing step, and DOM elements don't get a chance to
    // die during that.
    SVGInteger* mVal;
    SVGElement* mSVGElement;

    // nsISMILAttr methods
    virtual nsresult ValueFromString(
        const nsAString& aStr,
        const mozilla::dom::SVGAnimationElement* aSrcElement,
        nsSMILValue& aValue, bool& aPreventCachingOfSandwich) const override;
    virtual nsSMILValue GetBaseValue() const override;
    virtual void ClearAnimValue() override;
    virtual nsresult SetAnimValue(const nsSMILValue& aValue) override;
  };
};

}  // namespace mozilla

#endif  //__NS_SVGINTEGER_H__
