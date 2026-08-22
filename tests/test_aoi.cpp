#include <algorithm>
#include <cmath>
#include <cstdint>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "neon/neon.hpp"
#include "aoi.hpp"
#include "game_server.hpp"
#include "helpers.hpp"

using namespace neon;

// Test-suite 6.5: AOI interest management.
// ---------------------------------------------------------------------------
// Part 1 exercises the pure AoiGrid: cell math (floor division, boundary
// positions, negative cells), radius-0 focus-cell queries, the (2r+1)^2 window,
// the change of the set when the focus crosses a boundary, and determinism.
// Part 2 is a real GameServer + loopback client: the client receives MsgSpawn
// exactly for the entities in its interest set (and NOT for far ones), the
// focus moving via input triggers spawns/despawns as entities enter/leave, the
// controlled entity is always replicated, and identical focus paths produce
// identical replication streams.

namespace {

constexpr float kCell = server::AoiGrid::kDefaultCellSize; // 32

int64_t CellOf(float coord) {
    return static_cast<int64_t>(std::floor(static_cast<double>(coord) / kCell));
}

// InterestSet as an ordered set (single temporary, so begin/end match).
std::set<uint64_t> InterestAsSet(const server::AoiGrid& grid, float x, float z, int r) {
    const std::vector<uint64_t> v = grid.InterestSet(x, z, r);
    return std::set<uint64_t>(v.begin(), v.end());
}

// The stable (id<<32)|generation key GameServer uses on the wire.
uint64_t TestEntityKey(const ecs::Entity& e) {
    return (static_cast<uint64_t>(e.id) << 32) | static_cast<uint64_t>(e.generation);
}

// One world entity as the server's AOI sees it (id + x/z for the grid).
struct WorldEnt {
    uint64_t id = 0;
    std::string name;
    float x = 0.0f, z = 0.0f;
};

// Collects every position-bearing entity (SceneTransform + CTransformBind,
// deduplicated by key) with its SceneName/kind. Mirrors BroadcastSnapshot's
// collection so tests can build independent AOI expectations.
std::vector<WorldEnt> CollectWorld(server::GameServer& server) {
    std::vector<WorldEnt> out;
    std::set<uint64_t> seen;
    ecs::World& world = server.World();
    {
        auto view = world.ViewAll<scene::SceneTransform>();
        for (size_t i = 0; i < view.Size(); ++i) {
            ecs::Entity e = world.EntityAt<scene::SceneTransform>(i);
            const scene::SceneTransform* t = world.Get<scene::SceneTransform>(e);
            if (!t) continue;
            const uint64_t key = TestEntityKey(e);
            if (seen.count(key)) continue;
            seen.insert(key);
            WorldEnt we;
            we.id = key;
            if (const scene::SceneName* n = world.Get<scene::SceneName>(e)) we.name = n->name;
            we.x = t->pos.x;
            we.z = t->pos.z;
            out.push_back(we);
        }
    }
    {
        auto view = world.ViewAll<script::CTransformBind>();
        for (size_t i = 0; i < view.Size(); ++i) {
            ecs::Entity e = world.EntityAt<script::CTransformBind>(i);
            const script::CTransformBind* t = world.Get<script::CTransformBind>(e);
            if (!t) continue;
            const uint64_t key = TestEntityKey(e);
            if (seen.count(key)) continue;
            seen.insert(key);
            WorldEnt we;
            we.id = key;
            we.name = "player";
            we.x = t->pos.x;
            we.z = t->pos.z;
            out.push_back(we);
        }
    }
    return out;
}

// The id of the world entity whose name matches `name` ("" for none).
uint64_t KeyOfName(const std::vector<WorldEnt>& ents, const std::string& name) {
    for (const WorldEnt& e : ents)
        if (e.name == name) return e.id;
    return 0;
}

// Independent AOI expectation: the ids of `ents` inside the (2r+1)^2 cells
// around (focusX, focusZ), using the same floor-division cell math as AoiGrid.
std::set<uint64_t> ExpectedInterest(const std::vector<WorldEnt>& ents, float focusX,
                                    float focusZ, int radius) {
    std::set<uint64_t> out;
    const int64_t cx = CellOf(focusX);
    const int64_t cz = CellOf(focusZ);
    for (const WorldEnt& e : ents) {
        const int64_t ex = CellOf(e.x);
        const int64_t ez = CellOf(e.z);
        if (std::llabs(ex - cx) <= radius && std::llabs(ez - cz) <= radius) out.insert(e.id);
    }
    return out;
}

// ---------------------------------------------------------------------------
// Server integration fixture: the controller script spawns a "player" and moves
// it +1 z per fixed tick on forward input; the scene scatters transform-only
// props across many cells so AOI visibly filters them.
// ---------------------------------------------------------------------------

const char* kAoiControllerLua = R"(
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

// Props, with their (cell x, cell z) at cellSize 32:
//   NearA (0,0) NearB (0,0) MidX (1,0) MidZ (0,1)
//   FarX (3,0)  FarZ (0,3)  OutX (-4,0) OutZ (0,-4)
// The initial focus (the player at 0,0) therefore sees NearA/B, MidX, MidZ and
// the Host + player; FarX/FarZ/OutX/OutZ stay outside until the focus moves.
const char* kAoiSceneJson = R"({
  "entities": [
    {
      "name": "Host",
      "components": {
        "transform": {"pos": [0, 0, 0]},
        "script": {"backend": "lua", "path": "scripts/controller.lua"}
      }
    },
    {"name": "NearA", "components": {"transform": {"pos": [5, 0, 0]}}},
    {"name": "NearB", "components": {"transform": {"pos": [-5, 0, 0]}}},
    {"name": "MidX",  "components": {"transform": {"pos": [40, 0, 0]}}},
    {"name": "MidZ",  "components": {"transform": {"pos": [0, 0, 40]}}},
    {"name": "FarX",  "components": {"transform": {"pos": [100, 0, 0]}}},
    {"name": "FarZ",  "components": {"transform": {"pos": [0, 0, 100]}}},
    {"name": "OutX",  "components": {"transform": {"pos": [-100, 0, 0]}}},
    {"name": "OutZ",  "components": {"transform": {"pos": [0, 0, -100]}}}
  ]
})";

void WriteAoiScene(const std::string& dir) {
    CHECK(test::WriteFileAll(dir + "/scene.json", kAoiSceneJson));
    const std::string scriptsDir = dir + "/scripts";
#if defined(_WIN32)
    CreateDirectoryA(scriptsDir.c_str(), nullptr);
#else
    ::mkdir(scriptsDir.c_str(), 0700);
#endif
    CHECK(test::WriteFileAll(scriptsDir + "/controller.lua", kAoiControllerLua));
}

server::GameServer::Config AoiCfg(const std::string& dir, uint64_t seed) {
    server::GameServer::Config cfg;
    cfg.port = 0;
    cfg.loopback = true;
    cfg.sceneJsonPath = dir + "/scene.json";
    cfg.scriptBaseDir = dir;
    cfg.rngSeed = seed;
    cfg.aoiCellSize = kCell;
    cfg.aoiRadiusCells = 1;
    return cfg;
}

// Minimal loopback client that also records MsgSpawn (kind/position) alongside
// snapshots and despawns.
struct AoiClient {
    net::UdpSocket sock;
    net::ReliableChannel chan;
    bool welcomed = false;
    uint64_t clientId = 0;
    std::vector<net::MsgSnapshot> snapshots;
    std::vector<net::MsgSpawn> spawns;
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
        } else if (m.header.msgId == static_cast<uint8_t>(net::MsgType::Snapshot)) {
            snapshots.push_back(std::get<net::MsgSnapshot>(m.payload));
        } else if (m.header.msgId == static_cast<uint8_t>(net::MsgType::Spawn)) {
            spawns.push_back(std::get<net::MsgSpawn>(m.payload));
        } else if (m.header.msgId == static_cast<uint8_t>(net::MsgType::Despawn)) {
            despawned.push_back(std::get<net::MsgDespawn>(m.payload).entityId);
        }
    }

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
};

// Ids of the spawn messages seen so far.
std::set<uint64_t> SpawnedIds(const AoiClient& c) {
    std::set<uint64_t> out;
    for (const net::MsgSpawn& s : c.spawns) out.insert(s.entityId);
    return out;
}

// Ids of the entities in a snapshot.
std::set<uint64_t> SnapIds(const net::MsgSnapshot& s) {
    std::set<uint64_t> out;
    for (const net::SnapshotEntity& e : s.entities) out.insert(e.id);
    return out;
}

} // namespace

// ---------------------------------------------------------------------------
// Pure AoiGrid
// ---------------------------------------------------------------------------

TEST(AoiGridInterestSetExactCells) {
    server::AoiGrid grid;
    grid.Update({
        {1, 5.0f, 0.0f},     // cell (0, 0)
        {2, 35.0f, 0.0f},    // cell (1, 0)
        {3, -20.0f, 0.0f},   // cell (-1, 0)
        {4, 0.0f, 35.0f},    // cell (0, 1)
        {5, 0.0f, -20.0f},   // cell (0, -1)
        {6, 35.0f, 35.0f},   // cell (1, 1)
        {7, 100.0f, 100.0f}, // cell (3, 3): far
        {8, -100.0f, 0.0f},  // cell (-4, 0): far
    });

    // Focus cell (0,0), radius 1 -> the (3x3) block x,z in [-1,1].
    std::vector<uint64_t> got = grid.InterestSet(0.0f, 0.0f, 1);
    std::set<uint64_t> gotSet(got.begin(), got.end());
    CHECK_EQ(gotSet.size(), 6u);
    CHECK(gotSet.count(1u) != 0);
    CHECK(gotSet.count(2u) != 0);
    CHECK(gotSet.count(3u) != 0);
    CHECK(gotSet.count(4u) != 0);
    CHECK(gotSet.count(5u) != 0);
    CHECK(gotSet.count(6u) != 0);
    CHECK(gotSet.count(7u) == 0);
    CHECK(gotSet.count(8u) == 0);
}

TEST(AoiGridRadiusZeroIsFocusCell) {
    server::AoiGrid grid;
    grid.Update({
        {1, 5.0f, 5.0f},   // same cell as the focus (0,0)
        {2, 35.0f, 5.0f},  // adjacent cell (1,0)
        {3, 10.0f, 10.0f}, // same cell (0,0)
    });
    std::vector<uint64_t> got = grid.InterestSet(0.0f, 0.0f, 0);
    std::set<uint64_t> gotSet(got.begin(), got.end());
    CHECK_EQ(gotSet.size(), 2u);
    CHECK(gotSet.count(1u) != 0);
    CHECK(gotSet.count(3u) != 0);
    CHECK(gotSet.count(2u) == 0);

    // A negative radius is a degenerate query: empty set.
    CHECK(grid.InterestSet(0.0f, 0.0f, -1).empty());
}

TEST(AoiGridFocusCrossingBoundaryChangesSet) {
    server::AoiGrid grid;
    grid.Update({
        {1, 5.0f, 0.0f},  // cell 0
        {2, 40.0f, 0.0f}, // cell 1
    });
    // Inside cell 0.
    const std::set<uint64_t> at0 = InterestAsSet(grid, 0.0f, 0.0f, 0);
    CHECK(at0.count(1u) != 0);
    CHECK(at0.count(2u) == 0);
    // Just short of the 32-unit boundary: still cell 0.
    const std::set<uint64_t> at31 = InterestAsSet(grid, 31.0f, 0.0f, 0);
    CHECK(at31.count(1u) != 0);
    CHECK(at31.count(2u) == 0);
    // At/over the boundary: cell 1.
    const std::set<uint64_t> at32 = InterestAsSet(grid, 32.0f, 0.0f, 0);
    CHECK(at32.count(1u) == 0);
    CHECK(at32.count(2u) != 0);
    // Same window at a larger radius: crossing changes which entity is the
    // "near" one but the block covers both cells at r=1.
    const std::set<uint64_t> block = InterestAsSet(grid, 40.0f, 0.0f, 1);
    CHECK(block.count(1u) != 0);
    CHECK(block.count(2u) != 0);
}

TEST(AoiGridBoundaryEntityBelongsToOneCell) {
    // floor division: exactly 32 -> cell 1, just below -> cell 0, negative ->
    // negative cell.
    CHECK_EQ(server::AoiGrid::CellCoord(32.0f, kCell), 1);
    CHECK_EQ(server::AoiGrid::CellCoord(31.999f, kCell), 0);
    CHECK_EQ(server::AoiGrid::CellCoord(0.0f, kCell), 0);
    CHECK_EQ(server::AoiGrid::CellCoord(-0.5f, kCell), -1);
    CHECK_EQ(server::AoiGrid::CellCoord(-32.0f, kCell), -1);

    server::AoiGrid grid;
    grid.Update({
        {1, 32.0f, 0.0f},  // exactly on the x boundary -> cell 1
        {2, 31.999f, 0.0f}, // just before -> cell 0
    });
    // Focus at the boundary: only the boundary entity (cell 1) is seen.
    const std::set<uint64_t> got = InterestAsSet(grid, 32.0f, 0.0f, 0);
    CHECK_EQ(got.size(), 1u);
    CHECK(got.count(1u) != 0);
    CHECK(got.count(2u) == 0);
}

TEST(AoiGridDeterministic) {
    const std::vector<server::AoiGrid::Entry> ents = {
        {1, 5.0f, 0.0f}, {2, 40.0f, 0.0f}, {3, -35.0f, 0.0f},
        {4, 5.0f, 40.0f}, {5, 200.0f, -200.0f}, {6, 0.0f, 0.0f},
    };
    server::AoiGrid a;
    server::AoiGrid b;
    a.Update(ents);
    // Same ids/positions, different order: the grid dedups by id and sorts
    // within a cell, so the interest set is identical.
    b.Update({
        {6, 0.0f, 0.0f}, {4, 5.0f, 40.0f}, {5, 200.0f, -200.0f},
        {3, -35.0f, 0.0f}, {2, 40.0f, 0.0f}, {1, 5.0f, 0.0f},
    });
    for (float fz : {-100.0f, 0.0f, 40.0f}) {
        for (float fx : {-50.0f, 0.0f, 32.0f, 100.0f}) {
            const std::vector<uint64_t> ga = a.InterestSet(fx, fz, 1);
            const std::vector<uint64_t> gb = b.InterestSet(fx, fz, 1);
            CHECK_EQ(ga.size(), gb.size());
            for (size_t i = 0; i < ga.size(); ++i) CHECK_EQ(ga[i], gb[i]);
        }
    }
}

TEST(AoiGridSetCellSize) {
    server::AoiGrid grid;
    grid.SetCellSize(10.0f);
    CHECK_NEAR(grid.CellSize(), 10.0, 1e-6);
    // Invalid sizes are ignored.
    grid.SetCellSize(0.0f);
    grid.SetCellSize(-4.0f);
    CHECK_NEAR(grid.CellSize(), 10.0, 1e-6);
    CHECK_EQ(server::AoiGrid::CellCoord(25.0f, 10.0f), 2);
    CHECK_EQ(server::AoiGrid::CellCoord(10.0f, 10.0f), 1);

    grid.Update({{1, 5.0f, 0.0f}, {2, 25.0f, 0.0f}});
    const std::set<uint64_t> got = InterestAsSet(grid, 0.0f, 0.0f, 0);
    // 5 -> cell 0 (in the focus cell), 25 -> cell 2 (out) at cellSize 10.
    CHECK(got.count(1u) != 0);
    CHECK(got.count(2u) == 0);
}

// ---------------------------------------------------------------------------
// Server integration
// ---------------------------------------------------------------------------

TEST(AoiServerSpawnsOnlyInterestSet) {
    test::TempDir tmp;
    WriteAoiScene(tmp.Str());
    server::GameServer server;
    CHECK(server.Start(AoiCfg(tmp.Str(), 4242)));
    CHECK(server.Running());

    AoiClient client;
    client.BindAndPeer(server.Port());
    uint64_t now = 0;
    auto stepBoth = [&]() {
        now += 17;
        server.Step(now);
        client.Pump(now);
    };

    client.SendJoin("aoi", 2);
    for (int i = 0; i < 200 && !client.welcomed; ++i) stepBoth();
    CHECK(client.welcomed);
    CHECK_EQ(server.ClientCount(), 1u);

    // Wait until the initial spawn burst (the whole interest set) arrives.
    for (int i = 0; i < 200 && client.spawns.empty(); ++i) stepBoth();
    CHECK(!client.spawns.empty());

    // The focus is the player, still at (0,0). Compute the expected interest
    // set independently from the world and compare with what was spawned.
    const uint64_t controlled = server.ControlledEntityKey();
    CHECK(controlled != 0u);
    const std::vector<WorldEnt> ents = CollectWorld(server);
    const std::set<uint64_t> expected = ExpectedInterest(ents, 0.0f, 0.0f, 1);
    CHECK_EQ(expected.size(), 6u); // Host + player + NearA/B + MidX + MidZ
    const std::set<uint64_t> spawned = SpawnedIds(client);
    CHECK_EQ(spawned, expected);

    // The far props never spawned.
    const uint64_t farX = KeyOfName(ents, "FarX");
    const uint64_t farZ = KeyOfName(ents, "FarZ");
    const uint64_t outX = KeyOfName(ents, "OutX");
    const uint64_t outZ = KeyOfName(ents, "OutZ");
    CHECK(farX != 0u);
    CHECK(spawned.count(farX) == 0);
    CHECK(spawned.count(farZ) == 0);
    CHECK(spawned.count(outX) == 0);
    CHECK(spawned.count(outZ) == 0);

    // Every snapshot contains exactly the interest set, and always the player.
    CHECK(!client.snapshots.empty());
    for (const net::MsgSnapshot& s : client.snapshots) {
        const std::set<uint64_t> ids = SnapIds(s);
        CHECK_EQ(ids.size(), s.entityCount);
        CHECK_EQ(ids, expected);
        CHECK(ids.count(controlled) != 0);
        CHECK(ids.count(farX) == 0);
        CHECK(ids.count(farZ) == 0);
    }

    // Spawns carry the right kind + position: the player is "player", the
    // scene props their entity name.
    bool sawPlayerKind = false;
    bool sawNearAKind = false;
    for (const net::MsgSpawn& s : client.spawns) {
        if (s.entityId == controlled) {
            CHECK_EQ(s.kind, std::string("player"));
            CHECK_NEAR(s.x, 0.0, 1e-4);
            CHECK_NEAR(s.z, 0.0, 1e-4);
            sawPlayerKind = true;
        }
        if (s.entityId == KeyOfName(ents, "NearA")) {
            CHECK_EQ(s.kind, std::string("NearA"));
            CHECK_NEAR(s.x, 5.0, 1e-4);
            sawNearAKind = true;
        }
    }
    CHECK(sawPlayerKind);
    CHECK(sawNearAKind);

    server.Shutdown();
}

TEST(AoiServerFocusMoveSpawnsAndDespawns) {
    test::TempDir tmp;
    WriteAoiScene(tmp.Str());
    server::GameServer server;
    CHECK(server.Start(AoiCfg(tmp.Str(), 777)));

    AoiClient client;
    client.BindAndPeer(server.Port());
    uint64_t now = 0;
    auto stepBoth = [&]() {
        now += 17;
        server.Step(now);
        client.Pump(now);
    };

    client.SendJoin("aoi", 2);
    for (int i = 0; i < 200 && !client.welcomed; ++i) stepBoth();
    CHECK(client.welcomed);
    for (int i = 0; i < 200 && client.spawns.size() < 6u; ++i) stepBoth();
    CHECK_EQ(client.spawns.size(), 6u);
    CHECK(client.despawned.empty());

    // Drive the focus forward: the player advances +1 z per fixed tick and
    // crosses cell boundaries. After 140 input ticks it is in z-cell >= 3, so
    // MidZ (z-cell 1) has LEFT the window and FarZ (z-cell 3) has ENTERED.
    client.SendInput(1, 0, 0.0f, 1.0f);
    for (int i = 0; i < 140; ++i) stepBoth();
    // Settle: drain any in-flight datagrams without advancing the sim.
    for (int i = 0; i < 50; ++i) {
        client.Pump(now);
        now += 1;
    }

    const std::vector<WorldEnt> ents = CollectWorld(server);
    const uint64_t midZ = KeyOfName(ents, "MidZ");
    const uint64_t farZ = KeyOfName(ents, "FarZ");
    const uint64_t controlled = server.ControlledEntityKey();
    CHECK(midZ != 0u);
    CHECK(farZ != 0u);

    // The player actually moved far enough to leave MidZ behind.
    const WorldEnt* playerEnt = nullptr;
    for (const WorldEnt& e : ents)
        if (e.id == controlled) playerEnt = &e;
    CHECK(playerEnt != nullptr);
    CHECK((playerEnt->z) > (96.0f)); // z-cell >= 3

    // MidZ left, FarZ entered during the move.
    bool sawMidZDespawn = false;
    for (uint64_t id : client.despawned)
        if (id == midZ) sawMidZDespawn = true;
    CHECK(sawMidZDespawn);
    const std::set<uint64_t> spawned = SpawnedIds(client);
    CHECK(spawned.count(farZ) != 0);

    // The final snapshot matches the expected interest set at the player's
    // final position, contains the player, and no longer contains MidZ.
    const std::set<uint64_t> expected =
        ExpectedInterest(ents, playerEnt->x, playerEnt->z, 1);
    CHECK(expected.count(midZ) == 0);
    CHECK(expected.count(farZ) != 0);
    const net::MsgSnapshot& last = client.snapshots.back();
    const std::set<uint64_t> lastIds = SnapIds(last);
    CHECK_EQ(lastIds, expected);
    CHECK(lastIds.count(controlled) != 0);

    // The x-remote props never entered, no matter how far the focus moved.
    CHECK(spawned.count(KeyOfName(ents, "FarX")) == 0);
    CHECK(spawned.count(KeyOfName(ents, "OutX")) == 0);
    CHECK(spawned.count(KeyOfName(ents, "OutZ")) == 0);

    // The controlled entity is in EVERY received snapshot (start to finish).
    CHECK(!client.snapshots.empty());
    for (const net::MsgSnapshot& s : client.snapshots) {
        const std::set<uint64_t> ids = SnapIds(s);
        CHECK(ids.count(controlled) != 0);
    }

    server.Shutdown();
}

// ---------------------------------------------------------------------------
// Determinism: identical focus paths produce identical replication streams.
// ---------------------------------------------------------------------------

namespace {

struct AoiRun {
    std::vector<uint64_t> spawnIds;
    std::vector<uint64_t> despawnIds;
    std::vector<std::vector<uint64_t>> snapSets; // per-snapshot, sorted
};

AoiRun RunAoiScenario(uint64_t seed) {
    test::TempDir tmp;
    WriteAoiScene(tmp.Str());
    server::GameServer server;
    CHECK(server.Start(AoiCfg(tmp.Str(), seed)));
    AoiClient client;
    client.BindAndPeer(server.Port());

    uint64_t now = 0;
    auto stepBoth = [&]() {
        now += 17;
        server.Step(now);
        client.Pump(now);
    };
    client.SendJoin("aoi", 2);
    for (int i = 0; i < 200 && !client.welcomed; ++i) stepBoth();
    CHECK(client.welcomed);
    for (int i = 0; i < 10; ++i) stepBoth();
    client.SendInput(1, 0, 0.0f, 1.0f);
    for (int i = 0; i < 140; ++i) stepBoth();
    for (int i = 0; i < 30; ++i) {
        client.Pump(now);
        now += 1;
    }

    AoiRun r;
    for (const net::MsgSpawn& s : client.spawns) r.spawnIds.push_back(s.entityId);
    r.despawnIds = client.despawned;
    for (const net::MsgSnapshot& s : client.snapshots) {
        std::vector<uint64_t> ids;
        for (const net::SnapshotEntity& e : s.entities) ids.push_back(e.id);
        std::sort(ids.begin(), ids.end());
        r.snapSets.push_back(std::move(ids));
    }
    server.Shutdown();
    return r;
}

} // namespace

TEST(AoiServerDeterministicAcrossRuns) {
    AoiRun a = RunAoiScenario(20260821u);
    AoiRun b = RunAoiScenario(20260821u);
    CHECK(!a.snapSets.empty());
    CHECK_EQ(a.spawnIds.size(), b.spawnIds.size());
    for (size_t i = 0; i < a.spawnIds.size(); ++i) CHECK_EQ(a.spawnIds[i], b.spawnIds[i]);
    CHECK_EQ(a.despawnIds.size(), b.despawnIds.size());
    for (size_t i = 0; i < a.despawnIds.size(); ++i) CHECK_EQ(a.despawnIds[i], b.despawnIds[i]);
    CHECK_EQ(a.snapSets.size(), b.snapSets.size());
    for (size_t i = 0; i < a.snapSets.size(); ++i) {
        CHECK_EQ(a.snapSets[i].size(), b.snapSets[i].size());
        for (size_t j = 0; j < a.snapSets[i].size(); ++j)
            CHECK_EQ(a.snapSets[i][j], b.snapSets[i][j]);
    }
}
