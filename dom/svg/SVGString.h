/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=8 sts=2 et sw=2 tw=80: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef __NS_SVGSTRING_H__
#define __NS_SVGSTRING_H__

#include "nsAutoPtr.h"
#include "nsError.h"
#include "mozilla/dom/SVGAnimatedString.h"
#include "mozilla/Attributes.h"
#include "mozilla/UniquePtr.h"

namespace mozilla {
namespace dom {
class SVGElement;
}

class SVGString {
 public:
  typedef mozilla::dom::SVGElement SVGElement;

  void Init(uint8_t aAttrEnum) {
    mAnimVal = nullptr;
    mAttrEnum = aAttrEnum;
    mIsBaseSet = false;
  }

  void SetBaseValue(const nsAString& aValue, SVGElement* aSVGElement,
                    bool aDoSetAttr);
  void GetBaseValue(nsAString& aValue, const SVGElement* aSVGElement) const {
    aSVGElement->GetStringBaseValue(mAttrEnum, aValue);
  }

  void SetAnimValue(const nsAString& aValue, SVGElement* aSVGElement);
  void GetAnimValue(nsAString& aValue, const SVGElement* aSVGElement) const;

  // Returns true if the animated value of this string has been explicitly
  // set (either by animation, or by taking on the base value which has been
  // explicitly set by markup or a DOM call), false otherwise.
  // If this returns false, the animated value is still valid, that is,
  // usable, and represents the default base value of the attribute.
  bool IsExplicitlySet() const { return !!mAnimVal || mIsBaseSet; }

  already_AddRefed<mozilla::dom::SVGAnimatedString> ToDOMAnimatedString(
      SVGElement* aSVGElement);

  mozilla::UniquePtr<nsISMILAttr> ToSMILAttr(SVGElement* aSVGElement);

 private:
  nsAutoPtr<nsString> mAnimVal;
  uint8_t mAttrEnum;  // element specified tracking for attribute
  bool mIsBaseSet;

 public:
  struct DOMAnimatedString final : public mozilla::dom::SVGAnimatedString {
    NS_DECL_CYCLE_COLLECTING_ISUPPORTS
    NS_DECL_CYCLE_COLLECTION_SCRIPT_HOLDER_CLASS(DOMAnimatedString)

    DOMAnimatedString(SVGString* aVal, SVGElement* aSVGElement)
        : mozilla::dom::SVGAnimatedString(aSVGElement), mVal(aVal) {}

    SVGString* mVal;  // kept alive because it belongs to content

    void GetBaseVal(nsAString& aResult) override {
      mVal->GetBaseValue(aResult, mSVGElement);
    }

    void SetBaseVal(const nsAString& aValue) override {
      mVal->SetBaseValue(aValue, mSVGElement, true);
    }

    void GetAnimVal(nsAString& aResult) override {
      mSVGElement->FlushAnimations();
      mVal->GetAnimValue(aResult, mSVGElement);
    }

   private:
    virtual ~DOMAnimatedString();
  };
  struct SMILString : public nsISMILAttr {
   public:
    SMILString(SVGString* aVal, SVGElement* aSVGElement)
        : mVal(aVal), mSVGElement(aSVGElement) {}

    // These will stay alive because a nsISMILAttr only lives as long
    // as the Compositing step, and DOM elements don't get a chance to
    // die during that.
    SVGString* mVal;
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

#endif  //__NS_SVGSTRING_H__
