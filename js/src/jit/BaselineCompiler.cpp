/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "jit/BaselineCompiler.h"

#include "mozilla/Casting.h"

#include "jit/BaselineIC.h"
#include "jit/BaselineJIT.h"
#include "jit/FixedList.h"
#include "jit/IonAnalysis.h"
#include "jit/JitcodeMap.h"
#include "jit/JitSpewer.h"
#include "jit/Linker.h"
#ifdef JS_ION_PERF
#include "jit/PerfSpewer.h"
#endif
#include "jit/SharedICHelpers.h"
#include "jit/VMFunctions.h"
#include "js/UniquePtr.h"
#include "vm/AsyncFunction.h"
#include "vm/AsyncIteration.h"
#include "vm/EnvironmentObject.h"
#include "vm/Interpreter.h"
#include "vm/JSFunction.h"
#include "vm/TraceLogging.h"
#include "vtune/VTuneWrapper.h"

#include "jit/BaselineFrameInfo-inl.h"
#include "jit/MacroAssembler-inl.h"
#include "vm/Interpreter-inl.h"
#include "vm/JSScript-inl.h"
#include "vm/NativeObject-inl.h"
#include "vm/TypeInference-inl.h"

using namespace js;
using namespace js::jit;

using mozilla::AssertedCast;
using mozilla::Maybe;

namespace js {
namespace jit {

BaselineCompilerHandler::BaselineCompilerHandler(TempAllocator& alloc,
                                                 JSScript* script)
    : alloc_(alloc),
      script_(script),
      compileDebugInstrumentation_(script->isDebuggee()) {}

BaselineInterpreterHandler::BaselineInterpreterHandler() {}

template <typename Handler>
template <typename... HandlerArgs>
BaselineCodeGen<Handler>::BaselineCodeGen(JSContext* cx, TempAllocator& alloc,
                                          JSScript* script,
                                          HandlerArgs&&... args)
    : handler(std::forward<HandlerArgs>(args)...),
      cx(cx),
      script(script),
      pc(script->code()),
      ionCompileable_(jit::IsIonEnabled(cx) && CanIonCompileScript(cx, script)),
      alloc_(alloc),
      analysis_(alloc, script),
      frame(script, masm),
      traceLoggerToggleOffsets_(cx),
      icEntryIndex_(0),
      pushedBeforeCall_(0),
#ifdef DEBUG
      inCall_(false),
#endif
      modifiesArguments_(false) {
}

BaselineCompiler::BaselineCompiler(JSContext* cx, TempAllocator& alloc,
                                   JSScript* script)
    : BaselineCodeGen(cx, alloc, script,
                      /* HandlerArgs = */ alloc, script),
      pcMappingEntries_(),
      profilerPushToggleOffset_(),
      profilerEnterFrameToggleOffset_(),
      profilerExitFrameToggleOffset_(),
      traceLoggerScriptTextIdOffset_() {
#ifdef JS_CODEGEN_NONE
  MOZ_CRASH();
#endif
}

bool BaselineCompilerHandler::init() {
  uint32_t len = script_->length();

  if (!labels_.init(alloc_, len)) {
    return false;
  }

  for (size_t i = 0; i < len; i++) {
    new (&labels_[i]) Label();
  }

  return true;
}

bool BaselineCompiler::init() {
  if (!analysis_.init(alloc_, cx->caches().gsnCache)) {
    return false;
  }

  if (!handler.init()) {
    return false;
  }

  if (!frame.init(alloc_)) {
    return false;
  }

  return true;
}

bool BaselineCompiler::addPCMappingEntry(bool addIndexEntry) {
  // Don't add multiple entries for a single pc.
  size_t nentries = pcMappingEntries_.length();
  if (nentries > 0 &&
      pcMappingEntries_[nentries - 1].pcOffset == script->pcToOffset(pc)) {
    return true;
  }

  PCMappingEntry entry;
  entry.pcOffset = script->pcToOffset(pc);
  entry.nativeOffset = masm.currentOffset();
  entry.slotInfo = getStackTopSlotInfo();
  entry.addIndexEntry = addIndexEntry;

  return pcMappingEntries_.append(entry);
}

MethodStatus BaselineCompiler::compile() {
  JitSpew(JitSpew_BaselineScripts, "Baseline compiling script %s:%u:%u (%p)",
          script->filename(), script->lineno(), script->column(), script);

  JitSpew(JitSpew_Codegen, "# Emitting baseline code for script %s:%u:%u",
          script->filename(), script->lineno(), script->column());

  TraceLoggerThread* logger = TraceLoggerForCurrentThread(cx);
  TraceLoggerEvent scriptEvent(TraceLogger_AnnotateScripts, script);
  AutoTraceLog logScript(logger, scriptEvent);
  AutoTraceLog logCompile(logger, TraceLogger_BaselineCompilation);

  AutoKeepTypeScripts keepTypes(cx);
  if (!script->ensureHasTypes(cx, keepTypes) ||
      !script->ensureHasAnalyzedArgsUsage(cx)) {
    return Method_Error;
  }

  // When code coverage is only enabled for optimizations, or when a Debugger
  // set the collectCoverageInfo flag, we have to create the ScriptCounts if
  // they do not exist.
  if (!script->hasScriptCounts() && cx->realm()->collectCoverage()) {
    if (!script->initScriptCounts(cx)) {
      return Method_Error;
    }
  }

  // Pin analysis info during compilation.
  AutoEnterAnalysis autoEnterAnalysis(cx);

  MOZ_ASSERT(!script->hasBaselineScript());

  if (!emitPrologue()) {
    return Method_Error;
  }

  MethodStatus status = emitBody();
  if (status != Method_Compiled) {
    return status;
  }

  if (!emitEpilogue()) {
    return Method_Error;
  }

  if (!emitOutOfLinePostBarrierSlot()) {
    return Method_Error;
  }

  Linker linker(masm);
  if (masm.oom()) {
    ReportOutOfMemory(cx);
    return Method_Error;
  }

  AutoFlushICache afc("Baseline");
  JitCode* code = linker.newCode(cx, CodeKind::Baseline);
  if (!code) {
    return Method_Error;
  }

  Rooted<EnvironmentObject*> templateEnv(cx);
  if (script->functionNonDelazifying()) {
    RootedFunction fun(cx, script->functionNonDelazifying());

    if (fun->needsNamedLambdaEnvironment()) {
      templateEnv =
          NamedLambdaObject::createTemplateObject(cx, fun, gc::TenuredHeap);
      if (!templateEnv) {
        return Method_Error;
      }
    }

    if (fun->needsCallObject()) {
      RootedScript scriptRoot(cx, script);
      templateEnv = CallObject::createTemplateObject(
          cx, scriptRoot, templateEnv, gc::TenuredHeap);
      if (!templateEnv) {
        return Method_Error;
      }
    }
  }

  // Encode the pc mapping table. See PCMappingIndexEntry for
  // more information.
  Vector<PCMappingIndexEntry> pcMappingIndexEntries(cx);
  CompactBufferWriter pcEntries;
  uint32_t previousOffset = 0;

  for (size_t i = 0; i < pcMappingEntries_.length(); i++) {
    PCMappingEntry& entry = pcMappingEntries_[i];

    if (entry.addIndexEntry) {
      PCMappingIndexEntry indexEntry;
      indexEntry.pcOffset = entry.pcOffset;
      indexEntry.nativeOffset = entry.nativeOffset;
      indexEntry.bufferOffset = pcEntries.length();
      if (!pcMappingIndexEntries.append(indexEntry)) {
        ReportOutOfMemory(cx);
        return Method_Error;
      }
      previousOffset = entry.nativeOffset;
    }

    // Use the high bit of the SlotInfo byte to indicate the
    // native code offset (relative to the previous op) > 0 and
    // comes next in the buffer.
    MOZ_ASSERT((entry.slotInfo.toByte() & 0x80) == 0);

    if (entry.nativeOffset == previousOffset) {
      pcEntries.writeByte(entry.slotInfo.toByte());
    } else {
      MOZ_ASSERT(entry.nativeOffset > previousOffset);
      pcEntries.writeByte(0x80 | entry.slotInfo.toByte());
      pcEntries.writeUnsigned(entry.nativeOffset - previousOffset);
    }

    previousOffset = entry.nativeOffset;
  }

  if (pcEntries.oom()) {
    ReportOutOfMemory(cx);
    return Method_Error;
  }

  // Note: There is an extra entry in the bytecode type map for the search
  // hint, see below.
  size_t bytecodeTypeMapEntries = script->nTypeSets() + 1;
  size_t resumeEntries =
      script->hasResumeOffsets() ? script->resumeOffsets().size() : 0;
  UniquePtr<BaselineScript> baselineScript(
      BaselineScript::New(
          script, bailoutPrologueOffset_.offset(),
          debugOsrPrologueOffset_.offset(), debugOsrEpilogueOffset_.offset(),
          profilerEnterFrameToggleOffset_.offset(),
          profilerExitFrameToggleOffset_.offset(), retAddrEntries_.length(),
          pcMappingIndexEntries.length(), pcEntries.length(),
          bytecodeTypeMapEntries, resumeEntries,
          traceLoggerToggleOffsets_.length()),
      JS::DeletePolicy<BaselineScript>(cx->runtime()));
  if (!baselineScript) {
    ReportOutOfMemory(cx);
    return Method_Error;
  }

  baselineScript->setMethod(code);
  baselineScript->setTemplateEnvironment(templateEnv);

  JitSpew(JitSpew_BaselineScripts,
          "Created BaselineScript %p (raw %p) for %s:%u:%u",
          (void*)baselineScript.get(), (void*)code->raw(), script->filename(),
          script->lineno(), script->column());

  MOZ_ASSERT(pcMappingIndexEntries.length() > 0);
  baselineScript->copyPCMappingIndexEntries(&pcMappingIndexEntries[0]);

  MOZ_ASSERT(pcEntries.length() > 0);
  baselineScript->copyPCMappingEntries(pcEntries);

  // Copy RetAddrEntries.
  if (retAddrEntries_.length() > 0) {
    baselineScript->copyRetAddrEntries(script, &retAddrEntries_[0]);
  }

  // If profiler instrumentation is enabled, toggle instrumentation on.
  if (cx->runtime()->jitRuntime()->isProfilerInstrumentationEnabled(
          cx->runtime())) {
    baselineScript->toggleProfilerInstrumentation(true);
  }

  if (modifiesArguments_) {
    baselineScript->setModifiesArguments();
  }
  if (analysis_.usesEnvironmentChain()) {
    baselineScript->setUsesEnvironmentChain();
  }

#ifdef JS_TRACE_LOGGING
  // Initialize the tracelogger instrumentation.
  baselineScript->initTraceLogger(script, traceLoggerToggleOffsets_);
#endif

  uint32_t* bytecodeMap = baselineScript->bytecodeTypeMap();
  FillBytecodeTypeMap(script, bytecodeMap);

  // The last entry in the last index found, and is used to avoid binary
  // searches for the sought entry when queries are in linear order.
  bytecodeMap[script->nTypeSets()] = 0;

  // Compute yield/await native resume addresses.
  baselineScript->computeResumeNativeOffsets(script);

  if (compileDebugInstrumentation()) {
    baselineScript->setHasDebugInstrumentation();
  }

  // Always register a native => bytecode mapping entry, since profiler can be
  // turned on with baseline jitcode on stack, and baseline jitcode cannot be
  // invalidated.
  {
    JitSpew(JitSpew_Profiling,
            "Added JitcodeGlobalEntry for baseline script %s:%u:%u (%p)",
            script->filename(), script->lineno(), script->column(),
            baselineScript.get());

    // Generate profiling string.
    char* str = JitcodeGlobalEntry::createScriptString(cx, script);
    if (!str) {
      return Method_Error;
    }

    JitcodeGlobalEntry::BaselineEntry entry;
    entry.init(code, code->raw(), code->rawEnd(), script, str);

    JitcodeGlobalTable* globalTable =
        cx->runtime()->jitRuntime()->getJitcodeGlobalTable();
    if (!globalTable->addEntry(entry)) {
      entry.destroy();
      ReportOutOfMemory(cx);
      return Method_Error;
    }

    // Mark the jitcode as having a bytecode map.
    code->setHasBytecodeMap();
  }

  script->setBaselineScript(cx->runtime(), baselineScript.release());

#ifdef JS_ION_PERF
  writePerfSpewerBaselineProfile(script, code);
#endif

#ifdef MOZ_VTUNE
  vtune::MarkScript(code, script, "baseline");
#endif

  return Method_Compiled;
}

void BaselineCompiler::emitInitializeLocals() {
  // Initialize all locals to |undefined|. Lexical bindings are temporal
  // dead zoned in bytecode.

  size_t n = frame.nlocals();
  if (n == 0) {
    return;
  }

  // Use R0 to minimize code size. If the number of locals to push is <
  // LOOP_UNROLL_FACTOR, then the initialization pushes are emitted directly
  // and inline.  Otherwise, they're emitted in a partially unrolled loop.
  static const size_t LOOP_UNROLL_FACTOR = 4;
  size_t toPushExtra = n % LOOP_UNROLL_FACTOR;

  masm.moveValue(UndefinedValue(), R0);

  // Handle any extra pushes left over by the optional unrolled loop below.
  for (size_t i = 0; i < toPushExtra; i++) {
    masm.pushValue(R0);
  }

  // Partially unrolled loop of pushes.
  if (n >= LOOP_UNROLL_FACTOR) {
    size_t toPush = n - toPushExtra;
    MOZ_ASSERT(toPush % LOOP_UNROLL_FACTOR == 0);
    MOZ_ASSERT(toPush >= LOOP_UNROLL_FACTOR);
    masm.move32(Imm32(toPush), R1.scratchReg());
    // Emit unrolled loop with 4 pushes per iteration.
    Label pushLoop;
    masm.bind(&pushLoop);
    for (size_t i = 0; i < LOOP_UNROLL_FACTOR; i++) {
      masm.pushValue(R0);
    }
    masm.branchSub32(Assembler::NonZero, Imm32(LOOP_UNROLL_FACTOR),
                     R1.scratchReg(), &pushLoop);
  }
}

bool BaselineCompiler::emitPrologue() {
#ifdef JS_USE_LINK_REGISTER
  // Push link register from generateEnterJIT()'s BLR.
  masm.pushReturnAddress();
  masm.checkStackAlignment();
#endif
  emitProfilerEnterFrame();

  if (script->trackRecordReplayProgress()) {
    masm.inc64(
        AbsoluteAddress(mozilla::recordreplay::ExecutionProgressCounter()));
  }

  masm.push(BaselineFrameReg);
  masm.moveStackPtrTo(BaselineFrameReg);
  masm.subFromStackPtr(Imm32(BaselineFrame::Size()));

  // Initialize BaselineFrame. For eval scripts, the env chain
  // is passed in R1, so we have to be careful not to clobber it.

  // Initialize BaselineFrame::flags.
  masm.store32(Imm32(0), frame.addressOfFlags());

  // Handle env chain pre-initialization (in case GC gets run
  // during stack check).  For global and eval scripts, the env
  // chain is in R1.  For function scripts, the env chain is in
  // the callee, nullptr is stored for now so that GC doesn't choke
  // on a bogus EnvironmentChain value in the frame.
  if (function()) {
    masm.storePtr(ImmPtr(nullptr), frame.addressOfEnvironmentChain());
  } else {
    masm.storePtr(R1.scratchReg(), frame.addressOfEnvironmentChain());
  }

  // Functions with a large number of locals require two stack checks.
  // The VMCall for a fallible stack check can only occur after the
  // env chain has been initialized, as that is required for proper
  // exception handling if the VMCall returns false.  The env chain
  // initialization can only happen after the UndefinedValues for the
  // local slots have been pushed. However by that time, the stack might
  // have grown too much.
  //
  // In these cases, we emit an extra, early, infallible check before pushing
  // the locals. The early check just sets a flag on the frame if the stack
  // check fails. If the flag is set, then the jitcode skips past the pushing
  // of the locals, and directly to env chain initialization followed by the
  // actual stack check, which will throw the correct exception.
  Label earlyStackCheckFailed;
  if (needsEarlyStackCheck()) {
    // Subtract the size of script->nslots() from the stack pointer.
    uint32_t slotsSize = script->nslots() * sizeof(Value);
    Register scratch = R1.scratchReg();
    masm.moveStackPtrTo(scratch);
    masm.subPtr(Imm32(slotsSize), scratch);

    // Set the OVER_RECURSED flag on the frame if the computed stack pointer
    // overflows the stack limit. We have to use the actual (*NoInterrupt)
    // stack limit here because we don't want to set the flag and throw an
    // overrecursion exception later in the interrupt case.
    Label stackCheckOk;
    masm.branchPtr(Assembler::BelowOrEqual,
                   AbsoluteAddress(cx->addressOfJitStackLimitNoInterrupt()),
                   scratch, &stackCheckOk);
    {
      masm.or32(Imm32(BaselineFrame::OVER_RECURSED), frame.addressOfFlags());
      masm.jump(&earlyStackCheckFailed);
    }
    masm.bind(&stackCheckOk);
  }

  emitInitializeLocals();

  if (needsEarlyStackCheck()) {
    masm.bind(&earlyStackCheckFailed);
  }

#ifdef JS_TRACE_LOGGING
  if (!emitTraceLoggerEnter()) {
    return false;
  }
#endif

  // Record the offset of the prologue, because Ion can bailout before
  // the env chain is initialized.
  bailoutPrologueOffset_ = CodeOffset(masm.currentOffset());

  // When compiling with Debugger instrumentation, set the debuggeeness of
  // the frame before any operation that can call into the VM.
  emitIsDebuggeeCheck();

  // Initialize the env chain before any operation that may
  // call into the VM and trigger a GC.
  if (!initEnvironmentChain()) {
    return false;
  }

  frame.assertSyncedStack();
  masm.debugAssertContextRealm(script->realm(), R1.scratchReg());

  if (!emitStackCheck()) {
    return false;
  }

  if (!emitDebugPrologue()) {
    return false;
  }

  if (!emitWarmUpCounterIncrement()) {
    return false;
  }

  if (!emitArgumentTypeChecks()) {
    return false;
  }

  return true;
}

bool BaselineCompiler::emitEpilogue() {
  // Record the offset of the epilogue, so we can do early return from
  // Debugger handlers during on-stack recompile.
  debugOsrEpilogueOffset_ = CodeOffset(masm.currentOffset());

  masm.bind(&return_);

#ifdef JS_TRACE_LOGGING
  if (!emitTraceLoggerExit()) {
    return false;
  }
#endif

  masm.moveToStackPtr(BaselineFrameReg);
  masm.pop(BaselineFrameReg);

  emitProfilerExitFrame();

  masm.ret();
  return true;
}

// On input:
//  R2.scratchReg() contains object being written to.
//  Called with the baseline stack synced, except for R0 which is preserved.
//  All other registers are usable as scratch.
// This calls:
//    void PostWriteBarrier(JSRuntime* rt, JSObject* obj);
bool BaselineCompiler::emitOutOfLinePostBarrierSlot() {
  masm.bind(&postBarrierSlot_);

  Register objReg = R2.scratchReg();
  AllocatableGeneralRegisterSet regs(GeneralRegisterSet::All());
  regs.take(R0);
  regs.take(objReg);
  regs.take(BaselineFrameReg);
  Register scratch = regs.takeAny();
#if defined(JS_CODEGEN_ARM) || defined(JS_CODEGEN_ARM64)
  // On ARM, save the link register before calling.  It contains the return
  // address.  The |masm.ret()| later will pop this into |pc| to return.
  masm.push(lr);
#elif defined(JS_CODEGEN_MIPS32) || defined(JS_CODEGEN_MIPS64)
  masm.push(ra);
#endif
  masm.pushValue(R0);

  masm.setupUnalignedABICall(scratch);
  masm.movePtr(ImmPtr(cx->runtime()), scratch);
  masm.passABIArg(scratch);
  masm.passABIArg(objReg);
  masm.callWithABI(JS_FUNC_TO_DATA_PTR(void*, PostWriteBarrier));

  masm.popValue(R0);
  masm.ret();
  return true;
}

template <typename Handler>
bool BaselineCodeGen<Handler>::emitNextIC() {
  // Emit a call to an IC stored in ICScript. Calls to this must match the
  // ICEntry order in ICScript: first the non-op IC entries for |this| and
  // formal arguments, then the for-op IC entries for JOF_IC ops.

  uint32_t pcOffset = script->pcToOffset(pc);

  // We don't use every ICEntry and we can skip unreachable ops, so we have
  // to loop until we find an ICEntry for the current pc.
  const ICEntry* entry;
  do {
    entry = &script->icScript()->icEntry(icEntryIndex_);
    icEntryIndex_++;
  } while (entry->pcOffset() < pcOffset);

  MOZ_RELEASE_ASSERT(entry->pcOffset() == pcOffset);
  MOZ_ASSERT_IF(entry->isForOp(), BytecodeOpHasIC(JSOp(*pc)));

  CodeOffset callOffset;
  EmitCallIC(masm, entry, &callOffset);

  RetAddrEntry::Kind kind =
      entry->isForOp() ? RetAddrEntry::Kind::IC : RetAddrEntry::Kind::NonOpIC;

  if (!retAddrEntries_.emplaceBack(script->pcToOffset(pc), kind, callOffset)) {
    ReportOutOfMemory(cx);
    return false;
  }

  return true;
}

template <typename Handler>
void BaselineCodeGen<Handler>::prepareVMCall() {
  pushedBeforeCall_ = masm.framePushed();
#ifdef DEBUG
  inCall_ = true;
#endif

  // Ensure everything is synced.
  frame.syncStack(0);

  // Save the frame pointer.
  masm.Push(BaselineFrameReg);
}

template <typename Handler>
bool BaselineCodeGen<Handler>::callVM(const VMFunction& fun,
                                      CallVMPhase phase) {
  TrampolinePtr code = cx->runtime()->jitRuntime()->getVMWrapper(fun);

#ifdef DEBUG
  // Assert prepareVMCall() has been called.
  MOZ_ASSERT(inCall_);
  inCall_ = false;

  // Assert the frame does not have an override pc when we're executing JIT
  // code.
  {
    Label ok;
    masm.branchTest32(Assembler::Zero, frame.addressOfFlags(),
                      Imm32(BaselineFrame::HAS_OVERRIDE_PC), &ok);
    masm.assumeUnreachable(
        "BaselineFrame shouldn't override pc when executing JIT code");
    masm.bind(&ok);
  }
#endif

  // Compute argument size. Note that this include the size of the frame pointer
  // pushed by prepareVMCall.
  uint32_t argSize = fun.explicitStackSlots() * sizeof(void*) + sizeof(void*);

  // Assert all arguments were pushed.
  MOZ_ASSERT(masm.framePushed() - pushedBeforeCall_ == argSize);

  Address frameSizeAddress(BaselineFrameReg,
                           BaselineFrame::reverseOffsetOfFrameSize());
  uint32_t frameVals = frame.nlocals() + frame.stackDepth();
  uint32_t frameBaseSize =
      BaselineFrame::FramePointerOffset + BaselineFrame::Size();
  uint32_t frameFullSize = frameBaseSize + (frameVals * sizeof(Value));
  if (phase == POST_INITIALIZE) {
    masm.store32(Imm32(frameFullSize), frameSizeAddress);
    uint32_t descriptor =
        MakeFrameDescriptor(frameFullSize + argSize, FrameType::BaselineJS,
                            ExitFrameLayout::Size());
    masm.push(Imm32(descriptor));
  } else {
    MOZ_ASSERT(phase == CHECK_OVER_RECURSED);
    Label afterWrite;
    Label writePostInitialize;

    // If OVER_RECURSED is set, then frame locals haven't been pushed yet.
    masm.branchTest32(Assembler::Zero, frame.addressOfFlags(),
                      Imm32(BaselineFrame::OVER_RECURSED),
                      &writePostInitialize);

    masm.move32(Imm32(frameBaseSize), ICTailCallReg);
    masm.jump(&afterWrite);

    masm.bind(&writePostInitialize);
    masm.move32(Imm32(frameFullSize), ICTailCallReg);

    masm.bind(&afterWrite);
    masm.store32(ICTailCallReg, frameSizeAddress);
    masm.add32(Imm32(argSize), ICTailCallReg);
    masm.makeFrameDescriptor(ICTailCallReg, FrameType::BaselineJS,
                             ExitFrameLayout::Size());
    masm.push(ICTailCallReg);
  }
  MOZ_ASSERT(fun.expectTailCall == NonTailCall);
  // Perform the call.
  masm.call(code);
  uint32_t callOffset = masm.currentOffset();
  masm.pop(BaselineFrameReg);

#ifdef DEBUG
  // Assert the frame does not have an override pc when we're executing JIT
  // code.
  {
    Label ok;
    masm.branchTest32(Assembler::Zero, frame.addressOfFlags(),
                      Imm32(BaselineFrame::HAS_OVERRIDE_PC), &ok);
    masm.assumeUnreachable("BaselineFrame shouldn't override pc after VM call");
    masm.bind(&ok);
  }
#endif

  return appendRetAddrEntry(RetAddrEntry::Kind::CallVM, callOffset);
}

typedef bool (*CheckOverRecursedBaselineFn)(JSContext*, BaselineFrame*);
static const VMFunction CheckOverRecursedBaselineInfo =
    FunctionInfo<CheckOverRecursedBaselineFn>(CheckOverRecursedBaseline,
                                              "CheckOverRecursedBaseline");

bool BaselineCompiler::emitStackCheck() {
  // If this is the late stack check for a frame which contains an early stack
  // check, then the early stack check might have failed and skipped past the
  // pushing of locals on the stack.
  //
  // If this is a possibility, then the OVER_RECURSED flag should be checked,
  // and the VMCall to CheckOverRecursedBaseline done unconditionally if it's
  // set.
  Label forceCall;
  if (needsEarlyStackCheck()) {
    masm.branchTest32(Assembler::NonZero, frame.addressOfFlags(),
                      Imm32(BaselineFrame::OVER_RECURSED), &forceCall);
  }

  Label skipCall;
  masm.branchStackPtrRhs(Assembler::BelowOrEqual,
                         AbsoluteAddress(cx->addressOfJitStackLimit()),
                         &skipCall);

  if (needsEarlyStackCheck()) {
    masm.bind(&forceCall);
  }

  prepareVMCall();
  masm.loadBaselineFramePtr(BaselineFrameReg, R1.scratchReg());
  pushArg(R1.scratchReg());

  CallVMPhase phase = POST_INITIALIZE;
  if (needsEarlyStackCheck()) {
    phase = CHECK_OVER_RECURSED;
  }

  if (!callVMNonOp(CheckOverRecursedBaselineInfo, phase)) {
    return false;
  }

  retAddrEntries_.back().setKind(RetAddrEntry::Kind::StackCheck);

  masm.bind(&skipCall);
  return true;
}

void BaselineCompiler::emitIsDebuggeeCheck() {
  if (compileDebugInstrumentation()) {
    masm.Push(BaselineFrameReg);
    masm.setupUnalignedABICall(R0.scratchReg());
    masm.loadBaselineFramePtr(BaselineFrameReg, R0.scratchReg());
    masm.passABIArg(R0.scratchReg());
    masm.callWithABI(JS_FUNC_TO_DATA_PTR(void*, jit::FrameIsDebuggeeCheck));
    masm.Pop(BaselineFrameReg);
  }
}

typedef bool (*DebugPrologueFn)(JSContext*, BaselineFrame*, jsbytecode*, bool*);
static const VMFunction DebugPrologueInfo =
    FunctionInfo<DebugPrologueFn>(jit::DebugPrologue, "DebugPrologue");

bool BaselineCompiler::emitDebugPrologue() {
  if (compileDebugInstrumentation()) {
    // Load pointer to BaselineFrame in R0.
    masm.loadBaselineFramePtr(BaselineFrameReg, R0.scratchReg());

    prepareVMCall();
    pushArg(ImmPtr(pc));
    pushArg(R0.scratchReg());
    if (!callVM(DebugPrologueInfo)) {
      return false;
    }

    // Fix up the RetAddrEntry appended by callVM for on-stack recompilation.
    retAddrEntries_.back().setKind(RetAddrEntry::Kind::DebugPrologue);

    // If the stub returns |true|, we have to return the value stored in the
    // frame's return value slot.
    Label done;
    masm.branchTest32(Assembler::Zero, ReturnReg, ReturnReg, &done);
    {
      masm.loadValue(frame.addressOfReturnValue(), JSReturnOperand);
      masm.jump(&return_);
    }
    masm.bind(&done);
  }

  debugOsrPrologueOffset_ = CodeOffset(masm.currentOffset());

  return true;
}

typedef bool (*CheckGlobalOrEvalDeclarationConflictsFn)(JSContext*,
                                                        HandleObject,
                                                        HandleScript);
static const VMFunction CheckGlobalOrEvalDeclarationConflictsInfo =
    FunctionInfo<CheckGlobalOrEvalDeclarationConflictsFn>(
        js::CheckGlobalOrEvalDeclarationConflicts,
        "CheckGlobalOrEvalDeclarationConflicts");

typedef bool (*InitFunctionEnvironmentObjectsFn)(JSContext*, BaselineFrame*);
static const VMFunction InitFunctionEnvironmentObjectsInfo =
    FunctionInfo<InitFunctionEnvironmentObjectsFn>(
        jit::InitFunctionEnvironmentObjects, "InitFunctionEnvironmentObjects");

bool BaselineCompiler::initEnvironmentChain() {
  CallVMPhase phase = POST_INITIALIZE;
  if (needsEarlyStackCheck()) {
    phase = CHECK_OVER_RECURSED;
  }

  RootedFunction fun(cx, function());
  if (fun) {
    // Use callee->environment as env chain. Note that we do this also
    // for needsSomeEnvironmentObject functions, so that the env chain
    // slot is properly initialized if the call triggers GC.
    Register callee = R0.scratchReg();
    Register scope = R1.scratchReg();
    masm.loadFunctionFromCalleeToken(frame.addressOfCalleeToken(), callee);
    masm.loadPtr(Address(callee, JSFunction::offsetOfEnvironment()), scope);
    masm.storePtr(scope, frame.addressOfEnvironmentChain());

    if (fun->needsFunctionEnvironmentObjects()) {
      // Call into the VM to create the proper environment objects.
      prepareVMCall();

      masm.loadBaselineFramePtr(BaselineFrameReg, R0.scratchReg());
      pushArg(R0.scratchReg());

      if (!callVMNonOp(InitFunctionEnvironmentObjectsInfo, phase)) {
        return false;
      }
    }
  } else if (module()) {
    // Modules use a pre-created scope object.
    Register scope = R1.scratchReg();
    masm.movePtr(ImmGCPtr(&module()->initialEnvironment()), scope);
    masm.storePtr(scope, frame.addressOfEnvironmentChain());
  } else {
    // EnvironmentChain pointer in BaselineFrame has already been initialized
    // in prologue, but we need to check for redeclaration errors.

    prepareVMCall();

    pushArg(ImmGCPtr(script));
    masm.loadPtr(frame.addressOfEnvironmentChain(), R0.scratchReg());
    pushArg(R0.scratchReg());

    if (!callVMNonOp(CheckGlobalOrEvalDeclarationConflictsInfo, phase)) {
      return false;
    }
  }

  return true;
}

typedef bool (*InterruptCheckFn)(JSContext*);
static const VMFunction InterruptCheckInfo =
    FunctionInfo<InterruptCheckFn>(InterruptCheck, "InterruptCheck");

template <typename Handler>
bool BaselineCodeGen<Handler>::emitInterruptCheck() {
  frame.syncStack(0);

  Label done;
  masm.branch32(Assembler::Equal, AbsoluteAddress(cx->addressOfInterruptBits()),
                Imm32(0), &done);

  prepareVMCall();
  if (!callVM(InterruptCheckInfo)) {
    return false;
  }

  masm.bind(&done);
  return true;
}

typedef bool (*IonCompileScriptForBaselineFn)(JSContext*, BaselineFrame*,
                                              jsbytecode*);
static const VMFunction IonCompileScriptForBaselineInfo =
    FunctionInfo<IonCompileScriptForBaselineFn>(IonCompileScriptForBaseline,
                                                "IonCompileScriptForBaseline");

template <typename Handler>
bool BaselineCodeGen<Handler>::emitWarmUpCounterIncrement(bool allowOsr) {
  // Emit no warm-up counter increments or bailouts if Ion is not
  // enabled, or if the script will never be Ion-compileable

  if (!ionCompileable_) {
    return true;
  }

  frame.assertSyncedStack();

  Register scriptReg = R2.scratchReg();
  Register countReg = R0.scratchReg();
  Address warmUpCounterAddr(scriptReg, JSScript::offsetOfWarmUpCounter());

  masm.movePtr(ImmGCPtr(script), scriptReg);
  masm.load32(warmUpCounterAddr, countReg);
  masm.add32(Imm32(1), countReg);
  masm.store32(countReg, warmUpCounterAddr);

  // If this is a loop inside a catch or finally block, increment the warmup
  // counter but don't attempt OSR (Ion only compiles the try block).
  if (analysis_.info(pc).loopEntryInCatchOrFinally) {
    MOZ_ASSERT(JSOp(*pc) == JSOP_LOOPENTRY);
    return true;
  }

  // OSR not possible at this loop entry.
  if (!allowOsr) {
    MOZ_ASSERT(JSOp(*pc) == JSOP_LOOPENTRY);
    return true;
  }

  Label skipCall;

  const OptimizationInfo* info =
      IonOptimizations.get(IonOptimizations.firstLevel());
  uint32_t warmUpThreshold = info->compilerWarmUpThreshold(script, pc);
  masm.branch32(Assembler::LessThan, countReg, Imm32(warmUpThreshold),
                &skipCall);

  masm.branchPtr(Assembler::Equal,
                 Address(scriptReg, JSScript::offsetOfIonScript()),
                 ImmPtr(ION_COMPILING_SCRIPT), &skipCall);

  // Try to compile and/or finish a compilation.
  if (JSOp(*pc) == JSOP_LOOPENTRY) {
    // During the loop entry we can try to OSR into ion.
    // The ic has logic for this.
    if (!emitNextIC()) {
      return false;
    }
  } else {
    // To call stubs we need to have an opcode. This code handles the
    // prologue and there is no dedicatd opcode present. Therefore use an
    // annotated vm call.
    prepareVMCall();

    masm.Push(ImmPtr(pc));
    masm.PushBaselineFramePtr(BaselineFrameReg, R0.scratchReg());

    if (!callVM(IonCompileScriptForBaselineInfo)) {
      return false;
    }

    // Annotate the RetAddrEntry as warmup counter.
    retAddrEntries_.back().setKind(RetAddrEntry::Kind::WarmupCounter);
  }
  masm.bind(&skipCall);

  return true;
}

bool BaselineCompiler::emitArgumentTypeChecks() {
  if (!function()) {
    return true;
  }

  frame.pushThis();
  frame.popRegsAndSync(1);

  if (!emitNextIC()) {
    return false;
  }

  for (size_t i = 0; i < function()->nargs(); i++) {
    frame.pushArg(i);
    frame.popRegsAndSync(1);

    if (!emitNextIC()) {
      return false;
    }
  }

  return true;
}

bool BaselineCompiler::emitDebugTrap() {
  MOZ_ASSERT(compileDebugInstrumentation());
  MOZ_ASSERT(frame.numUnsyncedSlots() == 0);

  bool enabled = script->stepModeEnabled() || script->hasBreakpointsAt(pc);

#if defined(JS_CODEGEN_ARM64)
  // Flush any pending constant pools to prevent incorrect
  // PCMappingEntry offsets. See Bug 1446819.
  masm.flush();
  // Fix up the PCMappingEntry to avoid any constant pool.
  pcMappingEntries_.back().nativeOffset = masm.currentOffset();
#endif

  // Emit patchable call to debug trap handler.
  JitCode* handler = cx->runtime()->jitRuntime()->debugTrapHandler(cx);
  if (!handler) {
    return false;
  }
  mozilla::DebugOnly<CodeOffset> offset = masm.toggledCall(handler, enabled);

#ifdef DEBUG
  // Patchable call offset has to match the pc mapping offset.
  PCMappingEntry& entry = pcMappingEntries_.back();
  MOZ_ASSERT((&offset)->offset() == entry.nativeOffset);
#endif

  // Add a RetAddrEntry for the return offset -> pc mapping.
  return appendRetAddrEntry(RetAddrEntry::Kind::DebugTrap,
                            masm.currentOffset());
}

#ifdef JS_TRACE_LOGGING
bool BaselineCompiler::emitTraceLoggerEnter() {
  AllocatableRegisterSet regs(RegisterSet::Volatile());
  Register loggerReg = regs.takeAnyGeneral();
  Register scriptReg = regs.takeAnyGeneral();

  Label noTraceLogger;
  if (!traceLoggerToggleOffsets_.append(masm.toggledJump(&noTraceLogger))) {
    return false;
  }

  masm.Push(loggerReg);
  masm.Push(scriptReg);

  masm.loadTraceLogger(loggerReg);

  // Script start.
  masm.movePtr(ImmGCPtr(script), scriptReg);
  masm.loadPtr(Address(scriptReg, JSScript::offsetOfBaselineScript()),
               scriptReg);
  Address scriptEvent(scriptReg,
                      BaselineScript::offsetOfTraceLoggerScriptEvent());
  masm.computeEffectiveAddress(scriptEvent, scriptReg);
  masm.tracelogStartEvent(loggerReg, scriptReg);

  // Engine start.
  masm.tracelogStartId(loggerReg, TraceLogger_Baseline, /* force = */ true);

  masm.Pop(scriptReg);
  masm.Pop(loggerReg);

  masm.bind(&noTraceLogger);

  return true;
}

bool BaselineCompiler::emitTraceLoggerExit() {
  AllocatableRegisterSet regs(RegisterSet::Volatile());
  Register loggerReg = regs.takeAnyGeneral();

  Label noTraceLogger;
  if (!traceLoggerToggleOffsets_.append(masm.toggledJump(&noTraceLogger))) {
    return false;
  }

  masm.Push(loggerReg);
  masm.loadTraceLogger(loggerReg);

  masm.tracelogStopId(loggerReg, TraceLogger_Baseline, /* force = */ true);
  masm.tracelogStopId(loggerReg, TraceLogger_Scripts, /* force = */ true);

  masm.Pop(loggerReg);

  masm.bind(&noTraceLogger);

  return true;
}

template <typename Handler>
bool BaselineCodeGen<Handler>::emitTraceLoggerResume(
    Register baselineScript, AllocatableGeneralRegisterSet& regs) {
  Register scriptId = regs.takeAny();
  Register loggerReg = regs.takeAny();

  Label noTraceLogger;
  if (!traceLoggerToggleOffsets_.append(masm.toggledJump(&noTraceLogger))) {
    return false;
  }

  masm.loadTraceLogger(loggerReg);

  Address scriptEvent(baselineScript,
                      BaselineScript::offsetOfTraceLoggerScriptEvent());
  masm.computeEffectiveAddress(scriptEvent, scriptId);
  masm.tracelogStartEvent(loggerReg, scriptId);
  masm.tracelogStartId(loggerReg, TraceLogger_Baseline, /* force = */ true);

  regs.add(loggerReg);
  regs.add(scriptId);

  masm.bind(&noTraceLogger);

  return true;
}
#endif

void BaselineCompiler::emitProfilerEnterFrame() {
  // Store stack position to lastProfilingFrame variable, guarded by a toggled
  // jump. Starts off initially disabled.
  Label noInstrument;
  CodeOffset toggleOffset = masm.toggledJump(&noInstrument);
  masm.profilerEnterFrame(masm.getStackPointer(), R0.scratchReg());
  masm.bind(&noInstrument);

  // Store the start offset in the appropriate location.
  MOZ_ASSERT(!profilerEnterFrameToggleOffset_.bound());
  profilerEnterFrameToggleOffset_ = toggleOffset;
}

void BaselineCompiler::emitProfilerExitFrame() {
  // Store previous frame to lastProfilingFrame variable, guarded by a toggled
  // jump. Starts off initially disabled.
  Label noInstrument;
  CodeOffset toggleOffset = masm.toggledJump(&noInstrument);
  masm.profilerExitFrame();
  masm.bind(&noInstrument);

  // Store the start offset in the appropriate location.
  MOZ_ASSERT(!profilerExitFrameToggleOffset_.bound());
  profilerExitFrameToggleOffset_ = toggleOffset;
}

MethodStatus BaselineCompiler::emitBody() {
  MOZ_ASSERT(pc == script->code());

  bool lastOpUnreachable = false;
  uint32_t emittedOps = 0;
  mozilla::DebugOnly<jsbytecode*> prevpc = pc;

  while (true) {
    JSOp op = JSOp(*pc);
    JitSpew(JitSpew_BaselineOp, "Compiling op @ %d: %s",
            int(script->pcToOffset(pc)), CodeName[op]);

    BytecodeInfo* info = analysis_.maybeInfo(pc);

    // Skip unreachable ops.
    if (!info) {
      // Test if last instructions and stop emitting in that case.
      pc += GetBytecodeLength(pc);
      if (pc >= script->codeEnd()) {
        break;
      }

      lastOpUnreachable = true;
      prevpc = pc;
      continue;
    }

    if (info->jumpTarget) {
      // Fully sync the stack if there are incoming jumps.
      frame.syncStack(0);
      frame.setStackDepth(info->stackDepth);
      masm.bind(handler.labelOf(pc));
    } else if (MOZ_UNLIKELY(compileDebugInstrumentation())) {
      // Also fully sync the stack if the debugger is enabled.
      frame.syncStack(0);
    } else {
      // At the beginning of any op, at most the top 2 stack-values are
      // unsynced.
      if (frame.stackDepth() > 2) {
        frame.syncStack(2);
      }
    }

    frame.assertValidState(*info);

    // Add a PC -> native mapping entry for the current op. These entries are
    // used when we need the native code address for a given pc, for instance
    // for bailouts from Ion, the debugger and exception handling. See
    // PCMappingIndexEntry for more information.
    bool addIndexEntry =
        (pc == script->code() || lastOpUnreachable || emittedOps > 100);
    if (addIndexEntry) {
      emittedOps = 0;
    }
    if (MOZ_UNLIKELY(!addPCMappingEntry(addIndexEntry))) {
      ReportOutOfMemory(cx);
      return Method_Error;
    }

    // Emit traps for breakpoints and step mode.
    if (MOZ_UNLIKELY(compileDebugInstrumentation()) && !emitDebugTrap()) {
      return Method_Error;
    }

    switch (op) {
      // ===== NOT Yet Implemented =====
      case JSOP_FORCEINTERPRETER:
        // Intentionally not implemented.
      case JSOP_SETINTRINSIC:
        // Run-once opcode during self-hosting initialization.
      case JSOP_UNUSED151:
      case JSOP_LIMIT:
        // === !! WARNING WARNING WARNING !! ===
        // Do you really want to sacrifice performance by not implementing
        // this operation in the BaselineCompiler?
        JitSpew(JitSpew_BaselineAbort, "Unhandled op: %s", CodeName[op]);
        return Method_CantCompile;

#define EMIT_OP(OP)                                            \
  case OP:                                                     \
    if (MOZ_UNLIKELY(!this->emit_##OP())) return Method_Error; \
    break;
        OPCODE_LIST(EMIT_OP)
#undef EMIT_OP
    }

    // If the main instruction is not a jump target, then we emit the
    //  corresponding code coverage counter.
    if (pc == script->main() && !BytecodeIsJumpTarget(op)) {
      if (!emit_JSOP_JUMPTARGET()) {
        return Method_Error;
      }
    }

    // Test if last instructions and stop emitting in that case.
    pc += GetBytecodeLength(pc);
    if (pc >= script->codeEnd()) {
      break;
    }

    emittedOps++;
    lastOpUnreachable = false;
#ifdef DEBUG
    prevpc = pc;
#endif
  }

  MOZ_ASSERT(JSOp(*prevpc) == JSOP_RETRVAL);
  return Method_Compiled;
}

template <typename Handler>
bool BaselineCodeGen<Handler>::emit_JSOP_NOP() {
  return true;
}

template <typename Handler>
bool BaselineCodeGen<Handler>::emit_JSOP_ITERNEXT() {
  return true;
}

template <typename Handler>
bool BaselineCodeGen<Handler>::emit_JSOP_NOP_DESTRUCTURING() {
  return true;
}

template <typename Handler>
bool BaselineCodeGen<Handler>::emit_JSOP_TRY_DESTRUCTURING() {
  return true;
}

template <typename Handler>
bool BaselineCodeGen<Handler>::emit_JSOP_LABEL() {
  return true;
}

template <typename Handler>
bool BaselineCodeGen<Handler>::emit_JSOP_POP() {
  frame.pop();
  return true;
}

template <typename Handler>
bool BaselineCodeGen<Handler>::emit_JSOP_POPN() {
  frame.popn(GET_UINT16(pc));
  return true;
}

template <typename Handler>
bool BaselineCodeGen<Handler>::emit_JSOP_DUPAT() {
  frame.syncStack(0);

  // DUPAT takes a value on the stack and re-pushes it on top.  It's like
  // GETLOCAL but it addresses from the top of the stack instead of from the
  // stack frame.

  int depth = -(GET_UINT24(pc) + 1);
  masm.loadValue(frame.addressOfStackValue(frame.peek(depth)), R0);
  frame.push(R0);
  return true;
}

template <typename Handler>
bool BaselineCodeGen<Handler>::emit_JSOP_DUP() {
  // Keep top stack value in R0, sync the rest so that we can use R1. We use
  // separate registers because every register can be used by at most one
  // StackValue.
  frame.popRegsAndSync(1);
  masm.moveValue(R0, R1);

  // inc/dec ops use DUP followed by ONE, ADD. Push R0 last to avoid a move.
  frame.push(R1);
  frame.push(R0);
  return true;
}

template <typename Handler>
bool BaselineCodeGen<Handler>::emit_JSOP_DUP2() {
  frame.syncStack(0);

  masm.loadValue(frame.addressOfStackValue(frame.peek(-2)), R0);
  masm.loadValue(frame.addressOfStackValue(frame.peek(-1)), R1);

  frame.push(R0);
  frame.push(R1);
  return true;
}

template <typename Handler>
bool BaselineCodeGen<Handler>::emit_JSOP_SWAP() {
  // Keep top stack values in R0 and R1.
  frame.popRegsAndSync(2);

  frame.push(R1);
  frame.push(R0);
  return true;
}

template <typename Handler>
bool BaselineCodeGen<Handler>::emit_JSOP_PICK() {
  frame.syncStack(0);

  // Pick takes a value on the stack and moves it to the top.
  // For instance, pick 2:
  //     before: A B C D E
  //     after : A B D E C

  // First, move value at -(amount + 1) into R0.
  int32_t depth = -(GET_INT8(pc) + 1);
  masm.loadValue(frame.addressOfStackValue(frame.peek(depth)), R0);

  // Move the other values down.
  depth++;
  for (; depth < 0; depth++) {
    Address source = frame.addressOfStackValue(frame.peek(depth));
    Address dest = frame.addressOfStackValue(frame.peek(depth - 1));
    masm.loadValue(source, R1);
    masm.storeValue(R1, dest);
  }

  // Push R0.
  frame.pop();
  frame.push(R0);
  return true;
}

template <typename Handler>
bool BaselineCodeGen<Handler>::emit_JSOP_UNPICK() {
  frame.syncStack(0);

  // Pick takes the top of the stack value and moves it under the nth value.
  // For instance, unpick 2:
  //     before: A B C D E
  //     after : A B E C D

  // First, move value at -1 into R0.
  masm.loadValue(frame.addressOfStackValue(frame.peek(-1)), R0);

  // Move the other values up.
  int32_t depth = -(GET_INT8(pc) + 1);
  for (int32_t i = -1; i > depth; i--) {
    Address source = frame.addressOfStackValue(frame.peek(i - 1));
    Address dest = frame.addressOfStackValue(frame.peek(i));
    masm.loadValue(source, R1);
    masm.storeValue(R1, dest);
  }

  // Store R0 under the nth value.
  Address dest = frame.addressOfStackValue(frame.peek(depth));
  masm.storeValue(R0, dest);
  return true;
}

template <>
void BaselineCompilerCodeGen::emitJump() {
  MOZ_ASSERT(IsJumpOpcode(JSOp(*pc)));
  frame.assertSyncedStack();

  jsbytecode* target = pc + GET_JUMP_OFFSET(pc);
  masm.jump(handler.labelOf(target));
}

template <>
void BaselineInterpreterCodeGen::emitJump() {
  // We have to add the current pc's jump offset to the frame's pc.
  MOZ_CRASH("NYI: interpreter emitJump");
}

template <>
void BaselineCompilerCodeGen::emitTestBooleanTruthy(bool branchIfTrue,
                                                    ValueOperand val) {
  MOZ_ASSERT(IsJumpOpcode(JSOp(*pc)));
  frame.assertSyncedStack();

  jsbytecode* target = pc + GET_JUMP_OFFSET(pc);
  masm.branchTestBooleanTruthy(branchIfTrue, val, handler.labelOf(target));
}

template <>
void BaselineInterpreterCodeGen::emitTestBooleanTruthy(bool branchIfTrue,
                                                       ValueOperand val) {
  Label done;
  masm.branchTestBooleanTruthy(!branchIfTrue, val, &done);
  emitJump();
  masm.bind(&done);
}

template <typename Handler>
bool BaselineCodeGen<Handler>::emit_JSOP_GOTO() {
  frame.syncStack(0);
  emitJump();
  return true;
}

template <typename Handler>
bool BaselineCodeGen<Handler>::emitToBoolean() {
  Label skipIC;
  masm.branchTestBoolean(Assembler::Equal, R0, &skipIC);

  // Call IC
  if (!emitNextIC()) {
    return false;
  }

  masm.bind(&skipIC);
  return true;
}

template <typename Handler>
bool BaselineCodeGen<Handler>::emitTest(bool branchIfTrue) {
  bool knownBoolean = frame.peek(-1)->isKnownBoolean();

  // Keep top stack value in R0.
  frame.popRegsAndSync(1);

  if (!knownBoolean && !emitToBoolean()) {
    return false;
  }

  // IC will leave a BooleanValue in R0, just need to branch on it.
  emitTestBooleanTruthy(branchIfTrue, R0);
  return true;
}

template <typename Handler>
bool BaselineCodeGen<Handler>::emit_JSOP_IFEQ() {
  return emitTest(false);
}

template <typename Handler>
bool BaselineCodeGen<Handler>::emit_JSOP_IFNE() {
  return emitTest(true);
}

template <typename Handler>
bool BaselineCodeGen<Handler>::emitAndOr(bool branchIfTrue) {
  bool knownBoolean = frame.peek(-1)->isKnownBoolean();

  // AND and OR leave the original value on the stack.
  frame.syncStack(0);

  masm.loadValue(frame.addressOfStackValue(frame.peek(-1)), R0);
  if (!knownBoolean && !emitToBoolean()) {
    return false;
  }

  emitTestBooleanTruthy(branchIfTrue, R0);
  return true;
}

template <typename Handler>
bool BaselineCodeGen<Handler>::emit_JSOP_AND() {
  return emitAndOr(false);
}

template <typename Handler>
bool BaselineCodeGen<Handler>::emit_JSOP_OR() {
  return emitAndOr(true);
}

template <typename Handler>
bool BaselineCodeGen<Handler>::emit_JSOP_NOT() {
  bool knownBoolean = frame.peek(-1)->isKnownBoolean();

  // Keep top stack value in R0.
  frame.popRegsAndSync(1);

  if (!knownBoolean && !emitToBoolean()) {
    return false;
  }

  masm.notBoolean(R0);

  frame.push(R0, JSVAL_TYPE_BOOLEAN);
  return true;
}

template <typename Handler>
bool BaselineCodeGen<Handler>::emit_JSOP_POS() {
  // Keep top stack value in R0.
  frame.popRegsAndSync(1);

  // Inline path for int32 and double.
  Label done;
  masm.branchTestNumber(Assembler::Equal, R0, &done);

  // Call IC.
  if (!emitNextIC()) {
    return false;
  }

  masm.bind(&done);
  frame.push(R0);
  return true;
}

template <typename Handler>
bool BaselineCodeGen<Handler>::emit_JSOP_LOOPHEAD() {
  if (!emit_JSOP_JUMPTARGET()) {
    return false;
  }
  return emitInterruptCheck();
}

template <typename Handler>
bool BaselineCodeGen<Handler>::emit_JSOP_LOOPENTRY() {
  if (!emit_JSOP_JUMPTARGET()) {
    return false;
  }
  frame.syncStack(0);
  if (!emitWarmUpCounterIncrement(LoopEntryCanIonOsr(pc))) {
    return false;
  }
  if (script->trackRecordReplayProgress()) {
    masm.inc64(
        AbsoluteAddress(mozilla::recordreplay::ExecutionProgressCounter()));
  }
  return true;
}

template <typename Handler>
bool BaselineCodeGen<Handler>::emit_JSOP_VOID() {
  frame.pop();
  frame.push(UndefinedValue());
  return true;
}

template <typename Handler>
bool BaselineCodeGen<Handler>::emit_JSOP_UNDEFINED() {
  // If this ever changes, change what JSOP_GIMPLICITTHIS does too.
  frame.push(UndefinedValue());
  return true;
}

template <typename Handler>
bool BaselineCodeGen<Handler>::emit_JSOP_HOLE() {
  frame.push(MagicValue(JS_ELEMENTS_HOLE));
  return true;
}

template <typename Handler>
bool BaselineCodeGen<Handler>::emit_JSOP_NULL() {
  frame.push(NullValue());
  return true;
}

typedef bool (*ThrowCheckIsObjectFn)(JSContext*, CheckIsObjectKind);
static const VMFunction ThrowCheckIsObjectInfo =
    FunctionInfo<ThrowCheckIsObjectFn>(ThrowCheckIsObject,
                                       "ThrowCheckIsObject");

template <typename Handler>
bool BaselineCodeGen<Handler>::emit_JSOP_CHECKISOBJ() {
  frame.syncStack(0);
  masm.loadValue(frame.addressOfStackValue(frame.peek(-1)), R0);

  Label ok;
  masm.branchTestObject(Assembler::Equal, R0, &ok);

  prepareVMCall();

  pushArg(Imm32(GET_UINT8(pc)));
  if (!callVM(ThrowCheckIsObjectInfo)) {
    return false;
  }

  masm.bind(&ok);
  return true;
}

typedef bool (*CheckIsCallableFn)(JSContext*, HandleValue, CheckIsCallableKind);
static const VMFunction CheckIsCallableInfo =
    FunctionInfo<CheckIsCallableFn>(CheckIsCallable, "CheckIsCallable");

template <typename Handler>
bool BaselineCodeGen<Handler>::emit_JSOP_CHECKISCALLABLE() {
  frame.syncStack(0);
  masm.loadValue(frame.addressOfStackValue(frame.peek(-1)), R0);

  prepareVMCall();

  pushArg(Imm32(GET_UINT8(pc)));
  pushArg(R0);
  if (!callVM(CheckIsCallableInfo)) {
    return false;
  }

  return true;
}

typedef bool (*ThrowUninitializedThisFn)(JSContext*, BaselineFrame* frame);
static const VMFunction ThrowUninitializedThisInfo =
    FunctionInfo<ThrowUninitializedThisFn>(BaselineThrowUninitializedThis,
                                           "BaselineThrowUninitializedThis");

typedef bool (*ThrowInitializedThisFn)(JSContext*);
static const VMFunction ThrowInitializedThisInfo =
    FunctionInfo<ThrowInitializedThisFn>(BaselineThrowInitializedThis,
                                         "BaselineThrowInitializedThis");

template <typename Handler>
bool BaselineCodeGen<Handler>::emit_JSOP_CHECKTHIS() {
  frame.syncStack(0);
  masm.loadValue(frame.addressOfStackValue(frame.peek(-1)), R0);

  return emitCheckThis(R0);
}

template <typename Handler>
bool BaselineCodeGen<Handler>::emit_JSOP_CHECKTHISREINIT() {
  frame.syncStack(0);
  masm.loadValue(frame.addressOfStackValue(frame.peek(-1)), R0);

  return emitCheckThis(R0, /* reinit = */ true);
}

template <typename Handler>
bool BaselineCodeGen<Handler>::emitCheckThis(ValueOperand val, bool reinit) {
  Label thisOK;
  if (reinit) {
    masm.branchTestMagic(Assembler::Equal, val, &thisOK);
  } else {
    masm.branchTestMagic(Assembler::NotEqual, val, &thisOK);
  }

  prepareVMCall();

  if (reinit) {
    if (!callVM(ThrowInitializedThisInfo)) {
      return false;
    }
  } else {
    masm.loadBaselineFramePtr(BaselineFrameReg, val.scratchReg());
    pushArg(val.scratchReg());

    if (!callVM(ThrowUninitializedThisInfo)) {
      return false;
    }
  }

  masm.bind(&thisOK);
  return true;
}

typedef bool (*ThrowBadDerivedReturnFn)(JSContext*, HandleValue);
static const VMFunction ThrowBadDerivedReturnInfo =
    FunctionInfo<ThrowBadDerivedReturnFn>(jit::ThrowBadDerivedReturn,
                                          "ThrowBadDerivedReturn");

template <typename Handler>
bool BaselineCodeGen<Handler>::emit_JSOP_CHECKRETURN() {
  MOZ_ASSERT(script->isDerivedClassConstructor());

  // Load |this| in R0, return value in R1.
  frame.popRegsAndSync(1);
  emitLoadReturnValue(R1);

  Label done, returnOK;
  masm.branchTestObject(Assembler::Equal, R1, &done);
  masm.branchTestUndefined(Assembler::Equal, R1, &returnOK);

  prepareVMCall();
  pushArg(R1);
  if (!callVM(ThrowBadDerivedReturnInfo)) {
    return false;
  }
  masm.assumeUnreachable("Should throw on bad derived constructor return");

  masm.bind(&returnOK);

  if (!emitCheckThis(R0)) {
    return false;
  }

  // Store |this| in the return value slot.
  masm.storeValue(R0, frame.addressOfReturnValue());
  masm.or32(Imm32(BaselineFrame::HAS_RVAL), frame.addressOfFlags());

  masm.bind(&done);
  return true;
}

typedef bool (*GetFunctionThisFn)(JSContext*, BaselineFrame*,
                                  MutableHandleValue);
static const VMFunction GetFunctionThisInfo = FunctionInfo<GetFunctionThisFn>(
    jit::BaselineGetFunctionThis, "BaselineGetFunctionThis");

template <typename Handler>
bool BaselineCodeGen<Handler>::emit_JSOP_FUNCTIONTHIS() {
  MOZ_ASSERT(function());
  MOZ_ASSERT(!function()->isArrow());

  frame.pushThis();

  // In strict mode code or self-hosted functions, |this| is left alone.
  if (script->strict() || (function() && function()->isSelfHostedBuiltin())) {
    return true;
  }

  // Load |thisv| in R0. Skip the call if it's already an object.
  Label skipCall;
  frame.popRegsAndSync(1);
  masm.branchTestObject(Assembler::Equal, R0, &skipCall);

  prepareVMCall();
  masm.loadBaselineFramePtr(BaselineFrameReg, R1.scratchReg());

  pushArg(R1.scratchReg());

  if (!callVM(GetFunctionThisInfo)) {
    return false;
  }

  masm.bind(&skipCall);
  frame.push(R0);
  return true;
}

typedef void (*GetNonSyntacticGlobalThisFn)(JSContext*, HandleObject,
                                            MutableHandleValue);
static const VMFunction GetNonSyntacticGlobalThisInfo =
    FunctionInfo<GetNonSyntacticGlobalThisFn>(js::GetNonSyntacticGlobalThis,
                                              "GetNonSyntacticGlobalThis");

template <typename Handler>
bool BaselineCodeGen<Handler>::emit_JSOP_GLOBALTHIS() {
  frame.syncStack(0);

  if (!script->hasNonSyntacticScope()) {
    LexicalEnvironmentObject* globalLexical =
        &script->global().lexicalEnvironment();
    masm.moveValue(globalLexical->thisValue(), R0);
    frame.push(R0);
    return true;
  }

  prepareVMCall();

  masm.loadPtr(frame.addressOfEnvironmentChain(), R0.scratchReg());
  pushArg(R0.scratchReg());

  if (!callVM(GetNonSyntacticGlobalThisInfo)) {
    return false;
  }

  frame.push(R0);
  return true;
}

template <typename Handler>
bool BaselineCodeGen<Handler>::emit_JSOP_TRUE() {
  frame.push(BooleanValue(true));
  return true;
}

template <typename Handler>
bool BaselineCodeGen<Handler>::emit_JSOP_FALSE() {
  frame.push(BooleanValue(false));
  return true;
}

template <typename Handler>
bool BaselineCodeGen<Handler>::emit_JSOP_ZERO() {
  frame.push(Int32Value(0));
  return true;
}

template <typename Handler>
bool BaselineCodeGen<Handler>::emit_JSOP_ONE() {
  frame.push(Int32Value(1));
  return true;
}

template <typename Handler>
bool BaselineCodeGen<Handler>::emit_JSOP_INT8() {
  frame.push(Int32Value(GET_INT8(pc)));
  return true;
}

template <typename Handler>
bool BaselineCodeGen<Handler>::emit_JSOP_INT32() {
  frame.push(Int32Value(GET_INT32(pc)));
  return true;
}

template <typename Handler>
bool BaselineCodeGen<Handler>::emit_JSOP_UINT16() {
  frame.push(Int32Value(GET_UINT16(pc)));
  return true;
}

template <typename Handler>
bool BaselineCodeGen<Handler>::emit_JSOP_UINT24() {
  frame.push(Int32Value(GET_UINT24(pc)));
  return true;
}

template <typename Handler>
bool BaselineCodeGen<Handler>::emit_JSOP_RESUMEINDEX() {
  return emit_JSOP_UINT24();
}

template <typename Handler>
bool BaselineCodeGen<Handler>::emit_JSOP_DOUBLE() {
  frame.push(script->getConst(GET_UINT32_INDEX(pc)));
  return true;
}

#ifdef ENABLE_BIGINT
template <typename Handler>
bool BaselineCodeGen<Handler>::emit_JSOP_BIGINT() {
  frame.push(script->getConst(GET_UINT32_INDEX(pc)));
  return true;
}
#endif

template <typename Handler>
bool BaselineCodeGen<Handler>::emit_JSOP_STRING() {
  frame.push(StringValue(script->getAtom(pc)));
  return true;
}

template <typename Handler>
bool BaselineCodeGen<Handler>::emit_JSOP_SYMBOL() {
  unsigned which = GET_UINT8(pc);
  JS::Symbol* sym = cx->runtime()->wellKnownSymbols->get(which);
  frame.push(SymbolValue(sym));
  return true;
}

typedef JSObject* (*DeepCloneObjectLiteralFn)(JSContext*, HandleObject,
                                              NewObjectKind);
static const VMFunction DeepCloneObjectLiteralInfo =
    FunctionInfo<DeepCloneObjectLiteralFn>(DeepCloneObjectLiteral,
                                           "DeepCloneObjectLiteral");

template <typename Handler>
bool BaselineCodeGen<Handler>::emit_JSOP_OBJECT() {
  if (cx->realm()->creationOptions().cloneSingletons()) {
    RootedObject obj(cx, script->getObject(GET_UINT32_INDEX(pc)));
    if (!obj) {
      return false;
    }

    prepareVMCall();

    pushArg(ImmWord(TenuredObject));
    pushArg(ImmGCPtr(obj));

    if (!callVM(DeepCloneObjectLiteralInfo)) {
      return false;
    }

    // Box and push return value.
    masm.tagValue(JSVAL_TYPE_OBJECT, ReturnReg, R0);
    frame.push(R0);
    return true;
  }

  cx->realm()->behaviors().setSingletonsAsValues();
  frame.push(ObjectValue(*script->getObject(pc)));
  return true;
}

template <typename Handler>
bool BaselineCodeGen<Handler>::emit_JSOP_CALLSITEOBJ() {
  RootedObject cso(cx, script->getObject(pc));
  RootedObject raw(cx, script->getObject(GET_UINT32_INDEX(pc) + 1));
  if (!cso || !raw) {
    return false;
  }

  if (!ProcessCallSiteObjOperation(cx, cso, raw)) {
    return false;
  }

  frame.push(ObjectValue(*cso));
  return true;
}

typedef JSObject* (*CloneRegExpObjectFn)(JSContext*, Handle<RegExpObject*>);
static const VMFunction CloneRegExpObjectInfo =
    FunctionInfo<CloneRegExpObjectFn>(CloneRegExpObject, "CloneRegExpObject");

template <typename Handler>
bool BaselineCodeGen<Handler>::emit_JSOP_REGEXP() {
  RootedObject reObj(cx, script->getRegExp(pc));

  prepareVMCall();
  pushArg(ImmGCPtr(reObj));
  if (!callVM(CloneRegExpObjectInfo)) {
    return false;
  }

  // Box and push return value.
  masm.tagValue(JSVAL_TYPE_OBJECT, ReturnReg, R0);
  frame.push(R0);
  return true;
}

typedef JSObject* (*LambdaFn)(JSContext*, HandleFunction, HandleObject);
static const VMFunction LambdaInfo =
    FunctionInfo<LambdaFn>(js::Lambda, "Lambda");

template <typename Handler>
bool BaselineCodeGen<Handler>::emit_JSOP_LAMBDA() {
  RootedFunction fun(cx, script->getFunction(GET_UINT32_INDEX(pc)));

  prepareVMCall();
  masm.loadPtr(frame.addressOfEnvironmentChain(), R0.scratchReg());

  pushArg(R0.scratchReg());
  pushArg(ImmGCPtr(fun));

  if (!callVM(LambdaInfo)) {
    return false;
  }

  // Box and push return value.
  masm.tagValue(JSVAL_TYPE_OBJECT, ReturnReg, R0);
  frame.push(R0);
  return true;
}

typedef JSObject* (*LambdaArrowFn)(JSContext*, HandleFunction, HandleObject,
                                   HandleValue);
static const VMFunction LambdaArrowInfo =
    FunctionInfo<LambdaArrowFn>(js::LambdaArrow, "LambdaArrow");

template <typename Handler>
bool BaselineCodeGen<Handler>::emit_JSOP_LAMBDA_ARROW() {
  // Keep pushed newTarget in R0.
  frame.popRegsAndSync(1);

  RootedFunction fun(cx, script->getFunction(GET_UINT32_INDEX(pc)));

  prepareVMCall();
  masm.loadPtr(frame.addressOfEnvironmentChain(), R2.scratchReg());

  pushArg(R0);
  pushArg(R2.scratchReg());
  pushArg(ImmGCPtr(fun));

  if (!callVM(LambdaArrowInfo)) {
    return false;
  }

  // Box and push return value.
  masm.tagValue(JSVAL_TYPE_OBJECT, ReturnReg, R0);
  frame.push(R0);
  return true;
}

typedef bool (*SetFunNameFn)(JSContext*, HandleFunction, HandleValue,
                             FunctionPrefixKind);
static const VMFunction SetFunNameInfo =
    FunctionInfo<SetFunNameFn>(js::SetFunctionNameIfNoOwnName, "SetFunName");

template <typename Handler>
bool BaselineCodeGen<Handler>::emit_JSOP_SETFUNNAME() {
  frame.popRegsAndSync(2);

  frame.push(R0);
  frame.syncStack(0);

  FunctionPrefixKind prefixKind = FunctionPrefixKind(GET_UINT8(pc));
  masm.unboxObject(R0, R0.scratchReg());

  prepareVMCall();

  pushArg(Imm32(int32_t(prefixKind)));
  pushArg(R1);
  pushArg(R0.scratchReg());
  return callVM(SetFunNameInfo);
}

template <typename Handler>
void BaselineCodeGen<Handler>::storeValue(const StackValue* source,
                                          const Address& dest,
                                          const ValueOperand& scratch) {
  switch (source->kind()) {
    case StackValue::Constant:
      masm.storeValue(source->constant(), dest);
      break;
    case StackValue::Register:
      masm.storeValue(source->reg(), dest);
      break;
    case StackValue::LocalSlot:
      masm.loadValue(frame.addressOfLocal(source->localSlot()), scratch);
      masm.storeValue(scratch, dest);
      break;
    case StackValue::ArgSlot:
      masm.loadValue(frame.addressOfArg(source->argSlot()), scratch);
      masm.storeValue(scratch, dest);
      break;
    case StackValue::ThisSlot:
      masm.loadValue(frame.addressOfThis(), scratch);
      masm.storeValue(scratch, dest);
      break;
    case StackValue::EvalNewTargetSlot:
      MOZ_ASSERT(script->isForEval());
      masm.loadValue(frame.addressOfEvalNewTarget(), scratch);
      masm.storeValue(scratch, dest);
      break;
    case StackValue::Stack:
      masm.loadValue(frame.addressOfStackValue(source), scratch);
      masm.storeValue(scratch, dest);
      break;
    default:
      MOZ_CRASH("Invalid kind");
  }
}

template <typename Handler>
bool BaselineCodeGen<Handler>::emit_JSOP_BITOR() {
  return emitBinaryArith();
}

template <typename Handler>
bool BaselineCodeGen<Handler>::emit_JSOP_BITXOR() {
  return emitBinaryArith();
}

template <typename Handler>
bool BaselineCodeGen<Handler>::emit_JSOP_BITAND() {
  return emitBinaryArith();
}

template <typename Handler>
bool BaselineCodeGen<Handler>::emit_JSOP_LSH() {
  return emitBinaryArith();
}

template <typename Handler>
bool BaselineCodeGen<Handler>::emit_JSOP_RSH() {
  return emitBinaryArith();
}

template <typename Handler>
bool BaselineCodeGen<Handler>::emit_JSOP_URSH() {
  return emitBinaryArith();
}

template <typename Handler>
bool BaselineCodeGen<Handler>::emit_JSOP_ADD() {
  return emitBinaryArith();
}

template <typename Handler>
bool BaselineCodeGen<Handler>::emit_JSOP_SUB() {
  return emitBinaryArith();
}

template <typename Handler>
bool BaselineCodeGen<Handler>::emit_JSOP_MUL() {
  return emitBinaryArith();
}

template <typename Handler>
bool BaselineCodeGen<Handler>::emit_JSOP_DIV() {
  return emitBinaryArith();
}

template <typename Handler>
bool BaselineCodeGen<Handler>::emit_JSOP_MOD() {
  return emitBinaryArith();
}

template <typename Handler>
bool BaselineCodeGen<Handler>::emit_JSOP_POW() {
  return emitBinaryArith();
}

template <typename Handler>
bool BaselineCodeGen<Handler>::emitBinaryArith() {
  // Keep top JSStack value in R0 and R2
  frame.popRegsAndSync(2);

  // Call IC
  if (!emitNextIC()) {
    return false;
  }

  // Mark R0 as pushed stack value.
  frame.push(R0);
  return true;
}

template <typename Handler>
bool BaselineCodeGen<Handler>::emitUnaryArith() {
  // Keep top stack value in R0.
  frame.popRegsAndSync(1);

  // Call IC
  if (!emitNextIC()) {
    return false;
  }

  // Mark R0 as pushed stack value.
  frame.push(R0);
  return true;
}

template <typename Handler>
bool BaselineCodeGen<Handler>::emit_JSOP_BITNOT() {
  return emitUnaryArith();
}

template <typename Handler>
bool BaselineCodeGen<Handler>::emit_JSOP_NEG() {
  return emitUnaryArith();
}

template <typename Handler>
bool BaselineCodeGen<Handler>::emit_JSOP_LT() {
  return emitCompare();
}

template <typename Handler>
bool BaselineCodeGen<Handler>::emit_JSOP_LE() {
  return emitCompare();
}

template <typename Handler>
bool BaselineCodeGen<Handler>::emit_JSOP_GT() {
  return emitCompare();
}

template <typename Handler>
bool BaselineCodeGen<Handler>::emit_JSOP_GE() {
  return emitCompare();
}

template <typename Handler>
bool BaselineCodeGen<Handler>::emit_JSOP_EQ() {
  return emitCompare();
}

template <typename Handler>
bool BaselineCodeGen<Handler>::emit_JSOP_NE() {
  return emitCompare();
}

template <typename Handler>
bool BaselineCodeGen<Handler>::emitCompare() {
  // CODEGEN

  // Keep top JSStack value in R0 and R1.
  frame.popRegsAndSync(2);

  // Call IC.
  if (!emitNextIC()) {
    return false;
  }

  // Mark R0 as pushed stack value.
  frame.push(R0, JSVAL_TYPE_BOOLEAN);
  return true;
}

template <typename Handler>
bool BaselineCodeGen<Handler>::emit_JSOP_STRICTEQ() {
  return emitCompare();
}

template <typename Handler>
bool BaselineCodeGen<Handler>::emit_JSOP_STRICTNE() {
  return emitCompare();
}

template <typename Handler>
bool BaselineCodeGen<Handler>::emit_JSOP_CONDSWITCH() {
  return true;
}

template <typename Handler>
bool BaselineCodeGen<Handler>::emit_JSOP_CASE() {
  frame.popRegsAndSync(1);

  Label done;
  masm.branchTestBooleanTruthy(/* branchIfTrue */ false, R0, &done);
  {
    // Pop the switch value if the case matches.
    masm.addToStackPtr(Imm32(sizeof(Value)));
    emitJump();
  }
  masm.bind(&done);
  return true;
}

template <typename Handler>
bool BaselineCodeGen<Handler>::emit_JSOP_DEFAULT() {
  frame.pop();
  return emit_JSOP_GOTO();
}

template <typename Handler>
bool BaselineCodeGen<Handler>::emit_JSOP_LINENO() {
  return true;
}

template <typename Handler>
bool BaselineCodeGen<Handler>::emit_JSOP_NEWARRAY() {
  frame.syncStack(0);

  uint32_t length = GET_UINT32(pc);
  MOZ_ASSERT(length <= INT32_MAX,
             "the bytecode emitter must fail to compile code that would "
             "produce JSOP_NEWARRAY with a length exceeding int32_t range");

  // Pass length in R0.
  masm.move32(Imm32(AssertedCast<int32_t>(length)), R0.scratchReg());

  if (!emitNextIC()) {
    return false;
  }

  frame.push(R0);
  return true;
}

typedef ArrayObject* (*NewArrayCopyOnWriteFn)(JSContext*, HandleArrayObject,
                                              gc::InitialHeap);
const VMFunction NewArrayCopyOnWriteInfo = FunctionInfo<NewArrayCopyOnWriteFn>(
    js::NewDenseCopyOnWriteArray, "NewDenseCopyOnWriteArray");

template <typename Handler>
bool BaselineCodeGen<Handler>::emit_JSOP_NEWARRAY_COPYONWRITE() {
  RootedScript scriptRoot(cx, script);
  JSObject* obj = ObjectGroup::getOrFixupCopyOnWriteObject(cx, scriptRoot, pc);
  if (!obj) {
    return false;
  }

  prepareVMCall();

  pushArg(Imm32(gc::DefaultHeap));
  pushArg(ImmGCPtr(obj));

  if (!callVM(NewArrayCopyOnWriteInfo)) {
    return false;
  }

  // Box and push return value.
  masm.tagValue(JSVAL_TYPE_OBJECT, ReturnReg, R0);
  frame.push(R0);
  return true;
}

template <typename Handler>
bool BaselineCodeGen<Handler>::emit_JSOP_INITELEM_ARRAY() {
  // Keep the object and rhs on the stack.
  frame.syncStack(0);

  // Load object in R0, index in R1.
  masm.loadValue(frame.addressOfStackValue(frame.peek(-2)), R0);
  uint32_t index = GET_UINT32(pc);
  MOZ_ASSERT(index <= INT32_MAX,
             "the bytecode emitter must fail to compile code that would "
             "produce JSOP_INITELEM_ARRAY with a length exceeding "
             "int32_t range");
  masm.moveValue(Int32Value(AssertedCast<int32_t>(index)), R1);

  // Call IC.
  if (!emitNextIC()) {
    return false;
  }

  // Pop the rhs, so that the object is on the top of the stack.
  frame.pop();
  return true;
}

template <typename Handler>
bool BaselineCodeGen<Handler>::emit_JSOP_NEWOBJECT() {
  frame.syncStack(0);

  if (!emitNextIC()) {
    return false;
  }

  frame.push(R0);
  return true;
}

template <typename Handler>
bool BaselineCodeGen<Handler>::emit_JSOP_NEWINIT() {
  frame.syncStack(0);

  if (!emitNextIC()) {
    return false;
  }

  frame.push(R0);
  return true;
}

template <typename Handler>
bool BaselineCodeGen<Handler>::emit_JSOP_INITELEM() {
  // Store RHS in the scratch slot.
  storeValue(frame.peek(-1), frame.addressOfScratchValue(), R2);
  frame.pop();

  // Keep object and index in R0 and R1.
  frame.popRegsAndSync(2);

  // Push the object to store the result of the IC.
  frame.push(R0);
  frame.syncStack(0);

  // Keep RHS on the stack.
  frame.pushScratchValue();

  // Call IC.
  if (!emitNextIC()) {
    return false;
  }

  // Pop the rhs, so that the object is on the top of the stack.
  frame.pop();
  return true;
}

template <typename Handler>
bool BaselineCodeGen<Handler>::emit_JSOP_INITHIDDENELEM() {
  return emit_JSOP_INITELEM();
}

typedef bool (*MutateProtoFn)(JSContext* cx, HandlePlainObject obj,
                              HandleValue newProto);
static const VMFunction MutateProtoInfo =
    FunctionInfo<MutateProtoFn>(MutatePrototype, "MutatePrototype");

template <typename Handler>
bool BaselineCodeGen<Handler>::emit_JSOP_MUTATEPROTO() {
  // Keep values on the stack for the decompiler.
  frame.syncStack(0);

  masm.unboxObject(frame.addressOfStackValue(frame.peek(-2)), R0.scratchReg());
  masm.loadValue(frame.addressOfStackValue(frame.peek(-1)), R1);

  prepareVMCall();

  pushArg(R1);
  pushArg(R0.scratchReg());

  if (!callVM(MutateProtoInfo)) {
    return false;
  }

  frame.pop();
  return true;
}

template <typename Handler>
bool BaselineCodeGen<Handler>::emit_JSOP_INITPROP() {
  // Load lhs in R0, rhs in R1.
  frame.syncStack(0);
  masm.loadValue(frame.addressOfStackValue(frame.peek(-2)), R0);
  masm.loadValue(frame.addressOfStackValue(frame.peek(-1)), R1);

  // Call IC.
  if (!emitNextIC()) {
    return false;
  }

  // Leave the object on the stack.
  frame.pop();
  return true;
}

template <typename Handler>
bool BaselineCodeGen<Handler>::emit_JSOP_INITLOCKEDPROP() {
  return emit_JSOP_INITPROP();
}

template <typename Handler>
bool BaselineCodeGen<Handler>::emit_JSOP_INITHIDDENPROP() {
  return emit_JSOP_INITPROP();
}

template <typename Handler>
bool BaselineCodeGen<Handler>::emit_JSOP_GETELEM() {
  // Keep top two stack values in R0 and R1.
  frame.popRegsAndSync(2);

  // Call IC.
  if (!emitNextIC()) {
    return false;
  }

  // Mark R0 as pushed stack value.
  frame.push(R0);
  return true;
}

template <typename Handler>
bool BaselineCodeGen<Handler>::emit_JSOP_GETELEM_SUPER() {
  // Store obj in the scratch slot.
  storeValue(frame.peek(-1), frame.addressOfScratchValue(), R2);
  frame.pop();

  // Keep receiver and index in R0 and R1.
  frame.popRegsAndSync(2);

  // Keep obj on the stack.
  frame.pushScratchValue();

  if (!emitNextIC()) {
    return false;
  }

  frame.pop();  // This value is also popped in InitFromBailout.
  frame.push(R0);
  return true;
}

template <typename Handler>
bool BaselineCodeGen<Handler>::emit_JSOP_CALLELEM() {
  return emit_JSOP_GETELEM();
}

template <typename Handler>
bool BaselineCodeGen<Handler>::emit_JSOP_SETELEM() {
  // Store RHS in the scratch slot.
  storeValue(frame.peek(-1), frame.addressOfScratchValue(), R2);
  frame.pop();

  // Keep object and index in R0 and R1.
  frame.popRegsAndSync(2);

  // Keep RHS on the stack.
  frame.pushScratchValue();

  // Call IC.
  if (!emitNextIC()) {
    return false;
  }

  return true;
}

template <typename Handler>
bool BaselineCodeGen<Handler>::emit_JSOP_STRICTSETELEM() {
  return emit_JSOP_SETELEM();
}

template <typename Handler>
bool BaselineCodeGen<Handler>::emitSetElemSuper(bool strict) {
  // Incoming stack is |receiver, propval, obj, rval|. We need to shuffle
  // stack to leave rval when operation is complete.

  // Pop rval into R0, then load receiver into R1 and replace with rval.
  frame.popRegsAndSync(1);
  masm.loadValue(frame.addressOfStackValue(frame.peek(-3)), R1);
  masm.storeValue(R0, frame.addressOfStackValue(frame.peek(-3)));

  prepareVMCall();

  pushArg(Imm32(strict));
  pushArg(R1);  // receiver
  pushArg(R0);  // rval
  masm.loadValue(frame.addressOfStackValue(frame.peek(-2)), R0);
  pushArg(R0);  // propval
  masm.unboxObject(frame.addressOfStackValue(frame.peek(-1)), R0.scratchReg());
  pushArg(R0.scratchReg());  // obj

  if (!callVM(SetObjectElementInfo)) {
    return false;
  }

  frame.popn(2);
  return true;
}

template <typename Handler>
bool BaselineCodeGen<Handler>::emit_JSOP_SETELEM_SUPER() {
  return emitSetElemSuper(/* strict = */ false);
}

template <typename Handler>
bool BaselineCodeGen<Handler>::emit_JSOP_STRICTSETELEM_SUPER() {
  return emitSetElemSuper(/* strict = */ true);
}

typedef bool (*DeleteElementFn)(JSContext*, HandleValue, HandleValue, bool*);
static const VMFunction DeleteElementStrictInfo = FunctionInfo<DeleteElementFn>(
    DeleteElementJit<true>, "DeleteElementStrict");
static const VMFunction DeleteElementNonStrictInfo =
    FunctionInfo<DeleteElementFn>(DeleteElementJit<false>,
                                  "DeleteElementNonStrict");

template <typename Handler>
bool BaselineCodeGen<Handler>::emitDelElem(bool strict) {
  // Keep values on the stack for the decompiler.
  frame.syncStack(0);
  masm.loadValue(frame.addressOfStackValue(frame.peek(-2)), R0);
  masm.loadValue(frame.addressOfStackValue(frame.peek(-1)), R1);

  prepareVMCall();

  pushArg(R1);
  pushArg(R0);
  if (!callVM(strict ? DeleteElementStrictInfo : DeleteElementNonStrictInfo)) {
    return false;
  }

  masm.boxNonDouble(JSVAL_TYPE_BOOLEAN, ReturnReg, R1);
  frame.popn(2);
  frame.push(R1);
  return true;
}

template <typename Handler>
bool BaselineCodeGen<Handler>::emit_JSOP_DELELEM() {
  return emitDelElem(/* strict = */ false);
}

template <typename Handler>
bool BaselineCodeGen<Handler>::emit_JSOP_STRICTDELELEM() {
  return emitDelElem(/* strict = */ true);
}

template <typename Handler>
bool BaselineCodeGen<Handler>::emit_JSOP_IN() {
  frame.popRegsAndSync(2);

  if (!emitNextIC()) {
    return false;
  }

  frame.push(R0);
  return true;
}

template <typename Handler>
bool BaselineCodeGen<Handler>::emit_JSOP_HASOWN() {
  frame.popRegsAndSync(2);

  if (!emitNextIC()) {
    return false;
  }

  frame.push(R0);
  return true;
}

template <typename Handler>
bool BaselineCodeGen<Handler>::emit_JSOP_GETGNAME() {
  if (script->hasNonSyntacticScope()) {
    return emit_JSOP_GETNAME();
  }

  RootedPropertyName name(cx, script->getName(pc));

  // These names are non-configurable on the global and cannot be shadowed.
  if (name == cx->names().undefined) {
    frame.push(UndefinedValue());
    return true;
  }
  if (name == cx->names().NaN) {
    frame.push(cx->runtime()->NaNValue);
    return true;
  }
  if (name == cx->names().Infinity) {
    frame.push(cx->runtime()->positiveInfinityValue);
    return true;
  }

  frame.syncStack(0);

  masm.movePtr(ImmGCPtr(&script->global().lexicalEnvironment()),
               R0.scratchReg());

  // Call IC.
  if (!emitNextIC()) {
    return false;
  }

  // Mark R0 as pushed stack value.
  frame.push(R0);
  return true;
}

template <typename Handler>
bool BaselineCodeGen<Handler>::emit_JSOP_BINDGNAME() {
  if (!script->hasNonSyntacticScope()) {
    // We can bind name to the global lexical scope if the binding already
    // exists, is initialized, and is writable (i.e., an initialized
    // 'let') at compile time.
    RootedPropertyName name(cx, script->getName(pc));
    Rooted<LexicalEnvironmentObject*> env(
        cx, &script->global().lexicalEnvironment());
    if (Shape* shape = env->lookup(cx, name)) {
      if (shape->writable() &&
          !env->getSlot(shape->slot()).isMagic(JS_UNINITIALIZED_LEXICAL)) {
        frame.push(ObjectValue(*env));
        return true;
      }
    } else if (Shape* shape = script->global().lookup(cx, name)) {
      // If the property does not currently exist on the global lexical
      // scope, we can bind name to the global object if the property
      // exists on the global and is non-configurable, as then it cannot
      // be shadowed.
      if (!shape->configurable()) {
        frame.push(ObjectValue(script->global()));
        return true;
      }
    }

    // Otherwise we have to use the environment chain.
  }

  return emitBindName(JSOP_BINDGNAME);
}

typedef JSObject* (*BindVarFn)(JSContext*, JSObject*);
static const VMFunction BindVarInfo =
    FunctionInfo<BindVarFn>(BindVarOperation, "BindVarOperation");

template <typename Handler>
bool BaselineCodeGen<Handler>::emit_JSOP_BINDVAR() {
  frame.syncStack(0);
  masm.loadPtr(frame.addressOfEnvironmentChain(), R0.scratchReg());

  prepareVMCall();
  pushArg(R0.scratchReg());

  if (!callVM(BindVarInfo)) {
    return false;
  }

  masm.tagValue(JSVAL_TYPE_OBJECT, ReturnReg, R0);
  frame.push(R0);
  return true;
}

template <typename Handler>
bool BaselineCodeGen<Handler>::emit_JSOP_SETPROP() {
  // Keep lhs in R0, rhs in R1.
  frame.popRegsAndSync(2);

  // Keep RHS on the stack.
  frame.push(R1);
  frame.syncStack(0);

  // Call IC.
  if (!emitNextIC()) {
    return false;
  }

  return true;
}

template <typename Handler>
bool BaselineCodeGen<Handler>::emit_JSOP_STRICTSETPROP() {
  return emit_JSOP_SETPROP();
}

template <typename Handler>
bool BaselineCodeGen<Handler>::emit_JSOP_SETNAME() {
  return emit_JSOP_SETPROP();
}

template <typename Handler>
bool BaselineCodeGen<Handler>::emit_JSOP_STRICTSETNAME() {
  return emit_JSOP_SETPROP();
}

template <typename Handler>
bool BaselineCodeGen<Handler>::emit_JSOP_SETGNAME() {
  return emit_JSOP_SETPROP();
}

template <typename Handler>
bool BaselineCodeGen<Handler>::emit_JSOP_STRICTSETGNAME() {
  return emit_JSOP_SETPROP();
}

typedef bool (*SetPropertySuperFn)(JSContext*, HandleObject, HandleValue,
                                   HandlePropertyName, HandleValue, bool);
static const VMFunction SetPropertySuperInfo =
    FunctionInfo<SetPropertySuperFn>(js::SetPropertySuper, "SetPropertySuper");

template <typename Handler>
bool BaselineCodeGen<Handler>::emitSetPropSuper(bool strict) {
  // Incoming stack is |receiver, obj, rval|. We need to shuffle stack to
  // leave rval when operation is complete.

  // Pop rval into R0, then load receiver into R1 and replace with rval.
  frame.popRegsAndSync(1);
  masm.loadValue(frame.addressOfStackValue(frame.peek(-2)), R1);
  masm.storeValue(R0, frame.addressOfStackValue(frame.peek(-2)));

  prepareVMCall();

  pushArg(Imm32(strict));
  pushArg(R0);  // rval
  pushArg(ImmGCPtr(script->getName(pc)));
  pushArg(R1);  // receiver
  masm.unboxObject(frame.addressOfStackValue(frame.peek(-1)), R0.scratchReg());
  pushArg(R0.scratchReg());  // obj

  if (!callVM(SetPropertySuperInfo)) {
    return false;
  }

  frame.pop();
  return true;
}

template <typename Handler>
bool BaselineCodeGen<Handler>::emit_JSOP_SETPROP_SUPER() {
  return emitSetPropSuper(/* strict = */ false);
}

template <typename Handler>
bool BaselineCodeGen<Handler>::emit_JSOP_STRICTSETPROP_SUPER() {
  return emitSetPropSuper(/* strict = */ true);
}

template <typename Handler>
bool BaselineCodeGen<Handler>::emit_JSOP_GETPROP() {
  // Keep object in R0.
  frame.popRegsAndSync(1);

  // Call IC.
  if (!emitNextIC()) {
    return false;
  }

  // Mark R0 as pushed stack value.
  frame.push(R0);
  return true;
}

template <typename Handler>
bool BaselineCodeGen<Handler>::emit_JSOP_CALLPROP() {
  return emit_JSOP_GETPROP();
}

template <typename Handler>
bool BaselineCodeGen<Handler>::emit_JSOP_LENGTH() {
  return emit_JSOP_GETPROP();
}

template <typename Handler>
bool BaselineCodeGen<Handler>::emit_JSOP_GETBOUNDNAME() {
  return emit_JSOP_GETPROP();
}

template <typename Handler>
bool BaselineCodeGen<Handler>::emit_JSOP_GETPROP_SUPER() {
  // Receiver -> R1, Object -> R0
  frame.popRegsAndSync(1);
  masm.loadValue(frame.addressOfStackValue(frame.peek(-1)), R1);
  frame.pop();

  if (!emitNextIC()) {
    return false;
  }

  frame.push(R0);
  return true;
}

typedef bool (*DeletePropertyFn)(JSContext*, HandleValue, HandlePropertyName,
                                 bool*);
static const VMFunction DeletePropertyStrictInfo =
    FunctionInfo<DeletePropertyFn>(DeletePropertyJit<true>,
                                   "DeletePropertyStrict");
static const VMFunction DeletePropertyNonStrictInfo =
    FunctionInfo<DeletePropertyFn>(DeletePropertyJit<false>,
                                   "DeletePropertyNonStrict");

template <typename Handler>
bool BaselineCodeGen<Handler>::emitDelProp(bool strict) {
  // Keep value on the stack for the decompiler.
  frame.syncStack(0);
  masm.loadValue(frame.addressOfStackValue(frame.peek(-1)), R0);

  prepareVMCall();

  pushArg(ImmGCPtr(script->getName(pc)));
  pushArg(R0);

  if (!callVM(strict ? DeletePropertyStrictInfo
                     : DeletePropertyNonStrictInfo)) {
    return false;
  }

  masm.boxNonDouble(JSVAL_TYPE_BOOLEAN, ReturnReg, R1);
  frame.pop();
  frame.push(R1);
  return true;
}

template <typename Handler>
bool BaselineCodeGen<Handler>::emit_JSOP_DELPROP() {
  return emitDelProp(/* strict = */ false);
}

template <typename Handler>
bool BaselineCodeGen<Handler>::emit_JSOP_STRICTDELPROP() {
  return emitDelProp(/* strict = */ true);
}

template <typename Handler>
void BaselineCodeGen<Handler>::getEnvironmentCoordinateObject(Register reg) {
  EnvironmentCoordinate ec(pc);

  masm.loadPtr(frame.addressOfEnvironmentChain(), reg);
  for (unsigned i = ec.hops(); i; i--) {
    masm.unboxObject(
        Address(reg, EnvironmentObject::offsetOfEnclosingEnvironment()), reg);
  }
}

template <typename Handler>
Address BaselineCodeGen<Handler>::getEnvironmentCoordinateAddressFromObject(
    Register objReg, Register reg) {
  EnvironmentCoordinate ec(pc);
  Shape* shape = EnvironmentCoordinateToEnvironmentShape(script, pc);

  if (shape->numFixedSlots() <= ec.slot()) {
    masm.loadPtr(Address(objReg, NativeObject::offsetOfSlots()), reg);
    return Address(reg, (ec.slot() - shape->numFixedSlots()) * sizeof(Value));
  }

  return Address(objReg, NativeObject::getFixedSlotOffset(ec.slot()));
}

template <typename Handler>
Address BaselineCodeGen<Handler>::getEnvironmentCoordinateAddress(
    Register reg) {
  getEnvironmentCoordinateObject(reg);
  return getEnvironmentCoordinateAddressFromObject(reg, reg);
}

template <typename Handler>
bool BaselineCodeGen<Handler>::emit_JSOP_GETALIASEDVAR() {
  frame.syncStack(0);

  Address address = getEnvironmentCoordinateAddress(R0.scratchReg());
  masm.loadValue(address, R0);

  if (ionCompileable_) {
    // No need to monitor types if we know Ion can't compile this script.
    if (!emitNextIC()) {
      return false;
    }
  }

  frame.push(R0);
  return true;
}

template <typename Handler>
bool BaselineCodeGen<Handler>::emit_JSOP_SETALIASEDVAR() {
  JSScript* outerScript = EnvironmentCoordinateFunctionScript(script, pc);
  if (outerScript && outerScript->treatAsRunOnce()) {
    // Type updates for this operation might need to be tracked, so treat
    // this as a SETPROP.

    // Load rhs into R1.
    frame.syncStack(0);
    masm.loadValue(frame.addressOfStackValue(frame.peek(-1)), R1);

    // Load and box lhs into R0.
    getEnvironmentCoordinateObject(R2.scratchReg());
    masm.tagValue(JSVAL_TYPE_OBJECT, R2.scratchReg(), R0);

    // Call SETPROP IC.
    if (!emitNextIC()) {
      return false;
    }

    return true;
  }

  // Keep rvalue in R0.
  frame.popRegsAndSync(1);
  Register objReg = R2.scratchReg();

  getEnvironmentCoordinateObject(objReg);
  Address address =
      getEnvironmentCoordinateAddressFromObject(objReg, R1.scratchReg());
  masm.guardedCallPreBarrier(address, MIRType::Value);
  masm.storeValue(R0, address);
  frame.push(R0);

  // Only R0 is live at this point.
  // Scope coordinate object is already in R2.scratchReg().
  Register temp = R1.scratchReg();

  Label skipBarrier;
  masm.branchPtrInNurseryChunk(Assembler::Equal, objReg, temp, &skipBarrier);
  masm.branchValueIsNurseryCell(Assembler::NotEqual, R0, temp, &skipBarrier);

  masm.call(&postBarrierSlot_);  // Won't clobber R0

  masm.bind(&skipBarrier);
  return true;
}

template <typename Handler>
bool BaselineCodeGen<Handler>::emit_JSOP_GETNAME() {
  frame.syncStack(0);

  masm.loadPtr(frame.addressOfEnvironmentChain(), R0.scratchReg());

  // Call IC.
  if (!emitNextIC()) {
    return false;
  }

  // Mark R0 as pushed stack value.
  frame.push(R0);
  return true;
}

template <typename Handler>
bool BaselineCodeGen<Handler>::emitBindName(JSOp op) {
  MOZ_ASSERT(op == JSOP_BINDNAME || op == JSOP_BINDGNAME);

  frame.syncStack(0);

  if (op == JSOP_BINDGNAME && !script->hasNonSyntacticScope()) {
    masm.movePtr(ImmGCPtr(&script->global().lexicalEnvironment()),
                 R0.scratchReg());
  } else {
    masm.loadPtr(frame.addressOfEnvironmentChain(), R0.scratchReg());
  }

  // Call IC.
  if (!emitNextIC()) {
    return false;
  }

  // Mark R0 as pushed stack value.
  frame.push(R0);
  return true;
}

template <typename Handler>
bool BaselineCodeGen<Handler>::emit_JSOP_BINDNAME() {
  return emitBindName(JSOP_BINDNAME);
}

typedef bool (*DeleteNameFn)(JSContext*, HandlePropertyName, HandleObject,
                             MutableHandleValue);
static const VMFunction DeleteNameInfo =
    FunctionInfo<DeleteNameFn>(DeleteNameOperation, "DeleteNameOperation");

template <typename Handler>
bool BaselineCodeGen<Handler>::emit_JSOP_DELNAME() {
  frame.syncStack(0);
  masm.loadPtr(frame.addressOfEnvironmentChain(), R0.scratchReg());

  prepareVMCall();

  pushArg(R0.scratchReg());
  pushArg(ImmGCPtr(script->getName(pc)));

  if (!callVM(DeleteNameInfo)) {
    return false;
  }

  frame.push(R0);
  return true;
}

template <typename Handler>
bool BaselineCodeGen<Handler>::emit_JSOP_GETIMPORT() {
  ModuleEnvironmentObject* env = GetModuleEnvironmentForScript(script);
  MOZ_ASSERT(env);

  ModuleEnvironmentObject* targetEnv;
  Shape* shape;
  MOZ_ALWAYS_TRUE(
      env->lookupImport(NameToId(script->getName(pc)), &targetEnv, &shape));

  EnsureTrackPropertyTypes(cx, targetEnv, shape->propid());

  frame.syncStack(0);

  uint32_t slot = shape->slot();
  Register scratch = R0.scratchReg();
  masm.movePtr(ImmGCPtr(targetEnv), scratch);
  if (slot < targetEnv->numFixedSlots()) {
    masm.loadValue(Address(scratch, NativeObject::getFixedSlotOffset(slot)),
                   R0);
  } else {
    masm.loadPtr(Address(scratch, NativeObject::offsetOfSlots()), scratch);
    masm.loadValue(
        Address(scratch, (slot - targetEnv->numFixedSlots()) * sizeof(Value)),
        R0);
  }

  // Imports are initialized by this point except in rare circumstances, so
  // don't emit a check unless we have to.
  if (targetEnv->getSlot(shape->slot()).isMagic(JS_UNINITIALIZED_LEXICAL)) {
    if (!emitUninitializedLexicalCheck(R0)) {
      return false;
    }
  }

  if (ionCompileable_) {
    // No need to monitor types if we know Ion can't compile this script.
    if (!emitNextIC()) {
      return false;
    }
  }

  frame.push(R0);
  return true;
}

template <typename Handler>
bool BaselineCodeGen<Handler>::emit_JSOP_GETINTRINSIC() {
  frame.syncStack(0);

  if (!emitNextIC()) {
    return false;
  }

  frame.push(R0);
  return true;
}

typedef bool (*DefVarFn)(JSContext*, HandleObject, HandleScript, jsbytecode*);
static const VMFunction DefVarInfo =
    FunctionInfo<DefVarFn>(DefVarOperation, "DefVarOperation");

template <typename Handler>
bool BaselineCodeGen<Handler>::emit_JSOP_DEFVAR() {
  frame.syncStack(0);

  masm.loadPtr(frame.addressOfEnvironmentChain(), R0.scratchReg());

  prepareVMCall();

  pushArg(ImmPtr(pc));
  pushArg(ImmGCPtr(script));
  pushArg(R0.scratchReg());

  return callVM(DefVarInfo);
}

typedef bool (*DefLexicalFn)(JSContext*, HandleObject, HandleScript,
                             jsbytecode*);
static const VMFunction DefLexicalInfo =
    FunctionInfo<DefLexicalFn>(DefLexicalOperation, "DefLexicalOperation");

template <typename Handler>
bool BaselineCodeGen<Handler>::emitDefLexical(JSOp op) {
  MOZ_ASSERT(op == JSOP_DEFCONST || op == JSOP_DEFLET);

  frame.syncStack(0);

  masm.loadPtr(frame.addressOfEnvironmentChain(), R0.scratchReg());

  prepareVMCall();

  pushArg(ImmPtr(pc));
  pushArg(ImmGCPtr(script));
  pushArg(R0.scratchReg());

  return callVM(DefLexicalInfo);
}

template <typename Handler>
bool BaselineCodeGen<Handler>::emit_JSOP_DEFCONST() {
  return emitDefLexical(JSOP_DEFCONST);
}

template <typename Handler>
bool BaselineCodeGen<Handler>::emit_JSOP_DEFLET() {
  return emitDefLexical(JSOP_DEFLET);
}

typedef bool (*DefFunOperationFn)(JSContext*, HandleScript, HandleObject,
                                  HandleFunction);
static const VMFunction DefFunOperationInfo =
    FunctionInfo<DefFunOperationFn>(DefFunOperation, "DefFunOperation");

template <typename Handler>
bool BaselineCodeGen<Handler>::emit_JSOP_DEFFUN() {
  frame.popRegsAndSync(1);
  masm.unboxObject(R0, R0.scratchReg());
  masm.loadPtr(frame.addressOfEnvironmentChain(), R1.scratchReg());

  prepareVMCall();

  pushArg(R0.scratchReg());
  pushArg(R1.scratchReg());
  pushArg(ImmGCPtr(script));

  return callVM(DefFunOperationInfo);
}

typedef bool (*InitPropGetterSetterFn)(JSContext*, jsbytecode*, HandleObject,
                                       HandlePropertyName, HandleObject);
static const VMFunction InitPropGetterSetterInfo =
    FunctionInfo<InitPropGetterSetterFn>(InitGetterSetterOperation,
                                         "InitPropGetterSetterOperation");

template <typename Handler>
bool BaselineCodeGen<Handler>::emitInitPropGetterSetter() {
  // Keep values on the stack for the decompiler.
  frame.syncStack(0);

  prepareVMCall();

  masm.unboxObject(frame.addressOfStackValue(frame.peek(-1)), R0.scratchReg());
  masm.unboxObject(frame.addressOfStackValue(frame.peek(-2)), R1.scratchReg());

  pushArg(R0.scratchReg());
  pushArg(ImmGCPtr(script->getName(pc)));
  pushArg(R1.scratchReg());
  pushArg(ImmPtr(pc));

  if (!callVM(InitPropGetterSetterInfo)) {
    return false;
  }

  frame.pop();
  return true;
}

template <typename Handler>
bool BaselineCodeGen<Handler>::emit_JSOP_INITPROP_GETTER() {
  return emitInitPropGetterSetter();
}

template <typename Handler>
bool BaselineCodeGen<Handler>::emit_JSOP_INITHIDDENPROP_GETTER() {
  return emitInitPropGetterSetter();
}

template <typename Handler>
bool BaselineCodeGen<Handler>::emit_JSOP_INITPROP_SETTER() {
  return emitInitPropGetterSetter();
}

template <typename Handler>
bool BaselineCodeGen<Handler>::emit_JSOP_INITHIDDENPROP_SETTER() {
  return emitInitPropGetterSetter();
}

typedef bool (*InitElemGetterSetterFn)(JSContext*, jsbytecode*, HandleObject,
                                       HandleValue, HandleObject);
static const VMFunction InitElemGetterSetterInfo =
    FunctionInfo<InitElemGetterSetterFn>(InitGetterSetterOperation,
                                         "InitElemGetterSetterOperation");

template <typename Handler>
bool BaselineCodeGen<Handler>::emitInitElemGetterSetter() {
  // Load index and value in R0 and R1, but keep values on the stack for the
  // decompiler.
  frame.syncStack(0);
  masm.loadValue(frame.addressOfStackValue(frame.peek(-2)), R0);
  masm.unboxObject(frame.addressOfStackValue(frame.peek(-1)), R1.scratchReg());

  prepareVMCall();

  pushArg(R1.scratchReg());
  pushArg(R0);
  masm.unboxObject(frame.addressOfStackValue(frame.peek(-3)), R0.scratchReg());
  pushArg(R0.scratchReg());
  pushArg(ImmPtr(pc));

  if (!callVM(InitElemGetterSetterInfo)) {
    return false;
  }

  frame.popn(2);
  return true;
}

template <typename Handler>
bool BaselineCodeGen<Handler>::emit_JSOP_INITELEM_GETTER() {
  return emitInitElemGetterSetter();
}

template <typename Handler>
bool BaselineCodeGen<Handler>::emit_JSOP_INITHIDDENELEM_GETTER() {
  return emitInitElemGetterSetter();
}

template <typename Handler>
bool BaselineCodeGen<Handler>::emit_JSOP_INITELEM_SETTER() {
  return emitInitElemGetterSetter();
}

template <typename Handler>
bool BaselineCodeGen<Handler>::emit_JSOP_INITHIDDENELEM_SETTER() {
  return emitInitElemGetterSetter();
}

template <typename Handler>
bool BaselineCodeGen<Handler>::emit_JSOP_INITELEM_INC() {
  // Keep the object and rhs on the stack.
  frame.syncStack(0);

  // Load object in R0, index in R1.
  masm.loadValue(frame.addressOfStackValue(frame.peek(-3)), R0);
  masm.loadValue(frame.addressOfStackValue(frame.peek(-2)), R1);

  // Call IC.
  if (!emitNextIC()) {
    return false;
  }

  // Pop the rhs
  frame.pop();

  // Increment index
  Address indexAddr = frame.addressOfStackValue(frame.peek(-1));
#ifdef DEBUG
  Label isInt32;
  masm.branchTestInt32(Assembler::Equal, indexAddr, &isInt32);
  masm.assumeUnreachable("INITELEM_INC index must be Int32");
  masm.bind(&isInt32);
#endif
  masm.incrementInt32Value(indexAddr);
  return true;
}

template <typename Handler>
bool BaselineCodeGen<Handler>::emit_JSOP_GETLOCAL() {
  frame.pushLocal(GET_LOCALNO(pc));
  return true;
}

template <typename Handler>
bool BaselineCodeGen<Handler>::emit_JSOP_SETLOCAL() {
  // Ensure no other StackValue refers to the old value, for instance i + (i =
  // 3). This also allows us to use R0 as scratch below.
  frame.syncStack(1);

  uint32_t local = GET_LOCALNO(pc);
  storeValue(frame.peek(-1), frame.addressOfLocal(local), R0);
  return true;
}

template <typename Handler>
bool BaselineCodeGen<Handler>::emitFormalArgAccess(uint32_t arg, bool get) {
  // Fast path: the script does not use |arguments| or formals don't
  // alias the arguments object.
  if (!script->argumentsAliasesFormals()) {
    if (get) {
      frame.pushArg(arg);
    } else {
      // See the comment in emit_JSOP_SETLOCAL.
      frame.syncStack(1);
      storeValue(frame.peek(-1), frame.addressOfArg(arg), R0);
    }

    return true;
  }

  // Sync so that we can use R0.
  frame.syncStack(0);

  // If the script is known to have an arguments object, we can just use it.
  // Else, we *may* have an arguments object (because we can't invalidate
  // when needsArgsObj becomes |true|), so we have to test HAS_ARGS_OBJ.
  Label done;
  if (!script->needsArgsObj()) {
    Label hasArgsObj;
    masm.branchTest32(Assembler::NonZero, frame.addressOfFlags(),
                      Imm32(BaselineFrame::HAS_ARGS_OBJ), &hasArgsObj);
    if (get) {
      masm.loadValue(frame.addressOfArg(arg), R0);
    } else {
      storeValue(frame.peek(-1), frame.addressOfArg(arg), R0);
    }
    masm.jump(&done);
    masm.bind(&hasArgsObj);
  }

  // Load the arguments object data vector.
  Register reg = R2.scratchReg();
  masm.loadPtr(
      Address(BaselineFrameReg, BaselineFrame::reverseOffsetOfArgsObj()), reg);
  masm.loadPrivate(Address(reg, ArgumentsObject::getDataSlotOffset()), reg);

  // Load/store the argument.
  Address argAddr(reg, ArgumentsData::offsetOfArgs() + arg * sizeof(Value));
  if (get) {
    masm.loadValue(argAddr, R0);
    frame.push(R0);
  } else {
    masm.guardedCallPreBarrier(argAddr, MIRType::Value);
    masm.loadValue(frame.addressOfStackValue(frame.peek(-1)), R0);
    masm.storeValue(R0, argAddr);

    MOZ_ASSERT(frame.numUnsyncedSlots() == 0);

    Register temp = R1.scratchReg();

    // Reload the arguments object
    Register reg = R2.scratchReg();
    masm.loadPtr(
        Address(BaselineFrameReg, BaselineFrame::reverseOffsetOfArgsObj()),
        reg);

    Label skipBarrier;

    masm.branchPtrInNurseryChunk(Assembler::Equal, reg, temp, &skipBarrier);
    masm.branchValueIsNurseryCell(Assembler::NotEqual, R0, temp, &skipBarrier);

    masm.call(&postBarrierSlot_);

    masm.bind(&skipBarrier);
  }

  masm.bind(&done);
  return true;
}

template <typename Handler>
bool BaselineCodeGen<Handler>::emit_JSOP_GETARG() {
  uint32_t arg = GET_ARGNO(pc);
  return emitFormalArgAccess(arg, /* get = */ true);
}

template <typename Handler>
bool BaselineCodeGen<Handler>::emit_JSOP_SETARG() {
  // Ionmonkey can't inline functions with SETARG with magic arguments.
  if (!script->argsObjAliasesFormals() && script->argumentsAliasesFormals()) {
    script->setUninlineable();
  }

  modifiesArguments_ = true;

  uint32_t arg = GET_ARGNO(pc);
  return emitFormalArgAccess(arg, /* get = */ false);
}

template <typename Handler>
bool BaselineCodeGen<Handler>::emit_JSOP_NEWTARGET() {
  if (script->isForEval()) {
    frame.pushEvalNewTarget();
    return true;
  }

  MOZ_ASSERT(function());
  frame.syncStack(0);

  if (function()->isArrow()) {
    // Arrow functions store their |new.target| value in an
    // extended slot.
    Register scratch = R0.scratchReg();
    masm.loadFunctionFromCalleeToken(frame.addressOfCalleeToken(), scratch);
    masm.loadValue(
        Address(scratch, FunctionExtended::offsetOfArrowNewTargetSlot()), R0);
    frame.push(R0);
    return true;
  }

  // if (isConstructing()) push(argv[Max(numActualArgs, numFormalArgs)])
  Label notConstructing, done;
  masm.branchTestPtr(Assembler::Zero, frame.addressOfCalleeToken(),
                     Imm32(CalleeToken_FunctionConstructing), &notConstructing);

  Register argvLen = R0.scratchReg();

  Address actualArgs(BaselineFrameReg, BaselineFrame::offsetOfNumActualArgs());
  masm.loadPtr(actualArgs, argvLen);

  Label useNFormals;

  masm.branchPtr(Assembler::Below, argvLen, Imm32(function()->nargs()),
                 &useNFormals);

  {
    BaseValueIndex newTarget(BaselineFrameReg, argvLen,
                             BaselineFrame::offsetOfArg(0));
    masm.loadValue(newTarget, R0);
    masm.jump(&done);
  }

  masm.bind(&useNFormals);

  {
    Address newTarget(
        BaselineFrameReg,
        BaselineFrame::offsetOfArg(0) + (function()->nargs() * sizeof(Value)));
    masm.loadValue(newTarget, R0);
    masm.jump(&done);
  }

  // else push(undefined)
  masm.bind(&notConstructing);
  masm.moveValue(UndefinedValue(), R0);

  masm.bind(&done);
  frame.push(R0);

  return true;
}

typedef bool (*ThrowRuntimeLexicalErrorFn)(JSContext* cx, unsigned);
static const VMFunction ThrowRuntimeLexicalErrorInfo =
    FunctionInfo<ThrowRuntimeLexicalErrorFn>(jit::ThrowRuntimeLexicalError,
                                             "ThrowRuntimeLexicalError");

template <typename Handler>
bool BaselineCodeGen<Handler>::emitThrowConstAssignment() {
  prepareVMCall();
  pushArg(Imm32(JSMSG_BAD_CONST_ASSIGN));
  return callVM(ThrowRuntimeLexicalErrorInfo);
}

template <typename Handler>
bool BaselineCodeGen<Handler>::emit_JSOP_THROWSETCONST() {
  return emitThrowConstAssignment();
}

template <typename Handler>
bool BaselineCodeGen<Handler>::emit_JSOP_THROWSETALIASEDCONST() {
  return emitThrowConstAssignment();
}

template <typename Handler>
bool BaselineCodeGen<Handler>::emit_JSOP_THROWSETCALLEE() {
  return emitThrowConstAssignment();
}

template <typename Handler>
bool BaselineCodeGen<Handler>::emitUninitializedLexicalCheck(
    const ValueOperand& val) {
  Label done;
  masm.branchTestMagicValue(Assembler::NotEqual, val, JS_UNINITIALIZED_LEXICAL,
                            &done);

  prepareVMCall();
  pushArg(Imm32(JSMSG_UNINITIALIZED_LEXICAL));
  if (!callVM(ThrowRuntimeLexicalErrorInfo)) {
    return false;
  }

  masm.bind(&done);
  return true;
}

template <typename Handler>
bool BaselineCodeGen<Handler>::emit_JSOP_CHECKLEXICAL() {
  frame.syncStack(0);
  masm.loadValue(frame.addressOfLocal(GET_LOCALNO(pc)), R0);
  return emitUninitializedLexicalCheck(R0);
}

template <typename Handler>
bool BaselineCodeGen<Handler>::emit_JSOP_INITLEXICAL() {
  return emit_JSOP_SETLOCAL();
}

template <typename Handler>
bool BaselineCodeGen<Handler>::emit_JSOP_INITGLEXICAL() {
  frame.popRegsAndSync(1);
  frame.push(ObjectValue(script->global().lexicalEnvironment()));
  frame.push(R0);
  return emit_JSOP_SETPROP();
}

template <typename Handler>
bool BaselineCodeGen<Handler>::emit_JSOP_CHECKALIASEDLEXICAL() {
  frame.syncStack(0);
  masm.loadValue(getEnvironmentCoordinateAddress(R0.scratchReg()), R0);
  return emitUninitializedLexicalCheck(R0);
}

template <typename Handler>
bool BaselineCodeGen<Handler>::emit_JSOP_INITALIASEDLEXICAL() {
  return emit_JSOP_SETALIASEDVAR();
}

template <typename Handler>
bool BaselineCodeGen<Handler>::emit_JSOP_UNINITIALIZED() {
  frame.push(MagicValue(JS_UNINITIALIZED_LEXICAL));
  return true;
}

template <typename Handler>
bool BaselineCodeGen<Handler>::emitCall(JSOp op) {
  MOZ_ASSERT(IsCallOp(op));

  frame.syncStack(0);

  uint32_t argc = GET_ARGC(pc);
  masm.move32(Imm32(argc), R0.scratchReg());

  // Call IC
  if (!emitNextIC()) {
    return false;
  }

  // Update FrameInfo.
  bool construct = op == JSOP_NEW || op == JSOP_SUPERCALL;
  frame.popn(2 + argc + construct);
  frame.push(R0);
  return true;
}

template <typename Handler>
bool BaselineCodeGen<Handler>::emitSpreadCall(JSOp op) {
  MOZ_ASSERT(IsCallOp(op));

  frame.syncStack(0);
  masm.move32(Imm32(1), R0.scratchReg());

  // Call IC
  if (!emitNextIC()) {
    return false;
  }

  // Update FrameInfo.
  bool construct = op == JSOP_SPREADNEW || op == JSOP_SPREADSUPERCALL;
  frame.popn(3 + construct);
  frame.push(R0);
  return true;
}

template <typename Handler>
bool BaselineCodeGen<Handler>::emit_JSOP_CALL() {
  return emitCall(JSOP_CALL);
}

template <typename Handler>
bool BaselineCodeGen<Handler>::emit_JSOP_CALL_IGNORES_RV() {
  return emitCall(JSOP_CALL_IGNORES_RV);
}

template <typename Handler>
bool BaselineCodeGen<Handler>::emit_JSOP_CALLITER() {
  return emitCall(JSOP_CALLITER);
}

template <typename Handler>
bool BaselineCodeGen<Handler>::emit_JSOP_NEW() {
  return emitCall(JSOP_NEW);
}

template <typename Handler>
bool BaselineCodeGen<Handler>::emit_JSOP_SUPERCALL() {
  return emitCall(JSOP_SUPERCALL);
}

template <typename Handler>
bool BaselineCodeGen<Handler>::emit_JSOP_FUNCALL() {
  return emitCall(JSOP_FUNCALL);
}

template <typename Handler>
bool BaselineCodeGen<Handler>::emit_JSOP_FUNAPPLY() {
  return emitCall(JSOP_FUNAPPLY);
}

template <typename Handler>
bool BaselineCodeGen<Handler>::emit_JSOP_EVAL() {
  return emitCall(JSOP_EVAL);
}

template <typename Handler>
bool BaselineCodeGen<Handler>::emit_JSOP_STRICTEVAL() {
  return emitCall(JSOP_STRICTEVAL);
}

template <typename Handler>
bool BaselineCodeGen<Handler>::emit_JSOP_SPREADCALL() {
  return emitSpreadCall(JSOP_SPREADCALL);
}

template <typename Handler>
bool BaselineCodeGen<Handler>::emit_JSOP_SPREADNEW() {
  return emitSpreadCall(JSOP_SPREADNEW);
}

template <typename Handler>
bool BaselineCodeGen<Handler>::emit_JSOP_SPREADSUPERCALL() {
  return emitSpreadCall(JSOP_SPREADSUPERCALL);
}

template <typename Handler>
bool BaselineCodeGen<Handler>::emit_JSOP_SPREADEVAL() {
  return emitSpreadCall(JSOP_SPREADEVAL);
}

template <typename Handler>
bool BaselineCodeGen<Handler>::emit_JSOP_STRICTSPREADEVAL() {
  return emitSpreadCall(JSOP_STRICTSPREADEVAL);
}

typedef bool (*OptimizeSpreadCallFn)(JSContext*, HandleValue, bool*);
static const VMFunction OptimizeSpreadCallInfo =
    FunctionInfo<OptimizeSpreadCallFn>(OptimizeSpreadCall,
                                       "OptimizeSpreadCall");

template <typename Handler>
bool BaselineCodeGen<Handler>::emit_JSOP_OPTIMIZE_SPREADCALL() {
  frame.syncStack(0);
  masm.loadValue(frame.addressOfStackValue(frame.peek(-1)), R0);

  prepareVMCall();
  pushArg(R0);

  if (!callVM(OptimizeSpreadCallInfo)) {
    return false;
  }

  masm.boxNonDouble(JSVAL_TYPE_BOOLEAN, ReturnReg, R0);
  frame.push(R0);
  return true;
}

typedef bool (*ImplicitThisFn)(JSContext*, HandleObject, HandlePropertyName,
                               MutableHandleValue);
const VMFunction ImplicitThisInfo = FunctionInfo<ImplicitThisFn>(
    ImplicitThisOperation, "ImplicitThisOperation");

template <typename Handler>
bool BaselineCodeGen<Handler>::emit_JSOP_IMPLICITTHIS() {
  frame.syncStack(0);
  masm.loadPtr(frame.addressOfEnvironmentChain(), R0.scratchReg());

  prepareVMCall();

  pushArg(ImmGCPtr(script->getName(pc)));
  pushArg(R0.scratchReg());

  if (!callVM(ImplicitThisInfo)) {
    return false;
  }

  frame.push(R0);
  return true;
}

template <typename Handler>
bool BaselineCodeGen<Handler>::emit_JSOP_GIMPLICITTHIS() {
  if (!script->hasNonSyntacticScope()) {
    frame.push(UndefinedValue());
    return true;
  }

  return emit_JSOP_IMPLICITTHIS();
}

template <typename Handler>
bool BaselineCodeGen<Handler>::emit_JSOP_INSTANCEOF() {
  frame.popRegsAndSync(2);

  if (!emitNextIC()) {
    return false;
  }

  frame.push(R0);
  return true;
}

template <typename Handler>
bool BaselineCodeGen<Handler>::emit_JSOP_TYPEOF() {
  frame.popRegsAndSync(1);

  if (!emitNextIC()) {
    return false;
  }

  frame.push(R0);
  return true;
}

template <typename Handler>
bool BaselineCodeGen<Handler>::emit_JSOP_TYPEOFEXPR() {
  return emit_JSOP_TYPEOF();
}

typedef bool (*ThrowMsgFn)(JSContext*, const unsigned);
static const VMFunction ThrowMsgInfo =
    FunctionInfo<ThrowMsgFn>(js::ThrowMsgOperation, "ThrowMsgOperation");

template <typename Handler>
bool BaselineCodeGen<Handler>::emit_JSOP_THROWMSG() {
  prepareVMCall();
  pushArg(Imm32(GET_UINT16(pc)));
  return callVM(ThrowMsgInfo);
}

typedef bool (*ThrowFn)(JSContext*, HandleValue);
static const VMFunction ThrowInfo = FunctionInfo<ThrowFn>(js::Throw, "Throw");

template <typename Handler>
bool BaselineCodeGen<Handler>::emit_JSOP_THROW() {
  // Keep value to throw in R0.
  frame.popRegsAndSync(1);

  prepareVMCall();
  pushArg(R0);

  return callVM(ThrowInfo);
}

template <typename Handler>
bool BaselineCodeGen<Handler>::emit_JSOP_TRY() {
  if (!emit_JSOP_JUMPTARGET()) {
    return false;
  }

  // Ionmonkey can't inline function with JSOP_TRY.
  script->setUninlineable();
  return true;
}

template <typename Handler>
bool BaselineCodeGen<Handler>::emit_JSOP_FINALLY() {
  // JSOP_FINALLY has a def count of 2, but these values are already on the
  // stack (they're pushed by JSOP_GOSUB). Update the compiler's stack state.
  frame.setStackDepth(frame.stackDepth() + 2);

  // To match the interpreter, emit an interrupt check at the start of the
  // finally block.
  return emitInterruptCheck();
}

template <typename Handler>
bool BaselineCodeGen<Handler>::emit_JSOP_GOSUB() {
  // Jump to the finally block.
  frame.syncStack(0);
  emitJump();
  return true;
}

static void LoadBaselineScriptResumeEntries(MacroAssembler& masm,
                                            JSScript* script, Register dest,
                                            Register scratch) {
  MOZ_ASSERT(dest != scratch);

  masm.movePtr(ImmGCPtr(script), dest);
  masm.loadPtr(Address(dest, JSScript::offsetOfBaselineScript()), dest);
  masm.load32(Address(dest, BaselineScript::offsetOfResumeEntriesOffset()),
              scratch);
  masm.addPtr(scratch, dest);
}

template <typename Handler>
bool BaselineCodeGen<Handler>::emit_JSOP_RETSUB() {
  frame.popRegsAndSync(2);

  Label isReturn;
  masm.branchTestBooleanTruthy(/* branchIfTrue = */ false, R0, &isReturn);

  // R0 is |true|. We need to throw R1.
  prepareVMCall();
  pushArg(R1);
  if (!callVM(ThrowInfo)) {
    return false;
  }

  masm.bind(&isReturn);

  // R0 is |false|. R1 contains the resumeIndex to jump to.
  Register scratch1 = R2.scratchReg();
  Register scratch2 = R0.scratchReg();
  LoadBaselineScriptResumeEntries(masm, script, scratch1, scratch2);
  masm.unboxInt32(R1, scratch2);
  masm.loadPtr(
      BaseIndex(scratch1, scratch2, ScaleFromElemWidth(sizeof(uintptr_t))),
      scratch1);
  masm.jump(scratch1);
  return true;
}

template <>
template <typename F1, typename F2>
MOZ_MUST_USE bool BaselineCompilerCodeGen::emitDebugInstrumentation(
    const F1& ifDebuggee, const Maybe<F2>& ifNotDebuggee) {
  // The JIT calls either ifDebuggee or (if present) ifNotDebuggee, because it
  // knows statically whether we're compiling with debug instrumentation.

  if (handler.compileDebugInstrumentation()) {
    return ifDebuggee();
  }

  if (ifNotDebuggee) {
    return (*ifNotDebuggee)();
  }

  return true;
}

template <>
template <typename F1, typename F2>
MOZ_MUST_USE bool BaselineInterpreterCodeGen::emitDebugInstrumentation(
    const F1& ifDebuggee, const Maybe<F2>& ifNotDebuggee) {
  // The interpreter emits both ifDebuggee and (if present) ifNotDebuggee
  // paths, with a branch based on the frame's DEBUGGEE flag.

  Label isNotDebuggee, done;
  masm.branchTest32(Assembler::Zero, frame.addressOfFlags(),
                    Imm32(BaselineFrame::DEBUGGEE), &isNotDebuggee);

  if (!ifDebuggee()) {
    return false;
  }

  if (ifNotDebuggee) {
    masm.jump(&done);
  }

  masm.bind(&isNotDebuggee);

  if (ifNotDebuggee && !(*ifNotDebuggee)()) {
    return false;
  }

  masm.bind(&done);
  return true;
}

typedef bool (*PushLexicalEnvFn)(JSContext*, BaselineFrame*,
                                 Handle<LexicalScope*>);
static const VMFunction PushLexicalEnvInfo =
    FunctionInfo<PushLexicalEnvFn>(jit::PushLexicalEnv, "PushLexicalEnv");

template <typename Handler>
bool BaselineCodeGen<Handler>::emit_JSOP_PUSHLEXICALENV() {
  LexicalScope& scope = script->getScope(pc)->template as<LexicalScope>();

  // Call a stub to push the block on the block chain.
  prepareVMCall();
  masm.loadBaselineFramePtr(BaselineFrameReg, R0.scratchReg());

  pushArg(ImmGCPtr(&scope));
  pushArg(R0.scratchReg());

  return callVM(PushLexicalEnvInfo);
}

typedef bool (*PopLexicalEnvFn)(JSContext*, BaselineFrame*);
static const VMFunction PopLexicalEnvInfo =
    FunctionInfo<PopLexicalEnvFn>(jit::PopLexicalEnv, "PopLexicalEnv");

typedef bool (*DebugLeaveThenPopLexicalEnvFn)(JSContext*, BaselineFrame*,
                                              jsbytecode*);
static const VMFunction DebugLeaveThenPopLexicalEnvInfo =
    FunctionInfo<DebugLeaveThenPopLexicalEnvFn>(
        jit::DebugLeaveThenPopLexicalEnv, "DebugLeaveThenPopLexicalEnv");

template <typename Handler>
bool BaselineCodeGen<Handler>::emit_JSOP_POPLEXICALENV() {
  prepareVMCall();

  masm.loadBaselineFramePtr(BaselineFrameReg, R0.scratchReg());

  auto ifDebuggee = [this]() {
    pushArg(ImmPtr(pc));
    pushArg(R0.scratchReg());
    return callVM(DebugLeaveThenPopLexicalEnvInfo);
  };
  auto ifNotDebuggee = [this]() {
    pushArg(R0.scratchReg());
    return callVM(PopLexicalEnvInfo);
  };
  return emitDebugInstrumentation(ifDebuggee, mozilla::Some(ifNotDebuggee));
}

typedef bool (*FreshenLexicalEnvFn)(JSContext*, BaselineFrame*);
static const VMFunction FreshenLexicalEnvInfo =
    FunctionInfo<FreshenLexicalEnvFn>(jit::FreshenLexicalEnv,
                                      "FreshenLexicalEnv");

typedef bool (*DebugLeaveThenFreshenLexicalEnvFn)(JSContext*, BaselineFrame*,
                                                  jsbytecode*);
static const VMFunction DebugLeaveThenFreshenLexicalEnvInfo =
    FunctionInfo<DebugLeaveThenFreshenLexicalEnvFn>(
        jit::DebugLeaveThenFreshenLexicalEnv,
        "DebugLeaveThenFreshenLexicalEnv");

template <typename Handler>
bool BaselineCodeGen<Handler>::emit_JSOP_FRESHENLEXICALENV() {
  prepareVMCall();

  masm.loadBaselineFramePtr(BaselineFrameReg, R0.scratchReg());

  auto ifDebuggee = [this]() {
    pushArg(ImmPtr(pc));
    pushArg(R0.scratchReg());
    return callVM(DebugLeaveThenFreshenLexicalEnvInfo);
  };
  auto ifNotDebuggee = [this]() {
    pushArg(R0.scratchReg());
    return callVM(FreshenLexicalEnvInfo);
  };
  return emitDebugInstrumentation(ifDebuggee, mozilla::Some(ifNotDebuggee));
}

typedef bool (*RecreateLexicalEnvFn)(JSContext*, BaselineFrame*);
static const VMFunction RecreateLexicalEnvInfo =
    FunctionInfo<RecreateLexicalEnvFn>(jit::RecreateLexicalEnv,
                                       "RecreateLexicalEnv");

typedef bool (*DebugLeaveThenRecreateLexicalEnvFn)(JSContext*, BaselineFrame*,
                                                   jsbytecode*);
static const VMFunction DebugLeaveThenRecreateLexicalEnvInfo =
    FunctionInfo<DebugLeaveThenRecreateLexicalEnvFn>(
        jit::DebugLeaveThenRecreateLexicalEnv,
        "DebugLeaveThenRecreateLexicalEnv");

template <typename Handler>
bool BaselineCodeGen<Handler>::emit_JSOP_RECREATELEXICALENV() {
  prepareVMCall();

  masm.loadBaselineFramePtr(BaselineFrameReg, R0.scratchReg());

  auto ifDebuggee = [this]() {
    pushArg(ImmPtr(pc));
    pushArg(R0.scratchReg());
    return callVM(DebugLeaveThenRecreateLexicalEnvInfo);
  };
  auto ifNotDebuggee = [this]() {
    pushArg(R0.scratchReg());
    return callVM(RecreateLexicalEnvInfo);
  };
  return emitDebugInstrumentation(ifDebuggee, mozilla::Some(ifNotDebuggee));
}

typedef bool (*DebugLeaveLexicalEnvFn)(JSContext*, BaselineFrame*, jsbytecode*);
static const VMFunction DebugLeaveLexicalEnvInfo =
    FunctionInfo<DebugLeaveLexicalEnvFn>(jit::DebugLeaveLexicalEnv,
                                         "DebugLeaveLexicalEnv");

template <typename Handler>
bool BaselineCodeGen<Handler>::emit_JSOP_DEBUGLEAVELEXICALENV() {
  auto ifDebuggee = [this]() {
    prepareVMCall();
    masm.loadBaselineFramePtr(BaselineFrameReg, R0.scratchReg());
    pushArg(ImmPtr(pc));
    pushArg(R0.scratchReg());
    return callVM(DebugLeaveLexicalEnvInfo);
  };
  return emitDebugInstrumentation(ifDebuggee);
}

typedef bool (*PushVarEnvFn)(JSContext*, BaselineFrame*, HandleScope);
static const VMFunction PushVarEnvInfo =
    FunctionInfo<PushVarEnvFn>(jit::PushVarEnv, "PushVarEnv");

template <typename Handler>
bool BaselineCodeGen<Handler>::emit_JSOP_PUSHVARENV() {
  prepareVMCall();
  masm.loadBaselineFramePtr(BaselineFrameReg, R0.scratchReg());
  pushArg(ImmGCPtr(script->getScope(pc)));
  pushArg(R0.scratchReg());

  return callVM(PushVarEnvInfo);
}

typedef bool (*PopVarEnvFn)(JSContext*, BaselineFrame*);
static const VMFunction PopVarEnvInfo =
    FunctionInfo<PopVarEnvFn>(jit::PopVarEnv, "PopVarEnv");

template <typename Handler>
bool BaselineCodeGen<Handler>::emit_JSOP_POPVARENV() {
  prepareVMCall();
  masm.loadBaselineFramePtr(BaselineFrameReg, R0.scratchReg());
  pushArg(R0.scratchReg());

  return callVM(PopVarEnvInfo);
}

typedef bool (*EnterWithFn)(JSContext*, BaselineFrame*, HandleValue,
                            Handle<WithScope*>);
static const VMFunction EnterWithInfo =
    FunctionInfo<EnterWithFn>(jit::EnterWith, "EnterWith");

template <typename Handler>
bool BaselineCodeGen<Handler>::emit_JSOP_ENTERWITH() {
  WithScope& withScope = script->getScope(pc)->template as<WithScope>();

  // Pop "with" object to R0.
  frame.popRegsAndSync(1);

  // Call a stub to push the object onto the environment chain.
  prepareVMCall();
  masm.loadBaselineFramePtr(BaselineFrameReg, R1.scratchReg());

  pushArg(ImmGCPtr(&withScope));
  pushArg(R0);
  pushArg(R1.scratchReg());

  return callVM(EnterWithInfo);
}

typedef bool (*LeaveWithFn)(JSContext*, BaselineFrame*);
static const VMFunction LeaveWithInfo =
    FunctionInfo<LeaveWithFn>(jit::LeaveWith, "LeaveWith");

template <typename Handler>
bool BaselineCodeGen<Handler>::emit_JSOP_LEAVEWITH() {
  // Call a stub to pop the with object from the environment chain.
  prepareVMCall();

  masm.loadBaselineFramePtr(BaselineFrameReg, R0.scratchReg());
  pushArg(R0.scratchReg());

  return callVM(LeaveWithInfo);
}

typedef bool (*GetAndClearExceptionFn)(JSContext*, MutableHandleValue);
static const VMFunction GetAndClearExceptionInfo =
    FunctionInfo<GetAndClearExceptionFn>(GetAndClearException,
                                         "GetAndClearException");

template <typename Handler>
bool BaselineCodeGen<Handler>::emit_JSOP_EXCEPTION() {
  prepareVMCall();

  if (!callVM(GetAndClearExceptionInfo)) {
    return false;
  }

  frame.push(R0);
  return true;
}

typedef bool (*OnDebuggerStatementFn)(JSContext*, BaselineFrame*,
                                      jsbytecode* pc, bool*);
static const VMFunction OnDebuggerStatementInfo =
    FunctionInfo<OnDebuggerStatementFn>(jit::OnDebuggerStatement,
                                        "OnDebuggerStatement");

template <typename Handler>
bool BaselineCodeGen<Handler>::emit_JSOP_DEBUGGER() {
  prepareVMCall();
  pushArg(ImmPtr(pc));

  frame.assertSyncedStack();
  masm.loadBaselineFramePtr(BaselineFrameReg, R0.scratchReg());
  pushArg(R0.scratchReg());

  if (!callVM(OnDebuggerStatementInfo)) {
    return false;
  }

  // If the stub returns |true|, return the frame's return value.
  Label done;
  masm.branchTest32(Assembler::Zero, ReturnReg, ReturnReg, &done);
  {
    masm.loadValue(frame.addressOfReturnValue(), JSReturnOperand);
    masm.jump(&return_);
  }
  masm.bind(&done);
  return true;
}

typedef bool (*DebugEpilogueFn)(JSContext*, BaselineFrame*, jsbytecode*);
static const VMFunction DebugEpilogueInfo = FunctionInfo<DebugEpilogueFn>(
    jit::DebugEpilogueOnBaselineReturn, "DebugEpilogueOnBaselineReturn");

template <typename Handler>
bool BaselineCodeGen<Handler>::emitReturn() {
  auto ifDebuggee = [this]() {
    // Move return value into the frame's rval slot.
    masm.storeValue(JSReturnOperand, frame.addressOfReturnValue());
    masm.or32(Imm32(BaselineFrame::HAS_RVAL), frame.addressOfFlags());

    // Load BaselineFrame pointer in R0.
    frame.syncStack(0);
    masm.loadBaselineFramePtr(BaselineFrameReg, R0.scratchReg());

    prepareVMCall();
    pushArg(ImmPtr(pc));
    pushArg(R0.scratchReg());
    if (!callVM(DebugEpilogueInfo)) {
      return false;
    }

    // Fix up the RetAddrEntry appended by callVM for on-stack recompilation.
    retAddrEntries_.back().setKind(RetAddrEntry::Kind::DebugEpilogue);

    masm.loadValue(frame.addressOfReturnValue(), JSReturnOperand);
    return true;
  };
  if (!emitDebugInstrumentation(ifDebuggee)) {
    return false;
  }

  // Only emit the jump if this JSOP_RETRVAL is not the last instruction.
  // Not needed for last instruction, because last instruction flows
  // into return label.
  if (pc + GetBytecodeLength(pc) < script->codeEnd()) {
    masm.jump(&return_);
  }

  return true;
}

template <typename Handler>
bool BaselineCodeGen<Handler>::emit_JSOP_RETURN() {
  MOZ_ASSERT(frame.stackDepth() == 1);

  frame.popValue(JSReturnOperand);
  return emitReturn();
}

template <typename Handler>
void BaselineCodeGen<Handler>::emitLoadReturnValue(ValueOperand val) {
  Label done, noRval;
  masm.branchTest32(Assembler::Zero, frame.addressOfFlags(),
                    Imm32(BaselineFrame::HAS_RVAL), &noRval);
  masm.loadValue(frame.addressOfReturnValue(), val);
  masm.jump(&done);

  masm.bind(&noRval);
  masm.moveValue(UndefinedValue(), val);

  masm.bind(&done);
}

template <typename Handler>
bool BaselineCodeGen<Handler>::emit_JSOP_RETRVAL() {
  MOZ_ASSERT(frame.stackDepth() == 0);

  masm.moveValue(UndefinedValue(), JSReturnOperand);

  if (!script->noScriptRval()) {
    // Return the value in the return value slot, if any.
    Label done;
    Address flags = frame.addressOfFlags();
    masm.branchTest32(Assembler::Zero, flags, Imm32(BaselineFrame::HAS_RVAL),
                      &done);
    masm.loadValue(frame.addressOfReturnValue(), JSReturnOperand);
    masm.bind(&done);
  }

  return emitReturn();
}

typedef bool (*ToIdFn)(JSContext*, HandleValue, MutableHandleValue);
static const VMFunction ToIdInfo =
    FunctionInfo<ToIdFn>(js::ToIdOperation, "ToIdOperation");

template <typename Handler>
bool BaselineCodeGen<Handler>::emit_JSOP_TOID() {
  // Load index in R0, but keep values on the stack for the decompiler.
  frame.syncStack(0);
  masm.loadValue(frame.addressOfStackValue(frame.peek(-1)), R0);

  // No-op if the index is trivally convertable to an id.
  Label done;
  masm.branchTestInt32(Assembler::Equal, R0, &done);
  masm.branchTestString(Assembler::Equal, R0, &done);
  masm.branchTestSymbol(Assembler::Equal, R0, &done);

  prepareVMCall();

  pushArg(R0);

  if (!callVM(ToIdInfo)) {
    return false;
  }

  masm.bind(&done);
  frame.pop();  // Pop index.
  frame.push(R0);
  return true;
}

typedef JSObject* (*ToAsyncFn)(JSContext*, HandleFunction);
static const VMFunction ToAsyncInfo =
    FunctionInfo<ToAsyncFn>(js::WrapAsyncFunction, "ToAsync");

template <typename Handler>
bool BaselineCodeGen<Handler>::emit_JSOP_TOASYNC() {
  frame.syncStack(0);
  masm.unboxObject(frame.addressOfStackValue(frame.peek(-1)), R0.scratchReg());

  prepareVMCall();
  pushArg(R0.scratchReg());

  if (!callVM(ToAsyncInfo)) {
    return false;
  }

  masm.tagValue(JSVAL_TYPE_OBJECT, ReturnReg, R0);
  frame.pop();
  frame.push(R0);
  return true;
}

typedef JSObject* (*ToAsyncGenFn)(JSContext*, HandleFunction);
static const VMFunction ToAsyncGenInfo =
    FunctionInfo<ToAsyncGenFn>(js::WrapAsyncGenerator, "ToAsyncGen");

template <typename Handler>
bool BaselineCodeGen<Handler>::emit_JSOP_TOASYNCGEN() {
  frame.syncStack(0);
  masm.unboxObject(frame.addressOfStackValue(frame.peek(-1)), R0.scratchReg());

  prepareVMCall();
  pushArg(R0.scratchReg());

  if (!callVM(ToAsyncGenInfo)) {
    return false;
  }

  masm.tagValue(JSVAL_TYPE_OBJECT, ReturnReg, R0);
  frame.pop();
  frame.push(R0);
  return true;
}

typedef JSObject* (*ToAsyncIterFn)(JSContext*, HandleObject, HandleValue);
static const VMFunction ToAsyncIterInfo =
    FunctionInfo<ToAsyncIterFn>(js::CreateAsyncFromSyncIterator, "ToAsyncIter");

template <typename Handler>
bool BaselineCodeGen<Handler>::emit_JSOP_TOASYNCITER() {
  frame.syncStack(0);
  masm.unboxObject(frame.addressOfStackValue(frame.peek(-2)), R0.scratchReg());
  masm.loadValue(frame.addressOfStackValue(frame.peek(-1)), R1);

  prepareVMCall();
  pushArg(R1);
  pushArg(R0.scratchReg());

  if (!callVM(ToAsyncIterInfo)) {
    return false;
  }

  masm.tagValue(JSVAL_TYPE_OBJECT, ReturnReg, R0);
  frame.popn(2);
  frame.push(R0);
  return true;
}

typedef bool (*TrySkipAwaitFn)(JSContext*, HandleValue, MutableHandleValue);
static const VMFunction TrySkipAwaitInfo =
    FunctionInfo<TrySkipAwaitFn>(jit::TrySkipAwait, "TrySkipAwait");

template <typename Handler>
bool BaselineCodeGen<Handler>::emit_JSOP_TRYSKIPAWAIT() {
  frame.syncStack(0);
  masm.loadValue(frame.addressOfStackValue(frame.peek(-1)), R0);

  prepareVMCall();
  pushArg(R0);

  if (!callVM(TrySkipAwaitInfo)) {
    return false;
  }

  Label cannotSkip, done;
  masm.branchTestMagicValue(Assembler::Equal, R0, JS_CANNOT_SKIP_AWAIT,
                            &cannotSkip);
  masm.moveValue(BooleanValue(true), R1);
  masm.jump(&done);

  masm.bind(&cannotSkip);
  masm.loadValue(frame.addressOfStackValue(frame.peek(-1)), R0);
  masm.moveValue(BooleanValue(false), R1);

  masm.bind(&done);

  frame.pop();
  frame.push(R0);
  frame.push(R1);
  return true;
}

typedef bool (*ThrowObjectCoercibleFn)(JSContext*, HandleValue);
static const VMFunction ThrowObjectCoercibleInfo =
    FunctionInfo<ThrowObjectCoercibleFn>(ThrowObjectCoercible,
                                         "ThrowObjectCoercible");

template <typename Handler>
bool BaselineCodeGen<Handler>::emit_JSOP_CHECKOBJCOERCIBLE() {
  frame.syncStack(0);
  masm.loadValue(frame.addressOfStackValue(frame.peek(-1)), R0);

  Label fail, done;

  masm.branchTestUndefined(Assembler::Equal, R0, &fail);
  masm.branchTestNull(Assembler::NotEqual, R0, &done);

  masm.bind(&fail);
  prepareVMCall();

  pushArg(R0);

  if (!callVM(ThrowObjectCoercibleInfo)) {
    return false;
  }

  masm.bind(&done);
  return true;
}

typedef JSString* (*ToStringFn)(JSContext*, HandleValue);
static const VMFunction ToStringInfo =
    FunctionInfo<ToStringFn>(ToStringSlow, "ToStringSlow");

template <typename Handler>
bool BaselineCodeGen<Handler>::emit_JSOP_TOSTRING() {
  // Keep top stack value in R0.
  frame.popRegsAndSync(1);

  // Inline path for string.
  Label done;
  masm.branchTestString(Assembler::Equal, R0, &done);

  prepareVMCall();

  pushArg(R0);

  // Call ToStringSlow which doesn't handle string inputs.
  if (!callVM(ToStringInfo)) {
    return false;
  }

  masm.tagValue(JSVAL_TYPE_STRING, ReturnReg, R0);

  masm.bind(&done);
  frame.push(R0);
  return true;
}

template <>
void BaselineCompilerCodeGen::emitGetTableSwitchIndex(ValueOperand val,
                                                      Register dest) {
  jsbytecode* defaultpc = pc + GET_JUMP_OFFSET(pc);
  Label* defaultLabel = handler.labelOf(defaultpc);

  int32_t low = GET_JUMP_OFFSET(pc + 1 * JUMP_OFFSET_LEN);
  int32_t high = GET_JUMP_OFFSET(pc + 2 * JUMP_OFFSET_LEN);
  int32_t length = high - low + 1;

  // Jump to the 'default' pc if not int32 (tableswitch is only used when
  // all cases are int32).
  masm.branchTestInt32(Assembler::NotEqual, val, defaultLabel);
  masm.unboxInt32(val, dest);

  // Subtract 'low'. Bounds check.
  if (low != 0) {
    masm.sub32(Imm32(low), dest);
  }
  masm.branch32(Assembler::AboveOrEqual, dest, Imm32(length), defaultLabel);
}

template <>
void BaselineInterpreterCodeGen::emitGetTableSwitchIndex(ValueOperand val,
                                                         Register dest) {
  MOZ_CRASH("NYI: interpreter emitTableSwitchJumpTableIndex");
}

template <typename Handler>
bool BaselineCodeGen<Handler>::emit_JSOP_TABLESWITCH() {
  frame.popRegsAndSync(1);

  uint32_t firstResumeIndex = GET_RESUMEINDEX(pc + 3 * JUMP_OFFSET_LEN);

  Register key = R0.scratchReg();
  Register scratch1 = R1.scratchReg();
  Register scratch2 = R2.scratchReg();

  // Call a stub to convert R0 from double to int32 if needed.
  // Note: this stub may clobber scratch1.
  masm.call(cx->runtime()->jitRuntime()->getDoubleToInt32ValueStub());

  // Load the index in the jump table in |key|, or branch to default pc if not
  // int32 or out-of-range.
  emitGetTableSwitchIndex(R0, key);

  // Jump to resumeEntries[firstResumeIndex + key].
  //
  // Note: BytecodeEmitter::allocateResumeIndex static_asserts
  // |firstResumeIndex * sizeof(uintptr_t)| fits in int32_t.

  LoadBaselineScriptResumeEntries(masm, script, scratch1, scratch2);
  masm.loadPtr(BaseIndex(scratch1, key, ScaleFromElemWidth(sizeof(uintptr_t)),
                         firstResumeIndex * sizeof(uintptr_t)),
               scratch1);
  masm.jump(scratch1);

  return true;
}

template <typename Handler>
bool BaselineCodeGen<Handler>::emit_JSOP_ITER() {
  frame.popRegsAndSync(1);

  if (!emitNextIC()) {
    return false;
  }

  frame.push(R0);
  return true;
}

template <typename Handler>
bool BaselineCodeGen<Handler>::emit_JSOP_MOREITER() {
  frame.syncStack(0);
  masm.loadValue(frame.addressOfStackValue(frame.peek(-1)), R0);

  if (!emitNextIC()) {
    return false;
  }

  frame.push(R0);
  return true;
}

template <typename Handler>
bool BaselineCodeGen<Handler>::emitIsMagicValue() {
  frame.syncStack(0);

  Label isMagic, done;
  masm.branchTestMagic(Assembler::Equal,
                       frame.addressOfStackValue(frame.peek(-1)), &isMagic);
  masm.moveValue(BooleanValue(false), R0);
  masm.jump(&done);

  masm.bind(&isMagic);
  masm.moveValue(BooleanValue(true), R0);

  masm.bind(&done);
  frame.push(R0, JSVAL_TYPE_BOOLEAN);
  return true;
}

template <typename Handler>
bool BaselineCodeGen<Handler>::emit_JSOP_ISNOITER() {
  return emitIsMagicValue();
}

template <typename Handler>
bool BaselineCodeGen<Handler>::emit_JSOP_ENDITER() {
  if (!emit_JSOP_JUMPTARGET()) {
    return false;
  }
  frame.popRegsAndSync(1);

  if (!emitNextIC()) {
    return false;
  }

  return true;
}

template <typename Handler>
bool BaselineCodeGen<Handler>::emit_JSOP_ISGENCLOSING() {
  return emitIsMagicValue();
}

template <typename Handler>
bool BaselineCodeGen<Handler>::emit_JSOP_GETRVAL() {
  frame.syncStack(0);

  emitLoadReturnValue(R0);

  frame.push(R0);
  return true;
}

template <typename Handler>
bool BaselineCodeGen<Handler>::emit_JSOP_SETRVAL() {
  // Store to the frame's return value slot.
  storeValue(frame.peek(-1), frame.addressOfReturnValue(), R2);
  masm.or32(Imm32(BaselineFrame::HAS_RVAL), frame.addressOfFlags());
  frame.pop();
  return true;
}

template <typename Handler>
bool BaselineCodeGen<Handler>::emit_JSOP_CALLEE() {
  MOZ_ASSERT(function());
  frame.syncStack(0);
  masm.loadFunctionFromCalleeToken(frame.addressOfCalleeToken(),
                                   R0.scratchReg());
  masm.tagValue(JSVAL_TYPE_OBJECT, R0.scratchReg(), R0);
  frame.push(R0);
  return true;
}

template <typename Handler>
bool BaselineCodeGen<Handler>::emit_JSOP_ENVCALLEE() {
  frame.syncStack(0);
  uint8_t numHops = GET_UINT8(pc);
  Register scratch = R0.scratchReg();

  masm.loadPtr(frame.addressOfEnvironmentChain(), scratch);
  for (unsigned i = 0; i < numHops; i++) {
    Address nextAddr(scratch,
                     EnvironmentObject::offsetOfEnclosingEnvironment());
    masm.unboxObject(nextAddr, scratch);
  }

  masm.loadValue(Address(scratch, CallObject::offsetOfCallee()), R0);
  frame.push(R0);
  return true;
}

typedef JSObject* (*HomeObjectSuperBaseFn)(JSContext*, HandleObject);
static const VMFunction HomeObjectSuperBaseInfo =
    FunctionInfo<HomeObjectSuperBaseFn>(HomeObjectSuperBase,
                                        "HomeObjectSuperBase");

template <typename Handler>
bool BaselineCodeGen<Handler>::emit_JSOP_SUPERBASE() {
  frame.popRegsAndSync(1);

  Register scratch = R0.scratchReg();
  Register proto = R1.scratchReg();

  // Unbox callee.
  masm.unboxObject(R0, scratch);

  // Load [[HomeObject]]
  Address homeObjAddr(scratch,
                      FunctionExtended::offsetOfMethodHomeObjectSlot());
#ifdef DEBUG
  Label isObject;
  masm.branchTestObject(Assembler::Equal, homeObjAddr, &isObject);
  masm.assumeUnreachable("[[HomeObject]] must be Object");
  masm.bind(&isObject);
#endif
  masm.unboxObject(homeObjAddr, scratch);

  // Load prototype from [[HomeObject]]
  masm.loadObjProto(scratch, proto);

  Label hasProto;
  MOZ_ASSERT(uintptr_t(TaggedProto::LazyProto) == 1);
  masm.branchPtr(Assembler::Above, proto, ImmWord(1), &hasProto);

  // Use VMCall for missing or lazy proto
  prepareVMCall();
  pushArg(scratch);  // [[HomeObject]]
  if (!callVM(HomeObjectSuperBaseInfo)) {
    return false;
  }
  masm.movePtr(ReturnReg, proto);

  // Box prototype and return
  masm.bind(&hasProto);
  masm.tagValue(JSVAL_TYPE_OBJECT, proto, R1);
  frame.push(R1);
  return true;
}

typedef JSObject* (*SuperFunOperationFn)(JSContext*, HandleObject);
static const VMFunction SuperFunOperationInfo =
    FunctionInfo<SuperFunOperationFn>(SuperFunOperation, "SuperFunOperation");

template <typename Handler>
bool BaselineCodeGen<Handler>::emit_JSOP_SUPERFUN() {
  frame.popRegsAndSync(1);

  Register callee = R0.scratchReg();
  Register proto = R1.scratchReg();
  Register scratch = R2.scratchReg();

  // Unbox callee.
  masm.unboxObject(R0, callee);

  // Load prototype of callee
  masm.loadObjProto(callee, proto);

  // Use VMCall for missing or lazy proto
  Label needVMCall;
  MOZ_ASSERT(uintptr_t(TaggedProto::LazyProto) == 1);
  masm.branchPtr(Assembler::BelowOrEqual, proto, ImmWord(1), &needVMCall);

  // Use VMCall for non-JSFunction objects (eg. Proxy)
  masm.branchTestObjClass(Assembler::NotEqual, proto, &JSFunction::class_,
                          scratch, proto, &needVMCall);

  // Use VMCall if not constructor
  masm.load16ZeroExtend(Address(proto, JSFunction::offsetOfFlags()), scratch);
  masm.branchTest32(Assembler::Zero, scratch, Imm32(JSFunction::CONSTRUCTOR),
                    &needVMCall);

  // Valid constructor
  Label hasSuperFun;
  masm.jump(&hasSuperFun);

  // Slow path VM Call
  masm.bind(&needVMCall);
  prepareVMCall();
  pushArg(callee);
  if (!callVM(SuperFunOperationInfo)) {
    return false;
  }
  masm.movePtr(ReturnReg, proto);

  // Box prototype and return
  masm.bind(&hasSuperFun);
  masm.tagValue(JSVAL_TYPE_OBJECT, proto, R1);
  frame.push(R1);
  return true;
}

typedef bool (*NewArgumentsObjectFn)(JSContext*, BaselineFrame*,
                                     MutableHandleValue);
static const VMFunction NewArgumentsObjectInfo =
    FunctionInfo<NewArgumentsObjectFn>(jit::NewArgumentsObject,
                                       "NewArgumentsObject");

template <typename Handler>
bool BaselineCodeGen<Handler>::emit_JSOP_ARGUMENTS() {
  frame.syncStack(0);

  MOZ_ASSERT(script->argumentsHasVarBinding());

  Label done;
  if (!script->needsArgsObj()) {
    // We assume the script does not need an arguments object. However, this
    // assumption can be invalidated later, see argumentsOptimizationFailed
    // in JSScript. Guard on the script's NeedsArgsObj flag.
    masm.moveValue(MagicValue(JS_OPTIMIZED_ARGUMENTS), R0);

    // If we don't need an arguments object, skip the VM call.
    Register scratch = R1.scratchReg();
    masm.movePtr(ImmGCPtr(script), scratch);
    masm.branchTest32(
        Assembler::Zero, Address(scratch, JSScript::offsetOfMutableFlags()),
        Imm32(uint32_t(JSScript::MutableFlags::NeedsArgsObj)), &done);
  }

  prepareVMCall();

  masm.loadBaselineFramePtr(BaselineFrameReg, R0.scratchReg());
  pushArg(R0.scratchReg());

  if (!callVM(NewArgumentsObjectInfo)) {
    return false;
  }

  masm.bind(&done);
  frame.push(R0);
  return true;
}

typedef bool (*RunOnceScriptPrologueFn)(JSContext*, HandleScript);
static const VMFunction RunOnceScriptPrologueInfo =
    FunctionInfo<RunOnceScriptPrologueFn>(js::RunOnceScriptPrologue,
                                          "RunOnceScriptPrologue");

template <typename Handler>
bool BaselineCodeGen<Handler>::emit_JSOP_RUNONCE() {
  frame.syncStack(0);

  prepareVMCall();

  masm.movePtr(ImmGCPtr(script), R0.scratchReg());
  pushArg(R0.scratchReg());

  return callVM(RunOnceScriptPrologueInfo);
}

template <typename Handler>
bool BaselineCodeGen<Handler>::emit_JSOP_REST() {
  frame.syncStack(0);

  if (!emitNextIC()) {
    return false;
  }

  // Mark R0 as pushed stack value.
  frame.push(R0);
  return true;
}

typedef JSObject* (*CreateGeneratorFn)(JSContext*, BaselineFrame*);
static const VMFunction CreateGeneratorInfo =
    FunctionInfo<CreateGeneratorFn>(jit::CreateGenerator, "CreateGenerator");

template <typename Handler>
bool BaselineCodeGen<Handler>::emit_JSOP_GENERATOR() {
  MOZ_ASSERT(frame.stackDepth() == 0);

  masm.loadBaselineFramePtr(BaselineFrameReg, R0.scratchReg());

  prepareVMCall();
  pushArg(R0.scratchReg());
  if (!callVM(CreateGeneratorInfo)) {
    return false;
  }

  masm.tagValue(JSVAL_TYPE_OBJECT, ReturnReg, R0);
  frame.push(R0);
  return true;
}

template <typename Handler>
bool BaselineCodeGen<Handler>::emit_JSOP_INITIALYIELD() {
  frame.syncStack(0);
  MOZ_ASSERT(frame.stackDepth() == 1);

  Register genObj = R2.scratchReg();
  masm.unboxObject(frame.addressOfStackValue(frame.peek(-1)), genObj);

  MOZ_ASSERT(GET_RESUMEINDEX(pc) == 0);
  masm.storeValue(Int32Value(0),
                  Address(genObj, GeneratorObject::offsetOfResumeIndexSlot()));

  Register envObj = R0.scratchReg();
  Address envChainSlot(genObj, GeneratorObject::offsetOfEnvironmentChainSlot());
  masm.loadPtr(frame.addressOfEnvironmentChain(), envObj);
  masm.guardedCallPreBarrier(envChainSlot, MIRType::Value);
  masm.storeValue(JSVAL_TYPE_OBJECT, envObj, envChainSlot);

  Register temp = R1.scratchReg();
  Label skipBarrier;
  masm.branchPtrInNurseryChunk(Assembler::Equal, genObj, temp, &skipBarrier);
  masm.branchPtrInNurseryChunk(Assembler::NotEqual, envObj, temp, &skipBarrier);
  masm.push(genObj);
  MOZ_ASSERT(genObj == R2.scratchReg());
  masm.call(&postBarrierSlot_);
  masm.pop(genObj);
  masm.bind(&skipBarrier);

  masm.tagValue(JSVAL_TYPE_OBJECT, genObj, JSReturnOperand);
  return emitReturn();
}

typedef bool (*NormalSuspendFn)(JSContext*, HandleObject, BaselineFrame*,
                                jsbytecode*, uint32_t);
static const VMFunction NormalSuspendInfo =
    FunctionInfo<NormalSuspendFn>(jit::NormalSuspend, "NormalSuspend");

template <typename Handler>
bool BaselineCodeGen<Handler>::emit_JSOP_YIELD() {
  // Store generator in R0.
  frame.popRegsAndSync(1);

  Register genObj = R2.scratchReg();
  masm.unboxObject(R0, genObj);

  MOZ_ASSERT(frame.stackDepth() >= 1);

  if (frame.stackDepth() == 1) {
    // If the expression stack is empty, we can inline the YIELD.

    masm.storeValue(
        Int32Value(GET_RESUMEINDEX(pc)),
        Address(genObj, GeneratorObject::offsetOfResumeIndexSlot()));

    Register envObj = R0.scratchReg();
    Address envChainSlot(genObj,
                         GeneratorObject::offsetOfEnvironmentChainSlot());
    masm.loadPtr(frame.addressOfEnvironmentChain(), envObj);
    masm.guardedCallPreBarrier(envChainSlot, MIRType::Value);
    masm.storeValue(JSVAL_TYPE_OBJECT, envObj, envChainSlot);

    Register temp = R1.scratchReg();
    Label skipBarrier;
    masm.branchPtrInNurseryChunk(Assembler::Equal, genObj, temp, &skipBarrier);
    masm.branchPtrInNurseryChunk(Assembler::NotEqual, envObj, temp,
                                 &skipBarrier);
    MOZ_ASSERT(genObj == R2.scratchReg());
    masm.call(&postBarrierSlot_);
    masm.bind(&skipBarrier);
  } else {
    masm.loadBaselineFramePtr(BaselineFrameReg, R1.scratchReg());

    prepareVMCall();
    pushArg(Imm32(frame.stackDepth()));
    pushArg(ImmPtr(pc));
    pushArg(R1.scratchReg());
    pushArg(genObj);

    if (!callVM(NormalSuspendInfo)) {
      return false;
    }
  }

  masm.loadValue(frame.addressOfStackValue(frame.peek(-1)), JSReturnOperand);
  return emitReturn();
}

template <typename Handler>
bool BaselineCodeGen<Handler>::emit_JSOP_AWAIT() {
  return emit_JSOP_YIELD();
}

typedef bool (*DebugAfterYieldFn)(JSContext*, BaselineFrame*, jsbytecode*,
                                  bool*);
static const VMFunction DebugAfterYieldInfo =
    FunctionInfo<DebugAfterYieldFn>(jit::DebugAfterYield, "DebugAfterYield");

template <typename Handler>
bool BaselineCodeGen<Handler>::emit_JSOP_DEBUGAFTERYIELD() {
  auto ifDebuggee = [this]() {
    frame.assertSyncedStack();
    masm.loadBaselineFramePtr(BaselineFrameReg, R0.scratchReg());
    prepareVMCall();
    pushArg(ImmPtr(pc));
    pushArg(R0.scratchReg());
    if (!callVM(DebugAfterYieldInfo)) {
      return false;
    }

    retAddrEntries_.back().setKind(RetAddrEntry::Kind::DebugAfterYield);

    Label done;
    masm.branchTest32(Assembler::Zero, ReturnReg, ReturnReg, &done);
    {
      masm.loadValue(frame.addressOfReturnValue(), JSReturnOperand);
      masm.jump(&return_);
    }
    masm.bind(&done);
    return true;
  };
  return emitDebugInstrumentation(ifDebuggee);
}

typedef bool (*FinalSuspendFn)(JSContext*, HandleObject, jsbytecode*);
static const VMFunction FinalSuspendInfo =
    FunctionInfo<FinalSuspendFn>(jit::FinalSuspend, "FinalSuspend");

template <typename Handler>
bool BaselineCodeGen<Handler>::emit_JSOP_FINALYIELDRVAL() {
  // Store generator in R0.
  frame.popRegsAndSync(1);
  masm.unboxObject(R0, R0.scratchReg());

  prepareVMCall();
  pushArg(ImmPtr(pc));
  pushArg(R0.scratchReg());

  if (!callVM(FinalSuspendInfo)) {
    return false;
  }

  masm.loadValue(frame.addressOfReturnValue(), JSReturnOperand);
  return emitReturn();
}

typedef bool (*InterpretResumeFn)(JSContext*, HandleObject, HandleValue,
                                  HandlePropertyName, MutableHandleValue);
static const VMFunction InterpretResumeInfo =
    FunctionInfo<InterpretResumeFn>(jit::InterpretResume, "InterpretResume");

typedef bool (*GeneratorThrowFn)(JSContext*, BaselineFrame*,
                                 Handle<GeneratorObject*>, HandleValue,
                                 uint32_t);
static const VMFunction GeneratorThrowOrReturnInfo =
    FunctionInfo<GeneratorThrowFn>(jit::GeneratorThrowOrReturn,
                                   "GeneratorThrowOrReturn", TailCall);

template <typename Handler>
bool BaselineCodeGen<Handler>::emit_JSOP_RESUME() {
  GeneratorObject::ResumeKind resumeKind = GeneratorObject::getResumeKind(pc);

  frame.syncStack(0);
  masm.assertStackAlignment(sizeof(Value), 0);

  AllocatableGeneralRegisterSet regs(GeneralRegisterSet::All());
  regs.take(BaselineFrameReg);

  // Load generator object.
  Register genObj = regs.takeAny();
  masm.unboxObject(frame.addressOfStackValue(frame.peek(-2)), genObj);

  // Load callee.
  Register callee = regs.takeAny();
  masm.unboxObject(Address(genObj, GeneratorObject::offsetOfCalleeSlot()),
                   callee);

  // Load the script. Note that we don't relazify generator scripts, so it's
  // guaranteed to be non-lazy.
  Register scratch1 = regs.takeAny();
  masm.loadPtr(Address(callee, JSFunction::offsetOfScript()), scratch1);

  // Load the BaselineScript or call a stub if we don't have one.
  Label interpret;
  masm.loadPtr(Address(scratch1, JSScript::offsetOfBaselineScript()), scratch1);
  masm.branchPtr(Assembler::BelowOrEqual, scratch1,
                 ImmPtr(BASELINE_DISABLED_SCRIPT), &interpret);

#ifdef JS_TRACE_LOGGING
  if (!emitTraceLoggerResume(scratch1, regs)) {
    return false;
  }
#endif

  // Push |undefined| for all formals.
  Register scratch2 = regs.takeAny();
  Label loop, loopDone;
  masm.load16ZeroExtend(Address(callee, JSFunction::offsetOfNargs()), scratch2);
  masm.bind(&loop);
  masm.branchTest32(Assembler::Zero, scratch2, scratch2, &loopDone);
  {
    masm.pushValue(UndefinedValue());
    masm.sub32(Imm32(1), scratch2);
    masm.jump(&loop);
  }
  masm.bind(&loopDone);

  // Push |undefined| for |this|.
  masm.pushValue(UndefinedValue());

  // Update BaselineFrame frameSize field and create the frame descriptor.
  masm.computeEffectiveAddress(
      Address(BaselineFrameReg, BaselineFrame::FramePointerOffset), scratch2);
  masm.subStackPtrFrom(scratch2);
  masm.store32(scratch2, Address(BaselineFrameReg,
                                 BaselineFrame::reverseOffsetOfFrameSize()));
  masm.makeFrameDescriptor(scratch2, FrameType::BaselineJS,
                           JitFrameLayout::Size());

  masm.Push(Imm32(0));  // actual argc
  masm.PushCalleeToken(callee, /* constructing = */ false);
  masm.Push(scratch2);  // frame descriptor

  regs.add(callee);

  // Load the return value.
  ValueOperand retVal = regs.takeAnyValue();
  masm.loadValue(frame.addressOfStackValue(frame.peek(-1)), retVal);

  // Push a fake return address on the stack. We will resume here when the
  // generator returns.
  Label genStart, returnTarget;
#ifdef JS_USE_LINK_REGISTER
  masm.call(&genStart);
#else
  masm.callAndPushReturnAddress(&genStart);
#endif

  // Add a RetAddrEntry so the return offset -> pc mapping works.
  if (!appendRetAddrEntry(RetAddrEntry::Kind::IC, masm.currentOffset())) {
    return false;
  }

  masm.jump(&returnTarget);
  masm.bind(&genStart);
#ifdef JS_USE_LINK_REGISTER
  masm.pushReturnAddress();
#endif

  // If profiler instrumentation is on, update lastProfilingFrame on
  // current JitActivation
  {
    Register scratchReg = scratch2;
    Label skip;
    AbsoluteAddress addressOfEnabled(
        cx->runtime()->geckoProfiler().addressOfEnabled());
    masm.branch32(Assembler::Equal, addressOfEnabled, Imm32(0), &skip);
    masm.loadJSContext(scratchReg);
    masm.loadPtr(Address(scratchReg, JSContext::offsetOfProfilingActivation()),
                 scratchReg);
    masm.storeStackPtr(
        Address(scratchReg, JitActivation::offsetOfLastProfilingFrame()));
    masm.bind(&skip);
  }

  // Construct BaselineFrame.
  masm.push(BaselineFrameReg);
  masm.moveStackPtrTo(BaselineFrameReg);
  masm.subFromStackPtr(Imm32(BaselineFrame::Size()));
  masm.assertStackAlignment(sizeof(Value), 0);

  // Store flags and env chain.
  masm.store32(Imm32(BaselineFrame::HAS_INITIAL_ENV), frame.addressOfFlags());
  masm.unboxObject(
      Address(genObj, GeneratorObject::offsetOfEnvironmentChainSlot()),
      scratch2);
  masm.storePtr(scratch2, frame.addressOfEnvironmentChain());

  // Store the arguments object if there is one.
  Label noArgsObj;
  Address argsObjSlot(genObj, GeneratorObject::offsetOfArgsObjSlot());
  masm.branchTestUndefined(Assembler::Equal, argsObjSlot, &noArgsObj);
  masm.unboxObject(argsObjSlot, scratch2);
  {
    masm.storePtr(scratch2, frame.addressOfArgsObj());
    masm.or32(Imm32(BaselineFrame::HAS_ARGS_OBJ), frame.addressOfFlags());
  }
  masm.bind(&noArgsObj);

  // Push expression slots if needed.
  Label noExprStack;
  Address exprStackSlot(genObj, GeneratorObject::offsetOfExpressionStackSlot());
  masm.branchTestNull(Assembler::Equal, exprStackSlot, &noExprStack);
  {
    masm.unboxObject(exprStackSlot, scratch2);

    Register initLength = regs.takeAny();
    masm.loadPtr(Address(scratch2, NativeObject::offsetOfElements()), scratch2);
    masm.load32(Address(scratch2, ObjectElements::offsetOfInitializedLength()),
                initLength);
    masm.store32(
        Imm32(0),
        Address(scratch2, ObjectElements::offsetOfInitializedLength()));

    Label loop, loopDone;
    masm.bind(&loop);
    masm.branchTest32(Assembler::Zero, initLength, initLength, &loopDone);
    {
      masm.pushValue(Address(scratch2, 0));
      masm.guardedCallPreBarrier(Address(scratch2, 0), MIRType::Value);
      masm.addPtr(Imm32(sizeof(Value)), scratch2);
      masm.sub32(Imm32(1), initLength);
      masm.jump(&loop);
    }
    masm.bind(&loopDone);
    regs.add(initLength);
  }

  masm.bind(&noExprStack);
  masm.pushValue(retVal);

  masm.switchToObjectRealm(genObj, scratch2);

  if (resumeKind == GeneratorObject::NEXT) {
    // Determine the resume address based on the resumeIndex and the
    // resumeIndex -> native table in the BaselineScript.
    masm.load32(
        Address(scratch1, BaselineScript::offsetOfResumeEntriesOffset()),
        scratch2);
    masm.addPtr(scratch2, scratch1);
    masm.unboxInt32(Address(genObj, GeneratorObject::offsetOfResumeIndexSlot()),
                    scratch2);
    masm.loadPtr(
        BaseIndex(scratch1, scratch2, ScaleFromElemWidth(sizeof(uintptr_t))),
        scratch1);

    // Mark as running and jump to the generator's JIT code.
    masm.storeValue(
        Int32Value(GeneratorObject::RESUME_INDEX_RUNNING),
        Address(genObj, GeneratorObject::offsetOfResumeIndexSlot()));
    masm.jump(scratch1);
  } else {
    MOZ_ASSERT(resumeKind == GeneratorObject::THROW ||
               resumeKind == GeneratorObject::RETURN);

    // Update the frame's frameSize field.
    masm.computeEffectiveAddress(
        Address(BaselineFrameReg, BaselineFrame::FramePointerOffset), scratch2);
    masm.movePtr(scratch2, scratch1);
    masm.subStackPtrFrom(scratch2);
    masm.store32(scratch2, Address(BaselineFrameReg,
                                   BaselineFrame::reverseOffsetOfFrameSize()));
    masm.loadBaselineFramePtr(BaselineFrameReg, scratch2);

    prepareVMCall();
    pushArg(Imm32(resumeKind));
    pushArg(retVal);
    pushArg(genObj);
    pushArg(scratch2);

    TrampolinePtr code =
        cx->runtime()->jitRuntime()->getVMWrapper(GeneratorThrowOrReturnInfo);

    // Create and push the frame descriptor.
    masm.subStackPtrFrom(scratch1);
    masm.makeFrameDescriptor(scratch1, FrameType::BaselineJS,
                             ExitFrameLayout::Size());
    masm.push(scratch1);

    // We have created a baseline frame as if we were the
    // callee. However, if we just did a regular call at this
    // point, our return address would be bogus: it would point at
    // self-hosted code, instead of the generator code that we are
    // pretending we are already executing. Instead, we push a
    // dummy return address. In jit::GeneratorThrowOrReturn,
    // we will set the baseline frame's overridePc. Frame iterators
    // will use the override pc instead of relying on the return
    // address.

    // On ARM64, the callee will push a bogus return address. On
    // other architectures, we push a null return address.
#ifndef JS_CODEGEN_ARM64
    masm.push(ImmWord(0));
#endif
    masm.jump(code);
  }

  // If the generator script has no JIT code, call into the VM.
  masm.bind(&interpret);

  prepareVMCall();
  if (resumeKind == GeneratorObject::NEXT) {
    pushArg(ImmGCPtr(cx->names().next));
  } else if (resumeKind == GeneratorObject::THROW) {
    pushArg(ImmGCPtr(cx->names().throw_));
  } else {
    MOZ_ASSERT(resumeKind == GeneratorObject::RETURN);
    pushArg(ImmGCPtr(cx->names().return_));
  }

  masm.loadValue(frame.addressOfStackValue(frame.peek(-1)), retVal);
  pushArg(retVal);
  pushArg(genObj);

  if (!callVM(InterpretResumeInfo)) {
    return false;
  }

  // After the generator returns, we restore the stack pointer, switch back to
  // the current realm, push the return value, and we're done.
  masm.bind(&returnTarget);
  masm.computeEffectiveAddress(frame.addressOfStackValue(frame.peek(-1)),
                               masm.getStackPointer());
  masm.switchToRealm(script->realm(), R2.scratchReg());
  frame.popn(2);
  frame.push(R0);
  return true;
}

typedef bool (*CheckSelfHostedFn)(JSContext*, HandleValue);
static const VMFunction CheckSelfHostedInfo = FunctionInfo<CheckSelfHostedFn>(
    js::Debug_CheckSelfHosted, "Debug_CheckSelfHosted");

template <typename Handler>
bool BaselineCodeGen<Handler>::emit_JSOP_DEBUGCHECKSELFHOSTED() {
#ifdef DEBUG
  frame.syncStack(0);

  masm.loadValue(frame.addressOfStackValue(frame.peek(-1)), R0);

  prepareVMCall();
  pushArg(R0);
  if (!callVM(CheckSelfHostedInfo)) {
    return false;
  }
#endif
  return true;
}

template <typename Handler>
bool BaselineCodeGen<Handler>::emit_JSOP_IS_CONSTRUCTING() {
  frame.push(MagicValue(JS_IS_CONSTRUCTING));
  return true;
}

template <typename Handler>
bool BaselineCodeGen<Handler>::emit_JSOP_JUMPTARGET() {
  if (!script->hasScriptCounts()) {
    return true;
  }
  PCCounts* counts = script->maybeGetPCCounts(pc);
  uint64_t* counterAddr = &counts->numExec();
  masm.inc64(AbsoluteAddress(counterAddr));
  return true;
}

typedef bool (*CheckClassHeritageOperationFn)(JSContext*, HandleValue);
static const VMFunction CheckClassHeritageOperationInfo =
    FunctionInfo<CheckClassHeritageOperationFn>(js::CheckClassHeritageOperation,
                                                "CheckClassHeritageOperation");

template <typename Handler>
bool BaselineCodeGen<Handler>::emit_JSOP_CHECKCLASSHERITAGE() {
  frame.syncStack(0);

  // Leave the heritage value on the stack.
  masm.loadValue(frame.addressOfStackValue(frame.peek(-1)), R0);

  prepareVMCall();
  pushArg(R0);
  return callVM(CheckClassHeritageOperationInfo);
}

template <typename Handler>
bool BaselineCodeGen<Handler>::emit_JSOP_INITHOMEOBJECT() {
  // Load HomeObject in R0.
  frame.popRegsAndSync(1);

  // Load function off stack
  Register func = R2.scratchReg();
  masm.unboxObject(frame.addressOfStackValue(frame.peek(-1)), func);

  // Set HOMEOBJECT_SLOT
  Address addr(func, FunctionExtended::offsetOfMethodHomeObjectSlot());
  masm.guardedCallPreBarrier(addr, MIRType::Value);
  masm.storeValue(R0, addr);

  Register temp = R1.scratchReg();
  Label skipBarrier;
  masm.branchPtrInNurseryChunk(Assembler::Equal, func, temp, &skipBarrier);
  masm.branchValueIsNurseryObject(Assembler::NotEqual, R0, temp, &skipBarrier);
  masm.call(&postBarrierSlot_);
  masm.bind(&skipBarrier);

  return true;
}

template <typename Handler>
bool BaselineCodeGen<Handler>::emit_JSOP_BUILTINPROTO() {
  // The builtin prototype is a constant for a given global.
  JSProtoKey key = static_cast<JSProtoKey>(GET_UINT8(pc));
  MOZ_ASSERT(key < JSProto_LIMIT);
  JSObject* builtin = GlobalObject::getOrCreatePrototype(cx, key);
  if (!builtin) {
    return false;
  }
  frame.push(ObjectValue(*builtin));
  return true;
}

typedef JSObject* (*ObjectWithProtoOperationFn)(JSContext*, HandleValue);
static const VMFunction ObjectWithProtoOperationInfo =
    FunctionInfo<ObjectWithProtoOperationFn>(js::ObjectWithProtoOperation,
                                             "ObjectWithProtoOperationInfo");

template <typename Handler>
bool BaselineCodeGen<Handler>::emit_JSOP_OBJWITHPROTO() {
  frame.syncStack(0);

  // Leave the proto value on the stack for the decompiler
  masm.loadValue(frame.addressOfStackValue(frame.peek(-1)), R0);

  prepareVMCall();
  pushArg(R0);
  if (!callVM(ObjectWithProtoOperationInfo)) {
    return false;
  }

  masm.tagValue(JSVAL_TYPE_OBJECT, ReturnReg, R0);
  frame.pop();
  frame.push(R0);
  return true;
}

typedef JSObject* (*FunWithProtoFn)(JSContext*, HandleFunction, HandleObject,
                                    HandleObject);
static const VMFunction FunWithProtoInfo = FunctionInfo<FunWithProtoFn>(
    js::FunWithProtoOperation, "FunWithProtoOperation");

template <typename Handler>
bool BaselineCodeGen<Handler>::emit_JSOP_FUNWITHPROTO() {
  frame.popRegsAndSync(1);

  masm.unboxObject(R0, R0.scratchReg());
  masm.loadPtr(frame.addressOfEnvironmentChain(), R1.scratchReg());

  prepareVMCall();
  pushArg(R0.scratchReg());
  pushArg(R1.scratchReg());
  pushArg(ImmGCPtr(script->getFunction(GET_UINT32_INDEX(pc))));
  if (!callVM(FunWithProtoInfo)) {
    return false;
  }

  masm.tagValue(JSVAL_TYPE_OBJECT, ReturnReg, R0);
  frame.push(R0);
  return true;
}

typedef JSFunction* (*MakeDefaultConstructorFn)(JSContext*, HandleScript,
                                                jsbytecode*, HandleObject);
static const VMFunction MakeDefaultConstructorInfo =
    FunctionInfo<MakeDefaultConstructorFn>(js::MakeDefaultConstructor,
                                           "MakeDefaultConstructor");

template <typename Handler>
bool BaselineCodeGen<Handler>::emit_JSOP_CLASSCONSTRUCTOR() {
  frame.syncStack(0);

  // Pass nullptr as prototype to MakeDefaultConstructor
  prepareVMCall();
  pushArg(ImmPtr(nullptr));
  pushArg(ImmPtr(pc));
  pushArg(ImmGCPtr(script));
  if (!callVM(MakeDefaultConstructorInfo)) {
    return false;
  }

  masm.tagValue(JSVAL_TYPE_OBJECT, ReturnReg, R0);
  frame.push(R0);
  return true;
}

template <typename Handler>
bool BaselineCodeGen<Handler>::emit_JSOP_DERIVEDCONSTRUCTOR() {
  frame.popRegsAndSync(1);

  masm.unboxObject(R0, R0.scratchReg());

  prepareVMCall();
  pushArg(R0.scratchReg());
  pushArg(ImmPtr(pc));
  pushArg(ImmGCPtr(script));
  if (!callVM(MakeDefaultConstructorInfo)) {
    return false;
  }

  masm.tagValue(JSVAL_TYPE_OBJECT, ReturnReg, R0);
  frame.push(R0);
  return true;
}

typedef JSObject* (*GetOrCreateModuleMetaObjectFn)(JSContext*, HandleObject);
static const VMFunction GetOrCreateModuleMetaObjectInfo =
    FunctionInfo<GetOrCreateModuleMetaObjectFn>(js::GetOrCreateModuleMetaObject,
                                                "GetOrCreateModuleMetaObject");

template <typename Handler>
bool BaselineCodeGen<Handler>::emit_JSOP_IMPORTMETA() {
  RootedModuleObject module(cx, GetModuleObjectForScript(script));
  MOZ_ASSERT(module);

  frame.syncStack(0);

  prepareVMCall();
  pushArg(ImmGCPtr(module));
  if (!callVM(GetOrCreateModuleMetaObjectInfo)) {
    return false;
  }

  masm.tagValue(JSVAL_TYPE_OBJECT, ReturnReg, R0);
  frame.push(R0);
  return true;
}

typedef JSObject* (*StartDynamicModuleImportFn)(JSContext*, HandleValue,
                                                HandleValue);
static const VMFunction StartDynamicModuleImportInfo =
    FunctionInfo<StartDynamicModuleImportFn>(js::StartDynamicModuleImport,
                                             "StartDynamicModuleImport");

template <typename Handler>
bool BaselineCodeGen<Handler>::emit_JSOP_DYNAMIC_IMPORT() {
  RootedValue referencingPrivate(cx,
                                 FindScriptOrModulePrivateForScript(script));

  // Put specifier value in R0.
  frame.popRegsAndSync(1);

  prepareVMCall();
  pushArg(R0);
  pushArg(referencingPrivate);
  if (!callVM(StartDynamicModuleImportInfo)) {
    return false;
  }

  masm.tagValue(JSVAL_TYPE_OBJECT, ReturnReg, R0);
  frame.push(R0);
  return true;
}

// Instantiate explicitly for now to make sure it compiles.
template class jit::BaselineCodeGen<BaselineInterpreterHandler>;

}  // namespace jit
}  // namespace js
