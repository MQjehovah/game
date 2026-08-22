#include <cstdint>
#include <cmath>
#include <string>
#include <vector>

#include "neon/neon.hpp"
#include "game_server.hpp"
#include "client_sync.hpp"
#include "net_input.hpp"
#include "helpers.hpp"

using namespace neon;

// Test-suite 6.4: client snapshot interpolation + prediction reconciliation.
// ---------------------------------------------------------------------------
// ClientSync (pure): interpolation between adjacent snapshots, spawn/despawn
// between snapshots, the ring-buffer eviction, the reconcile threshold query
// and determinism. Then loopback integration: a real GameServer + a thin
// client driver (ClientSync + a local prediction GameRuntime) asserting the
// client receives snapshots and the controlled entity's position converges to
// the server after a forced divergence.

namespace {

// Stable snapshot key: (id << 32) | generation (matches GameServer::EntityKey).
uint64_t Key(uint32_t id, uint32_t gen) {
    return (static_cast<uint64_t>(id) << 32) | static_cast<uint64_t>(gen);
}

// One script entity: on_start spawns a script-movable "player" (CTransformBind
// via Spawn/SetPosition); every tick increments the "ticks" GameVar and, when
// InputAxis("forward") > 0.5, the player's z advances by 1 per tick. Same
// fixture as test_server.cpp (both sides must run the identical scene).
const char* kControllerLua = R"(
function on_start(e)
  player = Spawn("player", { x = 0, y = 0, z = 0 })
  SetVar("ticks", 0)
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

void WriteSceneFixture(const std::string& dir) {
    CHECK(test::WriteFileAll(dir + "/scene.json", kSceneJson));
    const std::string scriptsDir = dir + "/scripts";
#if defined(_WIN32)
    CreateDirectoryA(scriptsDir.c_str(), nullptr);
#else
    ::mkdir(scriptsDir.c_str(), 0700);
#endif
    CHECK(test::WriteFileAll(scriptsDir + "/controller.lua", kControllerLua));
}

std::string ReadScene(const std::string& dir) {
    std::string text;
    CHECK(test::ReadFileAll(dir + "/scene.json", text));
    return text;
}

server::GameServer::Config SceneCfg(const std::string& dir, uint64_t seed) {
    server::GameServer::Config cfg;
    cfg.port = 0;
    cfg.loopback = true;
    cfg.sceneJsonPath = dir + "/scene.json";
    cfg.scriptBaseDir = dir;
    cfg.rngSeed = seed;
    return cfg;
}

// A thin networked client driver: a real UDP socket + ReliableChannel wired to
// a loopback GameServer, a ClientSync fed by the snapshot stream, and a LOCAL
// headless GameRuntime running the identical scene for prediction. The driver
// does exactly what the player's --connect mode does: send input to the server,
// step local prediction with the same input, and reconcile the controlled
// entity against the latest snapshot.
struct LoopbackDriver {
    net::UdpSocket sock;
    net::ReliableChannel chan;
    client::ClientSync sync;
    scene::GameRuntime local;
    server::NetInput localInput; // moveY = +1 -> InputAxis("forward")
    bool welcomed = false;
    uint32_t inputSeq = 0;
    size_t snapshotsReceived = 0;
    ecs::Entity controlled;
    uint64_t controlledKey = 0;
    math::Vec3 controlledStart;

    bool Start(const std::string& sceneJson, const std::string& scriptDir,
               uint16_t serverPort) {
        scene::GameRuntimeConfig rcfg;
        rcfg.assets = nullptr;
        rcfg.headless = true;
        rcfg.scriptBaseDir = scriptDir;
        rcfg.rngSeed = 20260821u;
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

    // Drain the socket + tick the channel (emits acks).
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

    void SendMoveForward() {
        net::MsgInput m{inputSeq++, 0, 0.0f, 1.0f};
        core::Status st = chan.Send(static_cast<uint8_t>(net::MsgType::Input),
                                    server::EncodeBody(m));
        CHECK(st.Ok());
    }

    // Finds the script-spawned entity (the controlled player) in the LOCAL
    // runtime world. Idempotent; the script spawns it in on_start at Start().
    void ResolveControlled() {
        if (controlledKey != 0) return;
        auto view = local.World().ViewAll<script::CTransformBind>();
        for (size_t i = 0; i < view.Size(); ++i) {
            ecs::Entity e = local.World().EntityAt<script::CTransformBind>(i);
            controlled = e;
            controlledKey = Key(e.id, e.generation);
            const script::CTransformBind* t = local.World().Get<script::CTransformBind>(e);
            if (t) controlledStart = t->pos;
            break;
        }
    }

    // Local prediction: feed the same forward input the wire carries, step the
    // local runtime one fixed tick.
    void StepLocal() {
        localInput.SetInput(0, 0.0f, 1.0f);
        local.Tick(1.0f / 60.0f);
        localInput.EndFrame();
    }

    // Reconciliation: snap the controlled entity to the server when it has
    // diverged past the threshold.
    void Reconcile() {
        if (controlledKey == 0) return;
        script::CTransformBind* t = local.World().Get<script::CTransformBind>(controlled);
        if (!t) return;
        math::Vec3 correction;
        if (sync.NeedsReconcile(controlledKey, t->pos, &correction)) t->pos = correction;
    }

    math::Vec3 ControlledPos() {
        const script::CTransformBind* t = local.World().Get<script::CTransformBind>(controlled);
        return t ? t->pos : math::Vec3{};
    }
};

// The server's latest snapshot z for the controlled entity key.
double ServerControlledZ(LoopbackDriver& c) {
    const net::MsgSnapshot* latest = c.sync.Latest();
    if (!latest) return -1e9;
    for (const net::SnapshotEntity& e : latest->entities)
        if (e.id == c.controlledKey) return static_cast<double>(e.z);
    return -1e9;
}

} // namespace

// ---------------------------------------------------------------------------
// Pure interpolation / reconciliation math
// ---------------------------------------------------------------------------

TEST(ClientSyncInterpolatesBetweenSnapshots) {
    client::ClientSync sync;
    const uint64_t key = Key(1, 1);
    net::MsgSnapshot a;
    a.tick = 100;
    a.entities.push_back(net::SnapshotEntity{key, 0, 0, 0, 0});
    net::MsgSnapshot b;
    b.tick = 110;
    b.entities.push_back(net::SnapshotEntity{key, 10, 0, 0, 0});
    sync.OnSnapshot(a);
    sync.OnSnapshot(b);
    CHECK_EQ(sync.BufferedSnapshots(), 2u);
    CHECK_NEAR(sync.CurrentServerTick(), 110.0, 1e-9);

    // Midpoint: lerp(S_a, S_b, 0.5) = (5,0,0).
    auto mid = sync.Sample(key, 105.0);
    CHECK(mid.Ok());
    CHECK_NEAR(mid.Value().pos.x, 5.0, 1e-4);
    // Exact snapshot ticks -> exact positions.
    auto start = sync.Sample(key, 100.0);
    CHECK(start.Ok());
    CHECK_NEAR(start.Value().pos.x, 0.0, 1e-4);
    auto end = sync.Sample(key, 110.0);
    CHECK(end.Ok());
    CHECK_NEAR(end.Value().pos.x, 10.0, 1e-4);
    // Clamp: before the oldest / past the newest hold the nearest state.
    auto before = sync.Sample(key, 99.0);
    CHECK_NEAR(before.Value().pos.x, 0.0, 1e-4);
    auto after = sync.Sample(key, 111.0);
    CHECK_NEAR(after.Value().pos.x, 10.0, 1e-4);
    // Unknown entity -> Err.
    CHECK(!sync.Sample(Key(99, 1), 105.0).Ok());
}

TEST(ClientSyncInterpolatesYawShortestArc) {
    client::ClientSync sync;
    const uint64_t key = Key(1, 1);
    net::MsgSnapshot a;
    a.tick = 10;
    a.entities.push_back(net::SnapshotEntity{key, 0, 0, 0, -3.0f}); // ~ -171 deg
    net::MsgSnapshot b;
    b.tick = 20;
    b.entities.push_back(net::SnapshotEntity{key, 0, 0, 0, 3.0f}); // ~ +171 deg
    sync.OnSnapshot(a);
    sync.OnSnapshot(b);
    // Shortest arc between ~-171 and ~+171 deg crosses ±180 deg (NOT through
    // 0): the midpoint heading is ~pi, proving yaw wraps instead of lerping
    // the long way through zero.
    auto mid = sync.Sample(key, 15.0);
    CHECK(mid.Ok());
    CHECK_NEAR(std::fabs(mid.Value().yaw), math::kPi, 1e-3);
}

TEST(ClientSyncReconcileSnapsOnDivergence) {
    client::ClientSync::Config cfg;
    cfg.reconcileThreshold = 2.0f;
    client::ClientSync sync(cfg);
    const uint64_t key = Key(2, 3);
    net::MsgSnapshot a;
    a.tick = 50;
    a.entities.push_back(net::SnapshotEntity{key, 10, 0, 0, 0});
    sync.OnSnapshot(a);

    // Far divergence (6 > 2): needs reconcile, correction = server position.
    math::Vec3 farPos{16, 0, 0};
    math::Vec3 correction;
    CHECK(sync.NeedsReconcile(key, farPos, &correction));
    CHECK_NEAR(correction.x, 10.0, 1e-5);
    CHECK_NEAR(correction.y, 0.0, 1e-5);
    CHECK_NEAR(correction.z, 0.0, 1e-5);
    // Within threshold (0.5 < 2): no snap.
    CHECK(!sync.NeedsReconcile(key, math::Vec3{10.5f, 0, 0}, nullptr));
    // On the threshold: not beyond -> no snap.
    CHECK(!sync.NeedsReconcile(key, math::Vec3{12.0f, 0, 0}, nullptr));
    // Unknown entity -> no correction.
    CHECK(!sync.NeedsReconcile(Key(9, 1), farPos, nullptr));
    // Empty buffer -> no correction.
    client::ClientSync empty(cfg);
    CHECK(!empty.NeedsReconcile(key, farPos, nullptr));

    // Config::controlledEntityId convenience.
    cfg.controlledEntityId = key;
    client::ClientSync sync2(cfg);
    sync2.OnSnapshot(a);
    math::Vec3 c2;
    CHECK(sync2.CheckControlled(farPos, &c2));
    CHECK_NEAR(c2.x, 10.0, 1e-5);
}

TEST(ClientSyncSpawnDespawnBetweenSnapshots) {
    client::ClientSync sync;
    const uint64_t keyA = Key(1, 1);
    const uint64_t keyB = Key(2, 1);
    net::MsgSnapshot a;
    a.tick = 10;
    a.entities.push_back(net::SnapshotEntity{keyA, 1, 0, 0, 0});
    net::MsgSnapshot b;
    b.tick = 20;
    b.entities.push_back(net::SnapshotEntity{keyB, 7, 0, 0, 0});
    sync.OnSnapshot(a);
    sync.OnSnapshot(b);

    // A was in S_a but not S_b: holds S_a's position between the snapshots.
    auto holdA = sync.Sample(keyA, 15.0);
    CHECK(holdA.Ok());
    CHECK_NEAR(holdA.Value().pos.x, 1.0, 1e-5);
    // B appeared in S_b but not S_a: shows at its S_b position.
    auto appearB = sync.Sample(keyB, 15.0);
    CHECK(appearB.Ok());
    CHECK_NEAR(appearB.Value().pos.x, 7.0, 1e-5);

    // After MsgDespawn(A): gone from sampling until it reappears.
    sync.OnDespawn(keyA);
    CHECK(!sync.Sample(keyA, 15.0).Ok());
    // Reappears in a later snapshot: despawned flag clears.
    net::MsgSnapshot c;
    c.tick = 30;
    c.entities.push_back(net::SnapshotEntity{keyA, 11, 0, 0, 0});
    sync.OnSnapshot(c);
    auto back = sync.Sample(keyA, 25.0);
    CHECK(back.Ok());
    CHECK_NEAR(back.Value().pos.x, 11.0, 1e-5);
}

TEST(ClientSyncRingBufferEvictsOldest) {
    client::ClientSync::Config cfg;
    cfg.maxSnapshots = 3;
    client::ClientSync sync(cfg);
    const uint64_t key = Key(1, 1);
    for (int t = 1; t <= 5; ++t) {
        net::MsgSnapshot s;
        s.tick = static_cast<uint32_t>(t);
        s.entities.push_back(net::SnapshotEntity{key, static_cast<float>(t), 0, 0, 0});
        sync.OnSnapshot(s);
    }
    CHECK_EQ(sync.BufferedSnapshots(), 3u);
    // Interpolation between the surviving snapshots still works.
    auto s = sync.Sample(key, 3.5);
    CHECK(s.Ok());
    CHECK_NEAR(s.Value().pos.x, 3.5, 1e-4);
    // The evicted oldest (t=1) is gone; a render tick before the window clamps
    // to the new oldest (t=3).
    auto clamped = sync.Sample(key, 1.0);
    CHECK(clamped.Ok());
    CHECK_NEAR(clamped.Value().pos.x, 3.0, 1e-4);
}

TEST(ClientSyncIgnoresStaleAndDuplicateSnapshots) {
    client::ClientSync sync;
    const uint64_t key = Key(1, 1);
    net::MsgSnapshot s;
    s.tick = 10;
    s.entities.push_back(net::SnapshotEntity{key, 1, 0, 0, 0});
    sync.OnSnapshot(s);
    CHECK_EQ(sync.BufferedSnapshots(), 1u);
    // Older tick: ignored.
    net::MsgSnapshot stale;
    stale.tick = 5;
    stale.entities.push_back(net::SnapshotEntity{key, -1, 0, 0, 0});
    sync.OnSnapshot(stale);
    CHECK_EQ(sync.BufferedSnapshots(), 1u);
    // Duplicate tick: ignored.
    sync.OnSnapshot(s);
    CHECK_EQ(sync.BufferedSnapshots(), 1u);
    // Clear resets everything.
    sync.Clear();
    CHECK_EQ(sync.BufferedSnapshots(), 0u);
    CHECK(!sync.Sample(key, 10.0).Ok());
}

TEST(ClientSyncInterpolationDeterministic) {
    // Two instances fed an identical snapshot stream produce identical
    // interpolated positions at every render tick (no hidden RNG).
    const uint64_t key = Key(1, 1);
    std::vector<net::MsgSnapshot> stream;
    for (int t = 0; t <= 20; ++t) {
        net::MsgSnapshot s;
        s.tick = static_cast<uint32_t>(t);
        s.entities.push_back(net::SnapshotEntity{key, static_cast<float>(t) * 1.5f,
                                                 0.0f, static_cast<float>(t) * 0.25f,
                                                 static_cast<float>(t) * 0.1f});
        stream.push_back(s);
    }

    std::vector<math::Vec3> a, b;
    client::ClientSync sa, sb;
    for (const net::MsgSnapshot& s : stream) {
        sa.OnSnapshot(s);
        sb.OnSnapshot(s);
    }
    for (double rt = 0.0; rt <= 20.0; rt += 0.25) {
        auto ra = sa.Sample(key, rt);
        auto rb = sb.Sample(key, rt);
        CHECK(ra.Ok());
        CHECK(rb.Ok());
        a.push_back(ra.Value().pos);
        b.push_back(rb.Value().pos);
    }
    CHECK_EQ(a.size(), b.size());
    CHECK((a.size()) > (0u));
    for (size_t i = 0; i < a.size(); ++i) {
        CHECK_NEAR(a[i].x, b[i].x, 1e-6);
        CHECK_NEAR(a[i].y, b[i].y, 1e-6);
        CHECK_NEAR(a[i].z, b[i].z, 1e-6);
    }
}

// ---------------------------------------------------------------------------
// Loopback integration: real server + thin client driver
// ---------------------------------------------------------------------------

// Drives one full loopback run and records the interpolated trajectory of the
// controlled entity plus the final predicted position. Deterministic: same
// seed + same fake-clock schedule -> identical snapshot stream -> identical
// client trajectory.
struct LoopbackRun {
    std::vector<double> interpZ;
    math::Vec3 finalControlledPos;
    size_t snapshots = 0;
    double finalServerZ = 0.0;
};

LoopbackRun RunLoopback(const std::string& sceneJson, const std::string& dir,
                        uint64_t seed) {
    server::GameServer server;
    CHECK(server.Start(SceneCfg(dir, seed)));
    LoopbackDriver client;
    CHECK(client.Start(sceneJson, dir, server.Port()));
    client.SendJoin();

    uint64_t now = 0;
    auto stepAll = [&]() {
        now += 17; // ~1 fixed 60Hz step per Step() call
        server.Step(now);
        client.Pump(now);
        client.SendMoveForward();
        client.ResolveControlled();
        client.StepLocal();
        client.Reconcile();
    };

    for (int i = 0; i < 400 && !client.welcomed; ++i) stepAll();
    CHECK(client.welcomed);

    LoopbackRun run;
    for (int i = 0; i < 60; ++i) {
        stepAll();
        if (client.controlledKey != 0) {
            const double rt =
                client.sync.CurrentServerTick() - static_cast<double>(client::kInterpDelayTicks);
            auto s = client.sync.Sample(client.controlledKey, rt);
            if (s.Ok()) run.interpZ.push_back(s.Value().pos.z);
        }
    }
    run.finalControlledPos = client.ControlledPos();
    run.snapshots = client.snapshotsReceived;
    run.finalServerZ = ServerControlledZ(client);
    server.Shutdown();
    return run;
}

TEST(ClientSyncLoopbackConvergence) {
    test::TempDir tmp;
    WriteSceneFixture(tmp.Str());
    const std::string json = ReadScene(tmp.Str());
    server::GameServer server;
    CHECK(server.Start(SceneCfg(tmp.Str(), 12345)));
    LoopbackDriver client;
    CHECK(client.Start(json, tmp.Str(), server.Port()));
    client.SendJoin();

    uint64_t now = 0;
    auto stepAll = [&]() {
        now += 17;
        server.Step(now);
        client.Pump(now);
        client.SendMoveForward();
        client.ResolveControlled();
        client.StepLocal();
        client.Reconcile();
    };

    for (int i = 0; i < 400 && !client.welcomed; ++i) stepAll();
    CHECK(client.welcomed);
    for (int i = 0; i < 60; ++i) stepAll();
    CHECK((client.snapshotsReceived) > (0u));
    CHECK((client.controlledKey) != (0u));

    // Same input on both sides: the local prediction and the server stay close
    // (within the reconcile threshold -> no constant snapping).
    double serverZ = ServerControlledZ(client);
    CHECK_NEAR(client.ControlledPos().z, serverZ, 6.0);

    // Force a divergence: teleport the local controlled entity far away.
    script::CTransformBind* t =
        client.local.World().Get<script::CTransformBind>(client.controlled);
    CHECK(t != nullptr);
    t->pos = {0, 0, 500};

    // The next snapshot diverges past the threshold -> reconciliation snaps
    // the local entity back to the server, then both advance together again.
    bool snapped = false;
    for (int i = 0; i < 12 && !snapped; ++i) {
        stepAll();
        serverZ = ServerControlledZ(client);
        if (std::fabs(static_cast<double>(client.ControlledPos().z) - serverZ) < 3.0)
            snapped = true;
    }
    CHECK(snapped);
    CHECK_NEAR(client.ControlledPos().z, serverZ, 3.0);
    server.Shutdown();
}

TEST(ClientSyncLoopbackDeterminism) {
    test::TempDir tmp;
    WriteSceneFixture(tmp.Str());
    const std::string json = ReadScene(tmp.Str());

    LoopbackRun a = RunLoopback(json, tmp.Str(), 777);
    LoopbackRun b = RunLoopback(json, tmp.Str(), 777);
    CHECK((a.snapshots) > (0u));
    CHECK_EQ(a.snapshots, b.snapshots);
    CHECK_EQ(a.interpZ.size(), b.interpZ.size());
    CHECK((a.interpZ.size()) > (0u));
    for (size_t i = 0; i < a.interpZ.size(); ++i) CHECK_NEAR(a.interpZ[i], b.interpZ[i], 1e-6);
    CHECK_NEAR(a.finalControlledPos.z, b.finalControlledPos.z, 1e-5);
    CHECK_NEAR(a.finalServerZ, b.finalServerZ, 1e-5);
}
