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
