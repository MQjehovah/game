#include <cmath>
#include <string>

#include "neon/neon.hpp"
#include "neon/scene/game_runtime.hpp"
#include "neon/scene/status.hpp"
#include "helpers.hpp"

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

    // Name resolution through the built-in table.
    CHECK_EQ(scene::StatusIdByName("burning"), scene::kStatusBurning);
    CHECK_EQ(scene::StatusIdByName("regen"), scene::kStatusRegen);
    CHECK_EQ(scene::StatusIdByName("mystery"), 0u);
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
        ApplyStatus(e, "burning", 2, 3)
      end
      function on_update(e, dt)
        if HasStatus(e, "burning") then
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
        ApplyStatus(e, "burning", 2, 3)
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
        ApplyStatus(e, "regen", 2, 100)
        ApplyStatus(FindNamedEntity("dead"), "regen", 2, 100)
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
                            {{"burning", 3.0f, 2.0f}});
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
                        {{name="burning", duration=3, magnitude=2}})
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
