/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=8 sts=2 et sw=2 tw=80: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef mozilla_dom_SVGFilterElement_h
#define mozilla_dom_SVGFilterElement_h

#include "SVGEnum.h"
#include "nsSVGLength2.h"
#include "SVGString.h"
#include "mozilla/dom/SVGElement.h"

class nsSVGFilterFrame;
class nsSVGFilterInstance;

nsresult NS_NewSVGFilterElement(
    nsIContent** aResult, already_AddRefed<mozilla::dom::NodeInfo>&& aNodeInfo);

namespace mozilla {
namespace dom {
class SVGAnimatedLength;

typedef SVGElement SVGFilterElementBase;

class SVGFilterElement : public SVGFilterElementBase {
  friend class ::nsSVGFilterFrame;
  friend class ::nsSVGFilterInstance;

 protected:
  friend nsresult(::NS_NewSVGFilterElement(
      nsIContent** aResult,
      already_AddRefed<mozilla::dom::NodeInfo>&& aNodeInfo));
  explicit SVGFilterElement(
      already_AddRefed<mozilla::dom::NodeInfo>&& aNodeInfo);
  virtual JSObject* WrapNode(JSContext* cx,
                             JS::Handle<JSObject*> aGivenProto) override;

 public:
  // nsIContent
  virtual nsresult Clone(dom::NodeInfo*, nsINode** aResult) const override;
  NS_IMETHOD_(bool) IsAttributeMapped(const nsAtom* aAttribute) const override;

  // nsSVGSVGElement methods:
  virtual bool HasValidDimensions() const override;

  // WebIDL
  already_AddRefed<SVGAnimatedLength> X();
  already_AddRefed<SVGAnimatedLength> Y();
  already_AddRefed<SVGAnimatedLength> Width();
  already_AddRefed<SVGAnimatedLength> Height();
  already_AddRefed<SVGAnimatedEnumeration> FilterUnits();
  already_AddRefed<SVGAnimatedEnumeration> PrimitiveUnits();
  already_AddRefed<SVGAnimatedString> Href();

 protected:
  virtual LengthAttributesInfo GetLengthInfo() override;
  virtual EnumAttributesInfo GetEnumInfo() override;
  virtual StringAttributesInfo GetStringInfo() override;

  enum { ATTR_X, ATTR_Y, ATTR_WIDTH, ATTR_HEIGHT };
  nsSVGLength2 mLengthAttributes[4];
  static LengthInfo sLengthInfo[4];

  enum { FILTERUNITS, PRIMITIVEUNITS };
  SVGEnum mEnumAttributes[2];
  static EnumInfo sEnumInfo[2];

  enum { HREF, XLINK_HREF };
  SVGString mStringAttributes[2];
  static StringInfo sStringInfo[2];
};

}  // namespace dom
}  // namespace mozilla

#endif  // mozilla_dom_SVGFilterElement_h
