#include <cmath>
#include <string>

#if defined(_WIN32)
#include <direct.h>
#else
#include <sys/stat.h>
#endif

#include "neon/neon.hpp"
#include "neon/scene/game_runtime.hpp"
#include "helpers.hpp"
#include "test_backend.hpp"

using namespace neon;

namespace {

// Two entities: one runs a Lua script that bumps GameVars.gold every tick, the
// other runs a behavior tree whose 0.5s wait node gates a blackboard write.
const char* kSimScene = R"({
  "entities": [
    {
      "name": "Counter",
      "components": {
        "transform": {"pos": [0, 0, 0]},
        "script": {"backend": "lua", "path": "counter.lua"}
      }
    },
    {
      "name": "Walker",
      "components": {
        "transform": {"pos": [1, 0, 0]},
        "behaviorTree": {"tree": "{\"root\":{\"type\":\"sequence\",\"children\":[{\"type\":\"action\",\"name\":\"wait\",\"args\":{\"seconds\":0.5}},{\"type\":\"blackboard_set\",\"args\":{\"key\":\"bt_done\",\"value\":true}}]}}"}
      }
    }
  ]
})";

const char* kCounterLua = R"(
function on_start(e)
  SetVar("started", true)
end
function on_update(e, dt)
  local g = GetVar("gold")
  if g == nil then g = 0 end
  SetVar("gold", g + 1)
end
)";

// --- Dual-backend coexistence (Lua + JS in one scene) -----------------------

// A JS counter entity + two scripted behavior trees (one per backend) that
// exercise the previously-unwired bt::Context::callScript hook.
const char* kDualScene = R"({
  "entities": [
    {
      "name": "LuaCounter",
      "components": {
        "transform": {"pos": [0, 0, 0]},
        "script": {"backend": "lua", "path": "counter.lua"}
      }
    },
    {
      "name": "JsCounter",
      "components": {
        "transform": {"pos": [1, 0, 0]},
        "script": {"backend": "js", "path": "counter.js"}
      }
    },
    {
      "name": "JsTree",
      "components": {
        "transform": {"pos": [2, 0, 0]},
        "script": {"backend": "js", "path": "tree_script.js"},
        "behaviorTree": {"tree": "{\"root\":{\"type\":\"sequence\",\"children\":[{\"type\":\"run_script\",\"args\":{\"script\":\"treeStep\"}},{\"type\":\"script_bool\",\"args\":{\"script\":\"treeReady\"}}]}}"}
      }
    },
    {
      "name": "LuaTree",
      "components": {
        "transform": {"pos": [3, 0, 0]},
        "script": {"backend": "lua", "path": "tree_lua.lua"},
        "behaviorTree": {"tree": "{\"root\":{\"type\":\"sequence\",\"children\":[{\"type\":\"run_script\",\"args\":{\"script\":\"treeStepLua\"}},{\"type\":\"script_bool\",\"args\":{\"script\":\"treeReadyLua\"}}]}}"}
      }
    }
  ]
})";

const char* kJsCounter = R"(
function on_start(e) {
  SetVar("js_started", true);
}
function on_update(e, dt) {
  var g = GetVar("js_gold");
  if (typeof g !== "number") g = 0;
  SetVar("js_gold", g + 1);
}
)";

const char* kJsTreeScript = R"(
function treeStep(e) {
  SetVar("tree_ran", true);
}
function treeReady(e) {
  return true;
}
)";

const char* kLuaTreeScript = R"(
function treeStepLua(e)
  SetVar("tree_lua_ran", true)
end
function treeReadyLua(e)
  return true
end
)";

} // namespace

// ---------------------------------------------------------------------------
// The T2.9 smoke test: 120 headless ticks, scripts and BT both ran.
// ---------------------------------------------------------------------------

TEST(GameRuntimeSimScriptsAndBehaviorTrees) {
    scene::GameRuntime runtime;
    scene::GameRuntimeConfig cfg;
    cfg.readScript = [](const std::string& path) {
        return path == "counter.lua" ? std::string(kCounterLua) : std::string();
    };
    core::Status st = runtime.Start(kSimScene, cfg);
    CHECK(st.Ok());
    CHECK(runtime.Running());
    CHECK_EQ(runtime.EntityCount(), 2u);
    CHECK_EQ(runtime.ScriptCount(), 1u);
    CHECK_EQ(runtime.BehaviorTreeCount(), 1u);

    // on_start fired during Start.
    CHECK(runtime.GameVars().Get("started").type == script::Value::Type::Bool);
    CHECK(runtime.GameVars().Get("started").boolean);

    for (int i = 0; i < 120; ++i) runtime.Tick(1.0f / 60.0f);

    CHECK_NEAR(runtime.SimTime(), 120.0f / 60.0f, 1e-4);
    CHECK(runtime.GameVars().Get("gold").type == script::Value::Type::Number);
    CHECK_EQ(runtime.GameVars().Get("gold").number, 120.0); // one per tick

    // The 0.5s wait elapsed (120 ticks at 1/60 = 2.0s), so the blackboard_set
    // child of the sequence ran and wrote bt_done.
    ecs::Entity btEnt;
    {
        auto view = runtime.World().ViewAll<scene::SceneBehaviorTree>();
        CHECK_EQ(view.Size(), 1u);
        if (view.Size() == 1u) {
            btEnt = runtime.World().EntityAt<scene::SceneBehaviorTree>(0);
        }
    }
    CHECK(btEnt.IsValid());
    script::Value done = runtime.EntityBlackboardValue(btEnt, "bt_done");
    CHECK(done.type == script::Value::Type::Bool);
    CHECK(done.boolean);

    runtime.Stop();
    CHECK(!runtime.Running());
}

TEST(GameRuntimeDiskScriptPath) {
    test::TempDir tmp;
    const std::string dir = tmp.Str();
    CHECK(test::WriteFileAll(dir + "/counter.lua", kCounterLua));

    scene::GameRuntime runtime;
    scene::GameRuntimeConfig cfg;
    cfg.scriptBaseDir = dir; // default disk reader resolves scripts/<base>/counter.lua
    core::Status st = runtime.Start(kSimScene, cfg);
    CHECK(st.Ok());
    CHECK_EQ(runtime.ScriptCount(), 1u);

    runtime.Tick(1.0f / 60.0f);
    runtime.Tick(1.0f / 60.0f);
    CHECK_EQ(runtime.GameVars().Get("gold").number, 2.0);
}

#ifdef NEON_ENABLE_JS
TEST(GameRuntimeMixedLuaAndJsBackends) {
    scene::GameRuntime runtime;
    scene::GameRuntimeConfig cfg;
    cfg.readScript = [](const std::string& path) {
        if (path == "counter.lua") return std::string(kCounterLua);
        if (path == "counter.js") return std::string(kJsCounter);
        if (path == "tree_script.js") return std::string(kJsTreeScript);
        if (path == "tree_lua.lua") return std::string(kLuaTreeScript);
        return std::string();
    };
    core::Status st = runtime.Start(kDualScene, cfg);
    CHECK(st.Ok());
    CHECK(runtime.Running());
    CHECK_EQ(runtime.ScriptCount(), 4u); // 2 counters + 2 tree scripts
    CHECK_EQ(runtime.BehaviorTreeCount(), 2u);

    // The JS on_start fired during Start (JS host loaded + captured).
    CHECK(runtime.GameVars().Get("js_started").type == script::Value::Type::Bool);
    CHECK(runtime.GameVars().Get("js_started").boolean);

    for (int i = 0; i < 120; ++i) runtime.Tick(1.0f / 60.0f);

    // Both languages bumped their own counter every tick.
    CHECK_EQ(runtime.GameVars().Get("gold").number, 120.0);    // Lua
    CHECK_EQ(runtime.GameVars().Get("js_gold").number, 120.0); // JS

    // Both behavior trees ran their script nodes through their own backend.
    CHECK(runtime.GameVars().Get("tree_ran").type == script::Value::Type::Bool);
    CHECK(runtime.GameVars().Get("tree_ran").boolean);
    CHECK(runtime.GameVars().Get("tree_lua_ran").type == script::Value::Type::Bool);
    CHECK(runtime.GameVars().Get("tree_lua_ran").boolean);

    runtime.Stop();
    CHECK(!runtime.Running());
}
#endif // NEON_ENABLE_JS

TEST(GameRuntimeMissingScriptIsNonFatal) {
    scene::GameRuntime runtime;
    scene::GameRuntimeConfig cfg;
    cfg.readScript = [](const std::string&) { return std::string(); }; // file never found
    core::Status st = runtime.Start(kSimScene, cfg);
    CHECK(st.Ok());
    CHECK(runtime.Running());
    CHECK_EQ(runtime.ScriptCount(), 0u); // skipped, not fatal
    CHECK_EQ(runtime.BehaviorTreeCount(), 1u);

    for (int i = 0; i < 30; ++i) runtime.Tick(1.0f / 60.0f); // must not crash
    CHECK_NEAR(runtime.SimTime(), 0.5f, 1e-4);
}

// Custom (plugin/game-data) components without a registered factory are stored
// in the entity's SceneData and readable by scripts via EntityComponent.
TEST(GameRuntimeCustomComponentDataReadable) {
    const char* scene = R"({
  "entities": [
    {
      "name": "Hero",
      "components": {
        "transform": {"pos": [0, 0, 0]},
        "script": {"backend": "lua", "path": "reader.lua"},
        "inventory": {"slots": 18, "maxWeight": 50}
      }
    }
  ]
})";
    const char* reader = R"(
function on_start(e)
  local inv = EntityComponent(e, "inventory")
  if inv ~= nil then
    SetVar("got_slots", inv.slots)
    SetVar("got_weight", inv.maxWeight)
  end
end
)";
    scene::GameRuntime runtime;
    scene::GameRuntimeConfig cfg;
    cfg.readScript = [&](const std::string& path) {
        return path == "reader.lua" ? std::string(reader) : std::string();
    };
    core::Status st = runtime.Start(scene, cfg);
    CHECK(st.Ok());
    CHECK(runtime.Running());
    CHECK(runtime.GameVars().Get("got_slots").type == script::Value::Type::Number);
    CHECK_EQ(runtime.GameVars().Get("got_slots").number, 18.0);
    CHECK_EQ(runtime.GameVars().Get("got_weight").number, 50.0);
    runtime.Stop();
}

// Runtime prefab instantiation: SpawnPrefab expands a prefab (components +
// script), the script reads its custom component via EntityComponent and can
// mutate it via SetEntityComponent.
TEST(GameRuntimeSpawnPrefabAtRuntime) {
    test::TempDir tmp;
    const std::string dir = tmp.Str();
#if defined(_WIN32)
    ::_mkdir((dir + "/prefabs").c_str());
#else
    ::mkdir((dir + "/prefabs").c_str(), 0777);
#endif
    const char* peaLua = R"(
function on_start(e)
  local p = EntityComponent(e, "pea")
  if p ~= nil then
    SetVar("pea_damage", p.damage)
    SetVar("pea_speed", p.speed)
  end
  SetEntityComponent(e, "pea", { damage = 99, speed = 1 })
end
function on_update(e, dt)
  local p = EntityComponent(e, "pea")
  if p ~= nil and p.damage == 99 then SetVar("pea_updated", true) end
end
)";
    CHECK(test::WriteFileAll(
        dir + "/prefabs/pea.json",
        R"({"components": {
             "sprite": {"texture": "assets/sprites/pea.png"},
             "pea": {"damage": 10, "speed": 340},
             "script": {"backend": "lua", "path": "pea.lua", "vars": {}}
           }})"));
    CHECK(test::WriteFileAll(dir + "/pea.lua", peaLua));
    scene::GameRuntime runtime;
    scene::GameRuntimeConfig cfg;
    cfg.scriptBaseDir = dir;
    // Disk-backed prefab + script (mirrors the standalone repro).
    core::Status st = runtime.Start(R"({"entities":[]})", cfg);
    CHECK(st.Ok());

    const ecs::Entity e = runtime.SpawnPrefab("pea", {100, 200, 0});
    CHECK(e.IsValid());
    CHECK(runtime.GameVars().Get("pea_damage").type == script::Value::Type::Number);
    CHECK_EQ(runtime.GameVars().Get("pea_damage").number, 10.0);
    CHECK_EQ(runtime.GameVars().Get("pea_speed").number, 340.0);

    runtime.Tick(1.0f / 60.0f);
    CHECK(runtime.GameVars().Get("pea_updated").type == script::Value::Type::Bool);
    CHECK(runtime.GameVars().Get("pea_updated").boolean);

    // Unknown prefabs fail cleanly.
    CHECK(!runtime.SpawnPrefab("nope", {0, 0, 0}).IsValid());
    runtime.Stop();
}

TEST(GameRuntimeBadTreeIsNonFatal) {
    const char* json = R"({
      "entities": [
        {
          "name": "Broken",
          "components": {
            "transform": {"pos": [0, 0, 0]},
            "behaviorTree": {"tree": "{\"root\":{\"type\":\"teleport\"}}"}
          }
        }
      ]
    })";
    scene::GameRuntime runtime;
    core::Status st = runtime.Start(json, scene::GameRuntimeConfig{});
    CHECK(st.Ok());
    CHECK_EQ(runtime.BehaviorTreeCount(), 0u);
    runtime.Tick(1.0f / 60.0f); // must not crash
    CHECK_EQ(runtime.EntityCount(), 1u);
}

TEST(GameRuntimeRejectsInvalidScene) {
    scene::GameRuntime runtime;
    core::Status st = runtime.Start("this is not a scene json", scene::GameRuntimeConfig{});
    CHECK(!st.Ok());
    CHECK(!st.Error().empty());
    CHECK(!runtime.Running());

    // Structurally valid JSON but semantically invalid scene (no entities).
    core::Status st2 = runtime.Start(R"({"entities": [42]})", scene::GameRuntimeConfig{});
    CHECK(!st2.Ok());
    CHECK(!runtime.Running());
    CHECK_EQ(runtime.EntityCount(), 0u);
}

TEST(GameRuntimeRestartIsClean) {
    scene::GameRuntime runtime;
    scene::GameRuntimeConfig cfg;
    cfg.readScript = [](const std::string& path) {
        return path == "counter.lua" ? std::string(kCounterLua) : std::string();
    };
    CHECK(runtime.Start(kSimScene, cfg).Ok());
    for (int i = 0; i < 10; ++i) runtime.Tick(1.0f / 60.0f);
    CHECK(runtime.GameVars().Get("gold").number > 0.0);
    runtime.Stop();

    // Start again with a different scene: fresh state, no leftover vars.
    const char* empty = R"({"entities": [{"name": "Alone", "components": {
        "transform": {"pos": [5, 5, 5]}}}]})";
    CHECK(runtime.Start(empty, scene::GameRuntimeConfig{}).Ok());
    CHECK_EQ(runtime.EntityCount(), 1u);
    CHECK_EQ(runtime.ScriptCount(), 0u);
    CHECK_EQ(runtime.BehaviorTreeCount(), 0u);
    CHECK_NEAR(runtime.SimTime(), 0.0f, 1e-6);
    CHECK(runtime.GameVars().Get("gold").type == script::Value::Type::Nil);
    runtime.Tick(1.0f / 60.0f); // must not crash after restart
}

// ---------------------------------------------------------------------------
// Debug observability: ActiveTreePath surfaces the bt::Context::activePath of
// the entity's tree after each tick (editor playtest highlight).
// ---------------------------------------------------------------------------

TEST(GameRuntimeActiveTreePath) {
    scene::GameRuntime runtime;
    CHECK(runtime.Start(kSimScene, scene::GameRuntimeConfig{}).Ok());
    CHECK_EQ(runtime.BehaviorTreeCount(), 1u);

    ecs::Entity btEnt;
    {
        auto view = runtime.World().ViewAll<scene::SceneBehaviorTree>();
        CHECK_EQ(view.Size(), 1u);
        if (view.Size() == 1u) btEnt = runtime.World().EntityAt<scene::SceneBehaviorTree>(0);
    }
    CHECK(btEnt.IsValid());
    CHECK_EQ(runtime.ActiveTreePath(btEnt), std::string("")); // nothing ticked yet

    // Walker's tree: sequence [ wait(0.5s), blackboard_set ]. While the wait is
    // running, the deepest node that ran is the wait child ("0/0").
    for (int i = 0; i < 10; ++i) runtime.Tick(1.0f / 60.0f); // 0.166s < 0.5s
    CHECK_EQ(runtime.ActiveTreePath(btEnt), std::string("0/0"));

    // Tick until the wait completes: the tick that crosses 0.5s also runs the
    // blackboard_set child, so activePath names that child ("0/1").
    bool sawDone = false;
    for (int i = 0; i < 120; ++i) {
        runtime.Tick(1.0f / 60.0f);
        if (runtime.EntityBlackboardValue(btEnt, "bt_done").type ==
            script::Value::Type::Bool) {
            sawDone = true;
            CHECK_EQ(runtime.ActiveTreePath(btEnt), std::string("0/1"));
            break;
        }
    }
    CHECK(sawDone);

    // The wait restarts from 0 next cycle -> deepest node back to "0/0".
    for (int i = 0; i < 5; ++i) runtime.Tick(1.0f / 60.0f);
    CHECK_EQ(runtime.ActiveTreePath(btEnt), std::string("0/0"));

    // Entities without a tree report nothing.
    CHECK_EQ(runtime.ActiveTreePath(ecs::Entity{}), std::string(""));
    runtime.Stop();
}

// ---------------------------------------------------------------------------
// Draw path (headless backend): lazy mesh resolution + procedural primitives
// ---------------------------------------------------------------------------

TEST(GameRuntimeDrawResolvesMeshesHeadless) {
    test::TempDir tmp;
    std::string objPath = tmp.Str() + "/cube.obj";
    CHECK(test::WriteFileAll(objPath, "v 0 0 0\nv 1 0 0\nv 1 1 0\nv 0 1 0\nf 1 2 3\nf 1 3 4\n"));
    for (char& c : objPath) {
        if (c == '\\') c = '/'; // JSON strings reject raw backslashes
    }

    std::string json = std::string("{\"entities\":[") +
                       "{\"name\":\"A\",\"components\":{\"transform\":{\"pos\":[0,0,0]},\"mesh\":{\"meshKey\":\"obj:" +
                       objPath + "\"}}}," +
                       "{\"name\":\"B\",\"components\":{\"transform\":{\"pos\":[1,0,0]},\"mesh\":{\"meshKey\":\"cube\",\"material\":{\"colorHex\":\"#FF0000\",\"metallic\":0.5,\"roughness\":0.3}}}}]}";

    test::HeadlessAssetFixture fix;
    scene::GameRuntime runtime;
    scene::GameRuntimeConfig cfg;
    cfg.assets = &fix.assets;
    core::Status st = runtime.Start(json, cfg);
    CHECK(st.Ok());
    CHECK_EQ(runtime.DrawCount(), 2u);

    gfx::Camera cam;
    runtime.Draw(fix.renderer, cam); // resolves + draws without a GPU
    CHECK(runtime.Running());

    // A second Draw reuses the resolved meshes (no re-resolution / no crash).
    runtime.Draw(fix.renderer, cam);
    runtime.Stop();
}

TEST(GameRuntimeDrawUnknownKeyWarnsNoCrash) {
    const char* json = R"({"entities":[{"name":"A","components":{
        "transform":{"pos":[0,0,0]},"mesh":{"meshKey":"bogus"}}}]})";
    test::HeadlessAssetFixture fix;
    scene::GameRuntime runtime;
    scene::GameRuntimeConfig cfg;
    cfg.assets = &fix.assets;
    CHECK(runtime.Start(json, cfg).Ok());
    gfx::Camera cam;
    runtime.Draw(fix.renderer, cam); // unknown prefix: warn + skip, no crash
    runtime.Tick(1.0f / 60.0f);
}

// ---------------------------------------------------------------------------
// Single-host semantics (review fixes): global shadowing, script vars, logging
// ---------------------------------------------------------------------------

// Each script instance captures its own chunk's handlers, so a later-loaded
// script's on_update no longer shadows an earlier one for every entity: A runs
// a.lua's handler, B runs b.lua's, independently.
TEST(GameRuntimeScriptsDoNotShadowAcrossChunks) {
    const char* scene = R"({
      "entities": [
        {"name": "A", "components": {"transform": {"pos": [0,0,0]},
          "script": {"backend": "lua", "path": "a.lua"}}},
        {"name": "B", "components": {"transform": {"pos": [1,0,0]},
          "script": {"backend": "lua", "path": "b.lua"}}}
      ]
    })";
    const char* luaA = R"(
      function on_update(e, dt)
        SetVar("shadow_mark", "A")
      end
    )";
    const char* luaB = R"(
      function on_update(e, dt)
        local key = "tick_" .. string.format("%d", e.id)
        local c = GetVar(key)
        if c == nil then c = 0 end
        SetVar(key, c + 1)
      end
    )";
    scene::GameRuntime runtime;
    scene::GameRuntimeConfig cfg;
    cfg.readScript = [&](const std::string& p) {
        if (p == "a.lua") return std::string(luaA);
        if (p == "b.lua") return std::string(luaB);
        return std::string();
    };
    CHECK(runtime.Start(scene, cfg).Ok());
    CHECK_EQ(runtime.ScriptCount(), 2u);

    std::vector<ecs::Entity> scriptEnts;
    {
        auto view = runtime.World().ViewAll<scene::SceneScript>();
        for (size_t i = 0; i < view.Size(); ++i) {
            scriptEnts.push_back(runtime.World().EntityAt<scene::SceneScript>(i));
        }
    }
    CHECK_EQ(scriptEnts.size(), 2u);
    if (scriptEnts.size() != 2u) return;

    for (int i = 0; i < 10; ++i) runtime.Tick(1.0f / 60.0f);

    // Entity A ran a.lua's on_update (shadow_mark); entity B ran b.lua's
    // (its own tick counter) — no cross-chunk shadowing.
    CHECK(runtime.GameVars().Get("shadow_mark").type == script::Value::Type::String);
    CHECK_EQ(runtime.GameVars().Get("shadow_mark").str, std::string("A"));
    // Exactly one tick counter exists (entity B's script) and it advanced
    // every tick; entity A's script never ran b.lua's handler.
    int tickCounters = 0;
    for (const ecs::Entity& e : scriptEnts) {
        const std::string key = "tick_" + std::to_string(e.id);
        const script::Value v = runtime.GameVars().Get(key);
        if (v.type == script::Value::Type::Number) {
            ++tickCounters;
            CHECK_EQ(v.number, 10.0);
        }
    }
    CHECK_EQ(tickCounters, 1);
}

// Per-entity SceneScript.vars are set as Lua globals before on_start/on_update.
TEST(GameRuntimeScriptVarsAsGlobals) {
    const char* scene = R"({
      "entities": [
        {"name": "Miner", "components": {
          "transform": {"pos": [0,0,0]},
          "script": {"backend": "lua", "path": "miner.lua", "vars": {"factor": 3}}
        }}
      ]
    })";
    const char* lua = R"(
      function on_update(e, dt)
        local g = GetVar("gold")
        if g == nil then g = 0 end
        SetVar("gold", g + factor)
      end
    )";
    scene::GameRuntime runtime;
    scene::GameRuntimeConfig cfg;
    cfg.readScript = [&](const std::string&) { return std::string(lua); };
    CHECK(runtime.Start(scene, cfg).Ok());
    CHECK_EQ(runtime.ScriptCount(), 1u);
    for (int i = 0; i < 10; ++i) runtime.Tick(1.0f / 60.0f);
    CHECK_EQ(runtime.GameVars().Get("gold").number, 30.0); // 10 ticks * factor 3
}

// A failing on_update is logged once per script instance, not every tick.
TEST(GameRuntimeScriptErrorLoggedOnce) {
    const char* scene = R"({
      "entities": [
        {"name": "Boom", "components": {"transform": {"pos": [0,0,0]},
          "script": {"backend": "lua", "path": "boom.lua"}}}
      ]
    })";
    const char* lua = R"(
      function on_update(e, dt)
        local x = GetVar("missing")
        local y = x + 1 -- nil + 1 -> runtime error
      end
    )";
    scene::GameRuntime runtime;
    scene::GameRuntimeConfig cfg;
    cfg.readScript = [&](const std::string&) { return std::string(lua); };
    CHECK(runtime.Start(scene, cfg).Ok());

    for (int i = 0; i < 20; ++i) runtime.Tick(1.0f / 60.0f); // 20 failing ticks

    size_t logged = 0;
    for (const core::LogEntry& entry : core::GetRecentLogs(100)) {
        if (entry.text.find("on_update() failed") != std::string::npos) ++logged;
    }
    CHECK_EQ(logged, 1u); // throttled: first failure only
}

// ---------------------------------------------------------------------------
// Hero combat (T-skill): the new gameplay bindings (InputKey -> SpawnProjectile
// -> projectile damage, plus MeleeAttack) damage SceneHealth entities end to end.
// ---------------------------------------------------------------------------

namespace {
struct CombatInput : platform::IInput {
    bool key1 = false;
    bool key2 = false;
    bool leftDown = false;
    bool leftPressed = false;
    void HandleEvent(const platform::InputEvent&) override {}
    bool IsDown(platform::Key key) const override {
        return (key == platform::Key::D1 && key1) || (key == platform::Key::D2 && key2);
    }
    bool Pressed(platform::Key) const override { return false; }
    bool Released(platform::Key) const override { return false; }
    bool MouseDown(platform::MouseButton) const override { return leftDown; }
    bool MousePressed(platform::MouseButton b) const override {
        return b == platform::MouseButton::Left && leftPressed;
    }
    bool MouseReleased(platform::MouseButton) const override { return false; }
    math::Vec2 MousePos() const override { return {}; }
    math::Vec2 MouseDelta() const override { return {}; }
    float WheelDelta() const override { return 0.0f; }
    void EndFrame() override { leftPressed = false; }
};
} // namespace

TEST(GameRuntimeFireballHitsWolf) {
    // Hero at origin (100 hp) fires toward -Z; wolf at (0,0,-3) with 40 hp.
    const char* scene = R"({
      "entities": [
        {"name": "英雄", "components": {"transform": {"pos": [0,0,0]},
          "mesh": {"meshKey": "hero"},
          "health": {"hp": 100, "maxHp": 100},
          "script": {"backend": "lua", "path": "hero_fire.lua"}}},
        {"name": "野狼", "components": {"transform": {"pos": [0,0,-3]},
          "mesh": {"meshKey": "wolf"},
          "health": {"hp": 40, "maxHp": 40}}}
      ]
    })";
    const char* lua = R"(
      function on_start(e)
        SetVar("cd", 0)
        SetVar("ticks", 0)
        SetVar("fired", 0)
      end
      function on_update(e, dt)
        SetVar("ticks", (GetVar("ticks") or 0) + 1)
        local cd = GetVar("cd") or 0
        cd = math.max(0, cd - dt)
        SetVar("cd", cd)
        if InputKey("1") > 0 and cd <= 0 then
          SpawnProjectile({x=0, y=1, z=0}, {x=0, y=0, z=-1}, 14, 18, 2.0, e)
          SetVar("cd", 0.5)
          SetVar("fired", (GetVar("fired") or 0) + 1)
        end
      end
    )";
    CombatInput input;
    input.key1 = true;
    scene::GameRuntime runtime;
    scene::GameRuntimeConfig cfg;
    cfg.readScript = [&](const std::string&) { return std::string(lua); };
    cfg.input = &input;
    cfg.headless = true; // no renderer; pure simulation
    CHECK(runtime.Start(scene, cfg).Ok());

    const ecs::Entity wolf = runtime.FindNamedEntity("野狼");
    CHECK(wolf.IsValid());
    auto before = runtime.EntityHealth(wolf);
    CHECK_EQ(before.first, 40.0f);

    // Run ~1.5s: fireballs fly to the wolf and damage it.
    for (int i = 0; i < 90; ++i) runtime.Tick(1.0f / 60.0f);

    auto after = runtime.EntityHealth(wolf);
    CHECK(after.first < before.first);
    CHECK(after.first >= 0.0f);
}

TEST(GameRuntimeMeleeAttackHitsInArc) {
    const char* scene = R"({
      "entities": [
        {"name": "英雄", "components": {"transform": {"pos": [0,0,0]},
          "health": {"hp": 100, "maxHp": 100}}},
        {"name": "wolf_front", "components": {"transform": {"pos": [0,0,-1.5]},
          "health": {"hp": 40, "maxHp": 40}}},
        {"name": "wolf_back", "components": {"transform": {"pos": [0,0,1.5]},
          "health": {"hp": 40, "maxHp": 40}}}
      ]
    })";
    scene::GameRuntime runtime;
    scene::GameRuntimeConfig cfg;
    cfg.headless = true;
    CHECK(runtime.Start(scene, cfg).Ok());

    // Swing forward (-Z), range 2, 100deg arc, 12 damage.
    const int hits = runtime.MeleeAttack({0, 1, 0}, {0, 0, -1}, 2.0f, 100.0f, 12.0f);
    CHECK_EQ(hits, 1); // only the wolf in front

    const ecs::Entity front = runtime.FindNamedEntity("wolf_front");
    const ecs::Entity back = runtime.FindNamedEntity("wolf_back");
    CHECK_EQ(runtime.EntityHealth(front).first, 28.0f); // 40 - 12
    CHECK_EQ(runtime.EntityHealth(back).first, 40.0f);  // untouched
}

// ---------------------------------------------------------------------------
// Regression: script-spawned entities (Spawn()) must be readable/writable via
// GetPosition/SetPosition exactly like scene entities. The GameRuntime hooks
// (sceneGetPos/sceneSetPos) used to only look at SceneTransform, so a spawned
// player's CTransformBind stayed frozen at (0,0,0) — the server/determinism
// suites failed because their scripted player never moved. Both component
// flavors must work through the same binding.
// ---------------------------------------------------------------------------

TEST(GameRuntimeSpawnedEntityPositionReadWrite) {
    const char* scene = R"({
      "entities": [
        {"name": "Host", "components": {
          "transform": {"pos": [0,0,0]},
          "script": {"backend": "lua", "path": "spawner.lua"}}}
      ]
    })";
    const char* lua = R"(
      function on_start(e)
        player = Spawn("player", { x = 0, y = 0, z = 0 })
      end
      function on_update(e, dt)
        local p = GetPosition(player)
        if p ~= nil then
          SetPosition(player, { x = p.x, y = p.y, z = p.z + 1 })
        end
      end
    )";
    scene::GameRuntime runtime;
    scene::GameRuntimeConfig cfg;
    cfg.readScript = [&](const std::string&) { return std::string(lua); };
    cfg.headless = true;
    CHECK(runtime.Start(scene, cfg).Ok());
    CHECK_EQ(runtime.EntityCount(), 2u); // Host + script-spawned player

    for (int i = 0; i < 10; ++i) runtime.Tick(1.0f / 60.0f);

    // The spawned entity (CTransformBind) must have moved to z == 10.
    int seen = 0;
    math::Vec3 pos;
    auto view = runtime.World().ViewAll<script::CTransformBind>();
    for (size_t i = 0; i < view.Size(); ++i) {
        ecs::Entity e = runtime.World().EntityAt<script::CTransformBind>(i);
        const script::CTransformBind* t = runtime.World().Get<script::CTransformBind>(e);
        if (t) {
            ++seen;
            pos = t->pos;
        }
    }
    CHECK_EQ(seen, 1);
    CHECK_NEAR(pos.z, 10.0f, 1e-6);
}

// Godot-style scene tree: transform.parent (by name) resolves to a
// SceneParentLink; an unknown parent name rejects the scene.
TEST(GameRuntimeSceneTreeParentLink) {
    const char* scene = R"({
      "entities": [
        {"name": "Root", "components": {"transform": {"pos": [0,0,0]}}},
        {"name": "Child", "components": {"transform": {"pos": [5,0,0], "parent": "Root"}}}
      ]
    })";
    scene::GameRuntime runtime;
    scene::GameRuntimeConfig cfg;
    cfg.headless = true;
    CHECK(runtime.Start(scene, cfg).Ok());

    ecs::Entity root, child;
    auto view = runtime.World().ViewAll<scene::SceneName>();
    for (size_t i = 0; i < view.Size(); ++i) {
        ecs::Entity e = runtime.World().EntityAt<scene::SceneName>(i);
        const scene::SceneName* n = runtime.World().Get<scene::SceneName>(e);
        if (n && n->name == "Root") root = e;
        if (n && n->name == "Child") child = e;
    }
    CHECK(root.IsValid());
    CHECK(child.IsValid());
    const scene::SceneParentLink* link = runtime.World().Get<scene::SceneParentLink>(child);
    CHECK(link != nullptr);
    if (link) CHECK(link->parent == root);
    CHECK(runtime.World().Get<scene::SceneParentLink>(root) == nullptr);

    // Unknown parent name: the scene is rejected (no dangling links).
    const char* bad = R"({
      "entities": [
        {"name": "A", "components": {"transform": {"pos": [0,0,0], "parent": "Ghost"}}}
      ]
    })";
    scene::GameRuntime r2;
    CHECK(!r2.Start(bad, cfg).Ok());
}

// ChangeScene defers the swap to the end of Tick (never mid-Lua-call): the
// first tick queues it and applies it before the next tick runs.
TEST(GameRuntimeGroupsQuery) {
    // P1-1: entities with the "groups" component are queryable by name through
    // the script context (GetEntitiesInGroup).
    const std::string scene = R"({
      "entities": [
        {"name": "hero", "components": {"transform": {"pos": [0,0,0]}, "groups": {"groups": ["player","respawn"]}}},
        {"name": "wolf", "components": {"transform": {"pos": [1,0,0]}, "groups": {"groups": ["enemy"]}}},
        {"name": "tree", "components": {"transform": {"pos": [2,0,0]}}}
      ]
    })";
    scene::GameRuntime runtime;
    core::Status st = runtime.Start(scene, scene::GameRuntimeConfig{});
    CHECK(st.Ok());
    auto& ctx = runtime.ScriptContext();
    CHECK(ctx.entitiesInGroup);
    const auto players = ctx.entitiesInGroup("player");
    CHECK_EQ(players.size(), 1u);
    const auto enemies = ctx.entitiesInGroup("enemy");
    CHECK_EQ(enemies.size(), 1u);
    const auto respawn = ctx.entitiesInGroup("respawn");
    CHECK_EQ(respawn.size(), 1u);
    CHECK_EQ(respawn[0].id, players[0].id);
    CHECK(ctx.entitiesInGroup("missing").empty());
    CHECK(ctx.entitiesInGroup("").empty());
}

TEST(GameRuntimeChangeSceneDeferred) {
    const char* sceneA = R"({
      "entities": [
        {"name": "A", "components": {
          "transform": {"pos": [0,0,0]},
          "script": {"backend": "lua", "path": "a.lua"}}}
      ]
    })";
    const char* sceneB = R"({
      "entities": [
        {"name": "B", "components": {"transform": {"pos": [1,0,0]}}}
      ]
    })";
    const char* luaA = R"(
      function on_start(e) end
      function on_update(e, dt)
        if ChangeScene("scenes/b.json") == 1 then
          SetVar("switched", 1)
        end
      end
    )";
    scene::GameRuntime runtime;
    scene::GameRuntimeConfig cfg;
    cfg.headless = true;
    cfg.readScript = [&](const std::string& p) {
        if (p == "a.lua") return std::string(luaA);
        if (p == "scenes/b.json") return std::string(sceneB);
        return std::string();
    };
    CHECK(runtime.Start(sceneA, cfg).Ok());
    CHECK(runtime.FindNamedEntity("A").IsValid());
    runtime.Tick(1.0f / 60.0f); // on_update queues the swap; Tick applies it
    CHECK(runtime.Running());
    CHECK(!runtime.FindNamedEntity("A").IsValid());
    CHECK(runtime.FindNamedEntity("B").IsValid());
    CHECK_EQ(runtime.EntityCount(), 1u);
}
