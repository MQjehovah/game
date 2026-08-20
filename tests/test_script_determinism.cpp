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

// Runs a chunk that stores a string in the global `out` and returns it. Used
// where exact 64-bit integer draws must be compared losslessly (doubles would
// round): tostring'd math.random(0) values compared as strings.
std::string RunForString(script::IScriptHost& host, const std::string& source) {
    if (!host.Load(source)) return {};
    if (!host.Run().Ok()) return {};
    auto res = host.GetGlobal("out");
    if (!res.Ok() || res.Value().type != script::Value::Type::String) return {};
    return res.Value().str;
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

// The RNG stream continues across Load/Run cycles: a second chunk (no reseed)
// keeps drawing where the first stopped, matching a single 100-draw chunk.
TEST(DeterministicStreamContinuesAcrossLoadRun) {
    const char* kFirst = R"(
NMath.Seed(99)
local t = {}
for i = 1, 50 do t[i] = NMath.Random() end
out = t
)";
    const char* kSecond = R"(
local t = {}
for i = 1, 50 do t[i] = NMath.Random() end
out = t
)";
    const char* kFull = R"(
NMath.Seed(99)
local t = {}
for i = 1, 100 do t[i] = NMath.Random() end
out = t
)";
    auto a = MakeHost();
    auto b = MakeHost();
    auto first = DrawSequence(*a, kFirst);
    auto second = DrawSequence(*a, kSecond);
    first.insert(first.end(), second.begin(), second.end());
    auto full = DrawSequence(*b, kFull);
    CHECK_EQ(first.size(), 100u);
    CHECK_EQ(full.size(), 100u);
    CHECK(first == full);
    a->Shutdown();
    b->Shutdown();
}

// Seeding from C++ (SetRngSeed) is equivalent to the script-side NMath.Seed.
TEST(DeterministicSetRngSeedEqualsScriptSeed) {
    const char* kHostSeeded = R"(
local t = {}
for i = 1, 200 do
  if i % 2 == 0 then t[i] = math.random(100) else t[i] = NMath.Random() end
end
out = t
)";
    const char* kScriptSeeded = R"(
NMath.Seed(0xBEEF)
local t = {}
for i = 1, 200 do
  if i % 2 == 0 then t[i] = math.random(100) else t[i] = NMath.Random() end
end
out = t
)";
    auto a = MakeHost();
    auto b = MakeHost();
    a->SetRngSeed(0xBEEF);
    auto seqA = DrawSequence(*a, kHostSeeded);
    auto seqB = DrawSequence(*b, kScriptSeeded);
    CHECK_EQ(seqA.size(), 200u);
    CHECK_EQ(seqB.size(), 200u);
    CHECK(seqA == seqB);
    a->Shutdown();
    b->Shutdown();
}

// Reseeding mid-stream restarts from the new seed on both hosts identically.
TEST(DeterministicMidStreamReseed) {
    const char* kScript = R"(
NMath.Seed(11)
local t = {}
for i = 1, 25 do t[i] = NMath.Random() end
NMath.Seed(22)
for i = 26, 50 do t[i] = NMath.Random() end
out = t
)";
    auto a = MakeHost();
    auto b = MakeHost();
    auto seqA = DrawSequence(*a, kScript);
    auto seqB = DrawSequence(*b, kScript);
    CHECK_EQ(seqA.size(), 50u);
    CHECK_EQ(seqB.size(), 50u);
    CHECK(seqA == seqB);
    a->Shutdown();
    b->Shutdown();
}

// Shutdown + re-Init looks like a fresh host: RNG back to the default seed and
// clock back to 0, even after a custom seed/clock were applied.
TEST(DeterministicReinitResetsRngAndClock) {
    auto a = MakeHost();
    auto b = MakeHost();

    a->SetRngSeed(2026);
    a->SetSimClock(55.0);
    CHECK(a->Load("out = NMath.Time()"));
    CHECK(a->Run().Ok());
    auto customClock = a->GetGlobal("out");
    CHECK(customClock.Ok());
    CHECK_EQ(customClock.Value().number, 55.0);

    const char* kCustomDraws = R"(
local t = {}
for i = 1, 100 do t[i] = NMath.Random() end
out = t
)";
    auto customSeq = DrawSequence(*a, kCustomDraws);

    a->Shutdown();
    CHECK(a->Init());

    CHECK(a->Load("out = NMath.Time()"));
    CHECK(a->Run().Ok());
    auto resetClock = a->GetGlobal("out");
    CHECK(resetClock.Ok());
    CHECK_EQ(resetClock.Value().number, 0.0);

    auto resetSeq = DrawSequence(*a, kCustomDraws);
    auto freshSeq = DrawSequence(*b, kCustomDraws);
    CHECK_EQ(resetSeq.size(), 100u);
    CHECK_EQ(freshSeq.size(), 100u);
    CHECK(resetSeq == freshSeq);  // default seed after re-init == default seed fresh host
    CHECK(resetSeq != customSeq); // and differs from the pre-shutdown custom seed
    a->Shutdown();
    b->Shutdown();
}

// math.random(0) yields the full raw 64-bit draw; comparing tostring'd values
// as strings (not doubles) pins low-bit exactness across hosts.
TEST(DeterministicRandomZeroExactInt64) {
    const char* kScript = R"(
math.randomseed(4242)
local t = {}
for i = 1, 64 do t[i] = tostring(math.random(0)) end
out = table.concat(t, ",")
)";
    auto a = MakeHost();
    auto b = MakeHost();
    std::string strA = RunForString(*a, kScript);
    std::string strB = RunForString(*b, kScript);
    CHECK(!strA.empty());
    CHECK_EQ(strA, strB);
    CHECK(strA.find(',') != std::string::npos); // sanity: 64 distinct positions joined
    a->Shutdown();
    b->Shutdown();
}

// core::Rng cannot store a zero state, so seed 0 is documented to alias seed 1.
// Pin that behavior so the aliasing is a tested contract, not an accident.
TEST(DeterministicZeroSeedAliasesOne) {
    const char* kScript = R"(
local t = {}
for i = 1, 200 do t[i] = NMath.Random() end
out = t
)";
    auto zero = MakeHost();
    auto one = MakeHost();
    zero->SetRngSeed(0);
    one->SetRngSeed(1);
    auto seqZero = DrawSequence(*zero, kScript);
    auto seqOne = DrawSequence(*one, kScript);
    CHECK_EQ(seqZero.size(), 200u);
    CHECK_EQ(seqOne.size(), 200u);
    CHECK(seqZero == seqOne);
    zero->Shutdown();
    one->Shutdown();
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
