/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=8 sts=2 et sw=2 tw=80: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef __NS_SVGVIEWBOX_H__
#define __NS_SVGVIEWBOX_H__

#include "nsAutoPtr.h"
#include "nsCycleCollectionParticipant.h"
#include "nsError.h"
#include "mozilla/Attributes.h"
#include "nsISMILAttr.h"
#include "nsSVGAttrTearoffTable.h"
#include "mozilla/UniquePtr.h"
#include "mozilla/dom/SVGAnimatedRect.h"
#include "mozilla/dom/SVGIRect.h"

class nsSMILValue;

namespace mozilla {
namespace dom {
class SVGAnimationElement;
class SVGElement;
}  // namespace dom

struct SVGViewBoxRect {
  float x, y;
  float width, height;
  bool none;

  SVGViewBoxRect() : x(0.0), y(0.0), width(0.0), height(0.0), none(true) {}
  SVGViewBoxRect(float aX, float aY, float aWidth, float aHeight)
      : x(aX), y(aY), width(aWidth), height(aHeight), none(false) {}
  SVGViewBoxRect(const SVGViewBoxRect& rhs)
      : x(rhs.x),
        y(rhs.y),
        width(rhs.width),
        height(rhs.height),
        none(rhs.none) {}
  bool operator==(const SVGViewBoxRect& aOther) const;

  static nsresult FromString(const nsAString& aStr, SVGViewBoxRect* aViewBox);
};

class SVGViewBox {
 public:
  typedef mozilla::dom::SVGElement SVGElement;

  void Init();

  /**
   * Returns true if the corresponding "viewBox" attribute defined a rectangle
   * with finite values and nonnegative width/height.
   * Returns false if the viewBox was set to an invalid
   * string, or if any of the four rect values were too big to store in a
   * float, or the width/height are negative.
   */
  bool HasRect() const;

  /**
   * Returns true if the corresponding "viewBox" attribute either defined a
   * rectangle with finite values or the special "none" value.
   */
  bool IsExplicitlySet() const {
    if (mAnimVal || mHasBaseVal) {
      const SVGViewBoxRect& rect = GetAnimValue();
      return rect.none || (rect.width >= 0 && rect.height >= 0);
    }
    return false;
  }

  const SVGViewBoxRect& GetBaseValue() const { return mBaseVal; }
  void SetBaseValue(const SVGViewBoxRect& aRect, SVGElement* aSVGElement);
  const SVGViewBoxRect& GetAnimValue() const {
    return mAnimVal ? *mAnimVal : mBaseVal;
  }
  void SetAnimValue(const SVGViewBoxRect& aRect, SVGElement* aSVGElement);

  nsresult SetBaseValueString(const nsAString& aValue, SVGElement* aSVGElement,
                              bool aDoSetAttr);
  void GetBaseValueString(nsAString& aValue) const;

  already_AddRefed<mozilla::dom::SVGAnimatedRect> ToSVGAnimatedRect(
      SVGElement* aSVGElement);

  already_AddRefed<mozilla::dom::SVGIRect> ToDOMBaseVal(
      SVGElement* aSVGElement);

  already_AddRefed<mozilla::dom::SVGIRect> ToDOMAnimVal(
      SVGElement* aSVGElement);

  mozilla::UniquePtr<nsISMILAttr> ToSMILAttr(SVGElement* aSVGElement);

 private:
  SVGViewBoxRect mBaseVal;
  nsAutoPtr<SVGViewBoxRect> mAnimVal;
  bool mHasBaseVal;

 public:
  struct DOMBaseVal final : public mozilla::dom::SVGIRect {
    NS_DECL_CYCLE_COLLECTING_ISUPPORTS
    NS_DECL_CYCLE_COLLECTION_SCRIPT_HOLDER_CLASS(DOMBaseVal)

    DOMBaseVal(SVGViewBox* aVal, SVGElement* aSVGElement)
        : mozilla::dom::SVGIRect(), mVal(aVal), mSVGElement(aSVGElement) {}

    SVGViewBox* mVal;  // kept alive because it belongs to content
    RefPtr<SVGElement> mSVGElement;

    float X() const final { return mVal->GetBaseValue().x; }

    float Y() const final { return mVal->GetBaseValue().y; }

    float Width() const final { return mVal->GetBaseValue().width; }

    float Height() const final { return mVal->GetBaseValue().height; }

    void SetX(float aX, mozilla::ErrorResult& aRv) final;
    void SetY(float aY, mozilla::ErrorResult& aRv) final;
    void SetWidth(float aWidth, mozilla::ErrorResult& aRv) final;
    void SetHeight(float aHeight, mozilla::ErrorResult& aRv) final;

    virtual nsIContent* GetParentObject() const override { return mSVGElement; }

   private:
    virtual ~DOMBaseVal();
  };

  struct DOMAnimVal final : public mozilla::dom::SVGIRect {
    NS_DECL_CYCLE_COLLECTING_ISUPPORTS
    NS_DECL_CYCLE_COLLECTION_SCRIPT_HOLDER_CLASS(DOMAnimVal)

    DOMAnimVal(SVGViewBox* aVal, SVGElement* aSVGElement)
        : mozilla::dom::SVGIRect(), mVal(aVal), mSVGElement(aSVGElement) {}

    SVGViewBox* mVal;  // kept alive because it belongs to content
    RefPtr<SVGElement> mSVGElement;

    // Script may have modified animation parameters or timeline -- DOM getters
    // need to flush any resample requests to reflect these modifications.
    float X() const final {
      mSVGElement->FlushAnimations();
      return mVal->GetAnimValue().x;
    }

    float Y() const final {
      mSVGElement->FlushAnimations();
      return mVal->GetAnimValue().y;
    }

    float Width() const final {
      mSVGElement->FlushAnimations();
      return mVal->GetAnimValue().width;
    }

    float Height() const final {
      mSVGElement->FlushAnimations();
      return mVal->GetAnimValue().height;
    }

    void SetX(float aX, mozilla::ErrorResult& aRv) final {
      aRv.Throw(NS_ERROR_DOM_NO_MODIFICATION_ALLOWED_ERR);
    }

    void SetY(float aY, mozilla::ErrorResult& aRv) final {
      aRv.Throw(NS_ERROR_DOM_NO_MODIFICATION_ALLOWED_ERR);
    }

    void SetWidth(float aWidth, mozilla::ErrorResult& aRv) final {
      aRv.Throw(NS_ERROR_DOM_NO_MODIFICATION_ALLOWED_ERR);
    }

    void SetHeight(float aHeight, mozilla::ErrorResult& aRv) final {
      aRv.Throw(NS_ERROR_DOM_NO_MODIFICATION_ALLOWED_ERR);
    }

    virtual nsIContent* GetParentObject() const override { return mSVGElement; }

   private:
    virtual ~DOMAnimVal();
  };

  struct SMILViewBox : public nsISMILAttr {
   public:
    SMILViewBox(SVGViewBox* aVal, SVGElement* aSVGElement)
        : mVal(aVal), mSVGElement(aSVGElement) {}

    // These will stay alive because a nsISMILAttr only lives as long
    // as the Compositing step, and DOM elements don't get a chance to
    // die during that.
    SVGViewBox* mVal;
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

  static nsSVGAttrTearoffTable<SVGViewBox, mozilla::dom::SVGAnimatedRect>
      sSVGAnimatedRectTearoffTable;
};

}  // namespace mozilla

#endif  // __NS_SVGVIEWBOX_H__
