#include <string>
#include <vector>

#include "neon/neon.hpp"
#include "neon/script/script.hpp"
#include "helpers.hpp"

using namespace neon;

namespace {

// Fresh, initialized host shared by the functional tests. Each test shuts the
// host down so lifecycle paths are exercised per test.
std::unique_ptr<script::IScriptHost> MakeHost() {
    auto host = script::CreateLuaHost();
    CHECK(host != nullptr);
    CHECK(host->Init());
    return host;
}

// --- Native function fixtures -------------------------------------------------

script::Value NativeDoubleIt(script::IScriptHost& host, void* /*user*/) {
    script::Value v = host.GetArg(0);
    if (v.type != script::Value::Type::Number) return script::Value::Nil();
    return script::Value::Num(v.number * 2.0);
}

script::Value NativeTripleIt(script::IScriptHost& host, void* /*user*/) {
    script::Value v = host.GetArg(0);
    if (v.type != script::Value::Type::Number) return script::Value::Nil();
    return script::Value::Num(v.number * 3.0);
}

script::Value NativeSquare(script::IScriptHost& host, void* /*user*/) {
    script::Value v = host.GetArg(0);
    if (v.type != script::Value::Type::Number) return script::Value::Nil();
    return script::Value::Num(v.number * v.number);
}

script::Value NativeSum(script::IScriptHost& host, void* /*user*/) {
    CHECK_EQ(host.ArgCount(), 2);
    return script::Value::Num(host.GetArg(0).number + host.GetArg(1).number);
}

script::Value NativeScale(script::IScriptHost& host, void* user) {
    double factor = *static_cast<const double*>(user);
    return script::Value::Num(host.GetArg(0).number * factor);
}

script::Value NativeFail(script::IScriptHost& host, void* /*user*/) {
    host.SetError("native failure: kaboom");
    return script::Value::Nil();
}

script::Value NativeInner(script::IScriptHost& host, void* /*user*/) {
    CHECK_EQ(host.ArgCount(), 1);
    return script::Value::Num(host.GetArg(0).number * 10.0);
}

script::Value NativeOuter(script::IScriptHost& host, void* /*user*/) {
    CHECK_EQ(host.ArgCount(), 2);
    auto inner = host.Call("inner", {host.GetArg(1)});
    CHECK(inner.Ok());
    CHECK_EQ(host.ArgCount(), 2); // outer frame intact after the nested call
    return script::Value::Num(host.GetArg(0).number + inner.Value().number);
}

script::Value NativeGreet(script::IScriptHost& host, void* /*user*/) {
    return script::Value::Str(std::string("h\xC3\xA9llo \xE4\xB8\x96\xE7\x95\x8C \xF0\x9F\x8E\xAE"));
}

script::Value NativeThrows(script::IScriptHost& host, void* /*user*/) {
    throw std::runtime_error("native boom");
}

} // namespace

// ---------------------------------------------------------------------------
// script::Value + IScriptHost
// ---------------------------------------------------------------------------

TEST(ScriptValueHelpers) {
    script::Value nil = script::Value::Nil();
    CHECK(nil.type == script::Value::Type::Nil);

    script::Value num = script::Value::Num(10);
    CHECK(num.type == script::Value::Type::Number);
    CHECK_EQ(num.number, 10.0);

    script::Value str = script::Value::Str("hi");
    CHECK(str.type == script::Value::Type::String);
    CHECK_EQ(str.str, std::string("hi"));

    script::Value boolean = script::Value::Bool(true);
    CHECK(boolean.type == script::Value::Type::Bool);
    CHECK(boolean.boolean);
}

// Load compiles a chunk; Run executes it; GetGlobal reads the result back.
TEST(ScriptLoadSetAndReadNumber) {
    auto host = MakeHost();
    CHECK(host->Load("x = 10"));
    CHECK(host->Run().Ok());

    auto res = host->GetGlobal("x");
    CHECK(res.Ok());
    CHECK(res.Value().type == script::Value::Type::Number);
    CHECK_EQ(res.Value().number, 10.0);
    host->Shutdown();
}

// Define a global function, then invoke it through Call().
TEST(ScriptFunctionCall) {
    auto host = MakeHost();
    CHECK(host->Load("function add(a, b) return a + b end"));
    CHECK(host->Run().Ok());

    CHECK(host->HasFunction("add"));
    CHECK(!host->HasFunction("no_such_function"));

    auto res = host->Call("add", {script::Value::Num(2), script::Value::Num(3)});
    CHECK(res.Ok());
    CHECK(res.Value().type == script::Value::Type::Number);
    CHECK_EQ(res.Value().number, 5.0);
    host->Shutdown();
}

// Strings are opaque byte sequences: multi-byte UTF-8 round-trips untouched.
TEST(ScriptUtf8RoundTrip) {
    auto host = MakeHost();
    const std::string kText = "h\xC3\xA9llo \xE4\xB8\x96\xE7\x95\x8C \xF0\x9F\x8E\xAE";
    CHECK(host->Load("y = 'h\xC3\xA9llo \xE4\xB8\x96\xE7\x95\x8C \xF0\x9F\x8E\xAE'"));
    CHECK(host->Run().Ok());

    auto res = host->GetGlobal("y");
    CHECK(res.Ok());
    CHECK(res.Value().type == script::Value::Type::String);
    CHECK_EQ(res.Value().str, kText);
    host->Shutdown();
}

// A leading UTF-8 BOM (Notepad/PowerShell default) must not break Load: the
// host strips it before compiling, and the defined functions stay callable.
TEST(ScriptLoadBomTolerated) {
    auto host = MakeHost();
    const std::string source = std::string("\xEF\xBB\xBF", 3) +
                               "function add(a, b) return a + b end\n";
    CHECK(host->Load(source));
    CHECK(host->Run().Ok());
    CHECK(host->HasFunction("add"));
    auto res = host->Call("add", {script::Value::Num(2), script::Value::Num(3)});
    CHECK(res.Ok());
    CHECK_EQ(res.Value().number, 5.0);
    host->Shutdown();
}

// CheckSyntax (editor lint path) also tolerates a BOM'd source.
TEST(ScriptCheckSyntaxBomTolerated) {
    auto host = MakeHost();
    const std::string source = std::string("\xEF\xBB\xBF", 3) + "function on_update(e, dt) end\n";
    CHECK(host->CheckSyntax(source));
    host->Shutdown();
}

// A syntax error is reported at Load time; Run then errors without crashing.
TEST(ScriptSyntaxError) {
    auto host = MakeHost();
    CHECK(!host->Load("this is not lua !!!"));
    CHECK(!host->LastError().message.empty());
    CHECK(host->LastError().line > 0);

    auto res = host->Run();
    CHECK(!res.Ok());
    CHECK(!host->LastError().message.empty());
    host->Shutdown();
}

// A runtime error inside a valid chunk reports the failing source line.
TEST(ScriptRuntimeErrorReportsLine) {
    auto host = MakeHost();
    CHECK(host->Load("a = 1\nb = a + nil"));
    auto res = host->Run();
    CHECK(!res.Ok());
    CHECK_EQ(host->LastError().line, 2);
    CHECK(!host->LastError().message.empty());
    host->Shutdown();
}

// Globals set from C++ are visible to scripts and updates read back.
TEST(ScriptSetGlobalBeforeLoad) {
    auto host = MakeHost();
    host->SetGlobal("n", script::Value::Num(42));
    CHECK(host->Load("n = n * 2"));
    CHECK(host->Run().Ok());

    auto res = host->GetGlobal("n");
    CHECK(res.Ok());
    CHECK_EQ(res.Value().number, 84.0);
    host->Shutdown();
}

TEST(ScriptLifecycle) {
    auto host = script::CreateLuaHost();

    // Run() with no backing state errors cleanly.
    CHECK(!host->Run().Ok());
    CHECK(!host->LastError().message.empty());

    // Init (idempotent) then Run() before Load() still errors.
    CHECK(host->Init());
    CHECK(host->Init());
    CHECK(!host->Run().Ok());
    CHECK(!host->LastError().message.empty());

    // A full cycle works.
    CHECK(host->Load("x = 1"));
    CHECK(host->Run().Ok());
    auto res = host->GetGlobal("x");
    CHECK(res.Ok());
    CHECK_EQ(res.Value().number, 1.0);

    // Double shutdown is safe; operations after it do not crash.
    host->Shutdown();
    host->Shutdown();
    CHECK(!host->Run().Ok());

    // Re-init brings the host back to life.
    CHECK(host->Init());
    CHECK(host->Load("y = 7"));
    CHECK(host->Run().Ok());
    auto res2 = host->GetGlobal("y");
    CHECK(res2.Ok());
    CHECK_EQ(res2.Value().number, 7.0);
    host->Shutdown();
}

// Missing globals read back as Nil rather than erroring.
TEST(ScriptMissingGlobalIsNil) {
    auto host = MakeHost();
    auto res = host->GetGlobal("definitely_not_defined");
    CHECK(res.Ok());
    CHECK(res.Value().type == script::Value::Type::Nil);
    host->Shutdown();
}

// A registered native function is callable both from a Lua chunk and through
// the host's Call() path.
TEST(ScriptNativeRegisterAndCall) {
    auto host = MakeHost();
    host->Register("double_it", &NativeDoubleIt);

    CHECK(host->Load("return double_it(21)"));
    auto res = host->Run();
    CHECK(res.Ok());
    CHECK(res.Value().type == script::Value::Type::Number);
    CHECK_EQ(res.Value().number, 42.0);

    auto res2 = host->Call("double_it", {script::Value::Num(10)});
    CHECK(res2.Ok());
    CHECK_EQ(res2.Value().number, 20.0);
    host->Shutdown();
}

// Inside a native call, ArgCount/GetArg expose the script arguments.
TEST(ScriptNativeArgCountGetArg) {
    auto host = MakeHost();
    host->Register("sum", &NativeSum);
    CHECK(host->Load("return sum(3, 4)"));
    auto res = host->Run();
    CHECK(res.Ok());
    CHECK_EQ(res.Value().number, 7.0);
    host->Shutdown();
}

// The user pointer passed to Register reaches the native function.
TEST(ScriptNativeUserParam) {
    auto host = MakeHost();
    const double kFactor = 2.5;
    host->Register("scale", &NativeScale, const_cast<double*>(&kFactor));
    CHECK(host->Load("return scale(4)"));
    auto res = host->Run();
    CHECK(res.Ok());
    CHECK_EQ(res.Value().number, 10.0);
    host->Shutdown();
}

// Re-registering a name replaces the previous native function.
TEST(ScriptNativeRegisterOverwrites) {
    auto host = MakeHost();
    host->Register("scale", &NativeDoubleIt);
    host->Register("scale", &NativeTripleIt);
    CHECK(host->Load("return scale(5)"));
    auto res = host->Run();
    CHECK(res.Ok());
    CHECK_EQ(res.Value().number, 15.0);
    host->Shutdown();
}

// A native function that calls SetError fails the script call.
TEST(ScriptNativeSetError) {
    auto host = MakeHost();
    host->Register("boom", &NativeFail);
    CHECK(host->Load("return boom()"));
    auto res = host->Run();
    CHECK(!res.Ok());
    CHECK(!host->LastError().message.empty());
    CHECK(host->LastError().message.find("kaboom") != std::string::npos);
    host->Shutdown();
}

// A native function may re-enter the host from inside a call; each frame must
// observe its own ArgCount/GetArg (nested frame stack).
TEST(ScriptNativeReentrantCall) {
    auto host = MakeHost();
    host->Register("inner", &NativeInner);
    host->Register("outer", &NativeOuter);
    CHECK(host->Load("return outer(3, 4)"));
    auto res = host->Run();
    CHECK(res.Ok());
    CHECK_EQ(res.Value().number, 43.0); // 3 + (4 * 10)
    host->Shutdown();
}

// SetError surfaces through the host's Call() path as well as Run().
TEST(ScriptNativeSetErrorThroughCall) {
    auto host = MakeHost();
    host->Register("boom", &NativeFail);
    auto res = host->Call("boom", {});
    CHECK(!res.Ok());
    CHECK(!host->LastError().message.empty());
    CHECK(host->LastError().message.find("kaboom") != std::string::npos);
    host->Shutdown();
}

// A native function may return a string; its bytes round-trip untouched.
TEST(ScriptNativeStringReturn) {
    auto host = MakeHost();
    host->Register("greet", &NativeGreet);
    CHECK(host->Load("return greet()"));
    auto res = host->Run();
    CHECK(res.Ok());
    CHECK(res.Value().type == script::Value::Type::String);
    CHECK_EQ(res.Value().str,
             std::string("h\xC3\xA9llo \xE4\xB8\x96\xE7\x95\x8C \xF0\x9F\x8E\xAE"));
    host->Shutdown();
}

// ArgCount/GetArg outside any native call are safe no-ops.
TEST(ScriptNativeArgQueriesOutsideCall) {
    auto host = MakeHost();
    CHECK_EQ(host->ArgCount(), 0);
    CHECK(host->GetArg(0).type == script::Value::Type::Nil);
    CHECK(host->GetArg(3).type == script::Value::Type::Nil);
    host->Shutdown();
}

// Register before Init must not crash (accepted as a no-op).
TEST(ScriptNativeRegisterBeforeInit) {
    auto host = script::CreateLuaHost();
    host->Register("double_it", &NativeDoubleIt);
    CHECK(host->Init());
    CHECK(!host->HasFunction("double_it")); // the pre-Init registration is dropped
    host->Shutdown();
}

// A throwing native function is contained and surfaces as a script error
// rather than unwinding through Lua's setjmp frames.
TEST(ScriptNativeExceptionContained) {
    auto host = MakeHost();
    host->Register("explode", &NativeThrows);
    CHECK(host->Load("return explode()"));
    auto res = host->Run();
    CHECK(!res.Ok());
    CHECK(!host->LastError().message.empty());
    CHECK(host->LastError().message.find("native boom") != std::string::npos);
    host->Shutdown();
}

// Dotted-path registration ("A.B.f") creates nested tables (the editor plugin
// system registers NeonEditor.ui.* this way).
TEST(ScriptRegisterFieldNestedTables) {
    auto host = MakeHost();
    host->RegisterField("A.B", "triple", &NativeTripleIt);
    host->RegisterField("A.B", "square", &NativeSquare);
    host->RegisterField("A", "plain", &NativeDoubleIt);
    // Volume: the editor plugin API registers ~30 fields on two nested levels
    // (NeonEditor + NeonEditor.ui); exercise the same pattern.
    for (int i = 0; i < 30; ++i) {
        const std::string f1 = "f" + std::to_string(i);
        const std::string f2 = "g" + std::to_string(i);
        host->RegisterField("A.B", f1, &NativeDoubleIt);
        host->RegisterField("A", f2, &NativeTripleIt);
    }
    CHECK(host->Load("v1 = A.B.triple(5); v2 = A.B.square(4); v3 = A.plain(3);"));
    const auto runRes = host->Run();
    if (!runRes.Ok()) {
        std::fprintf(stderr, "DBG nested run error: %s (line %d)\n",
                     host->LastError().message.c_str(), host->LastError().line);
        CHECK(host->Load("diag = { a = type(A), ab = type(A.B), triple = type(A.B.triple), "
                         "plain = type(A.plain), g0 = type(A.g0), f0 = type(A.B.f0) }"));
        const auto d = host->Run();
        if (d.Ok()) {
            const auto v = host->GetGlobal("diag");
            if (v.Ok() && v.Value().type == script::Value::Type::Table) {
                for (const auto& kv : v.Value().table->fields)
                    std::fprintf(stderr, "DBG diag %s=%s\n", kv.first.c_str(),
                                 kv.second.str.c_str());
            }
        }
    }
    CHECK(runRes.Ok());
    auto v1 = host->GetGlobal("v1");
    CHECK(v1.Ok() && v1.Value().type == script::Value::Type::Number);
    CHECK_EQ(v1.Value().number, 15.0);
    auto v2 = host->GetGlobal("v2");
    CHECK(v2.Ok() && v2.Value().number == 16.0);
    auto v3 = host->GetGlobal("v3");
    CHECK(v3.Ok() && v3.Value().number == 6.0);
}

// P1-2: cooperative line breakpoints. The host latches a paused state at a
// breakpoint line, captures locals + callstack, and resumes (optionally
// stepping to the next line event).
TEST(ScriptDebuggerBreakpointPauseResume) {
    auto host = MakeHost();
    const char* src =
        "function run(x)\n"
        "  local a = x * 2\n"
        "  local b = a + 1\n"
        "  return b\n"
        "end\n";
    CHECK(host->Load(src));
    CHECK(host->Run().Ok());
    // Breakpoint on line 4 ("local b = a + 1").
    std::vector<int> bps = {4};
    host->SetScriptBreakpoints("test_script.lua", bps);
    CHECK(!host->DebuggerPaused());

    host->SetCurrentScript("test_script.lua");
    auto callRes = host->Call("run", {script::Value::Num(10.0)});
    CHECK(callRes.Ok());
    script::Value r = callRes.Value();
    CHECK_EQ(r.number, 21.0);  // the paused function still completes
    CHECK(host->DebuggerPaused());
    const auto& frame = host->PausedFrame();
    CHECK_EQ(frame.line, 4);
    bool hasA = false;
    for (const auto& l : frame.locals) {
        if (l.name == "a") {
            hasA = true;
            CHECK(l.value.find("20") != std::string::npos);
        }
    }
    CHECK(hasA);
    CHECK(!frame.callstack.empty());

    // Continue: runs until the next breakpoint (the same line fires again).
    host->DebuggerResume(false);
    CHECK(!host->DebuggerPaused());
    host->SetCurrentScript("test_script.lua");
    host->Call("run", {script::Value::Num(1.0)});
    CHECK(host->DebuggerPaused());

    // Step: the next line event after resume pauses again (same line here).
    host->DebuggerResume(true);
    host->SetCurrentScript("test_script.lua");
    host->Call("run", {script::Value::Num(2.0)});
    CHECK(host->DebuggerPaused());
    // Disable the debugger so shutdown is clean.
    host->SetDebuggerEnabled(false);
    host->DebuggerResume(false);
    host->Shutdown();
}

// Breakpoints are per-script: a matching source pauses; others do not.
TEST(ScriptDebuggerBreakpointPathMatching) {
    auto host = MakeHost();
    CHECK(host->Load("function f() return 1 end\n"));
    CHECK(host->Run().Ok());
    std::vector<int> bps = {1};
    host->SetScriptBreakpoints("other_script.lua", bps);
    host->SetCurrentScript("my_script.lua");
    host->Call("f", {});
    CHECK(!host->DebuggerPaused());
    host->Shutdown();
}

// A5 (2026-08-28): runaway scripts must abort with a ScriptError instead of
// freezing the engine forever; the memory cap raises a clean error too.
TEST(ScriptRunawayLoopAborts) {
    auto host = script::CreateLuaHost();
    CHECK(host != nullptr);
    CHECK(host->Init());
    host->SetInstructionBudget(200000); // small budget for a fast test
    CHECK(host->Load("while true do end"));
    auto r = host->Run();
    CHECK(!r.Ok());
    CHECK(host->LastError().message.find("budget") != std::string::npos);
    // The host survives and still runs healthy scripts afterwards.
    CHECK(host->Load("return 6*7"));
    auto ok = host->Run();
    CHECK(ok.Ok());
    CHECK_NEAR(ok.Value().number, 42.0, 1e-9);
    host->Shutdown();
}

TEST(ScriptMemoryLimitAborts) {
    auto host = script::CreateLuaHost();
    CHECK(host != nullptr);
    CHECK(host->Init());
    host->SetMemoryLimit(2u * 1024 * 1024); // 2 MiB
    CHECK(host->Load(
        "local t = {}\n"
        "for i = 1, 1000000 do t[i] = {i, i, i} end\n"));
    auto r = host->Run();
    CHECK(!r.Ok());
    CHECK(host->LastError().message.find("memory") != std::string::npos);
    host->Shutdown();
}
