/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "js/UbiNode.h"

#include "mozilla/Assertions.h"
#include "mozilla/Attributes.h"
#include "mozilla/Range.h"
#include "mozilla/Scoped.h"

#include <algorithm>

#include "builtin/String.h"

#include "jit/IonCode.h"
#include "js/Debug.h"
#include "js/TracingAPI.h"
#include "js/TypeDecls.h"
#include "js/UbiNodeUtils.h"
#include "js/Utility.h"
#include "js/Vector.h"
#include "util/Text.h"
#ifdef ENABLE_BIGINT
#include "vm/BigIntType.h"
#endif
#include "vm/Debugger.h"
#include "vm/EnvironmentObject.h"
#include "vm/GlobalObject.h"
#include "vm/JSContext.h"
#include "vm/JSObject.h"
#include "vm/JSScript.h"
#include "vm/Scope.h"
#include "vm/Shape.h"
#include "vm/StringType.h"
#include "vm/SymbolType.h"

#include "vm/Debugger-inl.h"
#include "vm/JSObject-inl.h"

using namespace js;

using JS::DispatchTyped;
using JS::HandleValue;
using JS::Value;
using JS::ZoneSet;
using JS::ubi::AtomOrTwoByteChars;
using JS::ubi::CoarseType;
using JS::ubi::Concrete;
using JS::ubi::Edge;
using JS::ubi::EdgeRange;
using JS::ubi::EdgeVector;
using JS::ubi::Node;
using JS::ubi::StackFrame;
using JS::ubi::TracerConcrete;
using JS::ubi::TracerConcreteWithRealm;
using mozilla::RangedPtr;

struct CopyToBufferMatcher {
  RangedPtr<char16_t> destination;
  size_t maxLength;

  CopyToBufferMatcher(RangedPtr<char16_t> destination, size_t maxLength)
      : destination(destination), maxLength(maxLength) {}

  template <typename CharT>
  static size_t copyToBufferHelper(const CharT* src, RangedPtr<char16_t> dest,
                                   size_t length) {
    size_t i = 0;
    for (; i < length; i++) {
      dest[i] = src[i];
    }
    return i;
  }

  size_t match(JSAtom* atom) {
    if (!atom) {
      return 0;
    }

    size_t length = std::min(atom->length(), maxLength);
    JS::AutoCheckCannotGC noGC;
    return atom->hasTwoByteChars()
               ? copyToBufferHelper(atom->twoByteChars(noGC), destination,
                                    length)
               : copyToBufferHelper(atom->latin1Chars(noGC), destination,
                                    length);
  }

  size_t match(const char16_t* chars) {
    if (!chars) {
      return 0;
    }

    size_t length = std::min(js_strlen(chars), maxLength);
    return copyToBufferHelper(chars, destination, length);
  }
};

size_t JS::ubi::AtomOrTwoByteChars::copyToBuffer(
    RangedPtr<char16_t> destination, size_t length) {
  CopyToBufferMatcher m(destination, length);
  return match(m);
}

struct LengthMatcher {
  size_t match(JSAtom* atom) { return atom ? atom->length() : 0; }

  size_t match(const char16_t* chars) { return chars ? js_strlen(chars) : 0; }
};

size_t JS::ubi::AtomOrTwoByteChars::length() {
  LengthMatcher m;
  return match(m);
}

size_t StackFrame::source(RangedPtr<char16_t> destination,
                          size_t length) const {
  auto s = source();
  return s.copyToBuffer(destination, length);
}

size_t StackFrame::functionDisplayName(RangedPtr<char16_t> destination,
                                       size_t length) const {
  auto name = functionDisplayName();
  return name.copyToBuffer(destination, length);
}

size_t StackFrame::sourceLength() { return source().length(); }

size_t StackFrame::functionDisplayNameLength() {
  return functionDisplayName().length();
}

// All operations on null ubi::Nodes crash.
CoarseType Concrete<void>::coarseType() const { MOZ_CRASH("null ubi::Node"); }
const char16_t* Concrete<void>::typeName() const {
  MOZ_CRASH("null ubi::Node");
}
JS::Zone* Concrete<void>::zone() const { MOZ_CRASH("null ubi::Node"); }
JS::Compartment* Concrete<void>::compartment() const {
  MOZ_CRASH("null ubi::Node");
}
JS::Realm* Concrete<void>::realm() const { MOZ_CRASH("null ubi::Node"); }

UniquePtr<EdgeRange> Concrete<void>::edges(JSContext*, bool) const {
  MOZ_CRASH("null ubi::Node");
}

Node::Size Concrete<void>::size(mozilla::MallocSizeOf mallocSizeof) const {
  MOZ_CRASH("null ubi::Node");
}

struct Node::ConstructFunctor : public js::BoolDefaultAdaptor<Value, false> {
  template <typename T>
  bool operator()(T* t, Node* node) {
    node->construct(t);
    return true;
  }
};

Node::Node(const JS::GCCellPtr& thing) {
  DispatchTyped(ConstructFunctor(), thing, this);
}

Node::Node(HandleValue value) {
  if (!DispatchTyped(ConstructFunctor(), value, this)) {
    construct<void>(nullptr);
  }
}

Value Node::exposeToJS() const {
  Value v;

  if (is<JSObject>()) {
    JSObject& obj = *as<JSObject>();
    if (obj.is<js::EnvironmentObject>()) {
      v.setUndefined();
    } else if (obj.is<JSFunction>() && js::IsInternalFunctionObject(obj)) {
      v.setUndefined();
    } else {
      v.setObject(obj);
    }
  } else if (is<JSString>()) {
    v.setString(as<JSString>());
  } else if (is<JS::Symbol>()) {
    v.setSymbol(as<JS::Symbol>());
  }
#ifdef ENABLE_BIGINT
  else if (is<BigInt>()) {
    v.setBigInt(as<BigInt>());
  }
#endif
  else {
    v.setUndefined();
  }

  ExposeValueToActiveJS(v);

  return v;
}

// A JS::CallbackTracer subclass that adds a Edge to a Vector for each
// edge on which it is invoked.
class EdgeVectorTracer : public JS::CallbackTracer {
  // The vector to which we add Edges.
  EdgeVector* vec;

  // True if we should populate the edge's names.
  bool wantNames;

  void onChild(const JS::GCCellPtr& thing) override {
    if (!okay) {
      return;
    }

    // Don't trace permanent atoms and well-known symbols that are owned by
    // a parent JSRuntime.
    if (thing.is<JSString>() && thing.as<JSString>().isPermanentAtom()) {
      return;
    }
    if (thing.is<JS::Symbol>() && thing.as<JS::Symbol>().isWellKnownSymbol()) {
      return;
    }

    char16_t* name16 = nullptr;
    if (wantNames) {
      // Ask the tracer to compute an edge name for us.
      char buffer[1024];
      getTracingEdgeName(buffer, sizeof(buffer));
      const char* name = buffer;

      // Convert the name to char16_t characters.
      name16 = js_pod_malloc<char16_t>(strlen(name) + 1);
      if (!name16) {
        okay = false;
        return;
      }

      size_t i;
      for (i = 0; name[i]; i++) {
        name16[i] = name[i];
      }
      name16[i] = '\0';
    }

    // The simplest code is correct! The temporary Edge takes
    // ownership of name; if the append succeeds, the vector element
    // then takes ownership; if the append fails, then the temporary
    // retains it, and its destructor will free it.
    if (!vec->append(Edge(name16, Node(thing)))) {
      okay = false;
      return;
    }
  }

 public:
  // True if no errors (OOM, say) have yet occurred.
  bool okay;

  EdgeVectorTracer(JSRuntime* rt, EdgeVector* vec, bool wantNames)
      : JS::CallbackTracer(rt), vec(vec), wantNames(wantNames), okay(true) {}
};

template <typename Referent>
JS::Zone* TracerConcrete<Referent>::zone() const {
  return get().zoneFromAnyThread();
}

template JS::Zone* TracerConcrete<JSScript>::zone() const;
template JS::Zone* TracerConcrete<js::LazyScript>::zone() const;
template JS::Zone* TracerConcrete<js::Shape>::zone() const;
template JS::Zone* TracerConcrete<js::BaseShape>::zone() const;
template JS::Zone* TracerConcrete<js::ObjectGroup>::zone() const;
template JS::Zone* TracerConcrete<js::RegExpShared>::zone() const;
template JS::Zone* TracerConcrete<js::Scope>::zone() const;
template JS::Zone* TracerConcrete<JS::Symbol>::zone() const;
#ifdef ENABLE_BIGINT
template JS::Zone* TracerConcrete<BigInt>::zone() const;
#endif
template JS::Zone* TracerConcrete<JSString>::zone() const;

template <typename Referent>
UniquePtr<EdgeRange> TracerConcrete<Referent>::edges(JSContext* cx,
                                                     bool wantNames) const {
  auto range = js::MakeUnique<SimpleEdgeRange>();
  if (!range) {
    return nullptr;
  }

  if (!range->addTracerEdges(cx->runtime(), ptr,
                             JS::MapTypeToTraceKind<Referent>::kind,
                             wantNames)) {
    return nullptr;
  }

  // Note: Clang 3.8 (or older) require an explicit construction of the
  // target UniquePtr type. When we no longer require to support these Clang
  // versions the return statement can be simplified to |return range;|.
  return UniquePtr<EdgeRange>(range.release());
}

template UniquePtr<EdgeRange> TracerConcrete<JSScript>::edges(
    JSContext* cx, bool wantNames) const;
template UniquePtr<EdgeRange> TracerConcrete<js::LazyScript>::edges(
    JSContext* cx, bool wantNames) const;
template UniquePtr<EdgeRange> TracerConcrete<js::Shape>::edges(
    JSContext* cx, bool wantNames) const;
template UniquePtr<EdgeRange> TracerConcrete<js::BaseShape>::edges(
    JSContext* cx, bool wantNames) const;
template UniquePtr<EdgeRange> TracerConcrete<js::ObjectGroup>::edges(
    JSContext* cx, bool wantNames) const;
template UniquePtr<EdgeRange> TracerConcrete<js::RegExpShared>::edges(
    JSContext* cx, bool wantNames) const;
template UniquePtr<EdgeRange> TracerConcrete<js::Scope>::edges(
    JSContext* cx, bool wantNames) const;
template UniquePtr<EdgeRange> TracerConcrete<JS::Symbol>::edges(
    JSContext* cx, bool wantNames) const;
#ifdef ENABLE_BIGINT
template UniquePtr<EdgeRange> TracerConcrete<BigInt>::edges(
    JSContext* cx, bool wantNames) const;
#endif
template UniquePtr<EdgeRange> TracerConcrete<JSString>::edges(
    JSContext* cx, bool wantNames) const;

template <typename Referent>
JS::Compartment* TracerConcreteWithRealm<Referent>::compartment() const {
  return TracerBase::get().compartment();
}

template <typename Referent>
Realm* TracerConcreteWithRealm<Referent>::realm() const {
  return TracerBase::get().realm();
}

template Realm* TracerConcreteWithRealm<JSScript>::realm() const;
template JS::Compartment* TracerConcreteWithRealm<JSScript>::compartment()
    const;

bool Concrete<JSObject>::hasAllocationStack() const {
  return !!js::Debugger::getObjectAllocationSite(get());
}

StackFrame Concrete<JSObject>::allocationStack() const {
  MOZ_ASSERT(hasAllocationStack());
  return StackFrame(js::Debugger::getObjectAllocationSite(get()));
}

const char* Concrete<JSObject>::jsObjectClassName() const {
  return Concrete::get().getClass()->name;
}

bool Concrete<JSObject>::jsObjectConstructorName(
    JSContext* cx, UniqueTwoByteChars& outName) const {
  JSAtom* name = Concrete::get().maybeConstructorDisplayAtom();
  if (!name) {
    outName.reset(nullptr);
    return true;
  }

  auto len = JS_GetStringLength(name);
  auto size = len + 1;

  outName.reset(cx->pod_malloc<char16_t>(size * sizeof(char16_t)));
  if (!outName) {
    return false;
  }

  mozilla::Range<char16_t> chars(outName.get(), size);
  if (!JS_CopyStringChars(cx, chars, name)) {
    return false;
  }

  outName[len] = '\0';
  return true;
}

JS::Compartment* Concrete<JSObject>::compartment() const {
  return Concrete::get().compartment();
}

Realm* Concrete<JSObject>::realm() const {
  // Cross-compartment wrappers are shared by all realms in the compartment,
  // so we return nullptr in that case.
  return JS::GetObjectRealmOrNull(&Concrete::get());
}

const char16_t Concrete<JS::Symbol>::concreteTypeName[] = u"JS::Symbol";
#ifdef ENABLE_BIGINT
const char16_t Concrete<BigInt>::concreteTypeName[] = u"JS::BigInt";
#endif
const char16_t Concrete<JSScript>::concreteTypeName[] = u"JSScript";
const char16_t Concrete<js::LazyScript>::concreteTypeName[] = u"js::LazyScript";
const char16_t Concrete<js::jit::JitCode>::concreteTypeName[] =
    u"js::jit::JitCode";
const char16_t Concrete<js::Shape>::concreteTypeName[] = u"js::Shape";
const char16_t Concrete<js::BaseShape>::concreteTypeName[] = u"js::BaseShape";
const char16_t Concrete<js::ObjectGroup>::concreteTypeName[] =
    u"js::ObjectGroup";
const char16_t Concrete<js::Scope>::concreteTypeName[] = u"js::Scope";
const char16_t Concrete<js::RegExpShared>::concreteTypeName[] =
    u"js::RegExpShared";

namespace JS {
namespace ubi {

RootList::RootList(JSContext* cx, Maybe<AutoCheckCannotGC>& noGC,
                   bool wantNames /* = false */)
    : noGC(noGC), cx(cx), edges(), wantNames(wantNames) {}

bool RootList::init() {
  EdgeVectorTracer tracer(cx->runtime(), &edges, wantNames);
  js::TraceRuntime(&tracer);
  if (!tracer.okay) {
    return false;
  }
  noGC.emplace();
  return true;
}

bool RootList::init(CompartmentSet& debuggees) {
  EdgeVector allRootEdges;
  EdgeVectorTracer tracer(cx->runtime(), &allRootEdges, wantNames);

  ZoneSet debuggeeZones;
  for (auto range = debuggees.all(); !range.empty(); range.popFront()) {
    if (!debuggeeZones.put(range.front()->zone())) {
      return false;
    }
  }

  js::TraceRuntime(&tracer);
  if (!tracer.okay) {
    return false;
  }
  TraceIncomingCCWs(&tracer, debuggees);
  if (!tracer.okay) {
    return false;
  }

  for (EdgeVector::Range r = allRootEdges.all(); !r.empty(); r.popFront()) {
    Edge& edge = r.front();

    JS::Compartment* compartment = edge.referent.compartment();
    if (compartment && !debuggees.has(compartment)) {
      continue;
    }

    Zone* zone = edge.referent.zone();
    if (zone && !debuggeeZones.has(zone)) {
      continue;
    }

    if (!edges.append(std::move(edge))) {
      return false;
    }
  }

  noGC.emplace();
  return true;
}

bool RootList::init(HandleObject debuggees) {
  MOZ_ASSERT(debuggees && JS::dbg::IsDebugger(*debuggees));
  js::Debugger* dbg = js::Debugger::fromJSObject(debuggees.get());

  CompartmentSet debuggeeCompartments;

  for (js::WeakGlobalObjectSet::Range r = dbg->allDebuggees(); !r.empty();
       r.popFront()) {
    if (!debuggeeCompartments.put(r.front()->compartment())) {
      return false;
    }
  }

  if (!init(debuggeeCompartments)) {
    return false;
  }

  // Ensure that each of our debuggee globals are in the root list.
  for (js::WeakGlobalObjectSet::Range r = dbg->allDebuggees(); !r.empty();
       r.popFront()) {
    if (!addRoot(JS::ubi::Node(static_cast<JSObject*>(r.front())),
                 u"debuggee global")) {
      return false;
    }
  }

  return true;
}

bool RootList::addRoot(Node node, const char16_t* edgeName) {
  MOZ_ASSERT(noGC.isSome());
  MOZ_ASSERT_IF(wantNames, edgeName);

  UniqueTwoByteChars name;
  if (edgeName) {
    name = js::DuplicateString(edgeName);
    if (!name) {
      return false;
    }
  }

  return edges.append(Edge(name.release(), node));
}

const char16_t Concrete<RootList>::concreteTypeName[] = u"JS::ubi::RootList";

UniquePtr<EdgeRange> Concrete<RootList>::edges(JSContext* cx,
                                               bool wantNames) const {
  MOZ_ASSERT_IF(wantNames, get().wantNames);
  return js::MakeUnique<PreComputedEdgeRange>(get().edges);
}

bool SimpleEdgeRange::addTracerEdges(JSRuntime* rt, void* thing,
                                     JS::TraceKind kind, bool wantNames) {
  EdgeVectorTracer tracer(rt, &edges, wantNames);
  js::TraceChildren(&tracer, thing, kind);
  settle();
  return tracer.okay;
}

void Concrete<JSObject>::construct(void* storage, JSObject* ptr) {
  if (ptr) {
    auto clasp = ptr->getClass();
    auto callback = ptr->compartment()
                        ->runtimeFromMainThread()
                        ->constructUbiNodeForDOMObjectCallback;
    if (clasp->isDOMClass() && callback) {
      AutoSuppressGCAnalysis suppress;
      callback(storage, ptr);
      return;
    }
  }
  new (storage) Concrete(ptr);
}

void SetConstructUbiNodeForDOMObjectCallback(JSContext* cx,
                                             void (*callback)(void*,
                                                              JSObject*)) {
  cx->runtime()->constructUbiNodeForDOMObjectCallback = callback;
}

}  // namespace ubi
}  // namespace JS
