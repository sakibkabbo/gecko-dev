/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=8 sts=2 et sw=2 tw=80: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef mozilla_dom_SVGFEFloodElement_h
#define mozilla_dom_SVGFEFloodElement_h

#include "SVGFilters.h"

nsresult NS_NewSVGFEFloodElement(
    nsIContent** aResult, already_AddRefed<mozilla::dom::NodeInfo>&& aNodeInfo);

namespace mozilla {
namespace dom {

typedef SVGFE SVGFEFloodElementBase;

class SVGFEFloodElement : public SVGFEFloodElementBase {
  friend nsresult(::NS_NewSVGFEFloodElement(
      nsIContent** aResult,
      already_AddRefed<mozilla::dom::NodeInfo>&& aNodeInfo));

 protected:
  explicit SVGFEFloodElement(
      already_AddRefed<mozilla::dom::NodeInfo>&& aNodeInfo)
      : SVGFEFloodElementBase(std::move(aNodeInfo)) {}
  virtual JSObject* WrapNode(JSContext* cx,
                             JS::Handle<JSObject*> aGivenProto) override;

 public:
  virtual bool SubregionIsUnionOfRegions() override { return false; }

  virtual FilterPrimitiveDescription GetPrimitiveDescription(
      nsSVGFilterInstance* aInstance, const IntRect& aFilterSubregion,
      const nsTArray<bool>& aInputsAreTainted,
      nsTArray<RefPtr<SourceSurface>>& aInputImages) override;
  virtual SVGString& GetResultImageName() override {
    return mStringAttributes[RESULT];
  }

  // nsIContent interface
  NS_IMETHOD_(bool) IsAttributeMapped(const nsAtom* aAttribute) const override;

  virtual nsresult Clone(dom::NodeInfo*, nsINode** aResult) const override;

 protected:
  virtual bool ProducesSRGB() override { return true; }

  virtual StringAttributesInfo GetStringInfo() override;

  enum { RESULT };
  SVGString mStringAttributes[1];
  static StringInfo sStringInfo[1];
};

}  // namespace dom
}  // namespace mozilla

#endif  // mozilla_dom_SVGFEFloodElement_h
