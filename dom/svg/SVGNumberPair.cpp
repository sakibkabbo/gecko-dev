/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=8 sts=2 et sw=2 tw=80: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "SVGNumberPair.h"
#include "nsSVGAttrTearoffTable.h"
#include "nsCharSeparatedTokenizer.h"
#include "nsSMILValue.h"
#include "SVGContentUtils.h"
#include "SVGNumberPairSMILType.h"

using namespace mozilla::dom;

namespace mozilla {

static nsSVGAttrTearoffTable<SVGNumberPair, SVGNumberPair::DOMAnimatedNumber>
    sSVGFirstAnimatedNumberTearoffTable;
static nsSVGAttrTearoffTable<SVGNumberPair, SVGNumberPair::DOMAnimatedNumber>
    sSVGSecondAnimatedNumberTearoffTable;

static nsresult ParseNumberOptionalNumber(const nsAString& aValue,
                                          float aValues[2]) {
  nsCharSeparatedTokenizerTemplate<nsContentUtils::IsHTMLWhitespace> tokenizer(
      aValue, ',', nsCharSeparatedTokenizer::SEPARATOR_OPTIONAL);
  if (tokenizer.whitespaceBeforeFirstToken()) {
    return NS_ERROR_DOM_SYNTAX_ERR;
  }

  uint32_t i;
  for (i = 0; i < 2 && tokenizer.hasMoreTokens(); ++i) {
    if (!SVGContentUtils::ParseNumber(tokenizer.nextToken(), aValues[i])) {
      return NS_ERROR_DOM_SYNTAX_ERR;
    }
  }
  if (i == 1) {
    aValues[1] = aValues[0];
  }

  if (i == 0 ||                                   // Too few values.
      tokenizer.hasMoreTokens() ||                // Too many values.
      tokenizer.whitespaceAfterCurrentToken() ||  // Trailing whitespace.
      tokenizer.separatorAfterCurrentToken()) {   // Trailing comma.
    return NS_ERROR_DOM_SYNTAX_ERR;
  }

  return NS_OK;
}

nsresult SVGNumberPair::SetBaseValueString(const nsAString& aValueAsString,
                                           SVGElement* aSVGElement) {
  float val[2];

  nsresult rv = ParseNumberOptionalNumber(aValueAsString, val);
  if (NS_FAILED(rv)) {
    return rv;
  }

  mBaseVal[0] = val[0];
  mBaseVal[1] = val[1];
  mIsBaseSet = true;
  if (!mIsAnimated) {
    mAnimVal[0] = mBaseVal[0];
    mAnimVal[1] = mBaseVal[1];
  } else {
    aSVGElement->AnimationNeedsResample();
  }

  // We don't need to call Will/DidChange* here - we're only called by
  // SVGElement::ParseAttribute under Element::SetAttr,
  // which takes care of notifying.
  return NS_OK;
}

void SVGNumberPair::GetBaseValueString(nsAString& aValueAsString) const {
  aValueAsString.Truncate();
  aValueAsString.AppendFloat(mBaseVal[0]);
  if (mBaseVal[0] != mBaseVal[1]) {
    aValueAsString.AppendLiteral(", ");
    aValueAsString.AppendFloat(mBaseVal[1]);
  }
}

void SVGNumberPair::SetBaseValue(float aValue, PairIndex aPairIndex,
                                 SVGElement* aSVGElement) {
  uint32_t index = (aPairIndex == eFirst ? 0 : 1);
  if (mIsBaseSet && mBaseVal[index] == aValue) {
    return;
  }
  nsAttrValue emptyOrOldValue = aSVGElement->WillChangeNumberPair(mAttrEnum);
  mBaseVal[index] = aValue;
  mIsBaseSet = true;
  if (!mIsAnimated) {
    mAnimVal[index] = aValue;
  } else {
    aSVGElement->AnimationNeedsResample();
  }
  aSVGElement->DidChangeNumberPair(mAttrEnum, emptyOrOldValue);
}

void SVGNumberPair::SetBaseValues(float aValue1, float aValue2,
                                  SVGElement* aSVGElement) {
  if (mIsBaseSet && mBaseVal[0] == aValue1 && mBaseVal[1] == aValue2) {
    return;
  }
  nsAttrValue emptyOrOldValue = aSVGElement->WillChangeNumberPair(mAttrEnum);
  mBaseVal[0] = aValue1;
  mBaseVal[1] = aValue2;
  mIsBaseSet = true;
  if (!mIsAnimated) {
    mAnimVal[0] = aValue1;
    mAnimVal[1] = aValue2;
  } else {
    aSVGElement->AnimationNeedsResample();
  }
  aSVGElement->DidChangeNumberPair(mAttrEnum, emptyOrOldValue);
}

void SVGNumberPair::SetAnimValue(const float aValue[2],
                                 SVGElement* aSVGElement) {
  if (mIsAnimated && mAnimVal[0] == aValue[0] && mAnimVal[1] == aValue[1]) {
    return;
  }
  mAnimVal[0] = aValue[0];
  mAnimVal[1] = aValue[1];
  mIsAnimated = true;
  aSVGElement->DidAnimateNumberPair(mAttrEnum);
}

already_AddRefed<SVGAnimatedNumber> SVGNumberPair::ToDOMAnimatedNumber(
    PairIndex aIndex, SVGElement* aSVGElement) {
  RefPtr<DOMAnimatedNumber> domAnimatedNumber =
      aIndex == eFirst ? sSVGFirstAnimatedNumberTearoffTable.GetTearoff(this)
                       : sSVGSecondAnimatedNumberTearoffTable.GetTearoff(this);
  if (!domAnimatedNumber) {
    domAnimatedNumber = new DOMAnimatedNumber(this, aIndex, aSVGElement);
    if (aIndex == eFirst) {
      sSVGFirstAnimatedNumberTearoffTable.AddTearoff(this, domAnimatedNumber);
    } else {
      sSVGSecondAnimatedNumberTearoffTable.AddTearoff(this, domAnimatedNumber);
    }
  }

  return domAnimatedNumber.forget();
}

SVGNumberPair::DOMAnimatedNumber::~DOMAnimatedNumber() {
  if (mIndex == eFirst) {
    sSVGFirstAnimatedNumberTearoffTable.RemoveTearoff(mVal);
  } else {
    sSVGSecondAnimatedNumberTearoffTable.RemoveTearoff(mVal);
  }
}

UniquePtr<nsISMILAttr> SVGNumberPair::ToSMILAttr(SVGElement* aSVGElement) {
  return MakeUnique<SMILNumberPair>(this, aSVGElement);
}

nsresult SVGNumberPair::SMILNumberPair::ValueFromString(
    const nsAString& aStr, const dom::SVGAnimationElement* /*aSrcElement*/,
    nsSMILValue& aValue, bool& aPreventCachingOfSandwich) const {
  float values[2];

  nsresult rv = ParseNumberOptionalNumber(aStr, values);
  if (NS_FAILED(rv)) {
    return rv;
  }

  nsSMILValue val(&SVGNumberPairSMILType::sSingleton);
  val.mU.mNumberPair[0] = values[0];
  val.mU.mNumberPair[1] = values[1];
  aValue = val;
  aPreventCachingOfSandwich = false;

  return NS_OK;
}

nsSMILValue SVGNumberPair::SMILNumberPair::GetBaseValue() const {
  nsSMILValue val(&SVGNumberPairSMILType::sSingleton);
  val.mU.mNumberPair[0] = mVal->mBaseVal[0];
  val.mU.mNumberPair[1] = mVal->mBaseVal[1];
  return val;
}

void SVGNumberPair::SMILNumberPair::ClearAnimValue() {
  if (mVal->mIsAnimated) {
    mVal->mIsAnimated = false;
    mVal->mAnimVal[0] = mVal->mBaseVal[0];
    mVal->mAnimVal[1] = mVal->mBaseVal[1];
    mSVGElement->DidAnimateNumberPair(mVal->mAttrEnum);
  }
}

nsresult SVGNumberPair::SMILNumberPair::SetAnimValue(
    const nsSMILValue& aValue) {
  NS_ASSERTION(aValue.mType == &SVGNumberPairSMILType::sSingleton,
               "Unexpected type to assign animated value");
  if (aValue.mType == &SVGNumberPairSMILType::sSingleton) {
    mVal->SetAnimValue(aValue.mU.mNumberPair, mSVGElement);
  }
  return NS_OK;
}

}  // namespace mozilla
