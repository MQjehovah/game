#include <string>
#include <vector>

#include "neon/neon.hpp"
#include "neon/script/script.hpp"
#include "helpers.hpp"

using namespace neon;

namespace {

// Fresh, initialized host. The sandbox (deterministic RNG, fixed clock,
// disabled io/os/package) is ON by default for every CreateLuaHost().
std::unique_ptr<script::IScriptHost> MakeHost() {
    auto host = script::CreateLuaHost();
    CHECK(host != nullptr);
    CHECK(host->Init());
    return host;
}

// Runs a chunk that fills the global `out` table with a numeric sequence, then
// reads it back as a vector<double>. Returns an empty vector when anything
// fails so a bad host surfaces as a size mismatch in the caller.
std::vector<double> DrawSequence(script::IScriptHost& host, const std::string& source) {
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

// Exercises all three math.random calling conventions through one stream.
const char* kMixedDrawScript = R"(
math.randomseed(12345)
local t = {}
for i = 1, 1000 do
  local m = i % 3
  if m == 0 then
    t[i] = math.random()
  elseif m == 1 then
    t[i] = math.random(100)
  else
    t[i] = math.random(-50, 50)
  end
end
out = t
)";

// Exercises the canonical NMath API through one stream.
const char* kNMathDrawScript = R"(
NMath.Seed(0xC0FFEE)
local t = {}
for i = 1, 1000 do
  local m = i % 3
  if m == 0 then
    t[i] = NMath.Random()
  elseif m == 1 then
    t[i] = NMath.Random(100)
  else
    t[i] = NMath.RandomRange(-50, 50)
  end
end
out = t
)";

} // namespace

// ---------------------------------------------------------------------------
// Deterministic RNG: same seed -> identical stream across fresh hosts
// ---------------------------------------------------------------------------

TEST(DeterministicMathRandomSameSeed) {
    auto a = MakeHost();
    auto b = MakeHost();
    auto seqA = DrawSequence(*a, kMixedDrawScript);
    auto seqB = DrawSequence(*b, kMixedDrawScript);
    CHECK_EQ(seqA.size(), 1000u);
    CHECK_EQ(seqB.size(), 1000u);
    CHECK(seqA == seqB); // bit-exact across hosts, no wall-clock involvement
    a->Shutdown();
    b->Shutdown();
}

// NMath.Seed produces the same stream for the same seed and a different stream
// for a different seed (probabilistically certain with xorshift64*).
TEST(DeterministicNMathSameSeedDifferentSeed) {
    auto a = MakeHost();
    auto b = MakeHost();
    auto seqA = DrawSequence(*a, kNMathDrawScript);
    auto seqB = DrawSequence(*b, kNMathDrawScript);
    CHECK_EQ(seqA.size(), 1000u);
    CHECK_EQ(seqB.size(), 1000u);
    CHECK(seqA == seqB);

    auto c = MakeHost();
    const char* kOtherSeed = R"(
NMath.Seed(0xDEAD)
local t = {}
for i = 1, 1000 do
  t[i] = NMath.Random()
end
out = t
)";
    auto seqC = DrawSequence(*c, kOtherSeed);
    CHECK_EQ(seqC.size(), 1000u);
    CHECK(seqA != seqC);
    a->Shutdown();
    b->Shutdown();
    c->Shutdown();
}

// math.random(n) and math.random(m, n) return integers inside their stated
// ranges, and degenerate ranges are honored.
TEST(DeterministicMathRandomRange) {
    auto host = MakeHost();
    const char* src = R"(
math.randomseed(7)
local ok = true
for i = 1, 2000 do
  local a = math.random(50)
  if a < 1 or a > 50 or a ~= math.floor(a) then ok = false end
  local b = math.random(-10, 30)
  if b < -10 or b > 30 or b ~= math.floor(b) then ok = false end
  if math.random(1, 1) ~= 1 then ok = false end
end
out = ok
)";
    CHECK(host->Load(src));
    CHECK(host->Run().Ok());
    auto res = host->GetGlobal("out");
    CHECK(res.Ok());
    CHECK(res.Value().type == script::Value::Type::Bool);
    CHECK(res.Value().boolean);
    host->Shutdown();
}

// ---------------------------------------------------------------------------
// Deterministic clock
// ---------------------------------------------------------------------------

// NMath.Time() mirrors the host's simulated clock; the default is 0 (never
// wall time) and SetSimClock pins it to a fixed value.
TEST(DeterministicNMathTime) {
    auto host = MakeHost();

    CHECK(host->Load("out = NMath.Time()"));
    CHECK(host->Run().Ok());
    auto res = host->GetGlobal("out");
    CHECK(res.Ok());
    CHECK(res.Value().type == script::Value::Type::Number);
    CHECK_EQ(res.Value().number, 0.0);

    host->SetSimClock(42.5);
    CHECK(host->Load("out = NMath.Time()"));
    CHECK(host->Run().Ok());
    auto res2 = host->GetGlobal("out");
    CHECK(res2.Ok());
    CHECK_EQ(res2.Value().number, 42.5);
    host->Shutdown();
}

// ---------------------------------------------------------------------------
// Sandbox: io/os/package/require are absent, print still works
// ---------------------------------------------------------------------------

TEST(DeterministicSandboxGlobalsNil) {
    auto host = MakeHost();
    const char* kDisabled[] = {"io", "os", "package", "require",
                               "collectgarbage", "dofile", "loadfile"};
    for (const char* name : kDisabled) {
        auto res = host->GetGlobal(name);
        CHECK(res.Ok());
        CHECK(res.Value().type == script::Value::Type::Nil);
    }
    // The deterministic core survives: math + NMath tables, print callable.
    const char* kTables[] = {"math", "NMath"};
    for (const char* name : kTables) {
        auto res = host->GetGlobal(name);
        CHECK(res.Ok());
        CHECK(res.Value().type == script::Value::Type::Table);
    }
    CHECK(host->HasFunction("print"));
    host->Shutdown();
}

// Calling the disabled escapes fails as a normal script error, not a crash.
TEST(DeterministicSandboxDisabledCallsError) {
    auto host = MakeHost();
    const char* kScripts[] = {
        "return os.execute('echo hi')",
        "return os.clock()",
        "return os.time()",
        "return io.open('x', 'w')",
        "return require('foo')",
        "return loadfile('x.lua')",
        "return dofile('x.lua')",
    };
    for (const char* src : kScripts) {
        CHECK(host->Load(src));
        auto res = host->Run();
        CHECK(!res.Ok());
        CHECK(!host->LastError().message.empty());
        CHECK(host->LastError().message.find("nil") != std::string::npos);
    }
    host->Shutdown();
}

// ---------------------------------------------------------------------------
// Host-side seeding + clock: reproducible across host recreation
// ---------------------------------------------------------------------------

TEST(DeterministicSetRngSeedAcrossHosts) {
    const char* kSeedScript = R"(
local t = {}
for i = 1, 500 do
  local m = i % 2
  if m == 0 then t[i] = math.random(0) else t[i] = NMath.Random() end
end
out = t
)";
    auto a = MakeHost();
    auto b = MakeHost();
    a->SetRngSeed(2026);
    b->SetRngSeed(2026);
    auto seqA = DrawSequence(*a, kSeedScript);
    auto seqB = DrawSequence(*b, kSeedScript);
    CHECK_EQ(seqA.size(), 500u);
    CHECK(seqA == seqB);

    auto c = MakeHost();
    c->SetRngSeed(2027);
    auto seqC = DrawSequence(*c, kSeedScript);
    CHECK_EQ(seqC.size(), 500u);
    CHECK(seqA != seqC);
    a->Shutdown();
    b->Shutdown();
    c->Shutdown();
}

// print is routed to the engine log and never crashes.
TEST(DeterministicPrintRuns) {
    auto host = MakeHost();
    CHECK(host->Load("print('hello', 1, true)"));
    CHECK(host->Run().Ok());
    CHECK(host->Load("print()"));
    CHECK(host->Run().Ok());
    host->Shutdown();
}
