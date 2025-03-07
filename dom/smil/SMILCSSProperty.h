/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=8 sts=2 et sw=2 tw=80: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

/* representation of a SMIL-animatable CSS property on an element */

#ifndef NS_SMILCSSPROPERTY_H_
#define NS_SMILCSSPROPERTY_H_

#include "mozilla/Attributes.h"
#include "nsISMILAttr.h"
#include "nsAtom.h"
#include "nsCSSPropertyID.h"
#include "nsCSSValue.h"

namespace mozilla {
class ComputedStyle;
namespace dom {
class Element;
}  // namespace dom

/**
 * SMILCSSProperty: Implements the nsISMILAttr interface for SMIL animations
 * that target CSS properties.  Represents a particular animation-targeted CSS
 * property on a particular element.
 */
class SMILCSSProperty : public nsISMILAttr {
 public:
  /**
   * Constructs a new SMILCSSProperty.
   * @param  aPropID   The CSS property we're interested in animating.
   * @param  aElement  The element whose CSS property is being animated.
   * @param  aBaseComputedStyle  The ComputedStyle to use when getting the base
   *                             value. If this is nullptr and GetBaseValue is
   *                             called, an empty nsSMILValue initialized with
   *                             the SMILCSSValueType will be returned.
   */
  SMILCSSProperty(nsCSSPropertyID aPropID, dom::Element* aElement,
                  ComputedStyle* aBaseComputedStyle);

  // nsISMILAttr methods
  virtual nsresult ValueFromString(
      const nsAString& aStr, const dom::SVGAnimationElement* aSrcElement,
      nsSMILValue& aValue, bool& aPreventCachingOfSandwich) const override;
  virtual nsSMILValue GetBaseValue() const override;
  virtual nsresult SetAnimValue(const nsSMILValue& aValue) override;
  virtual void ClearAnimValue() override;

  /**
   * Utility method - returns true if the given property is supported for
   * SMIL animation.
   *
   * @param   aProperty  The property to check for animation support.
   * @param   aBackend   The style backend to check for animation support.
   *                     This is a temporary measure until the Servo backend
   *                     supports all animatable properties (bug 1353918).
   * @return  true if the given property is supported for SMIL animation, or
   *          false otherwise
   */
  static bool IsPropertyAnimatable(nsCSSPropertyID aPropID);

 protected:
  nsCSSPropertyID mPropID;
  // Using non-refcounted pointer for mElement -- we know mElement will stay
  // alive for my lifetime because a nsISMILAttr (like me) only lives as long
  // as the Compositing step, and DOM elements don't get a chance to die during
  // that time.
  dom::Element* mElement;

  // The style to use when fetching base styles.
  //
  // As with mElement, since an nsISMILAttr only lives as long as the
  // compositing step and since ComposeAttribute holds an owning reference to
  // the base ComputedStyle, we can use a non-owning reference here.
  ComputedStyle* mBaseComputedStyle;
};

}  // namespace mozilla

#endif  // NS_SMILCSSPROPERTY_H_
