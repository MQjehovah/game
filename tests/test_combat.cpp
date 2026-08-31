#include <cmath>
#include <string>

#include "neon/neon.hpp"
#include "neon/scene/game_runtime.hpp"
#include "neon/scene/skills.hpp"
#include "neon/scene/status.hpp"
#include "helpers.hpp"

using namespace neon;

// ---------------------------------------------------------------------------
// M2 combat core tests: the status-effect framework (buffs/debuffs), the
// data-driven skill table, CastSkill (projectile / melee / box + cooldown +
// mana), the oriented attack box, and the Lua bindings on top of them.
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
// Skill table (data-driven JSON)
// ---------------------------------------------------------------------------

TEST(SkillTableParsesAndValidates) {
    scene::SkillTable table;
    std::string err;
    CHECK(table.Load(kSkillsJson, &err));
    CHECK_EQ(table.Size(), 3u);

    const scene::SkillDef* fire = table.Find("fireball");
    CHECK(fire != nullptr);
    if (fire) {
        CHECK_EQ(fire->kind, std::string("projectile"));
        CHECK_NEAR(fire->damage, 20.0, 1e-6);
        CHECK_NEAR(fire->cooldown, 0.5, 1e-6);
        CHECK_NEAR(fire->manaCost, 8.0, 1e-6);
        CHECK_EQ(fire->statuses.size(), 1u);
        if (!fire->statuses.empty()) {
            CHECK_EQ(fire->statuses[0].name, std::string("burning"));
            CHECK_NEAR(fire->statuses[0].duration, 3.0, 1e-6);
        }
    }
    CHECK(table.Find("nope") == nullptr);

    scene::SkillTable bad;
    CHECK(!bad.Load("{\"skills\":{\"x\":{\"kind\":\"boom\"}}}", &err));
    CHECK(!bad.Load(
        "{\"skills\":{\"x\":{\"kind\":\"melee\",\"statuses\":[{\"name\":\"mystery\",\"duration\":1}]}}}",
        &err));
    CHECK(!bad.Load("{\"nope\":{}}", &err));
}

// ---------------------------------------------------------------------------
// GameRuntime integration: projectile skill + status ticks
// ---------------------------------------------------------------------------

TEST(CombatProjectileSkillDamagesAndBurns) {
    scene::GameRuntime runtime;
    scene::GameRuntimeConfig cfg;
    cfg.headless = true;
    CHECK(runtime.Start(kCombatScene, cfg).Ok());
    std::string err;
    CHECK(runtime.LoadSkills(kSkillsJson, &err));

    const ecs::Entity wolf = runtime.FindNamedEntity("wolf_front");
    const ecs::Entity hero = runtime.FindNamedEntity("hero");
    CHECK(wolf.IsValid());
    CHECK(hero.IsValid());
    CHECK_EQ(runtime.EntityHealth(wolf).first, 50.0f);
    runtime.GameVars().Set("mana", script::Value::Num(50));

    // Fireball at the wolf 2 units away; travel ~0.14s at speed 14.
    CHECK_EQ(runtime.CastSkill("fireball", {0, 1, 0}, {0, 0, -1}, hero), 1);
    for (int i = 0; i < 30; ++i) runtime.Tick(1.0f / 60.0f);

    // Direct hit: 50 - 20 = 30. Burning is active but has not ticked yet
    // (first tick one second after application).
    CHECK_NEAR(runtime.EntityHealth(wolf).first, 30.0f, 1e-3);
    CHECK(runtime.HasStatus(wolf, scene::kStatusBurning));

    // 3 more seconds: burning ticks 3x (2 dmg each) then expires.
    for (int i = 0; i < 180; ++i) runtime.Tick(1.0f / 60.0f);
    CHECK_NEAR(runtime.EntityHealth(wolf).first, 24.0f, 1e-3);
    CHECK(!runtime.HasStatus(wolf, scene::kStatusBurning));
}

TEST(CombatSkillCooldownAndMana) {
    scene::GameRuntime runtime;
    scene::GameRuntimeConfig cfg;
    cfg.headless = true;
    CHECK(runtime.Start(kCombatScene, cfg).Ok());
    std::string err;
    CHECK(runtime.LoadSkills(kSkillsJson, &err));
    const ecs::Entity hero = runtime.FindNamedEntity("hero");

    // Fireball costs 8 mana: refused without the GameVar, then consumed.
    CHECK_EQ(runtime.CastSkill("fireball", {0, 1, 0}, {0, 0, -1}, hero), 0);
    runtime.GameVars().Set("mana", script::Value::Num(10));
    CHECK_EQ(runtime.CastSkill("fireball", {0, 1, 0}, {0, 0, -1}, hero), 1);
    CHECK_NEAR(runtime.GameVars().Get("mana").number, 2.0, 1e-6);

    // 0.5s cooldown: immediate re-cast refused, then allowed after 31 ticks.
    CHECK_EQ(runtime.CastSkill("fireball", {0, 1, 0}, {0, 0, -1}, hero), 0);
    CHECK(runtime.SkillCooldownLeft("fireball", hero) > 0.0f);
    for (int i = 0; i < 31; ++i) runtime.Tick(1.0f / 60.0f);
    CHECK_NEAR(runtime.SkillCooldownLeft("fireball", hero), 0.0, 1e-4);
    runtime.GameVars().Set("mana", script::Value::Num(50)); // replenish
    CHECK_EQ(runtime.CastSkill("fireball", {0, 1, 0}, {0, 0, -1}, hero), 1);
}

TEST(CombatMeleeSkillArc) {
    scene::GameRuntime runtime;
    scene::GameRuntimeConfig cfg;
    cfg.headless = true;
    CHECK(runtime.Start(kCombatScene, cfg).Ok());
    std::string err;
    CHECK(runtime.LoadSkills(kSkillsJson, &err));

    const ecs::Entity front = runtime.FindNamedEntity("wolf_front");
    const ecs::Entity side = runtime.FindNamedEntity("wolf_side");
    const ecs::Entity hero = runtime.FindNamedEntity("hero");

    // cleave: 100-deg arc forward (-Z), range 3. Hits the front wolf only
    // (side wolf is within range but outside the arc).
    CHECK_EQ(runtime.CastSkill("cleave", {0, 0, 0}, {0, 0, -1}, hero), 1);
    CHECK_NEAR(runtime.EntityHealth(front).first, 38.0f, 1e-4); // 50 - 12
    CHECK_NEAR(runtime.EntityHealth(side).first, 50.0f, 1e-4);
}

TEST(CombatAttackBoxOriented) {
    const char* boxScene = R"({
      "entities": [
        {"name": "a", "components": {"transform": {"pos": [0,0,-1]}, "health": {"hp": 50, "maxHp": 50}}},
        {"name": "b", "components": {"transform": {"pos": [0,0,-2.5]}, "health": {"hp": 50, "maxHp": 50}}},
        {"name": "c", "components": {"transform": {"pos": [2.5,0,0]}, "health": {"hp": 50, "maxHp": 50}}}
      ]
    })";
    scene::GameRuntime runtime;
    scene::GameRuntimeConfig cfg;
    cfg.headless = true;
    CHECK(runtime.Start(boxScene, cfg).Ok());

    const ecs::Entity a = runtime.FindNamedEntity("a");
    const ecs::Entity b = runtime.FindNamedEntity("b");
    const ecs::Entity c = runtime.FindNamedEntity("c");

    // yaw 0 box (half 2x2x1.5): |x|<=2, |y|<=2, |z|<=1.5 -> only "a".
    CHECK_EQ(runtime.AttackBox({0, 0, 0}, {2, 2, 1.5f}, 0.0f, 15.0f), 1);
    CHECK_NEAR(runtime.EntityHealth(a).first, 35.0f, 1e-4);

    // yaw 90 deg: the box rotates to face +X -> |x|<=1.5, |z|<=2. "a" is still
    // inside (|z|=1 <= 2), "b" (|z|=2.5) and "c" (|x|=2.5) are outside.
    CHECK_EQ(runtime.AttackBox({0, 0, 0}, {2, 2, 1.5f}, math::kPi * 0.5f, 15.0f), 1);
    CHECK_NEAR(runtime.EntityHealth(a).first, 20.0f, 1e-4);
    CHECK_NEAR(runtime.EntityHealth(b).first, 50.0f, 1e-4);
    CHECK_NEAR(runtime.EntityHealth(c).first, 50.0f, 1e-4);
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
