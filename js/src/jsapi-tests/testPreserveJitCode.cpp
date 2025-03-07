/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

// For js::jit::IsIonEnabled().
#include "jit/Ion.h"

#include "js/CompilationAndEvaluation.h"
#include "jsapi-tests/tests.h"

#include "vm/JSObject-inl.h"

using namespace JS;

static void ScriptCallback(JSRuntime* rt, void* data, JSScript* script,
                           const JS::AutoRequireNoGC& nogc) {
  unsigned& count = *static_cast<unsigned*>(data);
  if (script->hasIonScript()) {
    ++count;
  }
}

BEGIN_TEST(test_PreserveJitCode) {
  CHECK(testPreserveJitCode(false, 0));
  CHECK(testPreserveJitCode(true, 1));
  return true;
}

unsigned countIonScripts(JSObject* global) {
  unsigned count = 0;
  js::IterateScripts(cx, global->nonCCWRealm(), &count, ScriptCallback);
  return count;
}

bool testPreserveJitCode(bool preserveJitCode, unsigned remainingIonScripts) {
  cx->options().setBaseline(true);
  cx->options().setIon(true);
  cx->runtime()->setOffthreadIonCompilationEnabled(false);

  RootedObject global(cx, createTestGlobal(preserveJitCode));
  CHECK(global);
  JSAutoRealm ar(cx, global);

  // The Ion JIT may be unavailable due to --disable-ion or lack of support
  // for this platform.
  if (!js::jit::IsIonEnabled(cx)) {
    knownFail = true;
  }

  CHECK_EQUAL(countIonScripts(global), 0u);

  static const char source[] =
      "var i = 0;\n"
      "var sum = 0;\n"
      "while (i < 10) {\n"
      "    sum += i;\n"
      "    ++i;\n"
      "}\n"
      "return sum;\n";
  unsigned length = strlen(source);

  JS::CompileOptions options(cx);
  options.setFileAndLine(__FILE__, 1);

  JS::RootedFunction fun(cx);
  JS::AutoObjectVector emptyScopeChain(cx);
  CHECK(JS::CompileFunctionUtf8(cx, emptyScopeChain, options, "f", 0, nullptr,
                                source, length, &fun));

  RootedValue value(cx);
  for (unsigned i = 0; i < 1500; ++i) {
    CHECK(JS_CallFunction(cx, global, fun, JS::HandleValueArray::empty(),
                          &value));
  }
  CHECK_EQUAL(value.toInt32(), 45);
  CHECK_EQUAL(countIonScripts(global), 1u);

  NonIncrementalGC(cx, GC_NORMAL, gcreason::API);
  CHECK_EQUAL(countIonScripts(global), remainingIonScripts);

  NonIncrementalGC(cx, GC_SHRINK, gcreason::API);
  CHECK_EQUAL(countIonScripts(global), 0u);

  return true;
}

JSObject* createTestGlobal(bool preserveJitCode) {
  JS::RealmOptions options;
  options.creationOptions().setPreserveJitCode(preserveJitCode);
  return JS_NewGlobalObject(cx, getGlobalClass(), nullptr,
                            JS::FireOnNewGlobalHook, options);
}
END_TEST(test_PreserveJitCode)
