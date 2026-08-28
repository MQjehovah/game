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

// A scene with `n` transform-only entities (no scripts) — used to exceed the
// ~48-entity reliable-frame snapshot cap.
std::string BigScene(int n) {
    std::string s = "{\"entities\":[";
    for (int i = 0; i < n; ++i) {
        if (i != 0) s += ",";
        s += "{\"name\":\"e" + std::to_string(i) +
             "\",\"components\":{\"transform\":{\"pos\":[" + std::to_string(i) + ",0,0]}}}";
    }
    s += "]}";
    return s;
}

// A minimal loopback client: a real UDP socket + ReliableChannel wired to the
// server, collecting Welcome/Snapshot/Despawn messages as they arrive.
struct LoopbackClient {
    net::UdpSocket sock;
    net::ReliableChannel chan;
    bool welcomed = false;
    uint64_t clientId = 0;
    uint32_t welcomeTick = 0;
    bool loggedIn = false; // T6.6: MsgLoginOk received
    uint64_t accountId = 0;
    uint32_t loginTick = 0;
    std::vector<net::MsgCharList> charLists;
    size_t pongs = 0;
    std::vector<net::MsgSnapshot> snapshots;
    std::vector<uint64_t> despawned;
    std::vector<net::MsgRpc> rpcs;  // P2-4: received RPC messages
    // B13: in-progress snapshot fragmentation reassembly (mirrors ClientSync).
    uint32_t partTick = 0;
    bool partActive = false;
    uint32_t partCount = 0;
    std::vector<net::SnapshotEntity> partEntities;

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
        } else if (m.header.msgId == static_cast<uint8_t>(net::MsgType::LoginOk)) {
            const net::MsgLoginOk& ok = std::get<net::MsgLoginOk>(m.payload);
            loggedIn = true;
            accountId = ok.accountId;
            loginTick = ok.tick;
        } else if (m.header.msgId == static_cast<uint8_t>(net::MsgType::CharList)) {
            charLists.push_back(std::get<net::MsgCharList>(m.payload));
        } else if (m.header.msgId == static_cast<uint8_t>(net::MsgType::Snapshot)) {
            net::MsgSnapshot snap = std::get<net::MsgSnapshot>(m.payload);
            if (snap.partCount > 1) {
                // B13: reassemble fragments (arrive in order over the reliable
                // channel; retransmits are deduped by sequence number).
                if (!partActive || partTick != snap.tick || partCount != snap.partCount) {
                    partActive = true;
                    partTick = snap.tick;
                    partCount = snap.partCount;
                    partEntities.clear();
                }
                partEntities.insert(partEntities.end(), snap.entities.begin(),
                                    snap.entities.end());
                if (snap.partIndex + 1 == snap.partCount) {
                    net::MsgSnapshot merged;
                    merged.tick = snap.tick;
                    merged.entities = std::move(partEntities);
                    merged.entityCount = static_cast<uint32_t>(merged.entities.size());
                    snapshots.push_back(std::move(merged));
                    partActive = false;
                    partEntities.clear();
                }
            } else {
                snapshots.push_back(std::move(snap));
            }
        } else if (m.header.msgId == static_cast<uint8_t>(net::MsgType::Despawn)) {
            despawned.push_back(std::get<net::MsgDespawn>(m.payload).entityId);
        } else if (m.header.msgId == static_cast<uint8_t>(net::MsgType::Pong)) {
            ++pongs;
        } else if (m.header.msgId == static_cast<uint8_t>(net::MsgType::Rpc)) {
            rpcs.push_back(std::get<net::MsgRpc>(m.payload));
        }
    }

    void SendRpc(const std::string& name, const std::string& argsJson) {
        net::MsgRpc rpc{name, argsJson};
        chan.Send(static_cast<uint8_t>(net::MsgType::Rpc), net::EncodeBody(rpc));
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

    void SendLogin(const std::string& name, uint32_t clientVersion) {
        net::MsgLogin m{name, clientVersion};
        CHECK(chan.Send(static_cast<uint8_t>(net::MsgType::Login), server::EncodeBody(m)).Ok());
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
// T6.6 placeholder auth + character select: login -> MsgLoginOk + MsgCharList,
// then the existing join flow still works.
// ---------------------------------------------------------------------------

TEST(ServerLoginFlowAndCharSelect) {
    test::TempDir tmp;
    WriteSceneFixture(tmp.Str());
    server::GameServer server;
    CHECK(server.Start(SceneCfg(tmp.Str(), 555)));
    CHECK_EQ(server.AccountCount(), 0u);

    LoopbackClient client;
    client.BindAndPeer(server.Port());
    uint64_t now = 0;
    auto stepBoth = [&]() {
        now += 17;
        server.Step(now);
        client.Pump(now);
    };

    // Login -> MsgLoginOk{accountId, tick} + MsgCharList.
    client.SendLogin("alice", net::kProtocolVersion);
    for (int i = 0; i < 200 && !(client.loggedIn && !client.charLists.empty()); ++i)
        stepBoth();
    CHECK(client.loggedIn);
    CHECK((client.accountId) > (0u));
    CHECK_EQ(server.AccountCount(), 1u);
    CHECK_EQ(server.ClientCount(), 1u);

    // The placeholder roster: exactly one character, "主角".
    CHECK_EQ(client.charLists.size(), 1u);
    const net::MsgCharList& list = client.charLists.back();
    CHECK_EQ(list.count, 1u);
    CHECK_EQ(list.characters.size(), 1u);
    CHECK_EQ(list.characters[0].id, 1u);
    CHECK_EQ(list.characters[0].name, "主角");

    // After login, the transport join still proceeds to MsgWelcome.
    client.SendJoin("alice", 2);
    for (int i = 0; i < 200 && !client.welcomed; ++i) stepBoth();
    CHECK(client.welcomed);
    CHECK_EQ(server.ControllerClientId(), client.clientId);
    server.Shutdown();
}

TEST(ServerLoginRejectsEmptyName) {
    test::TempDir tmp;
    WriteSceneFixture(tmp.Str());
    server::GameServer server;
    CHECK(server.Start(SceneCfg(tmp.Str(), 556)));

    LoopbackClient client;
    client.BindAndPeer(server.Port());
    uint64_t now = 0;
    auto stepBoth = [&]() {
        now += 17;
        server.Step(now);
        client.Pump(now);
    };

    // Empty-name login: rejected — no MsgLoginOk, no char list, no account.
    client.SendLogin("", net::kProtocolVersion);
    for (int i = 0; i < 50; ++i) stepBoth();
    CHECK(!client.loggedIn);
    CHECK_EQ(client.charLists.size(), 0u);
    CHECK_EQ(server.AccountCount(), 0u);

    // A subsequent valid login from the same client is accepted.
    client.SendLogin("bob", net::kProtocolVersion);
    for (int i = 0; i < 200 && !(client.loggedIn && !client.charLists.empty()); ++i)
        stepBoth();
    CHECK(client.loggedIn);
    CHECK((client.accountId) > (0u));
    CHECK_EQ(server.AccountCount(), 1u);
    CHECK_EQ(client.charLists.size(), 1u);
    server.Shutdown();
}

TEST(ServerAccountCounterIncrements) {
    test::TempDir tmp;
    WriteSceneFixture(tmp.Str());
    server::GameServer server;
    CHECK(server.Start(SceneCfg(tmp.Str(), 557)));

    LoopbackClient a;
    a.BindAndPeer(server.Port());
    LoopbackClient b;
    b.BindAndPeer(server.Port());
    uint64_t now = 0;
    auto stepBoth = [&]() {
        now += 17;
        server.Step(now);
        a.Pump(now);
        b.Pump(now);
    };

    a.SendLogin("one", net::kProtocolVersion);
    for (int i = 0; i < 200 && !a.loggedIn; ++i) stepBoth();
    CHECK(a.loggedIn);
    b.SendLogin("two", net::kProtocolVersion);
    for (int i = 0; i < 200 && !b.loggedIn; ++i) stepBoth();
    CHECK(b.loggedIn);

    // Each login gets its own account id; the counter advances per login.
    CHECK((a.accountId) != (b.accountId));
    CHECK_EQ(server.AccountCount(), 2u);
    server.Shutdown();
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
// T6.7 two-client LAN smoke: ONE server + TWO clients. A logs in first -> the
// input controller (v1 single-controller); B logs in second -> observer. Both
// receive snapshots; the controller's input moves the player; the observer's
// snapshots show the SAME entity state (bit-exact, same world + same tick).
// ---------------------------------------------------------------------------

TEST(ServerTwoClientLanSmoke) {
    test::TempDir tmp;
    WriteSceneFixture(tmp.Str());
    server::GameServer server;
    CHECK(server.Start(SceneCfg(tmp.Str(), 2026)));
    CHECK_EQ(server.ClientCount(), 0u);

    LoopbackClient a, b;
    a.BindAndPeer(server.Port());
    b.BindAndPeer(server.Port());
    uint64_t now = 0;
    auto stepAll = [&]() {
        now += 17;
        server.Step(now);
        a.Pump(now);
        b.Pump(now);
    };

    // A is the first login -> controller.
    a.SendLogin("alice", net::kProtocolVersion);
    a.SendJoin("alice", 2);
    for (int i = 0; i < 200 && !a.welcomed; ++i) stepAll();
    CHECK(a.welcomed);
    CHECK_EQ(server.ClientCount(), 1u);
    CHECK_EQ(server.ControllerClientId(), a.clientId);

    // B joins second -> observer (still exactly one controller, A).
    b.SendLogin("bob", net::kProtocolVersion);
    b.SendJoin("bob", 2);
    for (int i = 0; i < 200 && !b.welcomed; ++i) stepAll();
    CHECK(b.welcomed);
    CHECK_EQ(server.ClientCount(), 2u);
    CHECK_EQ(server.ControllerClientId(), a.clientId);

    // The controlled entity's stable key (the script-spawned player).
    const uint64_t key = server.ControlledEntityKey();
    CHECK((key) != (0u));

    auto findEntity = [](const net::MsgSnapshot& s,
                         uint64_t id) -> const net::SnapshotEntity* {
        for (const net::SnapshotEntity& e : s.entities)
            if (e.id == id) return &e;
        return nullptr;
    };

    // Both clients already receive snapshots of the idle world (player at z=0).
    CHECK(!a.snapshots.empty());
    CHECK(!b.snapshots.empty());
    {
        const net::SnapshotEntity* ea = findEntity(a.snapshots.back(), key);
        const net::SnapshotEntity* eb = findEntity(b.snapshots.back(), key);
        CHECK(ea != nullptr);
        CHECK(eb != nullptr);
        CHECK_NEAR(ea->z, 0.0, 1e-6);
        CHECK_NEAR(eb->z, 0.0, 1e-6);
    }

    // Controller A holds moveY=1 for 60 ticks: the player advances z.
    a.SendInput(1, 0, 0.0f, 1.0f);
    for (int i = 0; i < 60; ++i) stepAll();

    // The controller's entity moved, and BOTH clients see the SAME state in
    // their latest snapshots (same world, same snapshot tick -> bit-exact).
    const net::SnapshotEntity* ea = findEntity(a.snapshots.back(), key);
    const net::SnapshotEntity* eb = findEntity(b.snapshots.back(), key);
    CHECK(ea != nullptr);
    CHECK(eb != nullptr);
    CHECK((ea->z) > (0.5f));
    CHECK_EQ(a.snapshots.back().tick, b.snapshots.back().tick);
    CHECK(ea->x == eb->x);
    CHECK(ea->y == eb->y);
    CHECK(ea->z == eb->z);

    // The world advanced on the server (GameVar ticks per fixed step).
    CHECK((server.GameVars().Get("ticks").number) > (0.0));

    // v1 input model: A releases, then B (observer) tries to drive the sim —
    // non-controller input is ignored, so the player stays put.
    a.SendInput(2, 0, 0.0f, 0.0f); // controller releases forward
    for (int i = 0; i < 5; ++i) stepAll();
    double zAfterStop = 0.0;
    {
        auto view = server.World().ViewAll<script::CTransformBind>();
        for (size_t i = 0; i < view.Size(); ++i) {
            ecs::Entity e = server.World().EntityAt<script::CTransformBind>(i);
            const script::CTransformBind* t = server.World().Get<script::CTransformBind>(e);
            if (t) zAfterStop = t->pos.z;
        }
    }
    b.SendInput(1, 0, 0.0f, 1.0f); // observer's forward: ignored
    for (int i = 0; i < 5; ++i) stepAll();
    {
        auto view = server.World().ViewAll<script::CTransformBind>();
        for (size_t i = 0; i < view.Size(); ++i) {
            ecs::Entity e = server.World().EntityAt<script::CTransformBind>(i);
            const script::CTransformBind* t = server.World().Get<script::CTransformBind>(e);
            if (t) CHECK_NEAR(t->pos.z, zAfterStop, 1e-6); // B had no effect
        }
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
// Multi-player (v2): each client controls its OWN player entity. A scene that
// defines on_player_join(clientId) spawns + binds a player per client; the
// server routes each client's MsgInput to that player's script, so A's input
// moves only A's player and B's input moves only B's.
// ---------------------------------------------------------------------------

namespace {

const char* kMultiplayerHostLua = R"(
function on_start(e)
  SetVar("ticks", 0)
end
function on_update(e, dt)
  SetVar("ticks", (GetVar("ticks") or 0) + 1)
end
function on_player_join(clientId)
  local p = Spawn("player", { x = 0, y = 0, z = clientId * 4 }, "scripts/player_controller.lua")
  BindPlayerToClient(p, clientId)
end
)";

const char* kPlayerControllerLua = R"(
function on_update(e, dt)
  local fwd = InputAxis("forward")
  if fwd > 0.5 then
    local p = GetPosition(e)
    if p ~= nil then
      SetPosition(e, { x = p.x, y = p.y, z = p.z + 1 })
    end
  end
end
)";

void WriteMultiplayerFixture(const std::string& dir) {
    const char* scene =
        R"({"entities":[{"name":"Host","components":{"transform":{"pos":[0,0,0]},)"
        R"("script":{"backend":"lua","path":"scripts/host.lua"}}}]})";
    CHECK(test::WriteFileAll(dir + "/scene.json", scene));
    const std::string scriptsDir = dir + "/scripts";
#if defined(_WIN32)
    CreateDirectoryA(scriptsDir.c_str(), nullptr);
#else
    ::mkdir(scriptsDir.c_str(), 0700);
#endif
    CHECK(test::WriteFileAll(scriptsDir + "/host.lua", kMultiplayerHostLua));
    CHECK(test::WriteFileAll(scriptsDir + "/player_controller.lua", kPlayerControllerLua));
}

// Stable snapshot key: (id << 32) | generation (matches GameServer::EntityKey).
uint64_t MultiplayerEntityKey(const ecs::Entity& e) {
    return (static_cast<uint64_t>(e.id) << 32) | static_cast<uint64_t>(e.generation);
}

} // namespace

TEST(ServerEachClientControlsOwnPlayer) {
    test::TempDir tmp;
    WriteMultiplayerFixture(tmp.Str());
    server::GameServer server;
    CHECK(server.Start(SceneCfg(tmp.Str(), 20260)));

    LoopbackClient a, b;
    a.BindAndPeer(server.Port());
    b.BindAndPeer(server.Port());
    uint64_t now = 0;
    auto stepAll = [&]() {
        now += 17;
        server.Step(now);
        a.Pump(now);
        b.Pump(now);
    };

    a.SendJoin("alice", 2);
    for (int i = 0; i < 200 && !a.welcomed; ++i) stepAll();
    CHECK(a.welcomed);
    b.SendJoin("bob", 2);
    for (int i = 0; i < 200 && !b.welcomed; ++i) stepAll();
    CHECK(b.welcomed);

    // Each client's player spawned at z = clientId * 4 (ids 1 and 2). Capture
    // their stable keys by spawn z so movement can be tracked independently.
    uint64_t keyA = 0, keyB = 0;
    {
        auto view = server.World().ViewAll<script::CTransformBind>();
        for (size_t i = 0; i < view.Size(); ++i) {
            ecs::Entity e = server.World().EntityAt<script::CTransformBind>(i);
            const script::CTransformBind* t = server.World().Get<script::CTransformBind>(e);
            if (!t) continue;
            if (std::fabs(t->pos.z - 4.0f) < 0.5f) keyA = MultiplayerEntityKey(e);
            else if (std::fabs(t->pos.z - 8.0f) < 0.5f) keyB = MultiplayerEntityKey(e);
        }
    }
    CHECK(keyA != 0);
    CHECK(keyB != 0);

    auto zByKey = [&](uint64_t key) -> float {
        auto view = server.World().ViewAll<script::CTransformBind>();
        for (size_t i = 0; i < view.Size(); ++i) {
            ecs::Entity e = server.World().EntityAt<script::CTransformBind>(i);
            if (MultiplayerEntityKey(e) != key) continue;
            const script::CTransformBind* t = server.World().Get<script::CTransformBind>(e);
            return t ? t->pos.z : -1.0f;
        }
        return -1.0f;
    };

    // A holds forward for 10 ticks: A's player moves, B's stays at spawn.
    a.SendInput(1, 0, 0.0f, 1.0f);
    for (int i = 0; i < 10; ++i) stepAll();
    CHECK(zByKey(keyA) > 4.0f);
    CHECK_NEAR(zByKey(keyB), 8.0f, 1e-4);

    // A releases, B holds forward: B's player moves, A's stays put.
    a.SendInput(2, 0, 0.0f, 0.0f);
    b.SendInput(1, 0, 0.0f, 1.0f);
    const float zA = zByKey(keyA);
    for (int i = 0; i < 10; ++i) stepAll();
    CHECK(zByKey(keyB) > 8.0f);
    CHECK_NEAR(zByKey(keyA), zA, 1e-4);

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

// ---------------------------------------------------------------------------
// B13 snapshot fragmentation: a scene with >~48 entities cannot fit in one
// reliable frame. Instead of dropping the snapshot (which froze the client on
// stale state), the server splits it into parts and the client reassembles the
// FULL entity set.
// ---------------------------------------------------------------------------

TEST(ServerSnapshotFragmentedAndDelivered) {
    server::GameServer server;
    server::GameServer::Config cfg;
    cfg.port = 0;
    cfg.loopback = true;
    cfg.sceneJson = BigScene(100); // ~100 entities >> the ~48-entity frame cap
    CHECK(server.Start(cfg));
    CHECK((server.World().EntityCount()) > (48u));

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

    for (int i = 0; i < 30; ++i) {
        now += 17;
        server.Step(now);
        client.Pump(now);
    }
    // Fragmented, not dropped: no oversized-snapshot losses, and the client
    // reassembled a snapshot with MORE than one unfragmented frame's worth of
    // entities (the old code silently froze on the previous state).
    CHECK_EQ(server.SnapshotTooBig(), 0u);
    CHECK(!client.snapshots.empty());
    CHECK((client.snapshots.back().entities.size()) > (48u));
    // Reassembly integrity: no entity appears twice across the parts.
    std::set<uint64_t> seen;
    for (const auto& e : client.snapshots.back().entities) seen.insert(e.id);
    CHECK_EQ(seen.size(), client.snapshots.back().entities.size());
    server.Shutdown();
}

// P2-4: production RPC + rooms. Two clients join the same room; a broadcast
// from one reaches both (room.chat), and a room.list reply echoes the room.
TEST(ServerRpcRoomsBroadcast) {
    server::GameServer::Config cfg;
    cfg.port = 0;
    cfg.loopback = true;
    cfg.sceneJson = "{\"entities\": []}";
    server::GameServer server;
    CHECK(server.Start(cfg));

    LoopbackClient a, b;
    a.BindAndPeer(server.Port());
    b.BindAndPeer(server.Port());
    uint64_t now = 0;
    auto join = [&](LoopbackClient& c) {
        c.SendJoin("player", 2);
        for (int i = 0; i < 200 && !c.welcomed; ++i) {
            now += 17;
            server.Step(now);
            c.Pump(now);
        }
        CHECK(c.welcomed);
    };
    join(a);
    join(b);

    // A joins room "alpha", B joins the same room.
    a.SendRpc("room.join", "{\"room\":\"alpha\"}");
    b.SendRpc("room.join", "{\"room\":\"alpha\"}");
    for (int i = 0; i < 30; ++i) {
        now += 17;
        server.Step(now);
        a.Pump(now);
        b.Pump(now);
    }
    bool aJoined = false;
    for (const net::MsgRpc& r : a.rpcs)
        if (r.name == "room.joined" && r.argsJson.find("alpha") != std::string::npos)
            aJoined = true;
    CHECK(aJoined);

    // Broadcast from A -> both A and B receive room.chat.
    a.rpcs.clear();
    b.rpcs.clear();
    a.SendRpc("room.broadcast", "{\"message\":\"hello room\"}");
    for (int i = 0; i < 30; ++i) {
        now += 17;
        server.Step(now);
        a.Pump(now);
        b.Pump(now);
    }
    auto gotChat = [](const std::vector<net::MsgRpc>& rpcs) {
        for (const net::MsgRpc& r : rpcs)
            if (r.name == "room.chat" &&
                r.argsJson.find("hello room") != std::string::npos)
                return true;
        return false;
    };
    CHECK(gotChat(a.rpcs));
    CHECK(gotChat(b.rpcs));
    server.Shutdown();
}

// P2-4 anti-cheat: input flooding is rate-limited and the client is banned
// after repeated violations; admin RPCs kick/ban by client id.
TEST(ServerAntiCheatInputFloodBans) {
    server::GameServer::Config cfg;
    cfg.port = 0;
    cfg.loopback = true;
    cfg.sceneJson = "{\"entities\": []}";
    cfg.maxInputsPerSecond = 5;   // tiny window: 5 inputs per second
    cfg.maxViolations = 2;
    server::GameServer server;
    CHECK(server.Start(cfg));

    LoopbackClient c;
    c.BindAndPeer(server.Port());
    uint64_t now = 0;
    c.SendJoin("flooder", 2);
    for (int i = 0; i < 200 && !c.welcomed; ++i) {
        now += 17;
        server.Step(now);
        c.Pump(now);
    }
    CHECK(c.welcomed);
    CHECK_EQ(server.ClientCount(), 1u);

    // Flood: send 30 inputs inside one 1s window (server only accepts 5).
    for (int i = 0; i < 30; ++i) {
        net::MsgInput in;
        in.seq = static_cast<uint32_t>(i);
        in.moveX = 0.5f;
        c.chan.Send(static_cast<uint8_t>(net::MsgType::Input), net::EncodeBody(in));
    }
    for (int i = 0; i < 10; ++i) {
        now += 17;
        server.Step(now);
        c.Pump(now);
    }
    // Two violations -> kicked + banned.
    CHECK_EQ(server.ClientCount(), 0u);

    // A banned name cannot rejoin.
    LoopbackClient c2;
    c2.BindAndPeer(server.Port());
    c2.SendJoin("flooder", 2);
    for (int i = 0; i < 200 && !c2.welcomed; ++i) {
        now += 17;
        server.Step(now);
        c2.Pump(now);
    }
    CHECK(!c2.welcomed);
    server.Shutdown();
}

// P2-4 anti-cheat: a client receives the deterministic world checksum RPC
// alongside the snapshot stream.
TEST(ServerWorldHashChecksum) {
    server::GameServer::Config cfg;
    cfg.port = 0;
    cfg.loopback = true;
    cfg.sceneJson = "{\"entities\": []}";
    server::GameServer server;
    CHECK(server.Start(cfg));

    LoopbackClient c;
    c.BindAndPeer(server.Port());
    uint64_t now = 0;
    c.SendJoin("checker", 2);
    for (int i = 0; i < 200 && !c.welcomed; ++i) {
        now += 17;
        server.Step(now);
        c.Pump(now);
    }
    CHECK(c.welcomed);
    for (int i = 0; i < 30 && c.rpcs.empty(); ++i) {
        now += 17;
        server.Step(now);
        c.Pump(now);
    }
    bool gotHash = false;
    for (const net::MsgRpc& r : c.rpcs)
        if (r.name == "world.hash" && r.argsJson.find("hash") != std::string::npos)
            gotHash = true;
    CHECK(gotHash);
    server.Shutdown();
}
