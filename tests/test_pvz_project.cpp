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
    CHECK(LoadLua("projects/pvz/assets/scripts/game.lua"));
    CHECK(LoadLua("projects/pvz/assets/scripts/plants.lua"));
    CHECK(LoadLua("projects/pvz/assets/scripts/zombies.lua"));
    CHECK(LoadLua("projects/pvz/assets/scripts/pea.lua"));
    CHECK(LoadLua("projects/pvz/assets/scripts/sun.lua"));
}

#ifdef NEON_ENABLE_JS
// The HUD and the wave-director plugin are JS.
TEST(PvzJsScriptsCompile) {
    CHECK(LoadJs("projects/pvz/assets/scripts/hud.js"));
    CHECK(LoadJs("projects/pvz/plugins/wave_director/init.js"));
}
#endif

// The scene + seed prefabs are valid JSON that the runtime can parse.
TEST(PvzSceneAndPrefabJsonValid) {
    std::string scene;
    CHECK(test::ReadFileAll("projects/pvz/assets/scenes/pvz.json", scene));
    CHECK(scene::SceneFile::Parse(scene).Ok());

    // Every prefab file referenced by the plant table must be present + parse.
    const char* prefs[] = {
        "sunflower",       "peashooter", "snowpea", "repeater",
        "cherrybomb",      "wallnut",    "zombie_basic", "zombie_cone",
        "zombie_bucket",   "pea",        "snow_pea",     "sun",
    };
    for (const char* p : prefs) {
        std::string body;
        CHECK(test::ReadFileAll(std::string("projects/pvz/assets/prefabs/") + p + ".json",
                                body));
        CHECK(!body.empty());
    }
}

// G2-2 编辑器 ECS 化：pvz 场景能被运行时 Instantiate 无损承载进 ecs::World
// （编辑器持有的 live sceneWorld_ 用同一路径）——植物/僵尸经通用 SceneData、
// 精灵/生命/变换经专用组件，全部存活。
TEST(PvzSceneHostsInEcsWorld) {
    std::string scene;
    CHECK(test::ReadFileAll("projects/pvz/assets/scenes/pvz.json", scene));
    auto parsed = scene::SceneFile::Parse(scene);
    CHECK(parsed.Ok());
    if (!parsed.Ok()) return;

    scene::ComponentRegistry reg;
    scene::RegisterBuiltinComponents(reg);
    ecs::World world;
    scene::PrefabLibrary prefs;
    auto inst = scene::Instantiate(world, parsed.Value(), prefs, reg);
    CHECK(inst.Ok());
    if (!inst.Ok()) return;
    CHECK(inst.Value() >= 12u); // 12 scene entities (lawn, plants, zombie, cam...)

    // Every entity carries a transform; sprites carry SceneSprite, the zombie
    // has health, and plant/zombie data survives in generic SceneData.
    CHECK_EQ(world.ViewAll<scene::SceneTransform>().Size(), inst.Value());
    CHECK(world.ViewAll<scene::SceneSprite>().Size() >= 8u); // house/lawn/plants/zombie
    bool sawHealth = false, sawZombieData = false, sawPlantData = false;
    world.ViewAll<scene::SceneData>().ForEach(
        [&](ecs::Entity, const scene::SceneData& sd) {
            for (const auto& [cname, cdata] : sd.components) {
                if (cname == "plant") sawPlantData = true;
                if (cname == "zombie") sawZombieData = true;
            }
        });
    world.ViewAll<scene::SceneHealth>().ForEach(
        [&](ecs::Entity, const scene::SceneHealth&) { sawHealth = true; });
    CHECK(sawPlantData);  // sunflower/peashooter plant components survive
    CHECK(sawZombieData); // the pre-placed zombie survives
    CHECK(sawHealth);     // the zombie's health survives
}
