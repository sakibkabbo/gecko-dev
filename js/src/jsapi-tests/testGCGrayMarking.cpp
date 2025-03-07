/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "gc/Heap.h"
#include "gc/WeakMap.h"
#include "gc/Zone.h"
#include "js/Proxy.h"
#include "jsapi-tests/tests.h"

using namespace js;
using namespace js::gc;

namespace js {

struct GCManagedObjectWeakMap : public ObjectWeakMap {
  using ObjectWeakMap::ObjectWeakMap;
};

}  // namespace js

namespace JS {

template <>
struct DeletePolicy<js::GCManagedObjectWeakMap>
    : public js::GCManagedDeletePolicy<js::GCManagedObjectWeakMap> {};

template <>
struct MapTypeToRootKind<js::GCManagedObjectWeakMap*> {
  static const JS::RootKind kind = JS::RootKind::Traceable;
};

template <>
struct GCPolicy<js::GCManagedObjectWeakMap*>
    : public NonGCPointerPolicy<js::GCManagedObjectWeakMap*> {};

}  // namespace JS

class AutoNoAnalysisForTest {
 public:
  AutoNoAnalysisForTest() {}
} JS_HAZ_GC_SUPPRESSED;

BEGIN_TEST(testGCGrayMarking) {
  AutoNoAnalysisForTest disableAnalysis;
  AutoDisableCompactingGC disableCompactingGC(cx);
#ifdef JS_GC_ZEAL
  AutoLeaveZeal nozeal(cx);
#endif /* JS_GC_ZEAL */

  CHECK(InitGlobals());
  JSAutoRealm ar(cx, global1);

  InitGrayRootTracer();

  bool ok = TestMarking() && TestWeakMaps() && TestUnassociatedWeakMaps() &&
            TestCCWs() && TestGrayUnmarking();

  global1 = nullptr;
  global2 = nullptr;
  RemoveGrayRootTracer();

  return ok;
}

bool TestMarking() {
  JSObject* sameTarget = AllocPlainObject();
  CHECK(sameTarget);

  JSObject* sameSource = AllocSameCompartmentSourceObject(sameTarget);
  CHECK(sameSource);

  JSObject* crossTarget = AllocPlainObject();
  CHECK(crossTarget);

  JSObject* crossSource = GetCrossCompartmentWrapper(crossTarget);
  CHECK(crossSource);

  // Test GC with black roots marks objects black.

  JS::RootedObject blackRoot1(cx, sameSource);
  JS::RootedObject blackRoot2(cx, crossSource);

  JS_GC(cx);

  CHECK(IsMarkedBlack(sameSource));
  CHECK(IsMarkedBlack(crossSource));
  CHECK(IsMarkedBlack(sameTarget));
  CHECK(IsMarkedBlack(crossTarget));

  // Test GC with black and gray roots marks objects black.

  grayRoots.grayRoot1 = sameSource;
  grayRoots.grayRoot2 = crossSource;

  JS_GC(cx);

  CHECK(IsMarkedBlack(sameSource));
  CHECK(IsMarkedBlack(crossSource));
  CHECK(IsMarkedBlack(sameTarget));
  CHECK(IsMarkedBlack(crossTarget));

  CHECK(!JS::ObjectIsMarkedGray(sameSource));

  // Test GC with gray roots marks object gray.

  blackRoot1 = nullptr;
  blackRoot2 = nullptr;

  JS_GC(cx);

  CHECK(IsMarkedGray(sameSource));
  CHECK(IsMarkedGray(crossSource));
  CHECK(IsMarkedGray(sameTarget));
  CHECK(IsMarkedGray(crossTarget));

  CHECK(JS::ObjectIsMarkedGray(sameSource));

  // Test ExposeToActiveJS marks gray objects black.

  JS::ExposeObjectToActiveJS(sameSource);
  JS::ExposeObjectToActiveJS(crossSource);
  CHECK(IsMarkedBlack(sameSource));
  CHECK(IsMarkedBlack(crossSource));
  CHECK(IsMarkedBlack(sameTarget));
  CHECK(IsMarkedBlack(crossTarget));

  // Test a zone GC with black roots marks gray object in other zone black.

  JS_GC(cx);

  CHECK(IsMarkedGray(crossSource));
  CHECK(IsMarkedGray(crossTarget));

  blackRoot1 = crossSource;
  CHECK(ZoneGC(crossSource->zone()));

  CHECK(IsMarkedBlack(crossSource));
  CHECK(IsMarkedBlack(crossTarget));

  blackRoot1 = nullptr;
  blackRoot2 = nullptr;
  grayRoots.grayRoot1 = nullptr;
  grayRoots.grayRoot2 = nullptr;

  return true;
}

bool TestWeakMaps() {
  JSObject* weakMap = JS::NewWeakMapObject(cx);
  CHECK(weakMap);

  JSObject* key;
  JSObject* value;
  {
    JS::RootedObject rootedMap(cx, weakMap);

    key = AllocWeakmapKeyObject();
    CHECK(key);

    value = AllocPlainObject();
    CHECK(value);

    weakMap = rootedMap;
  }

  {
    JS::RootedObject rootedMap(cx, weakMap);
    JS::RootedObject rootedKey(cx, key);
    JS::RootedValue rootedValue(cx, ObjectValue(*value));
    CHECK(SetWeakMapEntry(cx, rootedMap, rootedKey, rootedValue));
  }

  // Test the value of a weakmap entry is marked gray by GC if both the
  // weakmap and key are marked gray.

  grayRoots.grayRoot1 = weakMap;
  grayRoots.grayRoot2 = key;
  JS_GC(cx);
  CHECK(IsMarkedGray(weakMap));
  CHECK(IsMarkedGray(key));
  CHECK(IsMarkedGray(value));

  // Test the value of a weakmap entry is marked gray by GC if one of the
  // weakmap and the key is marked gray and the other black.

  JS::RootedObject blackRoot1(cx);
  blackRoot1 = weakMap;
  grayRoots.grayRoot1 = key;
  grayRoots.grayRoot2 = nullptr;
  JS_GC(cx);
  CHECK(IsMarkedBlack(weakMap));
  CHECK(IsMarkedGray(key));
  CHECK(IsMarkedGray(value));

  blackRoot1 = key;
  grayRoots.grayRoot1 = weakMap;
  grayRoots.grayRoot2 = nullptr;
  JS_GC(cx);
  CHECK(IsMarkedGray(weakMap));
  CHECK(IsMarkedBlack(key));
  CHECK(IsMarkedGray(value));

  // Test the value of a weakmap entry is marked black by GC if both the
  // weakmap and the key are marked black.

  JS::RootedObject blackRoot2(cx);
  grayRoots.grayRoot1 = nullptr;
  grayRoots.grayRoot2 = nullptr;

  blackRoot1 = weakMap;
  blackRoot2 = key;
  JS_GC(cx);
  CHECK(IsMarkedBlack(weakMap));
  CHECK(IsMarkedBlack(key));
  CHECK(IsMarkedBlack(value));

  blackRoot1 = key;
  blackRoot2 = weakMap;
  JS_GC(cx);
  CHECK(IsMarkedBlack(weakMap));
  CHECK(IsMarkedBlack(key));
  CHECK(IsMarkedBlack(value));

  // Test that a weakmap key is marked gray if it has a gray delegate and the
  // map is either gray or black.

  JSObject* delegate = UncheckedUnwrapWithoutExpose(key);
  blackRoot1 = weakMap;
  blackRoot2 = nullptr;
  grayRoots.grayRoot1 = delegate;
  grayRoots.grayRoot2 = nullptr;
  JS_GC(cx);
  CHECK(IsMarkedGray(delegate));
  CHECK(IsMarkedGray(key));
  CHECK(IsMarkedBlack(weakMap));
  CHECK(IsMarkedGray(value));

  blackRoot1 = nullptr;
  blackRoot2 = nullptr;
  grayRoots.grayRoot1 = weakMap;
  grayRoots.grayRoot2 = delegate;
  JS_GC(cx);
  CHECK(IsMarkedGray(delegate));
  CHECK(IsMarkedGray(key));
  CHECK(IsMarkedGray(weakMap));
  CHECK(IsMarkedGray(value));

  // Test that a weakmap key is marked gray if it has a black delegate but
  // the map is gray.

  blackRoot1 = delegate;
  blackRoot2 = nullptr;
  grayRoots.grayRoot1 = weakMap;
  grayRoots.grayRoot2 = nullptr;
  JS_GC(cx);
  CHECK(IsMarkedBlack(delegate));
  CHECK(IsMarkedGray(key));
  CHECK(IsMarkedGray(weakMap));
  CHECK(IsMarkedGray(value));

  blackRoot1 = delegate;
  blackRoot2 = nullptr;
  grayRoots.grayRoot1 = weakMap;
  grayRoots.grayRoot2 = key;
  JS_GC(cx);
  CHECK(IsMarkedBlack(delegate));
  CHECK(IsMarkedGray(key));
  CHECK(IsMarkedGray(weakMap));
  CHECK(IsMarkedGray(value));

  // Test that a weakmap key is marked black if it has a black delegate and
  // the map is black.

  blackRoot1 = delegate;
  blackRoot2 = weakMap;
  grayRoots.grayRoot1 = nullptr;
  grayRoots.grayRoot2 = nullptr;
  JS_GC(cx);
  CHECK(IsMarkedBlack(delegate));
  CHECK(IsMarkedBlack(key));
  CHECK(IsMarkedBlack(weakMap));
  CHECK(IsMarkedBlack(value));

  blackRoot1 = delegate;
  blackRoot2 = weakMap;
  grayRoots.grayRoot1 = key;
  grayRoots.grayRoot2 = nullptr;
  JS_GC(cx);
  CHECK(IsMarkedBlack(delegate));
  CHECK(IsMarkedBlack(key));
  CHECK(IsMarkedBlack(weakMap));
  CHECK(IsMarkedBlack(value));

  // Test what happens if there is a delegate but it is not marked for both
  // black and gray cases.

  delegate = nullptr;
  blackRoot1 = key;
  blackRoot2 = weakMap;
  grayRoots.grayRoot1 = nullptr;
  grayRoots.grayRoot2 = nullptr;
  JS_GC(cx);
  CHECK(IsMarkedBlack(key));
  CHECK(IsMarkedBlack(weakMap));
  CHECK(IsMarkedBlack(value));

  blackRoot1 = nullptr;
  blackRoot2 = nullptr;
  grayRoots.grayRoot1 = weakMap;
  grayRoots.grayRoot2 = key;
  JS_GC(cx);
  CHECK(IsMarkedGray(key));
  CHECK(IsMarkedGray(weakMap));
  CHECK(IsMarkedGray(value));

  blackRoot1 = nullptr;
  blackRoot2 = nullptr;
  grayRoots.grayRoot1 = nullptr;
  grayRoots.grayRoot2 = nullptr;

  return true;
}

bool TestUnassociatedWeakMaps() {
  // Make a weakmap that's not associated with a JSObject.
  auto weakMap = cx->make_unique<GCManagedObjectWeakMap>(cx);
  CHECK(weakMap);

  // Make sure this gets traced during GC.
  Rooted<GCManagedObjectWeakMap*> rootMap(cx, weakMap.get());

  JSObject* key = AllocWeakmapKeyObject();
  CHECK(key);

  JSObject* value = AllocPlainObject();
  CHECK(value);

  CHECK(weakMap->add(cx, key, value));

  // Test the value of a weakmap entry is marked gray by GC if the
  // key is marked gray.

  grayRoots.grayRoot1 = key;
  JS_GC(cx);
  CHECK(IsMarkedGray(key));
  CHECK(IsMarkedGray(value));

  // Test the value of a weakmap entry is marked gray by GC if the key is marked
  // gray.

  grayRoots.grayRoot1 = key;
  grayRoots.grayRoot2 = nullptr;
  JS_GC(cx);
  CHECK(IsMarkedGray(key));
  CHECK(IsMarkedGray(value));

  // Test the value of a weakmap entry is marked black by GC if the key is
  // marked black.

  JS::RootedObject blackRoot(cx);
  blackRoot = key;
  grayRoots.grayRoot1 = nullptr;
  grayRoots.grayRoot2 = nullptr;
  JS_GC(cx);
  CHECK(IsMarkedBlack(key));
  CHECK(IsMarkedBlack(value));

  // Test that a weakmap key is marked gray if it has a gray delegate.

  JSObject* delegate = UncheckedUnwrapWithoutExpose(key);
  blackRoot = nullptr;
  grayRoots.grayRoot1 = delegate;
  grayRoots.grayRoot2 = nullptr;
  JS_GC(cx);
  CHECK(IsMarkedGray(delegate));
  CHECK(IsMarkedGray(key));
  CHECK(IsMarkedGray(value));

  // Test that a weakmap key is marked black if it has a black delegate.

  blackRoot = delegate;
  grayRoots.grayRoot1 = nullptr;
  grayRoots.grayRoot2 = nullptr;
  JS_GC(cx);
  CHECK(IsMarkedBlack(delegate));
  CHECK(IsMarkedBlack(key));
  CHECK(IsMarkedBlack(value));

  blackRoot = delegate;
  grayRoots.grayRoot1 = key;
  grayRoots.grayRoot2 = nullptr;
  JS_GC(cx);
  CHECK(IsMarkedBlack(delegate));
  CHECK(IsMarkedBlack(key));
  CHECK(IsMarkedBlack(value));

  // Test what happens if there is a delegate but it is not marked for both
  // black and gray cases.

  delegate = nullptr;
  blackRoot = key;
  grayRoots.grayRoot1 = nullptr;
  grayRoots.grayRoot2 = nullptr;
  JS_GC(cx);
  CHECK(IsMarkedBlack(key));
  CHECK(IsMarkedBlack(value));

  blackRoot = nullptr;
  grayRoots.grayRoot1 = key;
  grayRoots.grayRoot2 = nullptr;
  JS_GC(cx);
  CHECK(IsMarkedGray(key));
  CHECK(IsMarkedGray(value));

  blackRoot = nullptr;
  grayRoots.grayRoot1 = nullptr;
  grayRoots.grayRoot2 = nullptr;

  return true;
}

bool TestCCWs() {
  JSObject* target = AllocPlainObject();
  CHECK(target);

  // Test getting a new wrapper doesn't return a gray wrapper.

  RootedObject blackRoot(cx, target);
  JSObject* wrapper = GetCrossCompartmentWrapper(target);
  CHECK(wrapper);
  CHECK(!IsMarkedGray(wrapper));

  // Test getting an existing wrapper doesn't return a gray wrapper.

  grayRoots.grayRoot1 = wrapper;
  grayRoots.grayRoot2 = nullptr;
  JS_GC(cx);
  CHECK(IsMarkedGray(wrapper));
  CHECK(IsMarkedBlack(target));

  CHECK(GetCrossCompartmentWrapper(target) == wrapper);
  CHECK(!IsMarkedGray(wrapper));

  // Test getting an existing wrapper doesn't return a gray wrapper
  // during incremental GC.

  JS_GC(cx);
  CHECK(IsMarkedGray(wrapper));
  CHECK(IsMarkedBlack(target));

  JS_SetGCParameter(cx, JSGC_MODE, JSGC_MODE_INCREMENTAL);
  JS::PrepareForFullGC(cx);
  js::SliceBudget budget(js::WorkBudget(1));
  cx->runtime()->gc.startDebugGC(GC_NORMAL, budget);
  CHECK(JS::IsIncrementalGCInProgress(cx));

  CHECK(!IsMarkedBlack(wrapper));
  CHECK(wrapper->zone()->isGCMarkingBlackOnly());

  CHECK(GetCrossCompartmentWrapper(target) == wrapper);
  CHECK(IsMarkedBlack(wrapper));

  JS::FinishIncrementalGC(cx, JS::gcreason::API);

  // Test behaviour of gray CCWs marked black by a barrier during incremental
  // GC.

  // Initial state: source and target are gray.
  blackRoot = nullptr;
  grayRoots.grayRoot1 = wrapper;
  grayRoots.grayRoot2 = nullptr;
  JS_GC(cx);
  CHECK(IsMarkedGray(wrapper));
  CHECK(IsMarkedGray(target));

  // Incremental zone GC started: the source is now unmarked.
  JS_SetGCParameter(cx, JSGC_MODE, JSGC_MODE_INCREMENTAL);
  JS::PrepareZoneForGC(wrapper->zone());
  budget = js::SliceBudget(js::WorkBudget(1));
  cx->runtime()->gc.startDebugGC(GC_NORMAL, budget);
  CHECK(JS::IsIncrementalGCInProgress(cx));
  CHECK(wrapper->zone()->isGCMarkingBlackOnly());
  CHECK(!target->zone()->wasGCStarted());
  CHECK(!IsMarkedBlack(wrapper));
  CHECK(!IsMarkedGray(wrapper));
  CHECK(IsMarkedGray(target));

  // Betweeen GC slices: source marked black by barrier, target is
  // still gray. Target will be marked gray
  // eventually. ObjectIsMarkedGray() is conservative and reports
  // that target is not marked gray; AssertObjectIsNotGray() will
  // assert.
  grayRoots.grayRoot1.get();
  CHECK(IsMarkedBlack(wrapper));
  CHECK(IsMarkedGray(target));
  CHECK(!JS::ObjectIsMarkedGray(target));

  // Final state: source and target are black.
  JS::FinishIncrementalGC(cx, JS::gcreason::API);
  CHECK(IsMarkedBlack(wrapper));
  CHECK(IsMarkedBlack(target));

  grayRoots.grayRoot1 = nullptr;
  grayRoots.grayRoot2 = nullptr;

  return true;
}

bool TestGrayUnmarking() {
  const size_t length = 2000;

  JSObject* chain = AllocObjectChain(length);
  CHECK(chain);

  RootedObject blackRoot(cx, chain);
  JS_GC(cx);
  size_t count;
  CHECK(IterateObjectChain(chain, ColorCheckFunctor(MarkColor::Black, &count)));
  CHECK(count == length);

  blackRoot = nullptr;
  grayRoots.grayRoot1 = chain;
  JS_GC(cx);
  CHECK(cx->runtime()->gc.areGrayBitsValid());
  CHECK(IterateObjectChain(chain, ColorCheckFunctor(MarkColor::Gray, &count)));
  CHECK(count == length);

  JS::ExposeObjectToActiveJS(chain);
  CHECK(cx->runtime()->gc.areGrayBitsValid());
  CHECK(IterateObjectChain(chain, ColorCheckFunctor(MarkColor::Black, &count)));
  CHECK(count == length);

  grayRoots.grayRoot1 = nullptr;

  return true;
}

struct ColorCheckFunctor {
  MarkColor color;
  size_t& count;

  ColorCheckFunctor(MarkColor colorArg, size_t* countArg)
      : color(colorArg), count(*countArg) {
    count = 0;
  }

  bool operator()(JSObject* obj) {
    if (!CheckCellColor(obj, color)) {
      return false;
    }

    NativeObject& nobj = obj->as<NativeObject>();
    if (!CheckCellColor(nobj.shape(), color)) {
      return false;
    }

    Shape* shape = nobj.shape();
    if (!CheckCellColor(shape, color)) {
      return false;
    }

    // Shapes and symbols are never marked gray.
    jsid id = shape->propid();
    if (JSID_IS_GCTHING(id) &&
        !CheckCellColor(JSID_TO_GCTHING(id).asCell(), MarkColor::Black)) {
      return false;
    }

    count++;
    return true;
  }
};

JS::PersistentRootedObject global1;
JS::PersistentRootedObject global2;

struct GrayRoots {
  JS::Heap<JSObject*> grayRoot1;
  JS::Heap<JSObject*> grayRoot2;
};

GrayRoots grayRoots;

bool InitGlobals() {
  global1.init(cx, global);
  if (!createGlobal()) {
    return false;
  }
  global2.init(cx, global);
  return global2 != nullptr;
}

void InitGrayRootTracer() {
  grayRoots.grayRoot1 = nullptr;
  grayRoots.grayRoot2 = nullptr;
  JS_SetGrayGCRootsTracer(cx, TraceGrayRoots, &grayRoots);
}

void RemoveGrayRootTracer() {
  grayRoots.grayRoot1 = nullptr;
  grayRoots.grayRoot2 = nullptr;
  JS_SetGrayGCRootsTracer(cx, nullptr, nullptr);
}

static void TraceGrayRoots(JSTracer* trc, void* data) {
  auto grayRoots = static_cast<GrayRoots*>(data);
  TraceEdge(trc, &grayRoots->grayRoot1, "gray root 1");
  TraceEdge(trc, &grayRoots->grayRoot2, "gray root 2");
}

JSObject* AllocPlainObject() {
  JS::RootedObject obj(cx, JS_NewPlainObject(cx));
  EvictNursery();

  MOZ_ASSERT(obj->compartment() == global1->compartment());
  return obj;
}

JSObject* AllocSameCompartmentSourceObject(JSObject* target) {
  JS::RootedObject source(cx, JS_NewPlainObject(cx));
  if (!source) {
    return nullptr;
  }

  JS::RootedObject obj(cx, target);
  if (!JS_DefineProperty(cx, source, "ptr", obj, 0)) {
    return nullptr;
  }

  EvictNursery();

  MOZ_ASSERT(source->compartment() == global1->compartment());
  return source;
}

JSObject* GetCrossCompartmentWrapper(JSObject* target) {
  MOZ_ASSERT(target->compartment() == global1->compartment());
  JS::RootedObject obj(cx, target);
  JSAutoRealm ar(cx, global2);
  if (!JS_WrapObject(cx, &obj)) {
    return nullptr;
  }

  EvictNursery();

  MOZ_ASSERT(obj->compartment() == global2->compartment());
  return obj;
}

JSObject* AllocWeakmapKeyObject() {
  JS::RootedObject delegate(cx, JS_NewPlainObject(cx));
  if (!delegate) {
    return nullptr;
  }

  JS::RootedObject key(cx,
                       js::Wrapper::New(cx, delegate, &js::Wrapper::singleton));

  EvictNursery();
  return key;
}

JSObject* AllocObjectChain(size_t length) {
  // Allocate a chain of linked JSObjects.

  // Use a unique property name so the shape is not shared with any other
  // objects.
  RootedString nextPropName(cx, JS_NewStringCopyZ(cx, "unique14142135"));
  RootedId nextId(cx);
  if (!JS_StringToId(cx, nextPropName, &nextId)) {
    return nullptr;
  }

  RootedObject head(cx);
  for (size_t i = 0; i < length; i++) {
    RootedValue next(cx, ObjectOrNullValue(head));
    head = AllocPlainObject();
    if (!head) {
      return nullptr;
    }
    if (!JS_DefinePropertyById(cx, head, nextId, next, 0)) {
      return nullptr;
    }
  }

  return head;
}

template <typename F>
bool IterateObjectChain(JSObject* chain, F f) {
  RootedObject obj(cx, chain);
  while (obj) {
    if (!f(obj)) {
      return false;
    }

    // Access the 'next' property via the object's slots to avoid triggering
    // gray marking assertions when calling JS_GetPropertyById.
    NativeObject& nobj = obj->as<NativeObject>();
    MOZ_ASSERT(nobj.slotSpan() == 1);
    obj = nobj.getSlot(0).toObjectOrNull();
  }

  return true;
}

static bool IsMarkedBlack(Cell* cell) {
  TenuredCell* tc = &cell->asTenured();
  return tc->isMarkedBlack();
}

static bool IsMarkedGray(Cell* cell) {
  TenuredCell* tc = &cell->asTenured();
  bool isGray = tc->isMarkedGray();
  MOZ_ASSERT_IF(isGray, tc->isMarkedAny());
  return isGray;
}

static bool CheckCellColor(Cell* cell, MarkColor color) {
  MOZ_ASSERT(color == MarkColor::Black || color == MarkColor::Gray);
  if (color == MarkColor::Black && !IsMarkedBlack(cell)) {
    printf("Found non-black cell: %p\n", cell);
    return false;
  } else if (color == MarkColor::Gray && !IsMarkedGray(cell)) {
    printf("Found non-gray cell: %p\n", cell);
    return false;
  }

  return true;
}

void EvictNursery() { cx->runtime()->gc.evictNursery(); }

bool ZoneGC(JS::Zone* zone) {
  uint32_t oldMode = JS_GetGCParameter(cx, JSGC_MODE);
  JS_SetGCParameter(cx, JSGC_MODE, JSGC_MODE_ZONE);
  JS::PrepareZoneForGC(zone);
  cx->runtime()->gc.gc(GC_NORMAL, JS::gcreason::API);
  CHECK(!cx->runtime()->gc.isFullGc());
  JS_SetGCParameter(cx, JSGC_MODE, oldMode);
  return true;
}

END_TEST(testGCGrayMarking)
