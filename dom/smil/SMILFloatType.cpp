/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=8 sts=2 et sw=2 tw=80: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "SMILFloatType.h"
#include "nsSMILValue.h"
#include "nsDebug.h"
#include <math.h>

namespace mozilla {

void SMILFloatType::Init(nsSMILValue& aValue) const {
  MOZ_ASSERT(aValue.IsNull(), "Unexpected value type");
  aValue.mU.mDouble = 0.0;
  aValue.mType = this;
}

void SMILFloatType::Destroy(nsSMILValue& aValue) const {
  MOZ_ASSERT(aValue.mType == this, "Unexpected SMIL value");
  aValue.mU.mDouble = 0.0;
  aValue.mType = SMILNullType::Singleton();
}

nsresult SMILFloatType::Assign(nsSMILValue& aDest,
                               const nsSMILValue& aSrc) const {
  MOZ_ASSERT(aDest.mType == aSrc.mType, "Incompatible SMIL types");
  MOZ_ASSERT(aDest.mType == this, "Unexpected SMIL value");
  aDest.mU.mDouble = aSrc.mU.mDouble;
  return NS_OK;
}

bool SMILFloatType::IsEqual(const nsSMILValue& aLeft,
                            const nsSMILValue& aRight) const {
  MOZ_ASSERT(aLeft.mType == aRight.mType, "Incompatible SMIL types");
  MOZ_ASSERT(aLeft.mType == this, "Unexpected type for SMIL value");

  return aLeft.mU.mDouble == aRight.mU.mDouble;
}

nsresult SMILFloatType::Add(nsSMILValue& aDest, const nsSMILValue& aValueToAdd,
                            uint32_t aCount) const {
  MOZ_ASSERT(aValueToAdd.mType == aDest.mType, "Trying to add invalid types");
  MOZ_ASSERT(aValueToAdd.mType == this, "Unexpected source type");
  aDest.mU.mDouble += aValueToAdd.mU.mDouble * aCount;
  return NS_OK;
}

nsresult SMILFloatType::ComputeDistance(const nsSMILValue& aFrom,
                                        const nsSMILValue& aTo,
                                        double& aDistance) const {
  MOZ_ASSERT(aFrom.mType == aTo.mType, "Trying to compare different types");
  MOZ_ASSERT(aFrom.mType == this, "Unexpected source type");

  const double& from = aFrom.mU.mDouble;
  const double& to = aTo.mU.mDouble;

  aDistance = fabs(to - from);

  return NS_OK;
}

nsresult SMILFloatType::Interpolate(const nsSMILValue& aStartVal,
                                    const nsSMILValue& aEndVal,
                                    double aUnitDistance,
                                    nsSMILValue& aResult) const {
  MOZ_ASSERT(aStartVal.mType == aEndVal.mType,
             "Trying to interpolate different types");
  MOZ_ASSERT(aStartVal.mType == this, "Unexpected types for interpolation");
  MOZ_ASSERT(aResult.mType == this, "Unexpected result type");

  const double& startVal = aStartVal.mU.mDouble;
  const double& endVal = aEndVal.mU.mDouble;

  aResult.mU.mDouble = (startVal + (endVal - startVal) * aUnitDistance);

  return NS_OK;
}

}  // namespace mozilla
