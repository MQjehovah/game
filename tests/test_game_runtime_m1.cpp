// M1 gameplay APIs: per-entity animation overrides, world->screen projection,
// floating combat texts and overhead plates — plus their script bindings.
#include <cmath>
#include <string>

#include "neon/neon.hpp"
#include "neon/scene/game_runtime.hpp"
#include "neon/scene/scene_file.hpp"
#include "helpers.hpp"

using namespace neon;

namespace {

// Two skinned "models" are unnecessary here: the API surface is exercised with
// a plain entity (no clips) for the negative paths, plus a scripted entity
// driving the Lua bindings (PlayAnimation on a non-skinned entity returns
// false but must not crash; FloatTexts/ScreenAnchors round-trip).
const char* kScene = R"({
  "entities": [
    {
      "name": "Actor",
      "components": {
        "transform": {"pos": [1, 2, 3]},
        "script": {"backend": "lua", "path": "m1.lua"}
      }
    }
  ]
})";

const char* kLua = R"(
function on_start(e)
  -- Negative path: a non-skinned entity refuses to play.
  SetVar("played", PlayAnimation(e, "attack", false, 0.2, 1.0))
  -- Float text via script.
  SpawnFloatText(1, 2, 3, "42", true, 2.0)
  -- Plate stamp.
  SetEntityPlate(e, "Wolf", 0.75)
  -- WorldToScreen without a prior Draw reports unavailable (nil) rather
  -- than crashing.
  local s = WorldToScreen(0, 0, 0)
  SetVar("w2s_nil", s == nil)
  -- ScreenAnchors pre-Draw is empty; FloatTexts already has ours (on_start
  -- runs after the hooks are wired, so the spawn landed).
  local a = ScreenAnchors()
  local p = EntityPlates()
  local f = FloatTexts()
  SetVar("anchors_pre", #a)
  SetVar("texts_pre", #f)
end
)";

} // namespace

TEST(GameRuntimeM1AnimationApis) {
    scene::GameRuntime runtime;
    scene::GameRuntimeConfig cfg;
    cfg.readScript = [](const std::string& path) {
        return path == "m1.lua" ? std::string(kLua) : std::string();
    };
    core::Status st = runtime.Start(kScene, cfg);
    CHECK(st.Ok());
    CHECK(runtime.Running());

    // Script-side negatives all behaved.
    auto boolVar = [&runtime](const char* k) -> bool {
        script::Value v = runtime.GameVars().Get(k);
        return v.type == script::Value::Type::Bool && v.boolean;
    };
    auto numVar = [&runtime](const char* k) -> double {
        script::Value v = runtime.GameVars().Get(k);
        return v.type == script::Value::Type::Number ? v.number : -1.0;
    };
    CHECK(runtime.GameVars().Get("played").type == script::Value::Type::Bool);
    CHECK(!runtime.GameVars().Get("played").boolean); // non-skinned -> false
    CHECK(boolVar("w2s_nil"));
    CHECK_EQ(numVar("anchors_pre"), 0.0);
    CHECK_EQ(numVar("texts_pre"), 1.0); // our on_start spawn is already live

    // The script-spawned float text is visible through the C++ API.
    CHECK_EQ(runtime.FloatTexts().size(), 1u);
    if (!runtime.FloatTexts().empty()) {
        CHECK(runtime.FloatTexts()[0].crit);
        CHECK_EQ(runtime.FloatTexts()[0].text, std::string("42"));
    }
    runtime.Tick(1.0f / 60.0f);

    // Direct C++ float text: lifetime expiry.
    runtime.SpawnFloatText({0, 0, 0}, "100", false, 0.5f);
    for (int i = 0; i < 40; ++i) runtime.Tick(1.0f / 60.0f); // 0.66s > 0.5s
    bool has100 = false;
    for (const auto& f : runtime.FloatTexts())
        if (f.text == "100") has100 = true;
    CHECK(!has100); // expired

    // Animation query APIs on an entity without overrides.
    ecs::Entity ent = runtime.FindNamedEntity("Actor");
    CHECK(ent.IsValid());
    CHECK(!runtime.AnimationFinished(ent));
    CHECK_NEAR(runtime.AnimationProgress(ent), 0.0f, 1e-5);

    // WorldToScreen before any Draw returns false (no view-proj yet).
    float sx = -1.0f, sy = -1.0f;
    CHECK(!runtime.WorldToScreen({0, 0, 0}, sx, sy));
}
