#include <cmath>
#include <fstream>
#include <iterator>
#include <string>

#if defined(_WIN32)
#include <direct.h>
#include <sys/stat.h>
#endif

#include "neon/neon.hpp"
#include "neon/scene/game_runtime.hpp"
#include "neon/scene/status.hpp"
#include "neon/scene/systems/draw_system.hpp"
#include "neon/scene/systems/hud_system.hpp"
#include "neon/scene/systems/physics_bridge.hpp"
#include "neon/scene/systems/plugin_system.hpp"
#include "neon/scene/systems/prefab_system.hpp"
#include "neon/scene/systems/projectile_system.hpp"
#include "neon/scene/systems/scene_tree_system.hpp"
#include "neon/scene/systems/script_canvas.hpp"
#include "neon/scene/systems/script_runtime.hpp"
#include "neon/scene/systems/status_system.hpp"
#include "neon/scene/systems/tween_system.hpp"
#include "neon/scene/systems/ui_system.hpp"
#include "helpers.hpp"
#include "test_backend.hpp"

using namespace neon;

// ---------------------------------------------------------------------------
// M2 combat core tests: the status-effect framework (buffs/debuffs), the
// spatial overlap queries, SpawnProjectile (range / hitRadius / statuses), and
// the Lua bindings + Gameplay base library on top of them.
// ---------------------------------------------------------------------------

namespace {

// Hero + two wolves for skill tests (front wolf in the -Z arc, side wolf out).
const char* kCombatScene = R"({
  "entities": [
    {"name": "hero", "components": {"transform": {"pos": [0,0,0]}, "health": {"hp": 100, "maxHp": 100}}},
    {"name": "wolf_front", "components": {"transform": {"pos": [0,0,-2]}, "health": {"hp": 50, "maxHp": 50}}},
    {"name": "wolf_side", "components": {"transform": {"pos": [3,0,0]}, "health": {"hp": 50, "maxHp": 50}}}
  ]
})";

const char* kSkillsJson = R"({
  "skills": {
    "fireball": {"kind": "projectile", "damage": 20, "cooldown": 0.5, "speed": 14,
                 "range": 30, "life": 2, "manaCost": 8,
                 "statuses": [{"name": "burning", "duration": 3, "magnitude": 2}]},
    "cleave": {"kind": "melee", "damage": 12, "meleeRange": 3, "arcDeg": 100, "cooldown": 0.8},
    "slam": {"kind": "box", "damage": 15, "boxHalfX": 2, "boxHalfY": 2, "boxHalfZ": 1.5}
  }
})";

// Returns the hit entry for `e` (nullptr when `e` is absent from `hits`).
const scene::GameRuntime::HealthHit* FindHit(
    const std::vector<scene::GameRuntime::HealthHit>& hits, ecs::Entity e) {
    for (const auto& h : hits) {
        if (h.entity == e) return &h;
    }
    return nullptr;
}

} // namespace

// ---------------------------------------------------------------------------
// Status framework (pure data layer)
// ---------------------------------------------------------------------------

TEST(StatusApplyQueryRemove) {
    scene::StatusComponent c;
    CHECK(!scene::HasStatus(c, scene::kStatusBurning));
    CHECK_NEAR(scene::StatusMagnitude(c, scene::kStatusBurning), 0.0, 1e-6);

    scene::ApplyStatus(c, scene::kStatusBurning, 3.0f, 2.0f);
    CHECK(scene::HasStatus(c, scene::kStatusBurning));
    CHECK_NEAR(scene::StatusMagnitude(c, scene::kStatusBurning), 2.0, 1e-6);

    // Re-applying refreshes the duration and replaces the magnitude.
    scene::ApplyStatus(c, scene::kStatusBurning, 5.0f, 4.0f);
    CHECK_NEAR(scene::StatusMagnitude(c, scene::kStatusBurning), 4.0, 1e-6);
    CHECK_EQ(c.effects.size(), 1u);

    scene::ApplyStatus(c, scene::kStatusPoison, 2.0f, 1.0f);
    CHECK_EQ(c.effects.size(), 2u);

    scene::RemoveStatus(c, scene::kStatusBurning);
    CHECK(!scene::HasStatus(c, scene::kStatusBurning));
    CHECK(scene::HasStatus(c, scene::kStatusPoison));
}

TEST(StatusTicksAndExpires) {
    scene::StatusComponent c;
    scene::ApplyStatus(c, scene::kStatusBurning, 3.0f, 2.0f); // ticks at 1s, 2s, 3s
    int ticks = 0;
    float total = 0.0f;
    for (int i = 0; i < 180; ++i) { // 3 seconds at 60Hz
        scene::TickStatus(c, 1.0f / 60.0f, [&](uint32_t id, float mag) {
            CHECK_EQ(id, scene::kStatusBurning);
            ++ticks;
            total += mag;
        });
    }
    CHECK_EQ(ticks, 3);
    CHECK_NEAR(total, 6.0, 1e-6);
    CHECK(!scene::HasStatus(c, scene::kStatusBurning)); // expired
}

// StatusSystem (the GameRuntime combat split): drives StatusComponents in an
// arbitrary ecs::World (no GameRuntime dependency) and forwards per-interval
// ticks to the Lua OnStatusTick global when a host is supplied. Mirrors the
// GameRuntime::TickStatuses/HasStatus/StatusMagnitude semantics.
TEST(StatusSystemTicksWorld) {
    scene::StatusSystem sys;
    ecs::World world;
    ecs::Entity e = world.Create();
    world.Add<scene::StatusComponent>(e);
    scene::ApplyStatus(*world.Get<scene::StatusComponent>(e), scene::kStatusBurning, 1.0f, 2.0f);
    CHECK(sys.Has(world, e, scene::kStatusBurning));
    CHECK_NEAR(sys.Magnitude(world, e, scene::kStatusBurning), 2.0, 1e-6);

    for (int i = 0; i < 60; ++i) sys.Tick(1.0f / 60.0f, world, nullptr); // 1s @ 60Hz
    CHECK(!sys.Has(world, e, scene::kStatusBurning));                     // expired
    CHECK_NEAR(sys.Magnitude(world, e, scene::kStatusBurning), 0.0, 1e-6);
}

TEST(StatusSystemTicksLuaOnStatusTick) {
    scene::StatusSystem sys;
    ecs::World world;
    ecs::Entity e = world.Create();
    world.Add<scene::StatusComponent>(e);
    scene::ApplyStatus(*world.Get<scene::StatusComponent>(e), scene::kStatusBurning, 2.0f, 5.0f);

    auto host = script::CreateLuaHost();
    CHECK(host != nullptr);
    CHECK(host->Init());
    CHECK(host->Load("ticks = 0\nfunction OnStatusTick(e, id, mag) ticks = ticks + 1 end"));
    CHECK(host->Run().Ok());

    for (int i = 0; i < 120; ++i) sys.Tick(1.0f / 60.0f, world, host.get()); // 2s @ 60Hz
    const auto ticks = host->GetGlobal("ticks");
    CHECK(ticks.Ok());
    CHECK_NEAR(ticks.Value().number, 2.0, 1e-6);
    CHECK(!sys.Has(world, e, scene::kStatusBurning)); // expired after 2s
    host->Shutdown();
}

// ---------------------------------------------------------------------------
// Lua bindings + World::Add idempotency regression
// ---------------------------------------------------------------------------

TEST(GameplayLibInjected) {
    const char* scene = R"({
      "entities": [
        {"name": "hero", "components": {
          "transform": {"pos": [0,0,0]},
          "health": {"hp": 100, "maxHp": 100},
          "script": {"backend": "lua", "path": "gameplay.lua"}}}
      ]
    })";
    const char* lua = R"(
      function on_start(e)
        SetVar("gp", Gameplay.version)
      end
    )";
    scene::GameRuntime runtime;
    scene::GameRuntimeConfig cfg;
    cfg.headless = true;
    cfg.readScript = [&](const std::string&) { return std::string(lua); };
    CHECK(runtime.Start(scene, cfg).Ok());
    CHECK_NEAR(runtime.GameVar("gp"), 1.0, 1e-6);
}

TEST(GameplayStatsRoundTrip) {
    const char* scene = R"({
      "entities": [
        {"name": "hero", "components": {
          "transform": {"pos": [0,0,0]},
          "health": {"hp": 100, "maxHp": 100},
          "script": {"backend": "lua", "path": "gameplay.lua"}}}
      ]
    })";
    const char* lua = R"(
      function on_start(e)
        Gameplay.Stats.Set("x", 10)
        Gameplay.Stats.Add("x", 5)
        Gameplay.Stats.Set("y", 100)
        SetVar("sum", Gameplay.Stats.Get("x") + Gameplay.Stats.Get("y"))
        Gameplay.Stats.Add("missing", 7) -- uninitialized key treated as 0
        SetVar("m", Gameplay.Stats.Get("missing"))
      end
    )";
    scene::GameRuntime runtime;
    scene::GameRuntimeConfig cfg;
    cfg.headless = true;
    cfg.readScript = [&](const std::string&) { return std::string(lua); };
    CHECK(runtime.Start(scene, cfg).Ok());
    CHECK_NEAR(runtime.GameVar("sum"), 115.0, 1e-6);
    CHECK_NEAR(runtime.GameVar("m"), 7.0, 1e-6);
}

TEST(GameplayCooldownsTick) {
    const char* scene = R"({
      "entities": [
        {"name": "hero", "components": {
          "transform": {"pos": [0,0,0]},
          "health": {"hp": 100, "maxHp": 100},
          "script": {"backend": "lua", "path": "gameplay.lua"}}}
      ]
    })";
    const char* lua = R"(
      function on_start(e)
        cd = Gameplay.Cooldowns.new()
        Gameplay.Cooldowns.set(cd, "fire", 2.0)
        SetVar("ready0", Gameplay.Cooldowns.ready(cd, "fire") and 1 or 0)
        Gameplay.Cooldowns.tick(cd, 1.5)
        SetVar("left", Gameplay.Cooldowns.left(cd, "fire"))
        Gameplay.Cooldowns.tick(cd, 0.6)
        SetVar("ready1", Gameplay.Cooldowns.ready(cd, "fire") and 1 or 0)
      end
    )";
    scene::GameRuntime runtime;
    scene::GameRuntimeConfig cfg;
    cfg.headless = true;
    cfg.readScript = [&](const std::string&) { return std::string(lua); };
    CHECK(runtime.Start(scene, cfg).Ok());
    CHECK_NEAR(runtime.GameVar("ready0"), 0.0, 1e-6);
    CHECK_NEAR(runtime.GameVar("left"), 0.5, 1e-6);
    CHECK_NEAR(runtime.GameVar("ready1"), 1.0, 1e-6);
}

TEST(CombatStatusBindingsViaLua) {
    const char* scene = R"({
      "entities": [
        {"name": "hero", "components": {
          "transform": {"pos": [0,0,0]},
          "health": {"hp": 100, "maxHp": 100},
          "script": {"backend": "lua", "path": "combat.lua"}}}
      ]
    })";
    const char* lua = R"(
      function on_start(e)
        Gameplay.ApplyStatus(e, "burning", 2, 3)
      end
      function on_update(e, dt)
        if Gameplay.HasStatus(e, "burning") then
          SetVar("burn_active", 1)
        end
      end
    )";
    scene::GameRuntime runtime;
    scene::GameRuntimeConfig cfg;
    cfg.headless = true;
    cfg.readScript = [&](const std::string&) { return std::string(lua); };
    CHECK(runtime.Start(scene, cfg).Ok());

    const ecs::Entity hero = runtime.FindNamedEntity("hero");
    CHECK(hero.IsValid());
    CHECK(runtime.HasStatus(hero, scene::kStatusBurning));
    CHECK_NEAR(runtime.StatusMagnitude(hero, scene::kStatusBurning), 3.0, 1e-6);

    // 2 seconds: burning ticks twice (3 dmg each), then expires; the script
    // saw the effect while it was active.
    for (int i = 0; i < 120; ++i) runtime.Tick(1.0f / 60.0f);
    CHECK_NEAR(runtime.GameVar("burn_active"), 1.0, 1e-6);
    CHECK_NEAR(runtime.EntityHealth(hero).first, 94.0f, 1e-3);
    CHECK(!runtime.HasStatus(hero, scene::kStatusBurning));
}

// Task 7: a project script's own OnStatusTick overrides the base library's
// default (last-write-wins over the shared global). The custom handler applies
// a fixed -7 per tick regardless of the burning magnitude, so the default
// burning rule (-mag) is bypassed entirely.
TEST(StatusTickCustomHandlerOverridesDefault) {
    const char* scene = R"({
      "entities": [
        {"name": "hero", "components": {
          "transform": {"pos": [0,0,0]},
          "health": {"hp": 100, "maxHp": 100},
          "script": {"backend": "lua", "path": "custom.lua"}}}
      ]
    })";
    const char* lua = R"(
      function OnStatusTick(ent, id, mag)
        local hp = GetHealth(ent)
        if hp ~= nil then SetHealth(ent, math.max(0, hp - 7)) end
      end
      function on_start(e)
        Gameplay.ApplyStatus(e, "burning", 2, 3)
      end
    )";
    scene::GameRuntime runtime;
    scene::GameRuntimeConfig cfg;
    cfg.headless = true;
    cfg.readScript = [&](const std::string&) { return std::string(lua); };
    CHECK(runtime.Start(scene, cfg).Ok());

    const ecs::Entity hero = runtime.FindNamedEntity("hero");
    CHECK(hero.IsValid());
    CHECK(runtime.HasStatus(hero, scene::kStatusBurning));

    // 2 seconds: burning ticks twice. Default burning would be -3/tick = -6;
    // the custom override applies -7/tick = -14 -> 100 - 14 = 86.
    for (int i = 0; i < 120; ++i) runtime.Tick(1.0f / 60.0f);
    CHECK_NEAR(runtime.EntityHealth(hero).first, 86.0f, 1e-3);
    CHECK(!runtime.HasStatus(hero, scene::kStatusBurning));
}

// Task 7 review fix: GetMaxHealth exposes the entity's real SceneHealth.maxHp
// (symmetric with GetHealth). The hero's maxHp (250) differs from hp (100) to
// prove we are not just returning hp.
TEST(GetMaxHealthBindingViaLua) {
    const char* scene = R"({
      "entities": [
        {"name": "hero", "components": {
          "transform": {"pos": [0,0,0]},
          "health": {"hp": 100, "maxHp": 250},
          "script": {"backend": "lua", "path": "maxhp.lua"}}}
      ]
    })";
    const char* lua = R"(
      function on_start(e)
        SetVar("maxhp", GetMaxHealth(e))
        SetVar("hp", GetHealth(e))
      end
    )";
    scene::GameRuntime runtime;
    scene::GameRuntimeConfig cfg;
    cfg.headless = true;
    cfg.readScript = [&](const std::string&) { return std::string(lua); };
    CHECK(runtime.Start(scene, cfg).Ok());
    CHECK_NEAR(runtime.GameVar("maxhp"), 250.0, 1e-6);
    CHECK_NEAR(runtime.GameVar("hp"), 100.0, 1e-6);
}

// Task 7 review fix: regen caps at the entity's real maxHp (not GetVar), and
// the h > 0 guard keeps regen from reviving a dead entity (hp <= 0).
TEST(RegenTickCapsAtMaxHpAndSkipsDead) {
    const char* scene = R"({
      "entities": [
        {"name": "hero", "components": {
          "transform": {"pos": [0,0,0]},
          "health": {"hp": 100, "maxHp": 100},
          "script": {"backend": "lua", "path": "regen.lua"}}},
        {"name": "dead", "components": {"transform": {"pos": [0,0,-2]},
          "health": {"hp": 0, "maxHp": 100}}}
      ]
    })";
    const char* lua = R"(
      function on_start(e)
        SetHealth(e, 40)
        Gameplay.ApplyStatus(e, "regen", 2, 100)
        Gameplay.ApplyStatus(FindNamedEntity("dead"), "regen", 2, 100)
      end
    )";
    scene::GameRuntime runtime;
    scene::GameRuntimeConfig cfg;
    cfg.headless = true;
    cfg.readScript = [&](const std::string&) { return std::string(lua); };
    CHECK(runtime.Start(scene, cfg).Ok());

    const ecs::Entity hero = runtime.FindNamedEntity("hero");
    const ecs::Entity dead = runtime.FindNamedEntity("dead");
    CHECK(hero.IsValid());
    CHECK(dead.IsValid());

    // 2 seconds: regen ticks twice (+100 each); the hero caps at maxHp 100,
    // the dead entity stays dead (h > 0 guard).
    for (int i = 0; i < 120; ++i) runtime.Tick(1.0f / 60.0f);
    CHECK_NEAR(runtime.EntityHealth(hero).first, 100.0f, 1e-3);
    CHECK_NEAR(runtime.EntityHealth(dead).first, 0.0f, 1e-3);
    CHECK(!runtime.HasStatus(hero, scene::kStatusRegen));
}

// World::Add must be idempotent: a second Add for the same entity+type must
// not orphan the pool's dense entry (double-add previously corrupted the
// SparseSet). The combat hooks rely on this when applying statuses.
TEST(WorldAddIdempotent) {
    ecs::World w;
    ecs::Entity e = w.Create();
    w.Add<scene::StatusComponent>(e);
    w.Add<scene::StatusComponent>(e); // must not duplicate
    {
        auto view = w.ViewAll<scene::StatusComponent>();
        CHECK_EQ(view.Size(), 1u);
    }
    w.Remove<scene::StatusComponent>(e);
    CHECK_EQ(w.ViewAll<scene::StatusComponent>().Size(), 0u);
}

// ---------------------------------------------------------------------------
// Spatial overlap queries (OverlapSphere / OverlapBox)
// ---------------------------------------------------------------------------

TEST(OverlapSphereQueriesHealthEntities) {
    scene::GameRuntime runtime;
    scene::GameRuntimeConfig cfg; cfg.headless = true;
    CHECK(runtime.Start(kCombatScene, cfg).Ok());
    // kCombatScene: hero(0,0,0) hp100, wolf_front(0,0,-2) hp50, wolf_side(3,0,0)
    // hp50. The hero is itself a SceneHealth entity, so it is returned too.
    const ecs::Entity hero = runtime.FindNamedEntity("hero");
    const ecs::Entity front = runtime.FindNamedEntity("wolf_front");
    const ecs::Entity side = runtime.FindNamedEntity("wolf_side");
    CHECK(hero.IsValid());
    CHECK(front.IsValid());
    CHECK(side.IsValid());

    // radius 2.5: hero + wolf_front (wolf_side at 3 is out).
    const auto hits = runtime.OverlapSphere({0,0,0}, 2.5f);
    CHECK_EQ(hits.size(), 2u);
    const scene::GameRuntime::HealthHit* heroHit = FindHit(hits, hero);
    const scene::GameRuntime::HealthHit* frontHit = FindHit(hits, front);
    CHECK(heroHit != nullptr);
    CHECK(frontHit != nullptr);
    CHECK(FindHit(hits, side) == nullptr);
    // The hit position matches the entity's SceneTransform pos.
    const scene::SceneTransform* ft = runtime.World().Get<scene::SceneTransform>(front);
    CHECK(ft != nullptr);
    CHECK_NEAR(frontHit->pos.x, ft->pos.x, 1e-4);
    CHECK_NEAR(frontHit->pos.y, ft->pos.y, 1e-4);
    CHECK_NEAR(frontHit->pos.z, ft->pos.z, 1e-4);
    CHECK_NEAR(frontHit->pos.z, -2.0f, 1e-4);

    // radius 4: all three are returned.
    const auto all = runtime.OverlapSphere({0,0,0}, 4.0f);
    CHECK_EQ(all.size(), 3u);
    CHECK(FindHit(all, hero) != nullptr);
    CHECK(FindHit(all, front) != nullptr);
    CHECK(FindHit(all, side) != nullptr);

    // yaw-0 box (|x|<=0.5, |y|<=10, |z|<=2.5): hero + wolf_front only.
    const auto box = runtime.OverlapBox({0,0,0}, {0.5f,10.0f,2.5f}, 0.0f);
    CHECK_EQ(box.size(), 2u);
    CHECK(FindHit(box, hero) != nullptr);
    CHECK(FindHit(box, front) != nullptr);
    CHECK(FindHit(box, side) == nullptr);
}

TEST(OverlapExcludesDeadEntities) {
    scene::GameRuntime runtime;
    scene::GameRuntimeConfig cfg; cfg.headless = true;
    CHECK(runtime.Start(kCombatScene, cfg).Ok());
    const ecs::Entity side = runtime.FindNamedEntity("wolf_side");
    CHECK(side.IsValid());

    // Alive: wolf_side is inside the 4-unit sphere.
    CHECK(FindHit(runtime.OverlapSphere({0,0,0}, 4.0f), side) != nullptr);

    // hp 0 entities are filtered out of every overlap query.
    scene::SceneHealth* h = runtime.World().Get<scene::SceneHealth>(side);
    CHECK(h != nullptr);
    h->hp = 0.0f;
    CHECK(FindHit(runtime.OverlapSphere({0,0,0}, 4.0f), side) == nullptr);
    CHECK_EQ(runtime.OverlapSphere({0,0,0}, 4.0f).size(), 2u); // hero + wolf_front
}

TEST(OverlapBoxRotatedYaw90) {
    const char* boxScene = R"({
      "entities": [
        {"name": "a", "components": {"transform": {"pos": [0,0,-1]}, "health": {"hp": 50, "maxHp": 50}}},
        {"name": "b", "components": {"transform": {"pos": [0,0,-2.5]}, "health": {"hp": 50, "maxHp": 50}}},
        {"name": "c", "components": {"transform": {"pos": [2.5,0,0]}, "health": {"hp": 50, "maxHp": 50}}}
      ]
    })";
    scene::GameRuntime runtime;
    scene::GameRuntimeConfig cfg; cfg.headless = true;
    CHECK(runtime.Start(boxScene, cfg).Ok());
    const ecs::Entity a = runtime.FindNamedEntity("a");
    const ecs::Entity b = runtime.FindNamedEntity("b");
    const ecs::Entity c = runtime.FindNamedEntity("c");
    CHECK(a.IsValid());
    CHECK(b.IsValid());
    CHECK(c.IsValid());

    // yaw 0 box (half 2x2x1.5): |x|<=2, |y|<=2, |z|<=1.5 -> only "a".
    const auto yaw0 = runtime.OverlapBox({0,0,0}, {2,2,1.5f}, 0.0f);
    CHECK_EQ(yaw0.size(), 1u);
    CHECK(FindHit(yaw0, a) != nullptr);
    CHECK(FindHit(yaw0, b) == nullptr);
    CHECK(FindHit(yaw0, c) == nullptr);

    // yaw 90 deg: the box rotates to face +X -> |x|<=1.5, |z|<=2. "a" is still
    // inside (|z|=1 <= 2), "b" (|z|=2.5) and "c" (|x|=2.5) are outside.
    const auto yaw90 = runtime.OverlapBox({0,0,0}, {2,2,1.5f}, math::kPi * 0.5f);
    CHECK_EQ(yaw90.size(), 1u);
    CHECK(FindHit(yaw90, a) != nullptr);
    CHECK(FindHit(yaw90, b) == nullptr);
    CHECK(FindHit(yaw90, c) == nullptr);
}

TEST(OverlapSphereRewindUsesHistoricalPose) {
    const char* lagScene = R"({
      "entities": [
        {"name": "hero", "components": {"transform": {"pos": [0,0,0]}, "health": {"hp": 100, "maxHp": 100}}},
        {"name": "wolf", "components": {"transform": {"pos": [0,0,-2]}, "health": {"hp": 50, "maxHp": 50}}}
      ]
    })";
    scene::GameRuntime runtime;
    scene::GameRuntimeConfig cfg; cfg.headless = true;
    CHECK(runtime.Start(lagScene, cfg).Ok());
    const ecs::Entity hero = runtime.FindNamedEntity("hero");
    const ecs::Entity wolf = runtime.FindNamedEntity("wolf");
    CHECK(hero.IsValid());
    CHECK(wolf.IsValid());

    for (int i = 0; i < 5; ++i) runtime.Tick(1.0f / 60.0f); // record wolf at (0,0,-2)
    scene::SceneTransform* t = runtime.World().Get<scene::SceneTransform>(wolf);
    CHECK(t != nullptr);
    t->pos = {0, 0, -5};
    runtime.Tick(1.0f / 60.0f); // record the new pose

    // Current pose (0,0,-5) is outside the 3-unit sphere; only the hero hits.
    const auto current = runtime.OverlapSphere({0,0,0}, 3.0f);
    CHECK_EQ(current.size(), 1u);
    CHECK(FindHit(current, hero) != nullptr);
    CHECK(FindHit(current, wolf) == nullptr);

    // Rewound 1 tick, the wolf's historical pose (0,0,-2) is inside and is
    // reported at that position.
    const auto rewound = runtime.OverlapSphere({0,0,0}, 3.0f, 1);
    CHECK_EQ(rewound.size(), 2u);
    CHECK(FindHit(rewound, hero) != nullptr);
    const scene::GameRuntime::HealthHit* wolfHit = FindHit(rewound, wolf);
    CHECK(wolfHit != nullptr);
    CHECK_NEAR(wolfHit->pos.z, -2.0f, 1e-4);
}

TEST(OverlapBindingsViaLua) {
    // Three 50-hp SceneHealth entities (hero carries the script). OverlapSphere
    // radius 5 catches all three; the returned entity handles round-trip back
    // through GetHealth, and OverlapBox exercises the yawDeg -> radians path.
    const char* scene = R"({
      "entities": [
        {"name": "hero", "components": {
          "transform": {"pos": [0,0,0]},
          "health": {"hp": 50, "maxHp": 50},
          "script": {"backend": "lua", "path": "overlap.lua"}}},
        {"name": "wolf_front", "components": {"transform": {"pos": [0,0,-2]}, "health": {"hp": 50, "maxHp": 50}}},
        {"name": "wolf_side", "components": {"transform": {"pos": [3,0,0]}, "health": {"hp": 50, "maxHp": 50}}}
      ]
    })";
    const char* lua = R"(
      function on_start(e)
        local hits = OverlapSphere({x=0,y=0,z=0}, 5)
        SetVar("n", #hits)
        local totalHp = 0
        local hasFront = 0
        local hasSide = 0
        for _, h in ipairs(hits) do
          local hp = GetHealth(h.entity)
          if hp ~= nil then totalHp = totalHp + hp end
          if h.x == 0 and h.z == -2 then hasFront = 1 end   -- wolf_front pos
          if h.x == 3 and h.z == 0 then hasSide = 1 end     -- wolf_side pos
        end
        SetVar("total", totalHp)
        SetVar("hasFront", hasFront)
        SetVar("hasSide", hasSide)
        -- yaw-0 box (half 2,10,0.5): |z|<=0.5 excludes the front wolf (z=-2).
        SetVar("box0", #OverlapBox({x=0,y=0,z=0}, {x=2,y=10,z=0.5}, 0))
        -- yaw-90 box: the rotation swaps half-extents -> |z|<=2 catches it.
        SetVar("box90", #OverlapBox({x=0,y=0,z=0}, {x=2,y=10,z=0.5}, 90))
      end
    )";
    scene::GameRuntime runtime;
    scene::GameRuntimeConfig cfg;
    cfg.headless = true;
    cfg.readScript = [&](const std::string&) { return std::string(lua); };
    CHECK(runtime.Start(scene, cfg).Ok());

    CHECK_NEAR(runtime.GameVar("n"), 3.0, 1e-6);
    CHECK_NEAR(runtime.GameVar("total"), 150.0, 1e-6);
    CHECK_NEAR(runtime.GameVar("hasFront"), 1.0, 1e-6);
    CHECK_NEAR(runtime.GameVar("hasSide"), 1.0, 1e-6);
    CHECK_NEAR(runtime.GameVar("box0"), 1.0, 1e-6);
    CHECK_NEAR(runtime.GameVar("box90"), 2.0, 1e-6);
}

// ---------------------------------------------------------------------------
// SpawnProjectile generalization: range / hitRadius / statuses (data-driven)
// ---------------------------------------------------------------------------

TEST(SpawnProjectileWithRangeHitRadiusAndStatuses) {
    scene::GameRuntime runtime;
    scene::GameRuntimeConfig cfg; cfg.headless = true;
    CHECK(runtime.Start(kCombatScene, cfg).Ok());
    const ecs::Entity wolf = runtime.FindNamedEntity("wolf_front");
    const ecs::Entity hero = runtime.FindNamedEntity("hero");
    runtime.SpawnProjectile({0,1,0}, {0,0,-1}, 14, 10, 2.0f, hero, 30.0f, 1.0f,
                            {{scene::kStatusBurning, 3.0f, 2.0f}});
    for (int i = 0; i < 30; ++i) runtime.Tick(1.0f/60.0f);
    CHECK_NEAR(runtime.EntityHealth(wolf).first, 40.0f, 1e-3); // 50 - 10
    CHECK(runtime.HasStatus(wolf, scene::kStatusBurning));
}

TEST(SpawnProjectileStatusesViaLua) {
    const char* scene = R"({
      "entities": [
        {"name": "hero", "components": {
          "transform": {"pos": [0,0,0]},
          "health": {"hp": 100, "maxHp": 100},
          "script": {"backend": "lua", "path": "combat.lua"}}},
        {"name": "wolf_front", "components": {
          "transform": {"pos": [0,0,-2]},
          "health": {"hp": 50, "maxHp": 50}}}
      ]
    })";
    const char* lua = R"(
      function on_start(e)
        SpawnProjectile({x=0,y=1,z=0}, {x=0,y=0,z=-1}, 14, 10, 2.0, e, 30, 1.0,
                        {{id=1, duration=3, magnitude=2}})
      end
    )";
    scene::GameRuntime runtime;
    scene::GameRuntimeConfig cfg;
    cfg.headless = true;
    cfg.readScript = [&](const std::string&) { return std::string(lua); };
    CHECK(runtime.Start(scene, cfg).Ok());

    const ecs::Entity wolf = runtime.FindNamedEntity("wolf_front");
    const ecs::Entity hero = runtime.FindNamedEntity("hero");
    CHECK(wolf.IsValid());
    CHECK(hero.IsValid());

    for (int i = 0; i < 30; ++i) runtime.Tick(1.0f / 60.0f);
    CHECK_NEAR(runtime.EntityHealth(wolf).first, 40.0f, 1e-3);
    CHECK(runtime.HasStatus(wolf, scene::kStatusBurning));
}

TEST(SpawnProjectileRangeLimitsTravel) {
    scene::GameRuntime runtime;
    scene::GameRuntimeConfig cfg; cfg.headless = true;
    CHECK(runtime.Start(kCombatScene, cfg).Ok());
    const ecs::Entity wolf = runtime.FindNamedEntity("wolf_front");
    const ecs::Entity hero = runtime.FindNamedEntity("hero");
    // range=0.5: the projectile expires after 0.5 units, short of the wolf at
    // 2 units, so the target is never reached.
    runtime.SpawnProjectile({0,1,0}, {0,0,-1}, 14, 10, 2.0f, hero, 0.5f);
    for (int i = 0; i < 30; ++i) runtime.Tick(1.0f/60.0f);
    CHECK_NEAR(runtime.EntityHealth(wolf).first, 50.0f, 1e-3);
}

TEST(SpawnProjectileHitRadiusGatesHit) {
    const char* scene = R"({
      "entities": [
        {"name": "hero", "components": {"transform": {"pos": [0,0,0]}, "health": {"hp": 100, "maxHp": 100}}},
        {"name": "target", "components": {"transform": {"pos": [1.2,0,-2]}, "health": {"hp": 50, "maxHp": 50}}}
      ]
    })";

    // hitRadius 0.8 < the target's 1.2 horizontal offset -> miss (hp stays 50).
    {
        scene::GameRuntime runtime;
        scene::GameRuntimeConfig cfg; cfg.headless = true;
        CHECK(runtime.Start(scene, cfg).Ok());
        const ecs::Entity hero = runtime.FindNamedEntity("hero");
        const ecs::Entity target = runtime.FindNamedEntity("target");
        runtime.SpawnProjectile({0,1,0}, {0,0,-1}, 14, 10, 2.0f, hero, 0.0f, 0.8f);
        for (int i = 0; i < 30; ++i) runtime.Tick(1.0f/60.0f);
        CHECK_NEAR(runtime.EntityHealth(target).first, 50.0f, 1e-3);
    }

    // hitRadius 1.5 > 1.2 -> hit (50 - 10 = 40).
    {
        scene::GameRuntime runtime;
        scene::GameRuntimeConfig cfg; cfg.headless = true;
        CHECK(runtime.Start(scene, cfg).Ok());
        const ecs::Entity hero = runtime.FindNamedEntity("hero");
        const ecs::Entity target = runtime.FindNamedEntity("target");
        runtime.SpawnProjectile({0,1,0}, {0,0,-1}, 14, 10, 2.0f, hero, 0.0f, 1.5f);
        for (int i = 0; i < 30; ++i) runtime.Tick(1.0f/60.0f);
        CHECK_NEAR(runtime.EntityHealth(target).first, 40.0f, 1e-3);
    }
}

// ---------------------------------------------------------------------------
// ProjectileSystem standalone (C1 split): the subsystem runs against a bare
// ecs::World + gfx::ParticleSystem with no GameRuntime, proving the migration
// extracted a self-contained component rather than a runtime-bound shim.
// ---------------------------------------------------------------------------

TEST(ProjectileSystemStandaloneHitsAndAppliesStatuses) {
    ecs::World world;
    ecs::Entity caster = world.Create();
    world.Add<scene::SceneHealth>(caster, scene::SceneHealth{100, 100});
    world.Add<scene::SceneTransform>(caster, scene::SceneTransform{{0, 0, 0}});
    ecs::Entity target = world.Create();
    world.Add<scene::SceneHealth>(target, scene::SceneHealth{50, 50});
    world.Add<scene::SceneTransform>(target, scene::SceneTransform{{0, 0, -2}});

    scene::ProjectileSystem ps;
    ps.Spawn({0, 1, 0}, {0, 0, -1}, 14, 10, 2.0f, caster, 30.0f, 1.0f,
             {{scene::kStatusBurning, 3.0f, 2.0f}});
    CHECK_EQ(ps.Count(), 1u);

    gfx::ParticleSystem particles;
    for (int i = 0; i < 30; ++i) ps.Tick(1.0f / 60.0f, world, particles);

    // The target took the direct damage and inherited the burning status; the
    // projectile was consumed by the hit (same rules as the GameRuntime path).
    scene::SceneHealth* h = world.Get<scene::SceneHealth>(target);
    CHECK(h != nullptr);
    CHECK_NEAR(h->hp, 40.0f, 1e-3); // 50 - 10
    const scene::StatusComponent* sc = world.Get<scene::StatusComponent>(target);
    CHECK(sc != nullptr);
    CHECK(scene::HasStatus(*sc, scene::kStatusBurning));
    CHECK_EQ(ps.Count(), 0u);
}

TEST(ProjectileSystemStandaloneCasterSelfHitFiltered) {
    ecs::World world;
    ecs::Entity caster = world.Create();
    world.Add<scene::SceneHealth>(caster, scene::SceneHealth{100, 100});
    world.Add<scene::SceneTransform>(caster, scene::SceneTransform{{0, 0, -2}});

    scene::ProjectileSystem ps;
    // Fired straight at the caster's own position; the never-self-hit rule
    // keeps the caster at full health even as the projectile overlaps it.
    ps.Spawn({0, 1, 0}, {0, 0, -1}, 14, 10, 2.0f, caster, 30.0f, 1.0f, {});
    gfx::ParticleSystem particles;
    for (int i = 0; i < 30; ++i) ps.Tick(1.0f / 60.0f, world, particles);

    CHECK_NEAR(world.Get<scene::SceneHealth>(caster)->hp, 100.0f, 1e-3);
}

// ---------------------------------------------------------------------------
// Gameplay 内嵌库：MeleeArc / AoE / BoxAttack / Projectile（纯 Lua 玩法函数）
// ---------------------------------------------------------------------------

// Hero carries the gameplay.lua script; the two wolves are pure SceneHealth
// targets (hero caster is filtered out of its own hits via SameEntity).
const char* kGameplayCombatScene = R"({
  "entities": [
    {"name": "hero", "components": {
      "transform": {"pos": [0,0,0]},
      "health": {"hp": 100, "maxHp": 100},
      "script": {"backend": "lua", "path": "gameplay.lua"}}},
    {"name": "wolf_front", "components": {"transform": {"pos": [0,0,-2]}, "health": {"hp": 50, "maxHp": 50}}},
    {"name": "wolf_side", "components": {"transform": {"pos": [3,0,0]}, "health": {"hp": 50, "maxHp": 50}}}
  ]
})";

TEST(GameplayMeleeArcLua) {
    const char* lua = R"(
      function on_start(e)
        Gameplay.MeleeArc({x=0,y=0,z=0}, {x=0,y=0,z=-1}, 3, 100, 28, e)
      end
    )";
    scene::GameRuntime runtime;
    scene::GameRuntimeConfig cfg;
    cfg.headless = true;
    cfg.readScript = [&](const std::string&) { return std::string(lua); };
    CHECK(runtime.Start(kGameplayCombatScene, cfg).Ok());

    const ecs::Entity front = runtime.FindNamedEntity("wolf_front");
    const ecs::Entity side = runtime.FindNamedEntity("wolf_side");
    CHECK_NEAR(runtime.EntityHealth(front).first, 22.0f, 1e-3); // 50 - 28
    CHECK_NEAR(runtime.EntityHealth(side).first, 50.0f, 1e-3); // out of the 100-deg arc
}

TEST(GameplayAoeLua) {
    const char* lua = R"(
      function on_start(e)
        Gameplay.AoE({x=0,y=0,z=0}, 2.5, 10, e)
      end
    )";
    scene::GameRuntime runtime;
    scene::GameRuntimeConfig cfg;
    cfg.headless = true;
    cfg.readScript = [&](const std::string&) { return std::string(lua); };
    CHECK(runtime.Start(kGameplayCombatScene, cfg).Ok());

    const ecs::Entity front = runtime.FindNamedEntity("wolf_front");
    const ecs::Entity side = runtime.FindNamedEntity("wolf_side");
    CHECK_NEAR(runtime.EntityHealth(front).first, 40.0f, 1e-3); // dist 2 <= 2.5
    CHECK_NEAR(runtime.EntityHealth(side).first, 50.0f, 1e-3);  // dist 3 > 2.5
}

TEST(GameplayProjectileLua) {
    const char* lua = R"(
      function on_start(e)
        Gameplay.Projectile({x=0,y=1,z=0}, {x=0,y=0,z=-1}, 14, 10, 2, 30, 1.0, e,
                            {{name="burning",duration=3,magnitude=2}})
      end
    )";
    scene::GameRuntime runtime;
    scene::GameRuntimeConfig cfg;
    cfg.headless = true;
    cfg.readScript = [&](const std::string&) { return std::string(lua); };
    CHECK(runtime.Start(kGameplayCombatScene, cfg).Ok());

    const ecs::Entity front = runtime.FindNamedEntity("wolf_front");
    for (int i = 0; i < 30; ++i) runtime.Tick(1.0f / 60.0f);
    CHECK_NEAR(runtime.EntityHealth(front).first, 40.0f, 1e-3); // 50 - 10
    CHECK(runtime.HasStatus(front, scene::kStatusBurning));
}

// ---------------------------------------------------------------------------
// Gameplay.SkillTable：JSON 技能表解析 + cast 分发（冷却/mana/kind）
// ---------------------------------------------------------------------------

// Builds a Lua script that embeds `kSkillsJson` as a long-string literal, then
// runs the caller-provided body (which sees `json` in scope).
static std::string SkillTableScript(const std::string& body) {
    return std::string(R"(
      local json = [=[)") + kSkillsJson + std::string(R"(]=]
      )") + body;
}

TEST(SkillTableCastMelee) {
    const std::string lua = SkillTableScript(R"(
      function on_start(e)
        local t = Gameplay.SkillTable.fromJson(json)
        Gameplay.SkillTable.cast(t, "cleave", {x=0,y=0,z=0}, {x=0,y=0,z=-1}, e)
      end
    )");
    scene::GameRuntime runtime;
    scene::GameRuntimeConfig cfg;
    cfg.headless = true;
    cfg.readScript = [&](const std::string&) { return lua; };
    CHECK(runtime.Start(kGameplayCombatScene, cfg).Ok());

    const ecs::Entity front = runtime.FindNamedEntity("wolf_front");
    const ecs::Entity side = runtime.FindNamedEntity("wolf_side");
    CHECK_NEAR(runtime.EntityHealth(front).first, 38.0f, 1e-3); // 50 - 12
    CHECK_NEAR(runtime.EntityHealth(side).first, 50.0f, 1e-3);  // out of the arc
}

TEST(SkillTableManaAndCooldown) {
    const std::string lua = SkillTableScript(R"(
      function on_start(e)
        local t = Gameplay.SkillTable.fromJson(json)
        -- No mana: fireball (manaCost 8) is refused, no cooldown recorded.
        SetVar("noMana", Gameplay.SkillTable.cast(t, "fireball", {x=0,y=1,z=0},
                                                  {x=0,y=0,z=-1}, e))
        Gameplay.Stats.Set("mana", 10)
        SetVar("castOk", Gameplay.SkillTable.cast(t, "fireball", {x=0,y=1,z=0},
                                                  {x=0,y=0,z=-1}, e))
        SetVar("manaAfter", Gameplay.Stats.Get("mana"))
        -- 0.5s cooldown still active -> immediate re-cast refused.
        SetVar("cdBlocked", Gameplay.SkillTable.cast(t, "fireball", {x=0,y=1,z=0},
                                                     {x=0,y=0,z=-1}, e))
      end
    )");
    scene::GameRuntime runtime;
    scene::GameRuntimeConfig cfg;
    cfg.headless = true;
    cfg.readScript = [&](const std::string&) { return lua; };
    CHECK(runtime.Start(kGameplayCombatScene, cfg).Ok());

    CHECK_NEAR(runtime.GameVar("noMana"), 0.0, 1e-6);
    CHECK_NEAR(runtime.GameVar("castOk"), 1.0, 1e-6);
    CHECK_NEAR(runtime.GameVar("manaAfter"), 2.0, 1e-6); // 10 - 8
    CHECK_NEAR(runtime.GameVar("cdBlocked"), 0.0, 1e-6);
}

// ---------------------------------------------------------------------------
// Gameplay.Inventory：通用背包（堆叠/上限/容量、remove/count、use 回调、货币、
// save/load 往返）。纯 Lua，仅依赖 Json.Parse。
// ---------------------------------------------------------------------------

TEST(InventoryStackingAndCapacity) {
    const char* scene = R"({
      "entities": [
        {"name": "hero", "components": {
          "transform": {"pos": [0,0,0]},
          "health": {"hp": 100, "maxHp": 100},
          "script": {"backend": "lua", "path": "inventory.lua"}}}
      ]
    })";
    const char* lua = R"(
      function on_start(e)
        local bag = Gameplay.Inventory.new(24)
        local potion = {id="potion", stackable=true, maxStack=10}
        SetVar("add1", Gameplay.Inventory.add(bag, potion, 3) and 1 or 0)
        SetVar("add2", Gameplay.Inventory.add(bag, potion, 7) and 1 or 0)
        SetVar("count10", Gameplay.Inventory.count(bag, "potion"))
        -- stack is full at 10; a further add would exceed maxStack -> false
        SetVar("addOver", Gameplay.Inventory.add(bag, potion, 1) and 1 or 0)
        SetVar("countStill10", Gameplay.Inventory.count(bag, "potion"))
        -- remove decrements and reports the actual amount taken
        SetVar("removed", Gameplay.Inventory.remove(bag, "potion", 4))
        SetVar("count6", Gameplay.Inventory.count(bag, "potion"))
        -- non-stackable: each item occupies its own slot; capacity 1 -> second refused
        local sword = {id="sword", stackable=false}
        local bag2 = Gameplay.Inventory.new(1)
        SetVar("sword1", Gameplay.Inventory.add(bag2, sword, 1) and 1 or 0)
        SetVar("sword2", Gameplay.Inventory.add(bag2, sword, 1) and 1 or 0)
        SetVar("swordCount", Gameplay.Inventory.count(bag2, "sword"))
      end
    )";
    scene::GameRuntime runtime;
    scene::GameRuntimeConfig cfg;
    cfg.headless = true;
    cfg.readScript = [&](const std::string&) { return std::string(lua); };
    CHECK(runtime.Start(scene, cfg).Ok());

    CHECK_NEAR(runtime.GameVar("add1"), 1.0, 1e-6);
    CHECK_NEAR(runtime.GameVar("add2"), 1.0, 1e-6);
    CHECK_NEAR(runtime.GameVar("count10"), 10.0, 1e-6);
    CHECK_NEAR(runtime.GameVar("addOver"), 0.0, 1e-6);
    CHECK_NEAR(runtime.GameVar("countStill10"), 10.0, 1e-6);
    CHECK_NEAR(runtime.GameVar("removed"), 4.0, 1e-6);
    CHECK_NEAR(runtime.GameVar("count6"), 6.0, 1e-6);
    CHECK_NEAR(runtime.GameVar("sword1"), 1.0, 1e-6);
    CHECK_NEAR(runtime.GameVar("sword2"), 0.0, 1e-6);
    CHECK_NEAR(runtime.GameVar("swordCount"), 1.0, 1e-6);
}

TEST(InventoryUseAndCurrency) {
    const char* scene = R"({
      "entities": [
        {"name": "hero", "components": {
          "transform": {"pos": [0,0,0]},
          "health": {"hp": 100, "maxHp": 100},
          "script": {"backend": "lua", "path": "inventory.lua"}}}
      ]
    })";
    const char* lua = R"(
      function on_start(e)
        local bag = Gameplay.Inventory.new(24)
        local potion = {id="potion", stackable=true, maxStack=10,
          onUse=function(ent) SetVar("used", (GetVar("used") or 0) + 1) end}
        Gameplay.Inventory.add(bag, potion, 2)
        SetVar("use1", Gameplay.Inventory.use(bag, "potion", e) and 1 or 0)
        SetVar("use2", Gameplay.Inventory.use(bag, "potion", e) and 1 or 0)
        SetVar("countAfterUse", Gameplay.Inventory.count(bag, "potion"))
        SetVar("useEmpty", Gameplay.Inventory.use(bag, "potion", e) and 1 or 0)
        SetVar("used", GetVar("used") or 0)
        -- currency add/subtract/query
        Gameplay.Inventory.addCurrency(bag, "gold", 100)
        Gameplay.Inventory.addCurrency(bag, "gold", -30)
        SetVar("gold", Gameplay.Inventory.getCurrency(bag, "gold"))
        SetVar("gems", Gameplay.Inventory.getCurrency(bag, "gems"))
      end
    )";
    scene::GameRuntime runtime;
    scene::GameRuntimeConfig cfg;
    cfg.headless = true;
    cfg.readScript = [&](const std::string&) { return std::string(lua); };
    CHECK(runtime.Start(scene, cfg).Ok());

    CHECK_NEAR(runtime.GameVar("use1"), 1.0, 1e-6);
    CHECK_NEAR(runtime.GameVar("use2"), 1.0, 1e-6);
    CHECK_NEAR(runtime.GameVar("countAfterUse"), 0.0, 1e-6);
    CHECK_NEAR(runtime.GameVar("useEmpty"), 0.0, 1e-6);
    CHECK_NEAR(runtime.GameVar("used"), 2.0, 1e-6);
    CHECK_NEAR(runtime.GameVar("gold"), 70.0, 1e-6);
    CHECK_NEAR(runtime.GameVar("gems"), 0.0, 1e-6);
}

TEST(InventorySaveLoadRoundTrip) {
    const char* scene = R"({
      "entities": [
        {"name": "hero", "components": {
          "transform": {"pos": [0,0,0]},
          "health": {"hp": 100, "maxHp": 100},
          "script": {"backend": "lua", "path": "inventory.lua"}}}
      ]
    })";
    const char* lua = R"(
      function on_start(e)
        local bag = Gameplay.Inventory.new(24)
        Gameplay.Inventory.add(bag, {id="potion", stackable=true, maxStack=10}, 7)
        Gameplay.Inventory.add(bag, {id="sword", stackable=false}, 3)
        Gameplay.Inventory.addCurrency(bag, "gold", 55)
        local json = Gameplay.Inventory.save(bag)
        SetVar("isStr", type(json) == "string" and 1 or 0)
        local defs = {
          potion = {id="potion", stackable=true, maxStack=10,
            onUse=function(ent) SetVar("usedAfter", (GetVar("usedAfter") or 0) + 1) end},
          sword = {id="sword", stackable=false},
        }
        local bag2 = Gameplay.Inventory.new(24)
        Gameplay.Inventory.load(bag2, json, defs)
        SetVar("potionCount", Gameplay.Inventory.count(bag2, "potion"))
        SetVar("swordCount", Gameplay.Inventory.count(bag2, "sword"))
        SetVar("goldAfter", Gameplay.Inventory.getCurrency(bag2, "gold"))
        -- defs re-attaches onUse after load (callbacks are never serialized)
        Gameplay.Inventory.use(bag2, "potion", e)
        SetVar("usedAfter", GetVar("usedAfter") or 0)
        SetVar("potionAfterUse", Gameplay.Inventory.count(bag2, "potion"))
      end
    )";
    scene::GameRuntime runtime;
    scene::GameRuntimeConfig cfg;
    cfg.headless = true;
    cfg.readScript = [&](const std::string&) { return std::string(lua); };
    CHECK(runtime.Start(scene, cfg).Ok());

    CHECK_NEAR(runtime.GameVar("isStr"), 1.0, 1e-6);
    CHECK_NEAR(runtime.GameVar("potionCount"), 7.0, 1e-6);
    CHECK_NEAR(runtime.GameVar("swordCount"), 3.0, 1e-6);
    CHECK_NEAR(runtime.GameVar("goldAfter"), 55.0, 1e-6);
    CHECK_NEAR(runtime.GameVar("usedAfter"), 1.0, 1e-6);
    CHECK_NEAR(runtime.GameVar("potionAfterUse"), 6.0, 1e-6);
}

TEST(InventoryUseOnUseFalseDoesNotConsume) {
    const char* scene = R"({
      "entities": [
        {"name": "hero", "components": {
          "transform": {"pos": [0,0,0]},
          "health": {"hp": 100, "maxHp": 100},
          "script": {"backend": "lua", "path": "inventory.lua"}}}
      ]
    })";
    const char* lua = R"(
      function on_start(e)
        local bag = Gameplay.Inventory.new(24)
        local key = {id="key", stackable=false, onUse=function(ent) return false end}
        Gameplay.Inventory.add(bag, key, 1)
        SetVar("useResult", Gameplay.Inventory.use(bag, "key", e) and 1 or 0)
        SetVar("countAfter", Gameplay.Inventory.count(bag, "key"))
      end
    )";
    scene::GameRuntime runtime;
    scene::GameRuntimeConfig cfg;
    cfg.headless = true;
    cfg.readScript = [&](const std::string&) { return std::string(lua); };
    CHECK(runtime.Start(scene, cfg).Ok());

    CHECK_NEAR(runtime.GameVar("useResult"), 0.0, 1e-6);
    CHECK_NEAR(runtime.GameVar("countAfter"), 1.0, 1e-6);
}

// ---------------------------------------------------------------------------
// Gameplay.FirstPerson / Gameplay.ThirdPerson：带状态控制器（相机 GameVar 语义）
//
// headless 下 InputMouseX/Y、ActionAxis、ActionDown 均返回 0（无输入），所以
// tick 后位置基本不变，但相机 GameVar 会被正确写入——这是可验证的核心行为。
// cameraFocus 是 table 型 GameVar（C++ 的 GameVar(name) 只读 number），所以
// 脚本侧把它转成 number 再回传（hasFocus / fx / fy / fz）。
// ---------------------------------------------------------------------------

TEST(GameplayFirstPersonCameraVars) {
    const char* scene = R"({
      "entities": [
        {"name": "hero", "components": {
          "transform": {"pos": [0,0,0]},
          "health": {"hp": 100, "maxHp": 100},
          "script": {"backend": "lua", "path": "fps.lua"}}}
      ]
    })";
    const char* lua = R"(
      function on_start(e)
        local c = Gameplay.FirstPerson.new(e)
        Gameplay.FirstPerson.tick(c, 0.016)
        SetVar("lock", GetVar("cameraMouseLock"))
        SetVar("dist", GetVar("cameraDist"))
        SetVar("yawEq", (GetVar("cameraYaw") == c.yaw) and 1 or 0)
        local f = GetVar("cameraFocus")
        SetVar("hasFocus", (f ~= nil) and 1 or 0)
        if f ~= nil then
          SetVar("fx", f.x or -1)
          SetVar("fy", f.y or -1)
          SetVar("fz", f.z or -1)
        end
      end
    )";
    scene::GameRuntime runtime;
    scene::GameRuntimeConfig cfg;
    cfg.headless = true;
    cfg.readScript = [&](const std::string&) { return std::string(lua); };
    CHECK(runtime.Start(scene, cfg).Ok());

    CHECK_NEAR(runtime.GameVar("lock"), 1.0, 1e-6);
    CHECK_NEAR(runtime.GameVar("dist"), 2.0, 1e-6);
    CHECK_NEAR(runtime.GameVar("yawEq"), 1.0, 1e-6);
    CHECK_NEAR(runtime.GameVar("hasFocus"), 1.0, 1e-6);
    // yaw=0：focus = eye - sin(yaw)*cd*dist（x 分量 0）、-cos(yaw)*cd*dist（z 负）。
    CHECK_NEAR(runtime.GameVar("fx"), 0.0, 1e-6);
    CHECK(runtime.GameVar("fz") < 0.0);
}

TEST(GameplayThirdPersonCameraVars) {
    const char* scene = R"({
      "entities": [
        {"name": "hero", "components": {
          "transform": {"pos": [0,0,0]},
          "health": {"hp": 100, "maxHp": 100},
          "script": {"backend": "lua", "path": "third.lua"}}}
      ]
    })";
    const char* lua = R"(
      function on_start(e)
        local c = Gameplay.ThirdPerson.new(e)
        Gameplay.ThirdPerson.tick(c, 0.016)
        SetVar("lock", GetVar("cameraMouseLock"))
        SetVar("dist", GetVar("cameraDist"))
        local f = GetVar("cameraFocus")
        SetVar("hasFocus", (f ~= nil) and 1 or 0)
        if f ~= nil then
          SetVar("fy", f.y or -1)
        end
      end
    )";
    scene::GameRuntime runtime;
    scene::GameRuntimeConfig cfg;
    cfg.headless = true;
    cfg.readScript = [&](const std::string&) { return std::string(lua); };
    CHECK(runtime.Start(scene, cfg).Ok());

    CHECK_NEAR(runtime.GameVar("lock"), 0.0, 1e-6);
    CHECK_NEAR(runtime.GameVar("dist"), 7.5, 1e-6);
    CHECK_NEAR(runtime.GameVar("hasFocus"), 1.0, 1e-6);
    CHECK_NEAR(runtime.GameVar("fy"), 1.2, 1e-6);
}

// ---------------------------------------------------------------------------
// Task 13: realm.lua 迁移到 Gameplay 库后，至少能加载 + 运行不报错。
// 直接读项目的 realm.lua 实际文本注入最小场景（hero 携带脚本），确认
// on_start 的 FindNamedEntity("狼_i") 返回 nil 是安全的，on_update 跑若干帧
// 不崩溃。文件缺失（cwd 不在仓库根）时跳过，避免误报。
// ---------------------------------------------------------------------------

TEST(NeonRealmScriptLoadsAndRuns) {
    std::string realm;
    const char* kPath = "projects/neon_realm/assets/scripts/realm.lua";
    if (!test::ReadFileAll(kPath, realm) || realm.empty()) return;

    const char* scene = R"({
      "entities": [
        {"name": "hero", "components": {
          "transform": {"pos": [0,0,0]},
          "health": {"hp": 100, "maxHp": 100},
          "script": {"backend": "lua", "path": "realm.lua"}}}
      ]
    })";
    scene::GameRuntime runtime;
    scene::GameRuntimeConfig cfg;
    cfg.headless = true;
    cfg.readScript = [&](const std::string&) { return realm; };
    CHECK(runtime.Start(scene, cfg).Ok());

    const ecs::Entity hero = runtime.FindNamedEntity("hero");
    CHECK(hero.IsValid());

    // on_start 已执行：realm.lua 会写入 level=1 等 GameVar，证明脚本真实加载。
    CHECK_NEAR(runtime.GameVar("level"), 1.0, 1e-6);

    // 约 1 秒（60 帧），覆盖 on_update 的 update_player / update_wolves /
    // update_waves / update_npc / update_hero_anim / update_camera / update_vfx。
    for (int i = 0; i < 60; ++i) runtime.Tick(1.0f / 60.0f);
}

// ---------------------------------------------------------------------------
// Task 2: HudSystem 独立单测（脱离 GameRuntime，直接测拆分后的 HUD 子系统）：
// 飘字 age 推进 / 头顶板查询 / WorldToScreen 投影 sanity。
// ---------------------------------------------------------------------------

TEST(HudSystemFloatTextAgesAndExpires) {
    scene::HudSystem hud;
    hud.SpawnFloatText({0, 1, 0}, "crit", true, 3.0f);
    hud.SpawnFloatText({3, 4, 5}, "dmg", false, 0.5f);
    CHECK_EQ(hud.FloatTexts().size(), 2u);
    CHECK(hud.FloatTexts()[0].crit);
    CHECK_EQ(hud.FloatTexts()[0].text, std::string("crit"));
    CHECK_NEAR(hud.FloatTexts()[0].age, 0.0f, 1e-6);

    hud.Tick(0.25f);
    CHECK_EQ(hud.FloatTexts().size(), 2u);
    CHECK_NEAR(hud.FloatTexts()[0].age, 0.25f, 1e-6);

    for (int i = 0; i < 20; ++i) hud.Tick(0.1f); // 2.25s total
    CHECK_EQ(hud.FloatTexts().size(), 1u);      // 0.5s "dmg" expired
    CHECK_EQ(hud.FloatTexts()[0].text, std::string("crit"));
    CHECK_NEAR(hud.FloatTexts()[0].age, 2.25f, 1e-6);

    for (int i = 0; i < 20; ++i) hud.Tick(0.1f); // 4.25s total > 3.0s
    CHECK(hud.FloatTexts().empty());
}

TEST(HudSystemEntityPlateRoundTrip) {
    ecs::World world;
    ecs::Entity e = world.Create();
    scene::HudSystem hud;
    hud.SetEntityPlate(e, "Wolf", 0.75f);
    CHECK_EQ(hud.EntityPlates().size(), 1u);
    const uint64_t key = (static_cast<uint64_t>(e.id) << 32) |
                         static_cast<uint64_t>(e.generation);
    const auto it = hud.EntityPlates().find(key);
    CHECK(it != hud.EntityPlates().end());
    CHECK_EQ(it->second.name, std::string("Wolf"));
    CHECK_NEAR(it->second.hpFrac, 0.75f, 1e-6);
}

TEST(HudSystemWorldToScreenProjection) {
    scene::HudSystem hud;
    // Before any CaptureView, the projection is unavailable (matches the
    // runtime's pre-Draw behavior).
    float x = -1.0f, y = -1.0f;
    CHECK(!hud.WorldToScreen({0, 0, 0}, x, y));

    // Ortho camera on +Z looking -Z: world origin lands at the viewport
    // centre (640, 360) for a 1280x720 design viewport.
    gfx::Camera cam;
    cam.position = {0, 0, 10};
    cam.target = {0, 0, 0};
    cam.up = {0, 1, 0};
    cam.ortho = true;
    cam.orthoSize = 5.0f;
    hud.CaptureView(cam, 16.0f / 9.0f, 1280.0f, 720.0f);

    CHECK(hud.WorldToScreen({0, 0, 0}, x, y));
    CHECK_NEAR(x, 640.0f, 1e-3f);
    CHECK_NEAR(y, 360.0f, 1e-3f);
    // An off-axis point moves right / up in viewport pixels.
    CHECK(hud.WorldToScreen({2, 0, 0}, x, y));
    CHECK(x > 640.0f);
    CHECK_NEAR(y, 360.0f, 1e-3f);
    CHECK_EQ(hud.DesignWidth(), 1280.0f);
}

// P1-3: TweenSystem drives the same easing as the runtime's old TickTweens —
// linear pos / ease-in scale advance over time and finished tweens are dropped.
TEST(TweenSystemStartTickWriteTransform) {
    ecs::World world;
    ecs::Entity e = world.Create();
    world.Add<scene::SceneTransform>(e, scene::SceneTransform{{0, 0, 0}});
    scene::TweenSystem ts;

    // time <= 0 is rejected (matches the old tweenStart guard).
    ts.Start(e, 0, {0, 0, 0}, {0, 0, 10}, 0.0f, 0);
    CHECK_EQ(ts.Count(), 0u);

    // Linear position tween: 0 -> z=10 over 1s.
    ts.Start(e, 0, {0, 0, 0}, {0, 0, 10}, 1.0f, 0);
    CHECK_EQ(ts.Count(), 1u);
    ts.Tick(0.5f, world);
    CHECK_NEAR(world.Get<scene::SceneTransform>(e)->pos.z, 5.0f, 1e-5f);
    ts.Tick(0.5f, world);
    CHECK_NEAR(world.Get<scene::SceneTransform>(e)->pos.z, 10.0f, 1e-5f);
    CHECK_EQ(ts.Count(), 0u); // finished tweens dropped

    // Ease-in scale tween: at a=0.5 the eased factor is 0.25 -> scale 1.25.
    ts.Start(e, 2, {1, 1, 1}, {2, 2, 2}, 1.0f, 1);
    ts.Tick(0.5f, world);
    const auto& sc = world.Get<scene::SceneTransform>(e)->scale;
    CHECK_NEAR(sc.x, 1.25f, 1e-5f);
    CHECK_NEAR(sc.y, 1.25f, 1e-5f);
    ts.Tick(0.5f, world);
    CHECK_NEAR(world.Get<scene::SceneTransform>(e)->scale.x, 2.0f, 1e-5f);
    CHECK_EQ(ts.Count(), 0u);

    // Clear drops pending tweens (Stop lifecycle).
    ts.Start(e, 0, {0, 0, 0}, {1, 1, 1}, 1.0f, 0);
    ts.Clear();
    CHECK_EQ(ts.Count(), 0u);
}

// Task 7: PrefabSystem owns the prefab library + the Spawn instance builder;
// GameRuntime injects the world-level instantiate callback. Standalone here:
// Load registers only *.json files (stem = prefab name) via the injected
// enumeration/read callbacks, and Spawn hands the parsed single-entity
// SceneFile (prefab + unique name + transform override) to the callback.
TEST(PrefabSystemLoadAndSpawn) {
    test::TempDir tmp;
    const std::string dir = tmp.Str();
#if defined(_WIN32)
    ::_mkdir((dir + "/assets").c_str());
    ::_mkdir((dir + "/assets/prefabs").c_str());
#else
    ::mkdir((dir + "/assets").c_str(), 0777);
    ::mkdir((dir + "/assets/prefabs").c_str(), 0777);
#endif
    CHECK(test::WriteFileAll(
        dir + "/assets/prefabs/pea.json",
        R"({"components": {"sprite": {"texture": "assets/sprites/pea.png"}}})"));
    CHECK(test::WriteFileAll(dir + "/assets/prefabs/readme.txt", "ignore me"));

    auto readFile = [&](const std::string& p) {
        std::ifstream in(p, std::ios::binary);
        return std::string((std::istreambuf_iterator<char>(in)),
                           std::istreambuf_iterator<char>());
    };

    // Load scans <base>/assets/prefabs via injected callbacks; only .json
    // files register, non-json entries are skipped silently.
    scene::PrefabSystem prefs;
    prefs.Load(
        dir,
        [&](const std::string& d) {
            return std::vector<std::string>{d + "/pea.json", d + "/readme.txt"};
        },
        readFile);
    CHECK_EQ(prefs.Count(), 1u);
    CHECK(prefs.Library().Has("pea"));
    CHECK(!prefs.Library().Has("readme"));

    // Unknown name -> no-op; without an instantiate callback Spawn also no-ops.
    CHECK(!prefs.Spawn("nope", {0, 0, 0}).IsValid());
    CHECK(!prefs.Spawn("pea", {0, 0, 0}).IsValid());

    // With a callback, Spawn forwards the parsed single-entity SceneFile:
    // prefab name, unique instance name and the transform override position.
    ecs::World world;
    ecs::Entity created;
    scene::PrefabSystem live;
    live.SetInstantiate([&](const scene::SceneFile& scene) {
        CHECK_EQ(scene.entities.size(), 1u);
        CHECK_EQ(scene.entities[0].prefab, std::string("pea"));
        CHECK(!scene.entities[0].name.empty());
        bool sawTransform = false;
        for (const auto& c : scene.entities[0].components) {
            if (c.name != "transform") continue;
            const core::Json* pos = c.data.Get("pos");
            CHECK(pos && pos->IsArray() && pos->Size() == 3);
            if (pos) {
                CHECK_NEAR(pos->At(0)->GetNumber(), 1.0, 1e-6);
                CHECK_NEAR(pos->At(1)->GetNumber(), 2.0, 1e-6);
                CHECK_NEAR(pos->At(2)->GetNumber(), 3.0, 1e-6);
            }
            sawTransform = true;
        }
        CHECK(sawTransform);
        created = world.Create();
        return created;
    });
    live.Load(
        dir,
        [&](const std::string& d) { return std::vector<std::string>{d + "/pea.json"}; },
        readFile);
    const ecs::Entity e = live.Spawn("pea", {1, 2, 3});
    CHECK(e.IsValid());
    CHECK_EQ(e.id, created.id);
    CHECK_EQ(live.Count(), 1u);
    live.Clear();
    CHECK_EQ(live.Count(), 0u);
}

// Task 8: PluginSystem owns the runtime plugin lifecycle (create manager,
// scan <scriptBaseDir>/plugins in dependency order -> on_load/on_start, tick,
// stop). GameRuntime just forwards event/command access; standalone here we
// verify the whole lifecycle through the wrapper + its forwarders.
TEST(PluginSystemLifecycle) {
    test::TempDir tmp;
    const std::string dir = tmp.Str();
#if defined(_WIN32)
    ::_mkdir((dir + "/plugins").c_str());
    ::_mkdir((dir + "/plugins/lua_mod").c_str());
#else
    ::mkdir((dir + "/plugins").c_str(), 0777);
    ::mkdir((dir + "/plugins/lua_mod").c_str(), 0777);
#endif
    CHECK(test::WriteFileAll(
        dir + "/plugins/lua_mod/plugin.json",
        R"({"id":"lua_mod","type":"runtime","backend":"lua","entry":"init.lua"})"));
    CHECK(test::WriteFileAll(
        dir + "/plugins/lua_mod/init.lua",
        R"(function on_load()
  Plugin.SetVar("ticks", 0)
  Plugin.On("tick", function(dt)
    Plugin.SetVar("ticks", Plugin.GetVar("ticks") + 1)
  end)
  Plugin.On("player_join", function(cid)
    Plugin.SetVar("joined", cid)
  end)
  Plugin.OnCommand("set_greeting", function(g)
    Plugin.SetVar("greeting", g)
    return true
  end)
end
function on_start()
  Plugin.SetVar("started", true)
end
)"));

    auto readFile = [&](const std::string& p) {
        std::ifstream in(p, std::ios::binary);
        return std::string((std::istreambuf_iterator<char>(in)),
                           std::istreambuf_iterator<char>());
    };

    // Idle state: no manager, forwarders no-op.
    script::ScriptContext ctx;
    scene::PluginSystem ps;
    CHECK(!ps.Active());
    CHECK(ps.Manager() == nullptr);
    std::string err;
    CHECK(!ps.DispatchEvent("player_join", {script::Value::Num(1)}));
    CHECK(!ps.RunCommand("set_greeting", {script::Value::Str("hi")}, &err));

    // Load scans <base>/plugins (injected reader), runs on_load + on_start.
    ps.Load(dir, readFile, &ctx, 12345);
    CHECK(ps.Active());
    CHECK(ps.Manager() != nullptr);
    CHECK_EQ(ps.Manager()->Count(), 1u);
    CHECK(ctx.gameVars.Get("plugin:lua_mod:started").type == script::Value::Type::Bool);

    // Tick dispatches to the subscribed handler with the sim clock injected.
    ps.Tick(1.0f / 60.0f, 0.0);
    ps.Tick(1.0f / 60.0f, 1.0 / 60.0);
    CHECK_EQ(ctx.gameVars.Get("plugin:lua_mod:ticks").number, 2.0);

    // Event + command forwarders reach the loaded manager.
    CHECK(ps.DispatchEvent("player_join", {script::Value::Num(42)}));
    CHECK_EQ(ctx.gameVars.Get("plugin:lua_mod:joined").number, 42.0);
    CHECK(ps.RunCommand("set_greeting", {script::Value::Str("hello")}, &err));
    CHECK_EQ(ctx.gameVars.Get("plugin:lua_mod:greeting").str, std::string("hello"));
    CHECK(!ps.RunCommand("missing", {}, &err));

    // Shutdown dispatches stop and reclaims the manager; forwarders no-op.
    ps.Shutdown();
    CHECK(!ps.Active());
    CHECK(ps.Manager() == nullptr);
    CHECK(!ps.DispatchEvent("player_join", {}));
    ps.Shutdown(); // idempotent
}

// Task 9: SceneTreeSystem 独立子系统 —— 直接用 ecs::World 驱动（不经过
// GameRuntime），验证层级遍历与世界变换缓存（父先子后、任意深度）。
TEST(SceneTreeSystemStandalone) {
    ecs::World world;
    scene::SceneTreeSystem tree;

    const ecs::Entity root = world.Create();
    const ecs::Entity child = world.Create();
    const ecs::Entity grand = world.Create();
    std::vector<ecs::Entity> deep;
    for (int i = 0; i < 9; ++i) deep.push_back(world.Create()); // 11-level chain

    world.Add<scene::SceneTransform>(root, {{1, 2, 3}});
    world.Add<scene::SceneTransform>(child, {{10, 0, 0}});
    world.Add<scene::SceneParentLink>(child, {root});
    world.Add<scene::SceneTransform>(grand, {{0, 5, 0}});
    world.Add<scene::SceneParentLink>(grand, {child});

    ecs::Entity prev = root;
    for (const ecs::Entity& d : deep) {
        world.Add<scene::SceneTransform>(d, {{1, 0, 0}});
        world.Add<scene::SceneParentLink>(d, {prev});
        prev = d;
    }

    auto translation = [](const math::Mat4& m) {
        return math::Vec3{m.m[3], m.m[7], m.m[11]};
    };

    const std::vector<ecs::Entity> children = tree.GetChildren(world, root);
    CHECK_EQ(children.size(), 2u); // child + deep[0]
    bool hasChild = false, hasDeep = false;
    for (const ecs::Entity& e : children) {
        if (e == child) hasChild = true;
        if (e == deep[0]) hasDeep = true;
    }
    CHECK(hasChild);
    CHECK(hasDeep);

    const std::vector<ecs::Entity> desc = tree.GetDescendants(world, root);
    CHECK_EQ(desc.size(), 11u); // child, grand, deep[0..8]

    tree.Rebuild(world);
    CHECK_NEAR(translation(tree.CachedLocalToWorld(root)).x, 1.0f, 1e-5f);
    CHECK_NEAR(translation(tree.CachedLocalToWorld(child)).x, 11.0f, 1e-5f);
    CHECK_NEAR(translation(tree.CachedLocalToWorld(grand)).x, 11.0f, 1e-5f);
    CHECK_NEAR(translation(tree.CachedLocalToWorld(grand)).y, 7.0f, 1e-5f);
    // 1 (root) + 9 * 1 = 10: beyond the old 8-level walk cap.
    CHECK_NEAR(translation(tree.CachedLocalToWorld(deep[8])).x, 10.0f, 1e-5f);

    // Mutations are reflected after a rebuild.
    world.Get<scene::SceneTransform>(child)->pos = {100.0f, 0.0f, 0.0f};
    tree.Rebuild(world);
    CHECK_NEAR(translation(tree.CachedLocalToWorld(grand)).x, 101.0f, 1e-5f);

    // Unknown entity -> identity.
    CHECK(translation(tree.CachedLocalToWorld(ecs::Entity{})).x == 0.0f);
}

// Task 10: ScriptCanvas 独立单测（脱离 GameRuntime，直接测拆分后的脚本 2D
// 画布子系统）：命令容器（Begin/Add/Count/Empty）、Commands() 指针接线
// （on_render 期间 scriptCtx_.draw2d 指向它）、Flush 把命令刷进 renderer
// overlay（headless NullBackend，无窗口无 GPU）。
TEST(ScriptCanvasStandalone) {
    scene::ScriptCanvas canvas;
    CHECK(canvas.Empty());
    CHECK_EQ(canvas.Count(), 0u);

    // Commands() exposes the live buffer the script bindings push into.
    std::vector<script::Draw2DCmd>* cmds = canvas.Commands();
    CHECK(cmds != nullptr);
    CHECK_EQ(cmds->size(), 0u);

    script::Draw2DCmd rect;
    rect.kind = script::Draw2DCmd::Kind::Rect;
    rect.x = 10.0f;
    rect.y = 20.0f;
    rect.w = 30.0f;
    rect.h = 40.0f;
    canvas.Add(rect);

    script::Draw2DCmd outline;
    outline.kind = script::Draw2DCmd::Kind::RectOutline;
    outline.thickness = 2.0f;
    canvas.Add(outline);

    script::Draw2DCmd text;
    text.kind = script::Draw2DCmd::Kind::Text;
    text.text = "hi";
    text.size = 16.0f;
    canvas.Add(text);

    CHECK(!canvas.Empty());
    CHECK_EQ(canvas.Count(), 3u);
    CHECK_EQ(cmds->size(), 3u); // same buffer as Add

    // Flush into a headless renderer: Rect/RectOutline push overlay quads, Text
    // is skipped (invalid default font). No window/GPU needed.
    gfx::Renderer renderer;
    renderer.AttachBackendForTesting(std::make_unique<test::NullBackend>());
    gfx::Font font;
    CHECK(!font.Valid());
    canvas.Flush(renderer, font);
    CHECK_EQ(canvas.Count(), 3u); // Flush does not consume the buffer

    canvas.Begin();
    CHECK(canvas.Empty());
    CHECK_EQ(canvas.Count(), 0u);
    CHECK_EQ(cmds->size(), 0u);
}

namespace {

// Minimal IUiSystem fake for the UiSystem thin-wrapper test (Task 11): every
// call is recorded so the test asserts forwarding without touching the real
// document-backed system (no VFS / ui/*.ui.json files needed).
class FakeUi : public ui::IUiSystem {
public:
    bool Show(const std::string& path) override {
        shown = path;
        return active = true;
    }
    void Hide() override { active = false; }
    bool Active() const override { return active; }
    void Update(const math::Vec2&, bool clickEdge) override { lastClickEdge = clickEdge; }
    bool Clicked(const std::string& name) const override {
        lastClicked = name;
        return name == "Start";
    }
    void SetText(const std::string& n, const std::string& t) override {
        textNode = n;
        textValue = t;
    }
    void SetFill(const std::string& n, float f) override {
        fillNode = n;
        fillValue = f;
    }
    void SetVisible(const std::string& n, bool v) override {
        visibleNode = n;
        visibleValue = v;
    }
    void SetColor(const std::string& n, float r, float g, float b, float a) override {
        colorNode = n;
        colorValue[0] = r;
        colorValue[1] = g;
        colorValue[2] = b;
        colorValue[3] = a;
    }
    void Draw(gfx::Renderer&, const gfx::Font&, const ui::UiTextureLoader&,
              const math::Vec2& viewportSize) override {
        drawViewport = viewportSize;
    }

    std::string shown;
    bool active = false;
    bool lastClickEdge = false;
    mutable std::string lastClicked;
    std::string textNode, textValue;
    std::string fillNode;
    float fillValue = 0.0f;
    std::string visibleNode;
    bool visibleValue = false;
    std::string colorNode;
    float colorValue[4] = {};
    math::Vec2 drawViewport;
};

} // namespace

// Task 11: UiSystem 独立单测（脱离 GameRuntime，直接测拆分后的游戏 UI 薄包装）：
// 未安装系统时所有调用安全 no-op（不崩、返回 false）；安装 FakeUi 后
// Set/Show/Hide/Active/Update/Clicked/SetText/SetFill/SetVisible/SetColor/
// ConsumeClicks/Draw 逐项转发到底层 IUiSystem，Raw() 暴露裸指针，Set(nullptr)
// 清空系统回到 no-op。
TEST(UiSystemStandalone) {
    scene::UiSystem ui;

    // No system installed: safe no-ops with sane defaults (no crash).
    CHECK(!ui.Show("ui/menu.ui.json"));
    CHECK(!ui.Active());
    CHECK(!ui.Clicked("Start"));
    CHECK(ui.Raw() == nullptr);
    ui.Hide();
    ui.SetText("Start", "GO");
    ui.SetFill("Hp", 0.5f);
    ui.SetVisible("Panel", false);
    ui.SetColor("Start", 1.0f, 0.0f, 0.0f, 1.0f);
    ui.ConsumeClicks();
    ui.Update({100.0f, 200.0f}, false);

    // Install a fake IUiSystem: every call forwards to it.
    auto fake = std::make_shared<FakeUi>();
    ui.Set(fake);
    CHECK_EQ(ui.Raw(), fake.get());

    CHECK(ui.Show("ui/menu.ui.json"));
    CHECK_EQ(fake->shown, "ui/menu.ui.json");
    CHECK(ui.Active());
    ui.Hide();
    CHECK(!ui.Active());

    ui.Update({100.0f, 200.0f}, true);
    CHECK(fake->lastClickEdge);

    CHECK(ui.Clicked("Start"));
    CHECK_EQ(fake->lastClicked, "Start");
    CHECK(!ui.Clicked("Other"));
    CHECK_EQ(fake->lastClicked, "Other");

    ui.SetText("Start", "GO");
    CHECK_EQ(fake->textNode, "Start");
    CHECK_EQ(fake->textValue, "GO");
    ui.SetFill("Hp", 0.75f);
    CHECK_EQ(fake->fillNode, "Hp");
    CHECK_NEAR(fake->fillValue, 0.75f, 1e-6f);
    ui.SetVisible("Panel", false);
    CHECK_EQ(fake->visibleNode, "Panel");
    CHECK(!fake->visibleValue);
    ui.SetColor("Start", 0.2f, 0.4f, 0.6f, 0.8f);
    CHECK_EQ(fake->colorNode, "Start");
    CHECK_NEAR(fake->colorValue[0], 0.2f, 1e-6f);
    CHECK_NEAR(fake->colorValue[1], 0.4f, 1e-6f);
    CHECK_NEAR(fake->colorValue[3], 0.8f, 1e-6f);

    ui.ConsumeClicks();
    CHECK(ui.Show("ui/pause.ui.json")); // ConsumeClicks leaves the doc usable
    CHECK(ui.Active());

    // Draw forwards the design-space viewport to the fake (headless renderer).
    gfx::Renderer renderer;
    renderer.AttachBackendForTesting(std::make_unique<test::NullBackend>());
    gfx::Font font;
    ui.Draw(renderer, font, {}, {1280.0f, 720.0f});
    CHECK_NEAR(fake->drawViewport.x, 1280.0f, 1e-6f);
    CHECK_NEAR(fake->drawViewport.y, 720.0f, 1e-6f);

    // Set(nullptr) clears the system: back to safe no-ops.
    ui.Set(nullptr);
    CHECK(ui.Raw() == nullptr);
    CHECK(!ui.Show("x"));
    CHECK(!ui.Active());
}

TEST(ScriptRuntimeStandalone) {
    // A fresh Lua host + world wired like GameRuntime::Start: ScriptRuntime
    // drives the load/dedup/capture + per-entity dispatch on its own (Task 13).
    std::unique_ptr<script::IScriptHost> host = script::CreateLuaHost();
    CHECK(host != nullptr);
    CHECK(host->Init());

    scene::ScriptRuntime rt;
    scene::ScriptRuntime::Content content;
    content.readScript = [](const std::string& path) -> std::string {
        if (path == "counter.lua") return R"(
function on_start(e)
  SetVar("started", true)
end
function on_update(e, dt)
  local g = GetVar("gold")
  if g == nil then g = 0 end
  SetVar("gold", g + 1)
end
)";
        if (path == "vars.lua") return R"(
function on_start(e)
  local m = msg
  if m == nil then m = "default" end
  SetVar("msg", m)
end
)";
        return "";
    };
    rt.Configure(std::move(content));

    ecs::World world;
    script::ScriptContext ctx;
    ctx.world = &world;
    script::RegisterEngineBindings(*host, ctx);

    scene::ScriptRuntime::Hosts hosts{host.get(), nullptr};

    // Two entities attach to the same chunk: it loads once, both get instances,
    // on_start runs for each, on_update ticks per entity.
    ecs::Entity a = world.Create();
    ecs::Entity b = world.Create();
    scene::SceneScript s;
    s.backend = "lua";
    s.path = "counter.lua";
    CHECK(rt.AttachOne(a, s, ctx, hosts));
    CHECK(rt.AttachOne(b, s, ctx, hosts));
    CHECK_EQ(rt.Count(), 2u);
    CHECK(ctx.gameVars.Get("started").type == script::Value::Type::Bool);
    CHECK(ctx.gameVars.Get("started").boolean);

    rt.Tick(1.0f / 60.0f, world, ctx);
    CHECK_EQ(ctx.gameVars.Get("gold").number, 2.0); // one per entity per tick

    // HasFunction / CallFunction drive the shared global namespace directly
    // (Lua-first lookup) — the server-facing on_player_join plumbing.
    CHECK(rt.HasFunction(hosts, "on_update"));
    CHECK(!rt.HasFunction(hosts, "no_such_fn"));
    CHECK(rt.CallFunction(ctx, hosts, "on_update",
                          {script::Value::Num(0), script::Value::Num(1.0f)}));
    CHECK_EQ(ctx.gameVars.Get("gold").number, 3.0);
    CHECK(!rt.CallFunction(ctx, hosts, "no_such_fn", {}));

    // A missing file is skipped without failing or registering an instance.
    scene::SceneScript missing;
    missing.backend = "lua";
    missing.path = "nope.lua";
    CHECK(!rt.AttachOne(world.Create(), missing, ctx, hosts));
    CHECK_EQ(rt.Count(), 2u);

    // Unsafe paths (".." / absolute) are rejected outright.
    scene::SceneScript evil;
    evil.backend = "lua";
    evil.path = "../secret.lua";
    CHECK(!rt.AttachOne(world.Create(), evil, ctx, hosts));
    CHECK_EQ(rt.Count(), 2u);

    // Clear resets instances AND load state: a re-attach reloads the chunk.
    rt.Clear();
    CHECK_EQ(rt.Count(), 0u);
    CHECK(rt.AttachOne(world.Create(), s, ctx, hosts));
    CHECK_EQ(rt.Count(), 1u);

    // Per-instance declared vars (A6): two entities of one script keep
    // independent injected globals, so on_start reads its OWN declared value.
    scene::SceneScript vars;
    vars.backend = "lua";
    vars.path = "vars.lua";
    vars.vars = core::Json::Parse(R"({"msg":"hello"})");
    CHECK(rt.AttachOne(world.Create(), vars, ctx, hosts));
    CHECK_EQ(rt.Instances().back().vars.table->fields.size(), 1u);
    CHECK_EQ(ctx.gameVars.Get("msg").str, std::string("hello"));

    host->Shutdown();
}

// ---------------------------------------------------------------------------
// PhysicsBridge (Task 15): bridges ECS rigidbody/character components to a
// physics::World — registration, fixed-step advance, transform writeback, and
// scripted teleport sync (A8). Standalone; no GameRuntime dependency.
// ---------------------------------------------------------------------------

TEST(PhysicsBridgeRegistersStepsAndSyncsBodies) {
    scene::PhysicsBridge bridge;
    std::unique_ptr<physics::World, std::function<void(physics::World*)>> world(
        new physics::World(), [](physics::World* w) { delete w; });
    bridge.SetWorld(std::move(world), nullptr);
    CHECK(bridge.World() != nullptr);

    ecs::World ew;
    ecs::Entity ball = ew.Create();
    ew.Add<scene::SceneRigidBody>(ball);
    scene::SceneRigidBody* rb = ew.Get<scene::SceneRigidBody>(ball);
    rb->dynamic = true;
    rb->radius = 0.5f;
    ew.Add<scene::SceneTransform>(ball);
    ew.Get<scene::SceneTransform>(ball)->pos = {0, 5, 0};

    bridge.RegisterBodies(ew);
    CHECK_EQ(bridge.BodyCount(), 1u);
    CHECK(rb->bodyId != 0); // body id stored back on the component

    // Fixed 60 Hz steps: 3s of sim -> the ball rests on the ground (y = radius).
    for (int i = 0; i < 180; ++i) {
        bridge.Step(1.0f / 60.0f, {0, -9.81f, 0});
        bridge.SyncBodies(ew);
    }
    CHECK_NEAR(ew.Get<scene::SceneTransform>(ball)->pos.y, 0.5f, 0.02f);

    // A8: scripted teleport also moves the physics body (next SyncBodies keeps
    // the transform; the body does not fall back during the writeback).
    bridge.SetBodyPosition(ew, ball, {3, 2, 0});
    bridge.SyncBodies(ew);
    CHECK_NEAR(ew.Get<scene::SceneTransform>(ball)->pos.x, 3.0f, 1e-5);
    CHECK_NEAR(ew.Get<scene::SceneTransform>(ball)->pos.y, 2.0f, 1e-5);

    bridge.Clear();
    CHECK_EQ(bridge.BodyCount(), 0u);
    CHECK(bridge.World() != nullptr); // world object survives; bodies cleared
}

TEST(PhysicsBridgeStaticBodiesAndCharacters) {
    scene::PhysicsBridge bridge;
    std::unique_ptr<physics::World, std::function<void(physics::World*)>> world(
        new physics::World(), [](physics::World* w) { delete w; });
    bridge.SetWorld(std::move(world), nullptr);

    // Character: the custom deterministic world has no controller -> invalid id.
    ecs::World ew;
    ecs::Entity ch = ew.Create();
    ew.Add<scene::SceneCharacter>(ch);
    scene::SceneCharacter* c = ew.Get<scene::SceneCharacter>(ch);
    c->radius = 0.4f;
    c->halfHeight = 0.9f;
    ew.Add<scene::SceneTransform>(ch);
    ew.Get<scene::SceneTransform>(ch)->pos = {1, 0, 0};
    bridge.RegisterCharacters(ew);
    CHECK_EQ(c->bodyId, 0u);
    CHECK_EQ(bridge.BodyCount(), 0u);

    // Static body: registered, but never synced (B14) and never moved.
    ecs::Entity box = ew.Create();
    ew.Add<scene::SceneRigidBody>(box);
    scene::SceneRigidBody* b = ew.Get<scene::SceneRigidBody>(box);
    b->dynamic = false;
    b->shape = "box";
    b->halfExtents = {1, 1, 1};
    ew.Add<scene::SceneTransform>(box);
    ew.Get<scene::SceneTransform>(box)->pos = {10, 0, 0};
    bridge.RegisterBodies(ew);
    CHECK_EQ(bridge.BodyCount(), 1u);

    for (int i = 0; i < 60; ++i) {
        bridge.Step(1.0f / 60.0f, {0, -9.81f, 0});
        bridge.SyncBodies(ew);
    }
    CHECK_NEAR(ew.Get<scene::SceneTransform>(box)->pos.x, 10.0f, 1e-6);
    CHECK_NEAR(ew.Get<scene::SceneTransform>(box)->pos.y, 0.0f, 1e-6);
}

TEST(PhysicsBridgeAcceptsInjectedWorld) {
    scene::PhysicsBridge bridge;
    physics::World injected; // owned by the test; the bridge must not delete it
    bridge.SetWorld(
        std::unique_ptr<physics::World, std::function<void(physics::World*)>>(
            &injected, [](physics::World*) {}),
        nullptr);
    CHECK(bridge.World() == &injected);

    ecs::World ew;
    ecs::Entity ball = ew.Create();
    ew.Add<scene::SceneRigidBody>(ball);
    scene::SceneRigidBody* rb = ew.Get<scene::SceneRigidBody>(ball);
    rb->dynamic = true;
    rb->radius = 0.5f;
    ew.Add<scene::SceneTransform>(ball);
    ew.Get<scene::SceneTransform>(ball)->pos = {0, 5, 0};

    bridge.RegisterBodies(ew);
    CHECK_EQ(injected.BodyCount(), 1u); // registered in the injected world
}

// ---------------------------------------------------------------------------
// DrawSystem (Task 16): 渲染编排子系统独立单测（脱离 GameRuntime，直接测拆分后的
// 绘制系统）：Build 收集 draw items、Resolve 解析网格 + LOD 链、MeshForEntity 按
// 相机距离选 LOD 级、整帧 Draw 走 NullBackend 不崩、SetSpriteFrames/SetSpriteSheet/
// AdvanceSprites 对 sprite 项安全、HasSkinned/Clear/Despawn 修剪生命周期。
// ---------------------------------------------------------------------------

TEST(DrawSystemStandaloneBuildResolveDraw) {
    test::HeadlessAssetFixture fix;

    ecs::World world;
    scene::AnimationSystem anims;
    scene::DrawSystem draw;

    // One LOD-chained mesh entity (cube -> sphere at 10u -> plane at 50u) and
    // one sprite entity whose texture is missing (resolve -> failed, skipped).
    ecs::Entity lodEnt = world.Create();
    world.Add<scene::SceneTransform>(lodEnt, scene::SceneTransform{{5, 0, 0}});
    world.Add<scene::SceneMesh>(lodEnt);
    scene::SceneMesh* mesh = world.Get<scene::SceneMesh>(lodEnt);
    mesh->meshKey = "cube";
    mesh->lod = {{10.0f, "sphere"}, {50.0f, "plane"}};

    ecs::Entity spriteEnt = world.Create();
    world.Add<scene::SceneTransform>(spriteEnt, scene::SceneTransform{{0, 0, 0}});
    world.Add<scene::SceneSprite>(spriteEnt);
    world.Get<scene::SceneSprite>(spriteEnt)->texture = "assets/sprites/__missing__.png";

    // The runtime's asset/path plumbing, injected the way GameRuntime does at
    // Start (identity path here; the fixture's AssetManager serves the cache).
    draw.Configure({&fix.assets, /*asyncMeshLoad=*/false,
                    [](const std::string& p) { return p; }});

    draw.Build(world, anims);
    CHECK_EQ(draw.DrawCount(), 2u);
    CHECK(!draw.HasSkinned(lodEnt)); // cube is not a skinned model

    // Resolve everything once: the cube resolves through the procedural path
    // plus its LOD chain; the sprite's missing texture marks it failed.
    gfx::Renderer& renderer = fix.renderer;
    draw.Resolve(world, renderer, anims);

    // MeshForEntity picks the LOD level by camera distance (GameRuntime parity).
    gfx::Camera cam;
    cam.position = {0, 0, 0};   // dist 5 -> level 0 (cube)
    CHECK_EQ(draw.MeshForEntity(lodEnt, cam, world).Name(), std::string("cube"));
    cam.position = {30, 0, 0};  // dist 25 -> [10, 50) -> sphere
    CHECK_EQ(draw.MeshForEntity(lodEnt, cam, world).Name(), std::string("sphere"));
    cam.position = {100, 0, 0}; // dist 95 -> plane
    CHECK_EQ(draw.MeshForEntity(lodEnt, cam, world).Name(), std::string("plane"));
    // Unknown entity -> invalid mesh.
    CHECK(!draw.MeshForEntity(world.Create(), cam, world).Valid());

    // A full Draw frame through the NullBackend (empty hosts / projectiles /
    // particles / canvas): must not crash and must keep the draw list intact.
    scene::HudSystem hud;
    scene::SceneTreeSystem sceneTree;
    scene::ProjectileSystem projectiles;
    scene::SceneParticleSystem particles;
    scene::ScriptCanvas canvas;
    script::ScriptContext ctx;
    std::set<uint64_t> hidden;
    float uiScale = 1.0f;
    math::Vec2 uiOffset;
    cam.position = {0, 0, 0};
    draw.Draw(renderer, cam, scene::DrawSystem::DrawParams{}, world, ctx,
              /*luaHost=*/nullptr, /*jsHost=*/nullptr, hidden, hud, sceneTree,
              anims, projectiles, particles, canvas, uiScale, uiOffset);
    CHECK_EQ(draw.DrawCount(), 2u);

    // Sprite-frame hooks are safe on the failed sprite item (no-op, no crash).
    draw.SetSpriteFrames(spriteEnt, {"a.png", "b.png"}, 2.0f);
    draw.SetSpriteSheet(spriteEnt, "atlas.png", 4, 8.0f);
    draw.AdvanceSprites(1.0f / 60.0f);

    // Despawn + rebuild drops the dead entity's draw item (Build prunes dead
    // draws, mirroring the runtime's script Spawn/Despawn handling).
    world.Destroy(lodEnt);
    draw.Build(world, anims);
    CHECK_EQ(draw.DrawCount(), 1u);
    CHECK(!draw.MeshForEntity(lodEnt, cam, world).Valid());

    draw.Clear();
    CHECK_EQ(draw.DrawCount(), 0u);
}
