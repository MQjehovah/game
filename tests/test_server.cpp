#include <cstdint>
#include <string>
#include <vector>

#include "neon/neon.hpp"
#include "game_server.hpp"
#include "helpers.hpp"

using namespace neon;

// Test-suite 6.3: headless authoritative server.
// ---------------------------------------------------------------------------
// Real loopback integration: a test client (net::UdpSocket + ReliableChannel)
// joins, sends inputs, and receives MsgSnapshot streams from a GameServer
// driven by a deterministic fake clock. The world (script GameVars + the
// spawned player's position) advances exactly with the server's fixed 60Hz
// steps, and two identical runs produce identical snapshot streams.

namespace {

// One script entity: on_start spawns a script-movable "player" (CTransformBind
// via Spawn/SetPosition); every tick increments the "ticks" GameVar and, when
// InputAxis("forward") > 0.5 (the controller's moveY > 0.5), the player's z
// advances by 1 per tick.
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

// Writes the scene + script into `dir` (a live TempDir; the scripts/ subdir is
// created under it). The runtime resolves scripts/<base>/scripts/controller.lua.
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

// A minimal loopback client: a real UDP socket + ReliableChannel wired to the
// server, collecting Welcome/Snapshot/Despawn messages as they arrive.
struct LoopbackClient {
    net::UdpSocket sock;
    net::ReliableChannel chan;
    bool welcomed = false;
    uint64_t clientId = 0;
    uint32_t welcomeTick = 0;
    size_t pongs = 0;
    std::vector<net::MsgSnapshot> snapshots;
    std::vector<uint64_t> despawned;

    void BindAndPeer(uint16_t serverPort) {
        core::Result<net::UdpSocket> res = net::UdpSocket::Create();
        CHECK(res.Ok());
        sock = std::move(res.Value());
        CHECK(sock.BindLoopback(0).Ok());
        CHECK(sock.SetPeer(net::NetAddress{"127.0.0.1", serverPort}).Ok());
        chan.SetOutbound([this](const std::vector<uint8_t>& bytes) {
            if (sock.Valid()) sock.Send(bytes.data(), bytes.size());
        });
        chan.SetDeliver([this](const net::DecodedMessage& m) { OnMessage(m); });
    }

    void OnMessage(const net::DecodedMessage& m) {
        if (m.header.msgId == static_cast<uint8_t>(net::MsgType::Welcome)) {
            const net::MsgWelcome& w = std::get<net::MsgWelcome>(m.payload);
            welcomed = true;
            clientId = w.clientId;
            welcomeTick = w.tick;
        } else if (m.header.msgId == static_cast<uint8_t>(net::MsgType::Snapshot)) {
            snapshots.push_back(std::get<net::MsgSnapshot>(m.payload));
        } else if (m.header.msgId == static_cast<uint8_t>(net::MsgType::Despawn)) {
            despawned.push_back(std::get<net::MsgDespawn>(m.payload).entityId);
        } else if (m.header.msgId == static_cast<uint8_t>(net::MsgType::Pong)) {
            ++pongs;
        }
    }

    // Drain everything the socket has + tick the channel (emits acks).
    void Pump(uint64_t now) {
        uint8_t buf[4096];
        for (;;) {
            core::Result<size_t> r = sock.Recv(buf, sizeof(buf));
            if (!r.Ok() || r.Value() == 0) break;
            chan.OnDatagram(buf, r.Value());
        }
        chan.Tick(now);
    }

    void SendJoin(const std::string& name, uint32_t version) {
        net::MsgJoin m{name, version};
        CHECK(chan.Send(static_cast<uint8_t>(net::MsgType::Join), server::EncodeBody(m)).Ok());
    }

    void SendInput(uint32_t seq, uint8_t buttons, float moveX, float moveY) {
        net::MsgInput m{seq, buttons, moveX, moveY};
        CHECK(chan.Send(static_cast<uint8_t>(net::MsgType::Input), server::EncodeBody(m)).Ok());
    }

    void SendPing(uint64_t t) {
        net::MsgPing m{t};
        CHECK(chan.Send(static_cast<uint8_t>(net::MsgType::Ping), server::EncodeBody(m)).Ok());
    }
};

// True when any replicated entity has moved forward (z > 0.5): the script
// player moved in response to input.
bool AnyEntityMoved(const std::vector<net::MsgSnapshot>& snaps) {
    if (snaps.empty()) return false;
    for (const net::SnapshotEntity& e : snaps.back().entities)
        if (e.z > 0.5f) return true;
    return false;
}

// Builds a server config for the temp-dir scene fixture.
server::GameServer::Config SceneCfg(const std::string& dir, uint64_t seed) {
    server::GameServer::Config cfg;
    cfg.port = 0;
    cfg.loopback = true;
    cfg.sceneJsonPath = dir + "/scene.json";
    cfg.scriptBaseDir = dir;
    cfg.rngSeed = seed;
    return cfg;
}

} // namespace

// ---------------------------------------------------------------------------
// Core loopback flow: join -> welcome -> input -> snapshots -> world advanced
// ---------------------------------------------------------------------------

TEST(ServerJoinInputSnapshotAndAdvance) {
    test::TempDir tmp;
    WriteSceneFixture(tmp.Str());
    server::GameServer server;
    CHECK(server.Start(SceneCfg(tmp.Str(), 4242)));
    CHECK(server.Running());
    CHECK((server.Port()) > (0u));

    LoopbackClient client;
    client.BindAndPeer(server.Port());

    uint64_t now = 0;
    auto stepBoth = [&]() {
        now += 17; // ~1 fixed 60Hz step per Step() call
        server.Step(now);
        client.Pump(now);
    };

    // Join -> MsgWelcome.
    client.SendJoin("tester", 2);
    for (int i = 0; i < 200 && !client.welcomed; ++i) stepBoth();
    CHECK(client.welcomed);
    CHECK((client.clientId) > (0u));
    CHECK_EQ(server.ClientCount(), 1u);
    CHECK_EQ(server.ControllerClientId(), client.clientId);

    // No input for 30 steps: the world still advances (GameVar per tick), but
    // no entity moves.
    const int tickAtJoin = server.CurrentTick();
    for (int i = 0; i < 30; ++i) stepBoth();
    CHECK((server.CurrentTick()) > (static_cast<uint32_t>(tickAtJoin)));
    CHECK(!client.snapshots.empty());
    CHECK_EQ(server.GameVars().Get("ticks").number, static_cast<double>(server.CurrentTick()));
    CHECK(!AnyEntityMoved(client.snapshots));

    // Controller sends moveY=1 (InputAxis("forward") -> +1): the spawned
    // player advances z by 1 per subsequent fixed tick.
    client.SendInput(1, 0, 0.0f, 1.0f);
    const int tickBeforeMove = server.CurrentTick();
    for (int i = 0; i < 30; ++i) stepBoth();
    const int tickAfterMove = server.CurrentTick();
    CHECK((tickAfterMove) > (tickBeforeMove));

    const net::MsgSnapshot& last = client.snapshots.back();
    CHECK((last.tick) > (static_cast<uint32_t>(tickAtJoin)));
    CHECK((last.entityCount) > (0u));
    CHECK(last.entities.size() == last.entityCount);

    // The replicated world advanced in response to the input: one moved entity,
    // moved by (ticks after input) units of z.
    int movedCount = 0;
    float movedZ = 0.0f;
    for (const net::SnapshotEntity& e : last.entities)
        if (e.z > 0.5f) {
            ++movedCount;
            movedZ = e.z;
        }
    CHECK_EQ(movedCount, 1);
    CHECK_NEAR(movedZ, static_cast<float>(tickAfterMove - tickBeforeMove), 1.0f);

    // Snapshots arrive strictly in order via the reliable channel.
    for (size_t i = 1; i < client.snapshots.size(); ++i)
        CHECK((client.snapshots[i].tick) > (client.snapshots[i - 1].tick));

    server.Shutdown();
    CHECK(!server.Running());
}

// ---------------------------------------------------------------------------
// Determinism: two identical runs (same scene, seed, Step sequence and input)
// produce identical world state AND an identical snapshot stream.
// ---------------------------------------------------------------------------

namespace {

struct RunResult {
    uint32_t finalTick = 0;
    double ticksVar = 0.0;
    std::vector<net::MsgSnapshot> snaps;
};

RunResult RunScenario(uint64_t seed) {
    test::TempDir tmp;
    WriteSceneFixture(tmp.Str());
    server::GameServer server;
    CHECK(server.Start(SceneCfg(tmp.Str(), seed)));
    LoopbackClient client;
    client.BindAndPeer(server.Port());

    uint64_t now = 0;
    auto stepBoth = [&]() {
        now += 17;
        server.Step(now);
        client.Pump(now);
    };
    client.SendJoin("tester", 2);
    for (int i = 0; i < 200 && !client.welcomed; ++i) stepBoth();
    CHECK(client.welcomed);
    for (int i = 0; i < 30; ++i) stepBoth();
    client.SendInput(1, 0, 0.0f, 1.0f);
    for (int i = 0; i < 30; ++i) stepBoth();
    // Settle: drain any in-flight datagrams without advancing the sim.
    for (int i = 0; i < 50; ++i) {
        client.Pump(now);
        now += 1;
    }

    RunResult r;
    r.finalTick = server.CurrentTick();
    r.ticksVar = server.GameVars().Get("ticks").number;
    r.snaps = client.snapshots;
    server.Shutdown();
    return r;
}

} // namespace

TEST(ServerDeterministicAcrossRuns) {
    RunResult a = RunScenario(777);
    RunResult b = RunScenario(777);
    CHECK((a.finalTick) > (0u));
    CHECK_EQ(a.finalTick, b.finalTick);
    CHECK_EQ(a.ticksVar, b.ticksVar);
    CHECK_EQ(a.snaps.size(), b.snaps.size());
    CHECK((a.snaps.size()) > (0u));
    for (size_t i = 0; i < a.snaps.size(); ++i) {
        const net::MsgSnapshot& x = a.snaps[i];
        const net::MsgSnapshot& y = b.snaps[i];
        CHECK_EQ(x.tick, y.tick);
        CHECK_EQ(x.entityCount, y.entityCount);
        CHECK_EQ(x.entities.size(), y.entities.size());
        for (size_t j = 0; j < x.entities.size(); ++j) {
            CHECK_EQ(x.entities[j].id, y.entities[j].id);
            CHECK_NEAR(x.entities[j].x, y.entities[j].x, 1e-4);
            CHECK_NEAR(x.entities[j].y, y.entities[j].y, 1e-4);
            CHECK_NEAR(x.entities[j].z, y.entities[j].z, 1e-4);
            CHECK_NEAR(x.entities[j].yaw, y.entities[j].yaw, 1e-4);
        }
    }
}

// ---------------------------------------------------------------------------
// Client model: unknown senders are ignored, a second join is accepted, and
// only the first joiner (the controller) drives the sim (v1).
// ---------------------------------------------------------------------------

TEST(ServerUnknownSenderIgnoredAndSecondClient) {
    test::TempDir tmp;
    WriteSceneFixture(tmp.Str());
    server::GameServer server;
    CHECK(server.Start(SceneCfg(tmp.Str(), 9090)));

    LoopbackClient a;
    a.BindAndPeer(server.Port());
    uint64_t now = 0;
    auto stepA = [&]() {
        now += 17;
        server.Step(now);
        a.Pump(now);
    };

    a.SendJoin("a", 2);
    for (int i = 0; i < 200 && !a.welcomed; ++i) stepA();
    CHECK(a.welcomed);
    CHECK_EQ(server.ClientCount(), 1u);

    // An unknown sender sends MsgInput without joining -> ignored (no client).
    net::UdpSocket stranger;
    core::Result<net::UdpSocket> sr = net::UdpSocket::Create();
    CHECK(sr.Ok());
    stranger = std::move(sr.Value());
    CHECK(stranger.BindLoopback(0).Ok());
    CHECK(stranger.SetPeer(net::NetAddress{"127.0.0.1", server.Port()}).Ok());
    net::ReliableChannel strangerChan;
    strangerChan.SetOutbound([&](const std::vector<uint8_t>& bytes) {
        stranger.Send(bytes.data(), bytes.size());
    });
    net::MsgInput stray{1, 0, 0.0f, 1.0f};
    CHECK(strangerChan.Send(static_cast<uint8_t>(net::MsgType::Input),
                            server::EncodeBody(stray)).Ok());
    now += 17;
    server.Step(now);
    CHECK_EQ(server.ClientCount(), 1u);
    CHECK_EQ(server.ControllerClientId(), a.clientId);

    // A second client joins via MsgJoin -> accepted; A stays the controller.
    LoopbackClient b;
    b.BindAndPeer(server.Port());
    b.SendJoin("b", 2);
    for (int i = 0; i < 200 && !b.welcomed; ++i) {
        now += 17;
        server.Step(now);
        a.Pump(now);
        b.Pump(now);
    }
    CHECK(b.welcomed);
    CHECK_EQ(server.ClientCount(), 2u);
    CHECK_EQ(server.ControllerClientId(), a.clientId);

    // v1 model: B's input is ignored (the world does not move)...
    b.SendInput(1, 0, 0.0f, 1.0f);
    for (int i = 0; i < 10; ++i) {
        now += 17;
        server.Step(now);
        a.Pump(now);
        b.Pump(now);
    }
    {
        bool moved = false;
        auto view = server.World().ViewAll<script::CTransformBind>();
        for (size_t i = 0; i < view.Size(); ++i) {
            ecs::Entity e = server.World().EntityAt<script::CTransformBind>(i);
            const script::CTransformBind* t = server.World().Get<script::CTransformBind>(e);
            if (t && t->pos.z > 0.5f) moved = true;
        }
        CHECK(!moved);
    }
    // ...while A's input drives it.
    a.SendInput(1, 0, 0.0f, 1.0f);
    for (int i = 0; i < 10; ++i) {
        now += 17;
        server.Step(now);
        a.Pump(now);
        b.Pump(now);
    }
    {
        bool moved = false;
        auto view = server.World().ViewAll<script::CTransformBind>();
        for (size_t i = 0; i < view.Size(); ++i) {
            ecs::Entity e = server.World().EntityAt<script::CTransformBind>(i);
            const script::CTransformBind* t = server.World().Get<script::CTransformBind>(e);
            if (t && t->pos.z > 0.5f) moved = true;
        }
        CHECK(moved);
    }
    server.Shutdown();
}

// ---------------------------------------------------------------------------
// Ping -> pong reply, and inactivity/reliable-channel timeout disconnect.
// ---------------------------------------------------------------------------

TEST(ServerPingPongAndTimeoutDisconnect) {
    test::TempDir tmp;
    WriteSceneFixture(tmp.Str());
    server::GameServer server;
    server::GameServer::Config cfg = SceneCfg(tmp.Str(), 31337);
    cfg.clientTimeoutMs = 5000;
    CHECK(server.Start(cfg));

    LoopbackClient client;
    client.BindAndPeer(server.Port());
    uint64_t now = 0;
    client.SendJoin("tester", 2);
    for (int i = 0; i < 200 && !client.welcomed; ++i) {
        now += 17;
        server.Step(now);
        client.Pump(now);
    }
    CHECK(client.welcomed);
    CHECK_EQ(server.ClientCount(), 1u);

    // Ping -> pong round trip over the reliable channel.
    client.SendPing(0xA5A5A5A5u);
    now += 17;
    server.Step(now);
    client.Pump(now);
    CHECK((client.pongs) > (0u));

    // The client goes silent while the server keeps stepping: the reliable
    // channel times out (~3s) or the inactivity timer (5s) fires.
    for (int i = 0; i < 500; ++i) {
        now += 17; // 8.5s of server time
        server.Step(now);
    }
    CHECK_EQ(server.ClientCount(), 0u);
    CHECK_EQ(server.ControllerClientId(), 0u);
    server.Shutdown();
}

// ---------------------------------------------------------------------------
// The committed data-driven sample (scripts + BT + prefab) runs headless under
// the server and its GameVars advance per fixed tick.
// ---------------------------------------------------------------------------

TEST(ServerRunsCommittedSampleHeadless) {
    const std::string base = "tests/data/neon_game_sample";
    server::GameServer server;
    server::GameServer::Config cfg;
    cfg.port = 0;
    cfg.loopback = true;
    cfg.sceneJsonPath = base + "/scenes/main.json";
    cfg.scriptBaseDir = base;
    CHECK(server.Start(cfg));
    CHECK(server.Running());
    CHECK((server.World().EntityCount()) > (0u));

    LoopbackClient client;
    client.BindAndPeer(server.Port());
    uint64_t now = 0;
    client.SendJoin("tester", 2);
    for (int i = 0; i < 200 && !client.welcomed; ++i) {
        now += 17;
        server.Step(now);
        client.Pump(now);
    }
    CHECK(client.welcomed);

    for (int i = 0; i < 60; ++i) {
        now += 17;
        server.Step(now);
        client.Pump(now);
    }
    CHECK_EQ(server.GameVars().Get("ticks").number, static_cast<double>(server.CurrentTick()));

    // Snapshots replicate every scene entity (all have transforms).
    CHECK(!client.snapshots.empty());
    CHECK_EQ(client.snapshots.back().entities.size(), server.World().EntityCount());
    server.Shutdown();
}
