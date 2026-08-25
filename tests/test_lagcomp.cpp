#include <cstdint>
#include <string>

#include "neon/neon.hpp"
#include "game_server.hpp"
#include "helpers.hpp"

using namespace neon;

// G3-4: server-side lag compensation. The runtime records every fixed tick's
// authoritative poses; hit tests can rewind to the pose a target had N ticks
// ago (where the shooter SAW it) while damage lands on the current entity.
// The authoritative server derives the rewind from its clients' measured RTT.

namespace {

const char* kLagScene = R"({
  "entities": [
    {"name": "hero", "components": {"transform": {"pos": [0,0,0]}, "health": {"hp": 100, "maxHp": 100}}},
    {"name": "wolf", "components": {"transform": {"pos": [0,0,-2]}, "health": {"hp": 50, "maxHp": 50}}}
  ]
})";

const char* kSkillsJson = R"({
  "skills": {
    "cleave": {"kind": "melee", "damage": 12, "meleeRange": 3, "arcDeg": 100, "cooldown": 0.8}
  }
})";

// Drives the runtime for `n` fixed ticks, then moves the wolf out of melee
// range and ticks once more (recording the new pose). Returns the runtime
// with the wolf's historical pose (0,0,-2) one tick in the past.
struct Fixture {
    std::unique_ptr<scene::GameRuntime> rt;
    ecs::Entity hero;
    ecs::Entity wolf;
};

Fixture MakeFixture() {
    Fixture f;
    f.rt = std::make_unique<scene::GameRuntime>();
    scene::GameRuntimeConfig cfg;
    cfg.headless = true;
    CHECK(f.rt->Start(kLagScene, cfg).Ok());
    f.hero = f.rt->FindNamedEntity("hero");
    f.wolf = f.rt->FindNamedEntity("wolf");
    CHECK(f.hero.IsValid());
    CHECK(f.wolf.IsValid());
    for (int i = 0; i < 5; ++i) f.rt->Tick(1.0f / 60.0f); // record poses at (0,0,-2)
    scene::SceneTransform* t = f.rt->World().Get<scene::SceneTransform>(f.wolf);
    CHECK(t != nullptr);
    t->pos = {0, 0, -5}; // now out of range; the historical pose stays at -2
    f.rt->Tick(1.0f / 60.0f);
    return f;
}

// Minimal loopback client used by the server integration test.
struct PingClient {
    net::UdpSocket sock;
    net::ReliableChannel chan;
    bool welcomed = false;

    void BindAndPeer(uint16_t serverPort) {
        core::Result<net::UdpSocket> res = net::UdpSocket::Create();
        CHECK(res.Ok());
        sock = std::move(res.Value());
        CHECK(sock.BindLoopback(0).Ok());
        CHECK(sock.SetPeer(net::NetAddress{"127.0.0.1", serverPort}).Ok());
        chan.SetOutbound([this](const std::vector<uint8_t>& bytes) {
            if (sock.Valid()) sock.Send(bytes.data(), bytes.size());
        });
        chan.SetDeliver([this](const net::DecodedMessage& m) {
            if (m.header.msgId == static_cast<uint8_t>(net::MsgType::Welcome))
                welcomed = true;
        });
    }
    void SendJoin() {
        net::MsgJoin m{"lagtester", 2};
        CHECK(chan.Send(static_cast<uint8_t>(net::MsgType::Join), server::EncodeBody(m)).Ok());
    }
    void SendPing(uint64_t t) {
        net::MsgPing m{t};
        CHECK(chan.Send(static_cast<uint8_t>(net::MsgType::Ping), server::EncodeBody(m)).Ok());
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
};

} // namespace

// The wolf was at (0,0,-2) one tick ago and is now at (0,0,-5): a plain
// melee misses (current pose), the rewound hit test connects and the damage
// lands on the CURRENT entity.
TEST(LagCompMeleeRewindHitsHistoricalPose) {
    Fixture f = MakeFixture();
    CHECK_EQ(f.rt->MeleeAttack({0, 0, 0}, {0, 0, -1}, 3.0f, 180.0f, 10.0f), 0);
    CHECK_NEAR(f.rt->EntityHealth(f.wolf).first, 50.0f, 1e-4);
    CHECK_EQ(f.rt->MeleeAttackLagComp({0, 0, 0}, {0, 0, -1}, 3.0f, 180.0f, 10.0f, 1), 1);
    CHECK_NEAR(f.rt->EntityHealth(f.wolf).first, 40.0f, 1e-4);
}

// Same rewind semantics for the oriented attack box.
TEST(LagCompAttackBoxRewind) {
    Fixture f = MakeFixture();
    // Box centered where the wolf WAS (-2), half 1: the hero at (0,0,0) is
    // outside (distance 2), the wolf's current pose (-5) is outside, and only
    // its rewound pose (-2) is inside.
    CHECK_EQ(f.rt->AttackBox({0, 0, -2}, {1, 1, 1}, 0.0f, 10.0f), 0);
    CHECK_EQ(f.rt->AttackBoxLagComp({0, 0, -2}, {1, 1, 1}, 0.0f, 10.0f, 1), 1);
    CHECK_NEAR(f.rt->EntityHealth(f.wolf).first, 40.0f, 1e-4);
}

// SetAutoLagComp makes the PLAIN attack (and data-driven skills) use the
// rewind - this is the path the authoritative server drives every tick.
TEST(LagCompAutoModeDrivesPlainAttacksAndSkills) {
    Fixture f = MakeFixture();
    f.rt->SetAutoLagComp(1);
    CHECK_EQ(f.rt->AutoLagCompTicks(), 1u);
    CHECK_EQ(f.rt->MeleeAttack({0, 0, 0}, {0, 0, -1}, 3.0f, 180.0f, 10.0f), 1);
    CHECK_NEAR(f.rt->EntityHealth(f.wolf).first, 40.0f, 1e-4);

    // Disable: the same attack now misses again.
    f.rt->SetAutoLagComp(0);
    CHECK_EQ(f.rt->MeleeAttack({0, 0, 0}, {0, 0, -1}, 3.0f, 180.0f, 10.0f), 0);

    // CastSkill's melee skill honours the auto rewind (12 dmg).
    f.rt->SetAutoLagComp(1);
    std::string err;
    CHECK(f.rt->LoadSkills(kSkillsJson, &err));
    CHECK_EQ(f.rt->CastSkill("cleave", {0, 0, 0}, {0, 0, -1}, f.hero), 1);
    CHECK_NEAR(f.rt->EntityHealth(f.wolf).first, 28.0f, 1e-4);
}

// LagCompPosition exposes the historical pose; a rewind deeper than the ring
// clamps to the oldest snapshot; entities absent from a snapshot fall back to
// their CURRENT pose (fresh spawns degrade to the plain hit test).
TEST(LagCompHistoryQueriesAndFreshFallback) {
    Fixture f = MakeFixture();
    math::Vec3 out;
    CHECK(f.rt->LagCompPosition(f.wolf, 1, out));
    CHECK_NEAR(out.z, -2.0f, 1e-4);
    CHECK(f.rt->LagCompPosition(f.wolf, 100, out)); // clamps to the oldest snapshot

    // A fresh entity (never in any snapshot) reports false and the rewound
    // attack still hits at its CURRENT position.
    // Move the wolf far away AND flush two ticks so neither its history nor
    // its current pose can be hit; only the fresh entity's current pose is.
    scene::SceneTransform* wt = f.rt->World().Get<scene::SceneTransform>(f.wolf);
    wt->pos = {0, 0, -20};
    f.rt->Tick(1.0f / 60.0f);
    f.rt->Tick(1.0f / 60.0f);
    ecs::Entity fresh = f.rt->World().Create();
    f.rt->World().Add<scene::SceneTransform>(fresh, scene::SceneTransform{});
    f.rt->World().Get<scene::SceneTransform>(fresh)->pos = {0, 0, -2};
    f.rt->World().Add<scene::SceneHealth>(fresh, scene::SceneHealth{50, 50});
    CHECK(!f.rt->LagCompPosition(fresh, 1, out));
    CHECK_EQ(f.rt->MeleeAttackLagComp({0, 0, 0}, {0, 0, -1}, 3.0f, 180.0f, 10.0f, 1), 1);
    CHECK_NEAR(f.rt->EntityHealth(fresh).first, 40.0f, 1e-4);
}

// The pose ring is capped: after more than kLagCompHistoryTicks ticks the
// oldest snapshot is dropped and the ring stops growing.
TEST(LagCompHistoryRingCapped) {
    scene::GameRuntime rt;
    scene::GameRuntimeConfig cfg;
    cfg.headless = true;
    CHECK(rt.Start(kLagScene, cfg).Ok());
    const ecs::Entity wolf = rt.FindNamedEntity("wolf");
    const uint32_t over = scene::GameRuntime::kLagCompHistoryTicks + 20;
    for (uint32_t i = 0; i < over; ++i) rt.Tick(1.0f / 60.0f);
    math::Vec3 out;
    CHECK(rt.LagCompPosition(wolf, scene::GameRuntime::kLagCompHistoryTicks - 1, out));
    CHECK(rt.LagCompPosition(wolf, 500, out)); // clamps to the oldest kept snapshot
}

// Authoritative server: a client's measured RTT drives the per-tick auto
// rewind (half-RTT in 60Hz ticks). Ping stamped at t=0, measured when the
// server clock is ~200ms -> rtt ~200ms -> one-way ~100ms -> 6 ticks.
TEST(LagCompServerRewindsByClientRtt) {
    test::TempDir tmp;
    server::GameServer::Config cfg;
    cfg.port = 0;
    cfg.loopback = true;
    cfg.sceneJson =
        R"({"entities":[{"name":"Host","components":{"transform":{"pos":[0,0,0]}}}]})";
    cfg.scriptBaseDir = tmp.Str();
    server::GameServer server;
    CHECK(server.Start(cfg));

    PingClient client;
    client.BindAndPeer(server.Port());

    uint64_t now = 0;
    auto stepBoth = [&]() {
        now += 17;
        server.Step(now);
        client.Pump(now);
    };

    client.SendJoin();
    for (int i = 0; i < 200 && !client.welcomed; ++i) stepBoth();
    CHECK(client.welcomed);

    // Advance the server clock to ~190ms, then send a ping stamped at t=0.
    // Two steps later the server has flushed + received it and measured an
    // RTT of ~220ms -> one-way ~110ms -> 6 fixed ticks of rewind.
    while (now < 183) stepBoth();
    client.SendPing(0); // stamped at t=0; the server measures on receipt
    stepBoth(); // client flushes the ping
    stepBoth(); // server receives it and runs the next fixed tick

    const uint32_t ticks = server.AutoLagCompTicks();
    CHECK_EQ(ticks, 6u); // (204/2) * 60 / 1000 = 6
    CHECK(server.Running());
    server.Shutdown();
}
