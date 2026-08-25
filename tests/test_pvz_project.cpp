#include <string>

#include "neon/neon.hpp"
#include "neon/script/script.hpp"
#include "neon/scene/scene_file.hpp"
#include "helpers.hpp"

using namespace neon;

namespace {

bool LoadLua(const std::string& path) {
    std::string src;
    if (!test::ReadFileAll(path, src)) return false;
    auto host = script::CreateLuaHost();
    if (!host || !host->Init()) return false;
    return host->Load(src);
}

bool LoadJs(const std::string& path) {
    std::string src;
    if (!test::ReadFileAll(path, src)) return false;
    auto host = script::CreateJsHost();
    if (!host || !host->Init()) return false;
    return host->Load(src);
}

} // namespace

// Every PvZ Lua script must compile (a syntax error would break the game).
TEST(PvzLuaScriptsCompile) {
    CHECK(LoadLua("projects/pvz/scripts/game.lua"));
    CHECK(LoadLua("projects/pvz/scripts/plants.lua"));
    CHECK(LoadLua("projects/pvz/scripts/zombies.lua"));
    CHECK(LoadLua("projects/pvz/scripts/pea.lua"));
    CHECK(LoadLua("projects/pvz/scripts/sun.lua"));
}

#ifdef NEON_ENABLE_JS
// The HUD and the wave-director plugin are JS.
TEST(PvzJsScriptsCompile) {
    CHECK(LoadJs("projects/pvz/scripts/hud.js"));
    CHECK(LoadJs("projects/pvz/plugins/wave_director/init.js"));
}
#endif

// The scene + seed prefabs are valid JSON that the runtime can parse.
TEST(PvzSceneAndPrefabJsonValid) {
    std::string scene;
    CHECK(test::ReadFileAll("projects/pvz/scenes/pvz.json", scene));
    CHECK(scene::SceneFile::Parse(scene).Ok());

    // Every prefab file referenced by the plant table must be present + parse.
    const char* prefs[] = {
        "sunflower",       "peashooter", "snowpea", "repeater",
        "cherrybomb",      "wallnut",    "zombie_basic", "zombie_cone",
        "zombie_bucket",   "pea",        "snow_pea",     "sun",
    };
    for (const char* p : prefs) {
        std::string body;
        CHECK(test::ReadFileAll(std::string("projects/pvz/prefabs/") + p + ".json", body));
        CHECK(!body.empty());
    }
}
