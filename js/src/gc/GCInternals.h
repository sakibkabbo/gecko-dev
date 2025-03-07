/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

/*
 * GC-internal definitions.
 */

#ifndef gc_GCInternals_h
#define gc_GCInternals_h

#include "mozilla/ArrayUtils.h"
#include "mozilla/Maybe.h"

#include "gc/GC.h"
#include "gc/RelocationOverlay.h"
#include "vm/HelperThreads.h"
#include "vm/Runtime.h"

namespace js {
namespace gc {

class MOZ_RAII AutoCheckCanAccessAtomsDuringGC {
#ifdef DEBUG
  JSRuntime* runtime;

 public:
  explicit AutoCheckCanAccessAtomsDuringGC(JSRuntime* rt) : runtime(rt) {
    // Ensure we're only used from within the GC.
    MOZ_ASSERT(JS::RuntimeHeapIsMajorCollecting());

    // Ensure there is no off-thread parsing running.
    MOZ_ASSERT(!rt->hasHelperThreadZones());

    // Set up a check to assert if we try to start an off-thread parse.
    runtime->setOffThreadParsingBlocked(true);
  }
  ~AutoCheckCanAccessAtomsDuringGC() {
    runtime->setOffThreadParsingBlocked(false);
  }
#else
 public:
  explicit AutoCheckCanAccessAtomsDuringGC(JSRuntime* rt) {}
#endif
};

// Abstract base class for exclusive heap access for tracing or GC.
class MOZ_RAII AutoHeapSession {
 public:
  ~AutoHeapSession();

 protected:
  AutoHeapSession(JSRuntime* rt, JS::HeapState state);

 private:
  AutoHeapSession(const AutoHeapSession&) = delete;
  void operator=(const AutoHeapSession&) = delete;

  JSRuntime* runtime;
  JS::HeapState prevState;
  AutoGeckoProfilerEntry profilingStackFrame;
};

class MOZ_RAII AutoGCSession : public AutoHeapSession {
 public:
  explicit AutoGCSession(JSRuntime* rt, JS::HeapState state)
      : AutoHeapSession(rt, state) {}

  AutoCheckCanAccessAtomsDuringGC& checkAtomsAccess() {
    return maybeCheckAtomsAccess.ref();
  }

  // During a GC we can check that it's not possible for anything else to be
  // using the atoms zone.
  mozilla::Maybe<AutoCheckCanAccessAtomsDuringGC> maybeCheckAtomsAccess;
};

class MOZ_RAII AutoTraceSession : public AutoLockAllAtoms,
                                  public AutoHeapSession {
 public:
  explicit AutoTraceSession(JSRuntime* rt)
      : AutoLockAllAtoms(rt), AutoHeapSession(rt, JS::HeapState::Tracing) {}
};

struct MOZ_RAII AutoFinishGC {
  explicit AutoFinishGC(JSContext* cx) { FinishGC(cx); }
};

// This class should be used by any code that needs exclusive access to the heap
// in order to trace through it.
class MOZ_RAII AutoPrepareForTracing : private AutoFinishGC,
                                       public AutoTraceSession {
 public:
  explicit AutoPrepareForTracing(JSContext* cx)
      : AutoFinishGC(cx), AutoTraceSession(cx->runtime()) {}
};

AbortReason IsIncrementalGCUnsafe(JSRuntime* rt);

#ifdef JS_GC_ZEAL

class MOZ_RAII AutoStopVerifyingBarriers {
  GCRuntime* gc;
  bool restartPreVerifier;

 public:
  AutoStopVerifyingBarriers(JSRuntime* rt, bool isShutdown) : gc(&rt->gc) {
    if (gc->isVerifyPreBarriersEnabled()) {
      gc->endVerifyPreBarriers();
      restartPreVerifier = !isShutdown;
    } else {
      restartPreVerifier = false;
    }
  }

  ~AutoStopVerifyingBarriers() {
    // Nasty special case: verification runs a minor GC, which *may* nest
    // inside of an outer minor GC. This is not allowed by the
    // gc::Statistics phase tree. So we pause the "real" GC, if in fact one
    // is in progress.
    gcstats::PhaseKind outer = gc->stats().currentPhaseKind();
    if (outer != gcstats::PhaseKind::NONE) {
      gc->stats().endPhase(outer);
    }
    MOZ_ASSERT(gc->stats().currentPhaseKind() == gcstats::PhaseKind::NONE);

    if (restartPreVerifier) {
      gc->startVerifyPreBarriers();
    }

    if (outer != gcstats::PhaseKind::NONE) {
      gc->stats().beginPhase(outer);
    }
  }
};
#else
struct MOZ_RAII AutoStopVerifyingBarriers {
  AutoStopVerifyingBarriers(JSRuntime*, bool) {}
};
#endif /* JS_GC_ZEAL */

#ifdef JSGC_HASH_TABLE_CHECKS
void CheckHashTablesAfterMovingGC(JSRuntime* rt);
void CheckHeapAfterGC(JSRuntime* rt);
#endif

struct MovingTracer : JS::CallbackTracer {
  explicit MovingTracer(JSRuntime* rt)
      : CallbackTracer(rt, TraceWeakMapKeysValues) {}

  void onObjectEdge(JSObject** objp) override;
  void onShapeEdge(Shape** shapep) override;
  void onStringEdge(JSString** stringp) override;
  void onScriptEdge(JSScript** scriptp) override;
  void onLazyScriptEdge(LazyScript** lazyp) override;
  void onBaseShapeEdge(BaseShape** basep) override;
  void onScopeEdge(Scope** basep) override;
  void onRegExpSharedEdge(RegExpShared** sharedp) override;
  void onChild(const JS::GCCellPtr& thing) override {
    MOZ_ASSERT(!thing.asCell()->isForwarded());
  }

#ifdef DEBUG
  TracerKind getTracerKind() const override { return TracerKind::Moving; }
#endif

 private:
  template <typename T>
  void updateEdge(T** thingp);
};

// Structure for counting how many times objects in a particular group have
// been tenured during a minor collection.
struct TenureCount {
  ObjectGroup* group;
  unsigned count;

  // ObjectGroups are never nursery-allocated, and TenureCounts are only used
  // in minor GC (not compacting GC), so prevent the analysis from
  // complaining about TenureCounts being held live across a minor GC.
} JS_HAZ_NON_GC_POINTER;

// Keep rough track of how many times we tenure objects in particular groups
// during minor collections, using a fixed size hash for efficiency at the cost
// of potential collisions.
struct TenureCountCache {
  static const size_t EntryShift = 4;
  static const size_t EntryCount = 1 << EntryShift;

  TenureCount entries[EntryCount] = {};  // zeroes

  TenureCountCache() = default;

  HashNumber hash(ObjectGroup* group) {
#if JS_BITS_PER_WORD == 32
    static const size_t ZeroBits = 3;
#else
    static const size_t ZeroBits = 4;
#endif

    uintptr_t word = uintptr_t(group);
    MOZ_ASSERT((word & ((1 << ZeroBits) - 1)) == 0);
    word >>= ZeroBits;
    return HashNumber((word >> EntryShift) ^ word);
  }

  TenureCount& findEntry(ObjectGroup* group) {
    return entries[hash(group) % EntryCount];
  }
};

struct MOZ_RAII AutoAssertNoNurseryAlloc {
#ifdef DEBUG
  AutoAssertNoNurseryAlloc();
  ~AutoAssertNoNurseryAlloc();
#else
  AutoAssertNoNurseryAlloc() {}
#endif
};

// Note that this class does not suppress buffer allocation/reallocation in the
// nursery, only Cells themselves.
class MOZ_RAII AutoSuppressNurseryCellAlloc {
  JSContext* cx_;

 public:
  explicit AutoSuppressNurseryCellAlloc(JSContext* cx) : cx_(cx) {
    cx_->nurserySuppressions_++;
  }
  ~AutoSuppressNurseryCellAlloc() { cx_->nurserySuppressions_--; }
};

/*
 * There are a couple of classes here that serve mostly as "tokens" indicating
 * that a condition holds. Some functions force the caller to possess such a
 * token because they would misbehave if the condition were false, and it is
 * far more clear to make the condition visible at the point where it can be
 * affected rather than just crashing in an assertion down in the place where
 * it is relied upon.
 */

/*
 * A class that serves as a token that the nursery in the current thread's zone
 * group is empty.
 */
class MOZ_RAII AutoAssertEmptyNursery {
 protected:
  JSContext* cx;

  mozilla::Maybe<AutoAssertNoNurseryAlloc> noAlloc;

  // Check that the nursery is empty.
  void checkCondition(JSContext* cx);

  // For subclasses that need to empty the nursery in their constructors.
  AutoAssertEmptyNursery() : cx(nullptr) {}

 public:
  explicit AutoAssertEmptyNursery(JSContext* cx) : cx(nullptr) {
    checkCondition(cx);
  }

  AutoAssertEmptyNursery(const AutoAssertEmptyNursery& other)
      : AutoAssertEmptyNursery(other.cx) {}
};

/*
 * Evict the nursery upon construction. Serves as a token indicating that the
 * nursery is empty. (See AutoAssertEmptyNursery, above.)
 *
 * Note that this is very improper subclass of AutoAssertHeapBusy, in that the
 * heap is *not* busy within the scope of an AutoEmptyNursery. I will most
 * likely fix this by removing AutoAssertHeapBusy, but that is currently
 * waiting on jonco's review.
 */
class MOZ_RAII AutoEmptyNursery : public AutoAssertEmptyNursery {
 public:
  explicit AutoEmptyNursery(JSContext* cx);
};

extern void DelayCrossCompartmentGrayMarking(JSObject* src);

inline bool IsOOMReason(JS::gcreason::Reason reason) {
  return reason == JS::gcreason::LAST_DITCH ||
         reason == JS::gcreason::MEM_PRESSURE;
}

TenuredCell* AllocateCellInGC(JS::Zone* zone, AllocKind thingKind);

} /* namespace gc */
} /* namespace js */

#endif /* gc_GCInternals_h */
