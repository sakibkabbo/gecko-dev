/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=8 sts=2 et sw=2 tw=80: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef mozilla_dom_SVGMarkerElement_h
#define mozilla_dom_SVGMarkerElement_h

#include "nsAutoPtr.h"
#include "SVGAngle.h"
#include "SVGAnimatedPreserveAspectRatio.h"
#include "SVGEnum.h"
#include "nsSVGLength2.h"
#include "SVGViewBox.h"
#include "mozilla/Attributes.h"
#include "mozilla/dom/SVGAnimatedEnumeration.h"
#include "mozilla/dom/SVGElement.h"
#include "mozilla/dom/SVGMarkerElementBinding.h"

class nsSVGMarkerFrame;
struct nsSVGMark;

nsresult NS_NewSVGMarkerElement(
    nsIContent** aResult, already_AddRefed<mozilla::dom::NodeInfo>&& aNodeInfo);

namespace mozilla {
namespace dom {

// Non-Exposed Marker Orientation Types
static const uint16_t SVG_MARKER_ORIENT_AUTO_START_REVERSE = 3;

class nsSVGOrientType {
 public:
  nsSVGOrientType()
      : mAnimVal(SVGMarkerElement_Binding::SVG_MARKER_ORIENT_ANGLE),
        mBaseVal(SVGMarkerElement_Binding::SVG_MARKER_ORIENT_ANGLE) {}

  nsresult SetBaseValue(uint16_t aValue, SVGElement* aSVGElement);

  // XXX FIXME like https://bugzilla.mozilla.org/show_bug.cgi?id=545550 but
  // without adding an mIsAnimated member...?
  void SetBaseValue(uint16_t aValue) { mAnimVal = mBaseVal = uint8_t(aValue); }
  // no need to notify, since SVGAngle does that
  void SetAnimValue(uint16_t aValue) { mAnimVal = uint8_t(aValue); }

  // we want to avoid exposing SVG_MARKER_ORIENT_AUTO_START_REVERSE to
  // Web content
  uint16_t GetBaseValue() const {
    return mAnimVal == SVG_MARKER_ORIENT_AUTO_START_REVERSE
               ? SVGMarkerElement_Binding::SVG_MARKER_ORIENT_UNKNOWN
               : mBaseVal;
  }
  uint16_t GetAnimValue() const {
    return mAnimVal == SVG_MARKER_ORIENT_AUTO_START_REVERSE
               ? SVGMarkerElement_Binding::SVG_MARKER_ORIENT_UNKNOWN
               : mAnimVal;
  }
  uint16_t GetAnimValueInternal() const { return mAnimVal; }

  already_AddRefed<SVGAnimatedEnumeration> ToDOMAnimatedEnum(
      SVGElement* aSVGElement);

 private:
  SVGEnumValue mAnimVal;
  SVGEnumValue mBaseVal;

  struct DOMAnimatedEnum final : public SVGAnimatedEnumeration {
    DOMAnimatedEnum(nsSVGOrientType* aVal, SVGElement* aSVGElement)
        : SVGAnimatedEnumeration(aSVGElement), mVal(aVal) {}

    nsSVGOrientType* mVal;  // kept alive because it belongs to content

    using mozilla::dom::SVGAnimatedEnumeration::SetBaseVal;
    virtual uint16_t BaseVal() override { return mVal->GetBaseValue(); }
    virtual void SetBaseVal(uint16_t aBaseVal, ErrorResult& aRv) override {
      aRv = mVal->SetBaseValue(aBaseVal, mSVGElement);
    }
    virtual uint16_t AnimVal() override { return mVal->GetAnimValue(); }
  };
};

typedef SVGElement SVGMarkerElementBase;

class SVGMarkerElement : public SVGMarkerElementBase {
  friend class ::nsSVGMarkerFrame;

 protected:
  friend nsresult(::NS_NewSVGMarkerElement(
      nsIContent** aResult,
      already_AddRefed<mozilla::dom::NodeInfo>&& aNodeInfo));
  explicit SVGMarkerElement(
      already_AddRefed<mozilla::dom::NodeInfo>&& aNodeInfo);
  virtual JSObject* WrapNode(JSContext* cx,
                             JS::Handle<JSObject*> aGivenProto) override;

 public:
  // nsIContent interface
  NS_IMETHOD_(bool) IsAttributeMapped(const nsAtom* name) const override;

  virtual nsresult AfterSetAttr(int32_t aNamespaceID, nsAtom* aName,
                                const nsAttrValue* aValue,
                                const nsAttrValue* aOldValue,
                                nsIPrincipal* aMaybeScriptedPrincipal,
                                bool aNotify) override;

  // nsSVGSVGElement methods:
  virtual bool HasValidDimensions() const override;

  // public helpers
  gfx::Matrix GetMarkerTransform(float aStrokeWidth, const nsSVGMark& aMark);
  SVGViewBoxRect GetViewBoxRect();
  gfx::Matrix GetViewBoxTransform();

  virtual nsresult Clone(dom::NodeInfo*, nsINode** aResult) const override;

  nsSVGOrientType* GetOrientType() { return &mOrientType; }

  // WebIDL
  already_AddRefed<SVGAnimatedRect> ViewBox();
  already_AddRefed<DOMSVGAnimatedPreserveAspectRatio> PreserveAspectRatio();
  already_AddRefed<SVGAnimatedLength> RefX();
  already_AddRefed<SVGAnimatedLength> RefY();
  already_AddRefed<SVGAnimatedEnumeration> MarkerUnits();
  already_AddRefed<SVGAnimatedLength> MarkerWidth();
  already_AddRefed<SVGAnimatedLength> MarkerHeight();
  already_AddRefed<SVGAnimatedEnumeration> OrientType();
  already_AddRefed<SVGAnimatedAngle> OrientAngle();
  void SetOrientToAuto();
  void SetOrientToAngle(DOMSVGAngle& angle, ErrorResult& rv);

 protected:
  virtual bool ParseAttribute(int32_t aNameSpaceID, nsAtom* aName,
                              const nsAString& aValue,
                              nsIPrincipal* aMaybeScriptedPrincipal,
                              nsAttrValue& aResult) override;

  void SetParentCoordCtxProvider(SVGViewportElement* aContext);

  virtual LengthAttributesInfo GetLengthInfo() override;
  virtual AngleAttributesInfo GetAngleInfo() override;
  virtual EnumAttributesInfo GetEnumInfo() override;
  virtual SVGViewBox* GetViewBox() override;
  virtual SVGAnimatedPreserveAspectRatio* GetPreserveAspectRatio() override;

  enum { REFX, REFY, MARKERWIDTH, MARKERHEIGHT };
  nsSVGLength2 mLengthAttributes[4];
  static LengthInfo sLengthInfo[4];

  enum { MARKERUNITS };
  SVGEnum mEnumAttributes[1];
  static SVGEnumMapping sUnitsMap[];
  static EnumInfo sEnumInfo[1];

  enum { ORIENT };
  SVGAngle mAngleAttributes[1];
  static AngleInfo sAngleInfo[1];

  SVGViewBox mViewBox;
  SVGAnimatedPreserveAspectRatio mPreserveAspectRatio;

  // derived properties (from 'orient') handled separately
  nsSVGOrientType mOrientType;

  SVGViewportElement* mCoordCtx;
  nsAutoPtr<gfx::Matrix> mViewBoxToViewportTransform;
};

}  // namespace dom
}  // namespace mozilla

#endif  // mozilla_dom_SVGMarkerElement_h
