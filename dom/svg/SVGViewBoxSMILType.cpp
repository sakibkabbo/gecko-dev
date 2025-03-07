/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=8 sts=2 et sw=2 tw=80: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "SVGViewBoxSMILType.h"
#include "nsSMILValue.h"
#include "SVGViewBox.h"
#include "nsDebug.h"
#include <math.h>

namespace mozilla {

/*static*/ SVGViewBoxSMILType SVGViewBoxSMILType::sSingleton;

void SVGViewBoxSMILType::Init(nsSMILValue& aValue) const {
  MOZ_ASSERT(aValue.IsNull(), "Unexpected value type");

  aValue.mU.mPtr = new SVGViewBoxRect();
  aValue.mType = this;
}

void SVGViewBoxSMILType::Destroy(nsSMILValue& aValue) const {
  MOZ_ASSERT(aValue.mType == this, "Unexpected SMIL value");
  delete static_cast<SVGViewBoxRect*>(aValue.mU.mPtr);
  aValue.mU.mPtr = nullptr;
  aValue.mType = SMILNullType::Singleton();
}

nsresult SVGViewBoxSMILType::Assign(nsSMILValue& aDest,
                                    const nsSMILValue& aSrc) const {
  MOZ_ASSERT(aDest.mType == aSrc.mType, "Incompatible SMIL types");
  MOZ_ASSERT(aDest.mType == this, "Unexpected SMIL value");

  const SVGViewBoxRect* src = static_cast<const SVGViewBoxRect*>(aSrc.mU.mPtr);
  SVGViewBoxRect* dst = static_cast<SVGViewBoxRect*>(aDest.mU.mPtr);
  *dst = *src;
  return NS_OK;
}

bool SVGViewBoxSMILType::IsEqual(const nsSMILValue& aLeft,
                                 const nsSMILValue& aRight) const {
  MOZ_ASSERT(aLeft.mType == aRight.mType, "Incompatible SMIL types");
  MOZ_ASSERT(aLeft.mType == this, "Unexpected type for SMIL value");

  const SVGViewBoxRect* leftBox =
      static_cast<const SVGViewBoxRect*>(aLeft.mU.mPtr);
  const SVGViewBoxRect* rightBox = static_cast<SVGViewBoxRect*>(aRight.mU.mPtr);
  return *leftBox == *rightBox;
}

nsresult SVGViewBoxSMILType::Add(nsSMILValue& aDest,
                                 const nsSMILValue& aValueToAdd,
                                 uint32_t aCount) const {
  MOZ_ASSERT(aValueToAdd.mType == aDest.mType, "Trying to add invalid types");
  MOZ_ASSERT(aValueToAdd.mType == this, "Unexpected source type");

  // See https://bugzilla.mozilla.org/show_bug.cgi?id=541884#c3 and the two
  // comments that follow that one for arguments for and against allowing
  // viewBox to be additive.

  return NS_ERROR_FAILURE;
}

nsresult SVGViewBoxSMILType::ComputeDistance(const nsSMILValue& aFrom,
                                             const nsSMILValue& aTo,
                                             double& aDistance) const {
  MOZ_ASSERT(aFrom.mType == aTo.mType, "Trying to compare different types");
  MOZ_ASSERT(aFrom.mType == this, "Unexpected source type");

  const SVGViewBoxRect* from =
      static_cast<const SVGViewBoxRect*>(aFrom.mU.mPtr);
  const SVGViewBoxRect* to = static_cast<const SVGViewBoxRect*>(aTo.mU.mPtr);

  if (from->none || to->none) {
    return NS_ERROR_FAILURE;
  }

  // We use the distances between the edges rather than the difference between
  // the x, y, width and height for the "distance". This is necessary in
  // order for the "distance" result that we calculate to be the same for a
  // given change in the left side as it is for an equal change in the opposite
  // side. See https://bugzilla.mozilla.org/show_bug.cgi?id=541884#c12

  float dLeft = to->x - from->x;
  float dTop = to->y - from->y;
  float dRight = (to->x + to->width) - (from->x + from->width);
  float dBottom = (to->y + to->height) - (from->y + from->height);

  aDistance =
      sqrt(dLeft * dLeft + dTop * dTop + dRight * dRight + dBottom * dBottom);

  return NS_OK;
}

nsresult SVGViewBoxSMILType::Interpolate(const nsSMILValue& aStartVal,
                                         const nsSMILValue& aEndVal,
                                         double aUnitDistance,
                                         nsSMILValue& aResult) const {
  MOZ_ASSERT(aStartVal.mType == aEndVal.mType,
             "Trying to interpolate different types");
  MOZ_ASSERT(aStartVal.mType == this, "Unexpected types for interpolation");
  MOZ_ASSERT(aResult.mType == this, "Unexpected result type");

  const SVGViewBoxRect* start =
      static_cast<const SVGViewBoxRect*>(aStartVal.mU.mPtr);
  const SVGViewBoxRect* end =
      static_cast<const SVGViewBoxRect*>(aEndVal.mU.mPtr);

  if (start->none || end->none) {
    return NS_ERROR_FAILURE;
  }

  SVGViewBoxRect* current = static_cast<SVGViewBoxRect*>(aResult.mU.mPtr);

  float x = (start->x + (end->x - start->x) * aUnitDistance);
  float y = (start->y + (end->y - start->y) * aUnitDistance);
  float width = (start->width + (end->width - start->width) * aUnitDistance);
  float height =
      (start->height + (end->height - start->height) * aUnitDistance);

  *current = SVGViewBoxRect(x, y, width, height);

  return NS_OK;
}

}  // namespace mozilla
