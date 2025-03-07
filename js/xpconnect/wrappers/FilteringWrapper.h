/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=8 sts=2 et sw=2 tw=80: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef __FilteringWrapper_h__
#define __FilteringWrapper_h__

#include "XrayWrapper.h"
#include "mozilla/Attributes.h"
#include "js/CallNonGenericMethod.h"
#include "js/Wrapper.h"

namespace xpc {

template <typename Base, typename Policy>
class FilteringWrapper : public Base {
 public:
  constexpr explicit FilteringWrapper(unsigned flags) : Base(flags) {}

  virtual bool enter(JSContext* cx, JS::Handle<JSObject*> wrapper,
                     JS::Handle<jsid> id, js::Wrapper::Action act,
                     bool mayThrow, bool* bp) const override;

  virtual bool getOwnPropertyDescriptor(
      JSContext* cx, JS::Handle<JSObject*> wrapper, JS::Handle<jsid> id,
      JS::MutableHandle<JS::PropertyDescriptor> desc) const override;
  virtual bool ownPropertyKeys(JSContext* cx, JS::Handle<JSObject*> wrapper,
                               JS::AutoIdVector& props) const override;

  virtual bool getPropertyDescriptor(
      JSContext* cx, JS::Handle<JSObject*> wrapper, JS::Handle<jsid> id,
      JS::MutableHandle<JS::PropertyDescriptor> desc) const override;
  virtual bool getOwnEnumerablePropertyKeys(
      JSContext* cx, JS::Handle<JSObject*> wrapper,
      JS::AutoIdVector& props) const override;
  virtual JSObject* enumerate(JSContext* cx,
                              JS::Handle<JSObject*> wrapper) const override;

  virtual bool call(JSContext* cx, JS::Handle<JSObject*> wrapper,
                    const JS::CallArgs& args) const override;
  virtual bool construct(JSContext* cx, JS::Handle<JSObject*> wrapper,
                         const JS::CallArgs& args) const override;

  virtual bool nativeCall(JSContext* cx, JS::IsAcceptableThis test,
                          JS::NativeImpl impl,
                          const JS::CallArgs& args) const override;

  virtual bool getPrototype(JSContext* cx, JS::HandleObject wrapper,
                            JS::MutableHandleObject protop) const override;

  static const FilteringWrapper singleton;
};

/*
 * The HTML5 spec mandates very particular object behavior for cross-origin DOM
 * objects (Window and Location), some of which runs contrary to the way that
 * other XrayWrappers behave. We use this class to implement those semantics.
 */
class CrossOriginXrayWrapper : public SecurityXrayDOM {
 public:
  constexpr explicit CrossOriginXrayWrapper(unsigned flags)
      : SecurityXrayDOM(flags) {}

  virtual bool getOwnPropertyDescriptor(
      JSContext* cx, JS::Handle<JSObject*> wrapper, JS::Handle<jsid> id,
      JS::MutableHandle<JS::PropertyDescriptor> desc) const override;
  virtual bool defineProperty(JSContext* cx, JS::Handle<JSObject*> wrapper,
                              JS::Handle<jsid> id,
                              JS::Handle<JS::PropertyDescriptor> desc,
                              JS::ObjectOpResult& result) const override;
  virtual bool ownPropertyKeys(JSContext* cx, JS::Handle<JSObject*> wrapper,
                               JS::AutoIdVector& props) const override;
  virtual bool delete_(JSContext* cx, JS::Handle<JSObject*> wrapper,
                       JS::Handle<jsid> id,
                       JS::ObjectOpResult& result) const override;

  virtual bool getPropertyDescriptor(
      JSContext* cx, JS::Handle<JSObject*> wrapper, JS::Handle<jsid> id,
      JS::MutableHandle<JS::PropertyDescriptor> desc) const override;

  virtual bool setPrototype(JSContext* cx, JS::HandleObject wrapper,
                            JS::HandleObject proto,
                            JS::ObjectOpResult& result) const override;
};

}  // namespace xpc

#endif /* __FilteringWrapper_h__ */
