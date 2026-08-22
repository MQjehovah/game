#include <cstdint>
#include <cstring>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "neon/neon.hpp"
#include "game_server.hpp"
#include "client_sync.hpp"
#include "net_input.hpp"
#include "scripted_input.hpp"
#include "helpers.hpp"

using namespace neon;

// Test-suite 6.7: deterministic-simulation acceptance + LAN two-client smoke.
// ---------------------------------------------------------------------------
// The core deliverable: prove the server's authoritative simulation and the
// client's LOCAL prediction produce IDENTICAL results for the same input
// stream. A fixed scripted MsgInput sequence (server::ScriptedInputs) is
// injected on BOTH sides — the server via GameServer::SetScriptedInputs (no
// socket client) and the client's headless GameRuntime via a server::NetInput.
// After N fixed 60Hz ticks the controlled entity's final position is compared
// BIT-EXACT and a state hash (positions of all replicated entities + the
// script's GameVars, FNV-1a over raw bytes) must match.
//
// A live-loopback variant drives a real client over a real server with the
// same scripted stream; because v1 reconciliation snaps on divergence rather
// than replaying, its guarantee is the reconcile bound (and, with identical
// inputs on a lossless loopback, the prediction tracks the server exactly —
// see the caveat below). The PURE two-sim comparison is the bit-exact proof.

namespace {

// A scene whose script both moves the controlled player on InputAxis("forward")
// and reacts to the jump/interact buttons, so the scripted input stream
// (forward held, then jump, then interact) shows up in GameVars and therefore
// in the state hash. Identical on the server and the client prediction.
const char* kDeterminismLua = R"(
function on_start(e)
  player = Spawn("player", { x = 0, y = 0, z = 0 })
  SetVar("ticks", 0)
  SetVar("jumps", 0)
  SetVar("interacts", 0)
end
function on_update(e, dt)
  local t = GetVar("ticks")
  if t == nil then t = 0 end
  t = t + 1
  SetVar("ticks", t)

  local fwd = InputAxis("forward")
  if fwd > 0.5 then
    local p = GetPosition(player)
    if p ~= nil then
      SetPosition(player, { x = p.x, y = p.y, z = p.z + 1 })
    end
  end
  if InputKey("space") > 0 then
    SetVar("jumps", GetVar("jumps") + 1)
  end
  if InputKey("f") > 0 then
    SetVar("interacts", GetVar("interacts") + 1)
  end
end
)";

const char* kSceneJson = R"({
  "entities": [
    {
      "name": "Host",
      "components": {
        "transform": {"pos": [0, 0, 0]},
        "script": {"backend": "lua", "path": "scripts/controller.lua"}
      }
    }
  ]
})";

void WriteFixture(const std::string& dir) {
    CHECK(test::WriteFileAll(dir + "/scene.json", kSceneJson));
    const std::string scriptsDir = dir + "/scripts";
#if defined(_WIN32)
    CreateDirectoryA(scriptsDir.c_str(), nullptr);
#else
    ::mkdir(scriptsDir.c_str(), 0700);
#endif
    CHECK(test::WriteFileAll(scriptsDir + "/controller.lua", kDeterminismLua));
}

std::string ReadScene(const std::string& dir) {
    std::string text;
    CHECK(test::ReadFileAll(dir + "/scene.json", text));
    return text;
}

// ---------------------------------------------------------------------------
// Deterministic state hash: FNV-1a 64-bit over the bit patterns of every
// replicated entity's position (sorted by stable entity key) and every GameVar
// (ascending key order). Bit-exact: floats hash by their IEEE-754 bits, so the
// hash differs the instant ANY simulated value differs by a single bit.
// ---------------------------------------------------------------------------

uint64_t Fnv1aBytes(uint64_t h, const void* data, size_t size) {
    const uint8_t* p = static_cast<const uint8_t*>(data);
    for (size_t i = 0; i < size; ++i) {
        h ^= p[i];
        h *= 1099511628211ULL;
    }
    return h;
}

uint64_t Fnv1aFloat(uint64_t h, float f) {
    uint32_t bits;
    std::memcpy(&bits, &f, sizeof(bits));
    return Fnv1aBytes(h, &bits, sizeof(bits));
}

// Stable snapshot key: (id << 32) | generation (matches GameServer::EntityKey).
uint64_t EntityKeyOf(const ecs::Entity& e) {
    return (static_cast<uint64_t>(e.id) << 32) | static_cast<uint64_t>(e.generation);
}

void HashValue(uint64_t& h, const script::Value& v) {
    const uint8_t type = static_cast<uint8_t>(v.type);
    h = Fnv1aBytes(h, &type, sizeof(type));
    switch (v.type) {
        case script::Value::Type::Number:
            h = Fnv1aBytes(h, &v.number, sizeof(v.number));
            break;
        case script::Value::Type::String:
            h = Fnv1aBytes(h, v.str.data(), v.str.size());
            break;
        case script::Value::Type::Bool:
            h = Fnv1aBytes(h, &v.boolean, 1);
            break;
        case script::Value::Type::Table:
            if (v.table) {
                const uint64_t n = v.table->array.size();
                h = Fnv1aBytes(h, &n, sizeof(n));
                for (const script::Value& e : v.table->array) HashValue(h, e);
                const uint64_t m = v.table->fields.size();
                h = Fnv1aBytes(h, &m, sizeof(m));
                for (const auto& kv : v.table->fields) {
                    h = Fnv1aBytes(h, kv.first.data(), kv.first.size());
                    HashValue(h, kv.second);
                }
            }
            break;
        default:
            break;
    }
}

uint64_t GameVarsHash(const script::GameVars& vars) {
    uint64_t h = 14695981039346656037ULL; // FNV-1a offset basis
    vars.ForEach([&](const std::string& key, const script::Value& v) {
        h = Fnv1aBytes(h, key.data(), key.size());
        HashValue(h, v);
    });
    return h;
}

// Positions of every replicated entity (SceneTransform + script CTransformBind)
// in ascending key order, then the script's GameVars. Identical computation on
// the server's world and the client's prediction world.
uint64_t WorldStateHash(ecs::World& world, script::GameVars& vars) {
    uint64_t h = 14695981039346656037ULL;
    std::map<uint64_t, math::Vec3> entities;
    {
        auto view = world.ViewAll<scene::SceneTransform>();
        for (size_t i = 0; i < view.Size(); ++i) {
            ecs::Entity e = world.EntityAt<scene::SceneTransform>(i);
            const scene::SceneTransform* t = world.Get<scene::SceneTransform>(e);
            if (t) entities[EntityKeyOf(e)] = t->pos;
        }
    }
    {
        auto view = world.ViewAll<script::CTransformBind>();
        for (size_t i = 0; i < view.Size(); ++i) {
            ecs::Entity e = world.EntityAt<script::CTransformBind>(i);
            const script::CTransformBind* t = world.Get<script::CTransformBind>(e);
            if (t) entities[EntityKeyOf(e)] = t->pos;
        }
    }
    for (const auto& kv : entities) {
        h = Fnv1aBytes(h, &kv.first, sizeof(kv.first));
        h = Fnv1aFloat(h, kv.second.x);
        h = Fnv1aFloat(h, kv.second.y);
        h = Fnv1aFloat(h, kv.second.z);
    }
    const uint64_t vh = GameVarsHash(vars);
    h = Fnv1aBytes(h, &vh, sizeof(vh));
    return h;
}

const math::Vec3 ControlledPos(ecs::World& world) {
    auto view = world.ViewAll<script::CTransformBind>();
    for (size_t i = 0; i < view.Size(); ++i) {
        ecs::Entity e = world.EntityAt<script::CTransformBind>(i);
        const script::CTransformBind* t = world.Get<script::CTransformBind>(e);
        if (t) return t->pos;
    }
    return math::Vec3{};
}

// ---------------------------------------------------------------------------
// The pure two-sim runners: server and client prediction consume the SAME
// scripted stream for the SAME number of fixed 60Hz ticks.
// ---------------------------------------------------------------------------

struct SimResult {
    math::Vec3 controlledPos;
    uint64_t stateHash = 0;
    double ticksVar = 0.0;
    double jumpsVar = 0.0;
    double interactsVar = 0.0;
};

constexpr uint32_t kScriptTicks = 120; // matches the documented script's range

// Server side: GameServer with SetScriptedInputs (no socket clients), stepped
// `ticks` times on a deterministic fake clock (17ms = one fixed 60Hz step).
SimResult RunServerScripted(const std::string& dir, uint64_t seed, uint32_t ticks) {
    server::GameServer server;
    server::GameServer::Config cfg;
    cfg.port = 0;
    cfg.loopback = true;
    cfg.sceneJsonPath = dir + "/scene.json";
    cfg.scriptBaseDir = dir;
    cfg.rngSeed = seed;
    CHECK(server.Start(cfg));
    server.SetScriptedInputs(server::ScriptedInputs());

    uint64_t now = 0;
    for (uint32_t i = 0; i < ticks; ++i) {
        now += 17;
        server.Step(now);
    }

    SimResult r;
    r.controlledPos = ControlledPos(server.World());
    r.stateHash = WorldStateHash(server.World(), server.GameVars());
    r.ticksVar = server.GameVars().Get("ticks").number;
    r.jumpsVar = server.GameVars().Get("jumps").number;
    r.interactsVar = server.GameVars().Get("interacts").number;
    CHECK_EQ(server.CurrentTick(), ticks);
    server.Shutdown();
    return r;
}

// Client side: a headless GameRuntime running the SAME scene, fed the same
// scripted stream through a server::NetInput, ticked `ticks` times.
SimResult RunClientScripted(const std::string& dir, uint64_t seed, uint32_t ticks) {
    scene::GameRuntime local;
    server::NetInput netInput;
    scene::GameRuntimeConfig cfg;
    cfg.assets = nullptr;
    cfg.headless = true;
    cfg.scriptBaseDir = dir;
    cfg.rngSeed = seed; // MUST match the server's seed (same RNG stream)
    cfg.input = &netInput;
    core::Status st = local.Start(ReadScene(dir), cfg);
    CHECK(st.Ok());

    const auto seq = server::ScriptedInputs();
    for (uint32_t i = 0; i < ticks; ++i) {
        const net::MsgInput* in = server::InputForTick(seq, i);
        netInput.SetInput(in ? in->buttons : 0, in ? in->moveX : 0, in ? in->moveY : 0);
        local.Tick(1.0f / 60.0f);
        netInput.EndFrame();
    }

    SimResult r;
    r.controlledPos = ControlledPos(local.World());
    r.stateHash = WorldStateHash(local.World(), local.GameVars());
    r.ticksVar = local.GameVars().Get("ticks").number;
    r.jumpsVar = local.GameVars().Get("jumps").number;
    r.interactsVar = local.GameVars().Get("interacts").number;
    local.Stop();
    return r;
}

// ---------------------------------------------------------------------------
// Live-loopback variant: a real client over a real server, both consuming the
// scripted stream. The client joins first (the v1 controller), sends each
// tick's scripted input over the wire, runs local prediction with the same
// input, and reconciles against the server's snapshot stream.
// ---------------------------------------------------------------------------

struct ScriptedLoopbackClient {
    net::UdpSocket sock;
    net::ReliableChannel chan;
    scene::GameRuntime local;
    server::NetInput localInput; // same server::NetInput mapping as the wire
    client::ClientSync sync;
    bool welcomed = false;
    uint32_t inputSeq = 0;
    size_t snapshotsReceived = 0;
    ecs::Entity controlled;
    uint64_t controlledKey = 0;
    bool reconciled = false; // a v1 snap-on-divergence occurred

    bool Start(const std::string& sceneJson, const std::string& scriptDir,
               uint16_t serverPort) {
        scene::GameRuntimeConfig rcfg;
        rcfg.assets = nullptr;
        rcfg.headless = true;
        rcfg.scriptBaseDir = scriptDir;
        rcfg.rngSeed = 424242; // must match the server's seed
        rcfg.input = &localInput;
        core::Status st = local.Start(sceneJson, rcfg);
        if (!st.Ok()) return false;

        core::Result<net::UdpSocket> res = net::UdpSocket::Create();
        if (!res.Ok()) return false;
        sock = std::move(res.Value());
        if (!sock.BindLoopback(0).Ok()) return false;
        if (!sock.SetPeer(net::NetAddress{"127.0.0.1", serverPort}).Ok()) return false;
        chan.SetOutbound([this](const std::vector<uint8_t>& bytes) {
            if (sock.Valid()) sock.Send(bytes.data(), bytes.size());
        });
        chan.SetDeliver([this](const net::DecodedMessage& m) { OnMessage(m); });
        return true;
    }

    void OnMessage(const net::DecodedMessage& m) {
        if (m.header.msgId == static_cast<uint8_t>(net::MsgType::Welcome)) {
            welcomed = true;
        } else if (m.header.msgId == static_cast<uint8_t>(net::MsgType::Snapshot)) {
            sync.OnSnapshot(std::get<net::MsgSnapshot>(m.payload));
            ++snapshotsReceived;
        } else if (m.header.msgId == static_cast<uint8_t>(net::MsgType::Despawn)) {
            sync.OnDespawn(std::get<net::MsgDespawn>(m.payload).entityId);
        }
    }

    void Pump(uint64_t now) {
        uint8_t buf[4096];
        for (;;) {
            core::Result<net::RecvPacket> r = sock.RecvFrom(buf, sizeof(buf));
            if (!r.Ok() || r.Value().size == 0) break;
            chan.OnDatagram(buf, r.Value().size);
        }
        chan.Tick(now);
    }

    void SendJoin() {
        net::MsgJoin m{"client", 2};
        CHECK(chan.Send(static_cast<uint8_t>(net::MsgType::Join), server::EncodeBody(m)).Ok());
    }

    // Sends the scripted input for `tick` over the wire.
    void SendScripted(uint32_t tick) {
        const net::MsgInput* in = server::InputForTick(server::ScriptedInputs(), tick);
        const uint8_t buttons = in ? in->buttons : 0;
        const float mx = in ? in->moveX : 0.0f;
        const float my = in ? in->moveY : 0.0f;
        net::MsgInput m{inputSeq++, buttons, mx, my};
        core::Status st = chan.Send(static_cast<uint8_t>(net::MsgType::Input),
                                    server::EncodeBody(m));
        CHECK(st.Ok());
    }

    // Local prediction with the SAME scripted input for `tick`.
    void StepLocalScripted(uint32_t tick) {
        const net::MsgInput* in = server::InputForTick(server::ScriptedInputs(), tick);
        localInput.SetInput(in ? in->buttons : 0, in ? in->moveX : 0, in ? in->moveY : 0);
        local.Tick(1.0f / 60.0f);
        localInput.EndFrame();
    }

    // Idle local step (keeps the local tick count aligned with the server
    // before the controller input starts).
    void StepLocalIdle() {
        localInput.SetInput(0, 0.0f, 0.0f);
        local.Tick(1.0f / 60.0f);
        localInput.EndFrame();
    }

    void ResolveControlled() {
        if (controlledKey != 0) return;
        auto view = local.World().ViewAll<script::CTransformBind>();
        for (size_t i = 0; i < view.Size(); ++i) {
            ecs::Entity e = local.World().EntityAt<script::CTransformBind>(i);
            controlled = e;
            controlledKey = EntityKeyOf(e);
            break;
        }
    }

    void Reconcile() {
        if (controlledKey == 0) return;
        script::CTransformBind* t = local.World().Get<script::CTransformBind>(controlled);
        if (!t) return;
        math::Vec3 correction;
        if (sync.NeedsReconcile(controlledKey, t->pos, &correction)) {
            t->pos = correction;
            reconciled = true; // v1: corrected by the authoritative snapshot
        }
    }

    math::Vec3 ControlledPos() {
        const script::CTransformBind* t = local.World().Get<script::CTransformBind>(controlled);
        return t ? t->pos : math::Vec3{};
    }
};

} // namespace

// ---------------------------------------------------------------------------
// The scripted-input helper itself: a fixed, documented, stable stream.
// ---------------------------------------------------------------------------

TEST(DeterminismScriptedSequenceIsStable) {
    const auto seq = server::ScriptedInputs();
    // Explicit entries: ticks 0..59 forward, tick 60 jump, tick 90 interact.
    CHECK_EQ(seq.size(), 62u);
    CHECK_EQ(seq[0].tick, 0u);
    CHECK_EQ(seq[0].input.moveY, 1.0f);
    CHECK_EQ(seq[59].tick, 59u);
    CHECK_EQ(seq[59].input.moveY, 1.0f);
    // Tick 60: forward released, jump pressed (one held frame).
    CHECK_EQ(seq[60].tick, 60u);
    CHECK_EQ(seq[60].input.moveY, 0.0f);
    CHECK_EQ(seq[60].input.buttons, server::NetInput::kButtonJump);
    // Tick 90: interact tap.
    CHECK_EQ(seq.back().tick, 90u);
    CHECK_EQ(seq.back().input.buttons, server::NetInput::kButtonInteract);
    // Ticks past the pattern hold no input.
    CHECK(server::InputForTick(seq, 91) == nullptr);
    CHECK(server::InputForTick(seq, 1000) == nullptr);
    // Every explicit tick appears exactly once, in ascending order.
    std::set<uint32_t> ticks;
    uint32_t prev = 0;
    bool ascending = true;
    for (const auto& s : seq) {
        if (!ticks.insert(s.tick).second) ascending = false;
        if (s.tick < prev) ascending = false;
        prev = s.tick;
        CHECK(s.input.seq == s.tick); // seq carries the tick for traceability
    }
    CHECK(ascending);
}

// ---------------------------------------------------------------------------
// THE deterministic-simulation acceptance: the server's authoritative sim and
// the client's local prediction consume the SAME scripted stream for the SAME
// 120 ticks and must end BIT-IDENTICAL (position) with IDENTICAL state hashes.
// ---------------------------------------------------------------------------

TEST(DeterminismServerMatchesClientPrediction) {
    test::TempDir tmp;
    WriteFixture(tmp.Str());
    const uint64_t seed = 424242;

    SimResult server = RunServerScripted(tmp.Str(), seed, kScriptTicks);
    SimResult client = RunClientScripted(tmp.Str(), seed, kScriptTicks);

    // The scripted stream's documented outcome: 60 forward ticks -> z == 60.
    CHECK_NEAR(server.controlledPos.z, 60.0, 1e-9);
    CHECK_EQ(server.ticksVar, static_cast<double>(kScriptTicks));
    CHECK_EQ(server.jumpsVar, 1.0);      // the single jump tap at tick 60
    CHECK_EQ(server.interactsVar, 1.0);  // the single interact tap at tick 90

    // BIT-EXACT final position (IEEE-754: == on floats is exact equality).
    CHECK(server.controlledPos.x == client.controlledPos.x);
    CHECK(server.controlledPos.y == client.controlledPos.y);
    CHECK(server.controlledPos.z == client.controlledPos.z);

    // GameVars advanced identically on both sims.
    CHECK_EQ(server.ticksVar, client.ticksVar);
    CHECK_EQ(server.jumpsVar, client.jumpsVar);
    CHECK_EQ(server.interactsVar, client.interactsVar);

    // State hash (all entity positions + GameVars) matches bit-for-bit.
    CHECK_EQ(server.stateHash, client.stateHash);
}

// The determinism guarantee is not pinned to one seed: ANY seed handed to BOTH
// sims produces an identical state hash, so a seed change never breaks the
// server/client equivalence (both consume the same stream).
TEST(DeterminismHashHoldsAcrossSeeds) {
    test::TempDir tmp;
    WriteFixture(tmp.Str());

    for (uint64_t seed : {777u, 999u, 0xC0FFEEu}) {
        SimResult a = RunServerScripted(tmp.Str(), seed, kScriptTicks);
        SimResult b = RunClientScripted(tmp.Str(), seed, kScriptTicks);
        CHECK_EQ(a.stateHash, b.stateHash);
        CHECK(a.controlledPos.x == b.controlledPos.x);
        CHECK(a.controlledPos.y == b.controlledPos.y);
        CHECK(a.controlledPos.z == b.controlledPos.z);
        CHECK_EQ(a.ticksVar, b.ticksVar);
        CHECK_EQ(a.jumpsVar, b.jumpsVar);
        CHECK_EQ(a.interactsVar, b.interactsVar);
    }
}

// ---------------------------------------------------------------------------
// Live-loopback variant: a real client over a real server, both fed the same
// scripted stream. The prediction tracks the authoritative server tick-for-tick
// on a lossless loopback. CAVEAT (documented): the v1 client reconciles by
// SNAPPING on divergence (no replay), so on a lossy/latency-different network
// the client prediction would be corrected — the live variant is guaranteed
// only within the reconcile bound. The PURE two-sim comparison above is the
// bit-exact deterministic proof.
// ---------------------------------------------------------------------------

TEST(DeterminismLiveLoopbackScriptedTracksServer) {
    test::TempDir tmp;
    WriteFixture(tmp.Str());
    const std::string sceneJson = ReadScene(tmp.Str());

    server::GameServer server;
    server::GameServer::Config cfg;
    cfg.port = 0;
    cfg.loopback = true;
    cfg.sceneJsonPath = tmp.Str() + "/scene.json";
    cfg.scriptBaseDir = tmp.Str();
    cfg.rngSeed = 424242;
    CHECK(server.Start(cfg));

    ScriptedLoopbackClient client;
    CHECK(client.Start(sceneJson, tmp.Str(), server.Port()));
    client.SendJoin();

    uint64_t now = 0;
    uint32_t step = 0;

    // Pre-join idle phase: the server has no controller input yet; the local
    // prediction idles identically, so both sims stay on the same tick count
    // before the scripted stream starts.
    for (int i = 0; i < 200 && !client.welcomed; ++i) {
        now += 17;
        server.Step(now);
        client.Pump(now);
        client.StepLocalIdle();
        client.ResolveControlled();
        ++step;
    }
    CHECK(client.welcomed);
    CHECK_EQ(server.ClientCount(), 1u);
    CHECK((server.ControllerClientId()) != (0u));

    // Scripted phase: the wire input for `step` is sent BEFORE the server's
    // Step of `step`, so the server applies it to the same tick the local
    // prediction does.
    for (uint32_t i = 0; i < kScriptTicks; ++i) {
        client.SendScripted(step);
        now += 17;
        server.Step(now);
        client.Pump(now);
        client.StepLocalScripted(step);
        client.Reconcile();
        ++step;
    }
    CHECK((client.snapshotsReceived) > (0u));
    CHECK((client.controlledKey) != (0u));

    const math::Vec3 serverPos = ControlledPos(server.World());
    const math::Vec3 localPos = client.ControlledPos();
    // The scripted stream's forward ticks moved the player well clear of its
    // spawn (exactly how far depends on when the join/welcome consumed the
    // first scripted tick; the server and the client agree regardless).
    CHECK((serverPos.z) > (30.0));

    // Live guarantee: the prediction stays within the v1 reconcile bound of
    // the authoritative server.
    CHECK_NEAR(localPos.z, serverPos.z, 2.0);
    CHECK_NEAR(localPos.x, serverPos.x, 1e-4);
    CHECK_NEAR(localPos.y, serverPos.y, 1e-4);

    // With identical scripted inputs on a lossless loopback the prediction
    // never diverged, so no reconciliation snap fired. (If this ever fails,
    // either the sims diverged or input timing slipped — investigate before
    // relaxing the bound.)
    CHECK(!client.reconciled);
    server.Shutdown();
}
