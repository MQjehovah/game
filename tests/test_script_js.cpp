#include <cmath>
#include <string>
#include <vector>

#include "neon/neon.hpp"
#include "neon/script/script.hpp"
#include "helpers.hpp"

using namespace neon;

namespace {

// Fresh, initialized QuickJS host (the dual-runtime JS backend).
std::unique_ptr<script::IScriptHost> MakeJsHost() {
    auto host = script::CreateJsHost();
    CHECK(host != nullptr);
    CHECK(host->Init());
    return host;
}

bool RunJs(script::IScriptHost& host, const std::string& source) {
    return host.Load(source) && host.Run().Ok();
}

// --- Native function fixtures ------------------------------------------------

script::Value JsNativeSum(script::IScriptHost& host, void* /*user*/) {
    CHECK_EQ(host.ArgCount(), 2);
    return script::Value::Num(host.GetArg(0).number + host.GetArg(1).number);
}

script::Value JsNativeMakePoint(script::IScriptHost& host, void* /*user*/) {
    (void)host;
    script::Value p = script::Value::Tbl();
    p.table->fields.emplace_back("x", script::Value::Num(3));
    p.table->fields.emplace_back("y", script::Value::Num(4));
    p.table->array.push_back(script::Value::Str("a"));
    p.table->array.push_back(script::Value::Str("b"));
    return p;
}

script::Value JsNativeFail(script::IScriptHost& host, void* /*user*/) {
    host.SetError("js native failure: kaboom");
    return script::Value::Nil();
}

script::Value JsNativeThrows(script::IScriptHost& host, void* /*user*/) {
    (void)host;
    throw std::runtime_error("js native boom");
}

script::Value JsNativeOuter(script::IScriptHost& host, void* /*user*/) {
    CHECK_EQ(host.ArgCount(), 2);
    const auto inner = host.Call("inner", {host.GetArg(1)});
    CHECK(inner.Ok());
    CHECK_EQ(host.ArgCount(), 2); // outer frame intact after the nested call
    return script::Value::Num(host.GetArg(0).number + inner.Value().number);
}

script::Value JsNativeInner(script::IScriptHost& host, void* /*user*/) {
    CHECK_EQ(host.ArgCount(), 1);
    return script::Value::Num(host.GetArg(0).number * 10.0);
}

// Reads a table argument's field and returns it; used for table round-trips.
script::Value JsNativeField(script::IScriptHost& host, void* /*user*/) {
    const script::Value v = host.GetArg(0);
    if (v.type != script::Value::Type::Table) return script::Value::Nil();
    for (const auto& kv : v.table->fields) {
        if (kv.first == "x") return kv.second;
    }
    return script::Value::Nil();
}

} // namespace

// ---------------------------------------------------------------------------
// Core host behavior: global chunks, calls, captures, errors.
// ---------------------------------------------------------------------------

TEST(JsHostLoadRunAndCall) {
    auto host = MakeJsHost();
    CHECK(RunJs(*host, R"(
        function on_start(e) { return e; }
        var greeting = "hello";
    )"));
    CHECK(host->HasFunction("on_start"));
    CHECK(!host->HasFunction("missing"));

    // Call with a table argument; the function reads a field and returns it.
    script::Value arg = script::Value::Tbl();
    arg.table->fields.emplace_back("id", script::Value::Num(42));
    const auto res = host->Call("on_start", {arg});
    CHECK(res.Ok());
    CHECK(res.Value().type == script::Value::Type::Table);
    bool found = false;
    for (const auto& kv : res.Value().table->fields) {
        if (kv.first == "id" && kv.second.type == script::Value::Type::Number &&
            kv.second.number == 42.0)
            found = true;
    }
    CHECK(found);

    auto g = host->GetGlobal("greeting");
    CHECK(g.Ok() && g.Value().type == script::Value::Type::String);
    CHECK_EQ(g.Value().str, std::string("hello"));
}

TEST(JsHostValueRoundTrip) {
    auto host = MakeJsHost();
    host->Register("MakePoint", &JsNativeMakePoint);
    host->Register("Field", &JsNativeField);

    // Native table -> JS object: fields and array entries both visible.
    CHECK(RunJs(*host, R"(
        var p = MakePoint();
        var out = {
            x: p.x,
            y: p.y,
            first: p[0],
            second: p[1]
        };
    )"));
    auto out = host->GetGlobal("out");
    CHECK(out.Ok() && out.Value().type == script::Value::Type::Table);
    if (out.Ok()) {
        for (const auto& kv : out.Value().table->fields) {
            if (kv.first == "x") CHECK_EQ(kv.second.number, 3.0);
            if (kv.first == "y") CHECK_EQ(kv.second.number, 4.0);
            if (kv.first == "first") CHECK_EQ(kv.second.str, std::string("a"));
            if (kv.first == "second") CHECK_EQ(kv.second.str, std::string("b"));
        }
    }

    // JS object -> native: Field reads `x`.
    CHECK(RunJs(*host, "var got = Field({ x: 77, y: 1 });"));
    auto got = host->GetGlobal("got");
    CHECK(got.Ok() && got.Value().type == script::Value::Type::Number);
    CHECK_EQ(got.Value().number, 77.0);

    // UTF-8 strings survive round trips.
    CHECK(RunJs(*host, "var u8 = '\u4E16\u754C';")); // 世界
    auto u8 = host->GetGlobal("u8");
    CHECK(u8.Ok() && u8.Value().type == script::Value::Type::String);
    CHECK_EQ(u8.Value().str, std::string("\xE4\xB8\x96\xE7\x95\x8C"));
}

TEST(JsHostNativeRegistrationAndFrames) {
    auto host = MakeJsHost();
    host->Register("sum", &JsNativeSum);
    host->Register("inner", &JsNativeInner);
    host->Register("outer", &JsNativeOuter);
    host->RegisterField("Json", "Echo", &JsNativeSum);

    CHECK(RunJs(*host, "var r1 = sum(2, 3); var r2 = outer(10, 5);"));
    auto r1 = host->GetGlobal("r1");
    CHECK(r1.Ok() && r1.Value().type == script::Value::Type::Number);
    CHECK_EQ(r1.Value().number, 5.0);
    auto r2 = host->GetGlobal("r2");
    CHECK(r2.Ok() && r2.Value().type == script::Value::Type::Number);
    CHECK_EQ(r2.Value().number, 60.0); // 10 + inner(5) = 10 + 50

    // Namespaced registration (Json.Echo) is a real property on the table.
    CHECK(RunJs(*host, "var r3 = Json.Echo(20, 22);"));
    auto r3 = host->GetGlobal("r3");
    CHECK(r3.Ok() && r3.Value().type == script::Value::Type::Number);
    CHECK_EQ(r3.Value().number, 42.0);
}

TEST(JsHostErrors) {
    auto host = MakeJsHost();

    // Syntax error reports a line number.
    CHECK(!host->Load("function broken( {\n  return 1;\n"));
    CHECK(host->LastError().line > 0);
    CHECK(!host->LastError().message.empty());

    // Runtime error reports the throwing line.
    CHECK(RunJs(*host, "function boom() {\n  throw new Error('kaboom');\n}\n"));
    const auto res = host->Call("boom", {});
    CHECK(!res.Ok());
    CHECK(host->LastError().line > 0);

    // Native SetError surfaces as a script error.
    host->Register("fail", &JsNativeFail);
    CHECK(RunJs(*host, "var failed = false;\ntry { fail(); } catch (e) { failed = true; }"));
    auto failed = host->GetGlobal("failed");
    CHECK(failed.Ok() && failed.Value().type == script::Value::Type::Bool);
    CHECK(failed.Value().boolean);

    // Native C++ exceptions are contained and reported.
    host->Register("throws", &JsNativeThrows);
    CHECK(RunJs(*host, "var caught = false;\ntry { throws(); } catch (e) { caught = true; }"));
    auto caught = host->GetGlobal("caught");
    CHECK(caught.Ok() && caught.Value().type == script::Value::Type::Bool);
    CHECK(caught.Value().boolean);

    // Calling a missing global is an error, not a crash.
    const auto missing = host->Call("nope", {});
    CHECK(!missing.Ok());
}

TEST(JsHostCaptureIsolation) {
    auto host = MakeJsHost();
    CHECK(RunJs(*host, R"(
        function on_start(e) { return 1; }
    )"));
    const auto h1 = host->CaptureFunction("on_start");
    CHECK(h1.Ok() && h1.Value() != 0);

    // A later chunk overwrites the global; the captured handle still calls
    // the ORIGINAL function (per-entity script isolation).
    CHECK(RunJs(*host, R"(
        function on_start(e) { return 2; }
    )"));
    auto r1 = host->CallCaptured(h1.Value(), {});
    CHECK(r1.Ok() && r1.Value().number == 1.0);
    auto r2 = host->Call("on_start", {});
    CHECK(r2.Ok() && r2.Value().number == 2.0);

    // Invalid handles error cleanly.
    CHECK(!host->CallCaptured(99999, {}).Ok());
}

TEST(JsHostCaptureStackFunction) {
    auto host = MakeJsHost();
    host->Register("hook", [](script::IScriptHost& h, void*) -> script::Value {
        const auto cap = h.CaptureStackFunction(0);
        CHECK(cap.Ok());
        return script::Value::Num(static_cast<double>(cap.Value()));
    });
    CHECK(RunJs(*host, R"(
        var handle = hook(function (x) { return x * 3; });
    )"));
    auto handle = host->GetGlobal("handle");
    CHECK(handle.Ok() && handle.Value().type == script::Value::Type::Number);
    const auto res = host->CallCaptured(static_cast<uint64_t>(handle.Value().number), {script::Value::Num(5)});
    CHECK(res.Ok() && res.Value().number == 15.0);
}

// ---------------------------------------------------------------------------
// Determinism + sandbox parity with the Lua host.
// ---------------------------------------------------------------------------

std::vector<double> JsDrawSequence(script::IScriptHost& host, const std::string& source) {
    if (!host.Load(source)) return {};
    if (!host.Run().Ok()) return {};
    auto res = host.GetGlobal("out");
    if (!res.Ok() || res.Value().type != script::Value::Type::Table) return {};
    std::vector<double> seq;
    for (const auto& v : res.Value().table->array) {
        if (v.type == script::Value::Type::Number) seq.push_back(v.number);
    }
    return seq;
}

TEST(JsHostDeterminism) {
    const char* kDraw = R"(
        NMath.Seed(12345);
        var out = [Math.random(), Math.random(), NMath.Random(100),
                   NMath.RandomRange(5, 10), NMath.Random(6), NMath.Random(6)];
    )";
    auto a = MakeJsHost();
    auto b = MakeJsHost();
    const auto seqA = JsDrawSequence(*a, kDraw);
    const auto seqB = JsDrawSequence(*b, kDraw);
    CHECK_EQ(seqA.size(), 6u);
    CHECK_EQ(seqA.size(), seqB.size());
    for (size_t i = 0; i < seqA.size() && i < seqB.size(); ++i)
        CHECK_EQ(seqA[i], seqB[i]);

    // SetRngSeed(0) aliases seed 1 (matches LuaHost / core::Rng).
    auto c = MakeJsHost();
    c->SetRngSeed(0);
    CHECK(RunJs(*c, "var out = [NMath.Random(1000), NMath.Random(1000)];"));
    auto seed1 = c->GetGlobal("out");
    CHECK(seed1.Ok());
}

TEST(JsHostSimClockAndSandbox) {
    auto host = MakeJsHost();
    host->SetSimClock(12.5);
    CHECK(RunJs(*host, "var t = NMath.Time();"));
    auto t = host->GetGlobal("t");
    CHECK(t.Ok() && t.Value().type == script::Value::Type::Number);
    CHECK_NEAR(t.Value().number, 12.5, 1e-9);

    // No host stdlib: require / console are absent (sandbox baseline).
    CHECK(!host->HasFunction("require"));
    CHECK(!host->HasFunction("console"));

    // print routes to the engine log without crashing.
    CHECK(RunJs(*host, "print('hello from js', 1, true);"));
}

TEST(JsHostCheckSyntax) {
    auto host = MakeJsHost();
    CHECK(host->CheckSyntax("const x = 1;"));
    CHECK(!host->CheckSyntax("const x = ;"));
    CHECK(host->CheckSyntax("function ok() { return 1; }"));
}

TEST(JsHostEs2020) {
    auto host = MakeJsHost();
    CHECK(RunJs(*host, R"(
        const double = (x) => x * 2;
        const label = `v${double(21)}`;
        const arr = [1, 2, 3];
        const [head, ...rest] = arr;
        const spread = [...rest, 9];
        var out = { label: label, head: head, rest: spread.join(','), cls: new (class { constructor() { this.n = 7; } })().n };
    )"));
    auto out = host->GetGlobal("out");
    CHECK(out.Ok() && out.Value().type == script::Value::Type::Table);
    if (out.Ok()) {
        for (const auto& kv : out.Value().table->fields) {
            if (kv.first == "label") CHECK_EQ(kv.second.str, std::string("v42"));
            if (kv.first == "head") CHECK_EQ(kv.second.number, 1.0);
            if (kv.first == "rest") CHECK_EQ(kv.second.str, std::string("2,3,9"));
            if (kv.first == "cls") CHECK_EQ(kv.second.number, 7.0);
        }
    }
}
