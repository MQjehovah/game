#include <cmath>
#include <string>

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

// All scripts share one Lua host, so a later-loaded script's on_update shadows
// the earlier one for EVERY entity. This test pins that documented behavior.
TEST(GameRuntimeScriptGlobalShadowing) {
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

    // scriptB loaded last: its on_update runs for BOTH entities. Entity A's
    // script (shadow_mark) never ran, and both entities' counters advanced
    // under scriptB's function.
    CHECK(runtime.GameVars().Get("shadow_mark").type == script::Value::Type::Nil);
    for (const ecs::Entity& e : scriptEnts) {
        const std::string key = "tick_" + std::to_string(e.id);
        CHECK(runtime.GameVars().Get(key).type == script::Value::Type::Number);
        CHECK_EQ(runtime.GameVars().Get(key).number, 10.0);
    }
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
