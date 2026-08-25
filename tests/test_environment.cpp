#include "neon/neon.hpp"
#include "neon/scene/scene_file.hpp"
#include "helpers.hpp"

using namespace neon;

TEST(EnvironmentParsesAndApplies) {
    const char* json = R"({"entities":[{"components":{
        "transform":{"pos":[0,0,0]},
        "environment":{
            "ambientColor":[0.2,0.3,0.4,1],
            "ambientStrength":0.6,
            "sunDir":[0,1,0],
            "sunColor":[1,0.5,0.2,1],
            "cameraFov":45,
            "cameraOrtho":true,
            "orthoSize":20,
            "designWidth":1024,
            "designHeight":512,
            "letterbox":false
        }
    }}]})";
    scene::ComponentRegistry reg;
    scene::RegisterBuiltinComponents(reg);
    ecs::World world;
    scene::PrefabLibrary prefs;
    auto res = scene::SceneFile::Parse(json);
    CHECK(res.Ok());
    CHECK(scene::Instantiate(world, res.Value(), prefs, reg).Ok());
    auto view = world.ViewAll<scene::SceneEnvironment>();
    CHECK_EQ(view.Size(), 1u);
    ecs::Entity ent = world.EntityAt<scene::SceneEnvironment>(0);
    const scene::SceneEnvironment* e = world.Get<scene::SceneEnvironment>(ent);
    CHECK(e != nullptr);
    CHECK_NEAR(e->ambientColor.r, 0.2, 1e-5);
    CHECK_NEAR(e->ambientStrength, 0.6, 1e-5);
    CHECK_NEAR(e->sunDir.y, 1.0, 1e-5);
    CHECK_NEAR(e->sunColor.g, 0.5, 1e-5);
    CHECK_NEAR(e->cameraFov, 45.0, 1e-5);
    CHECK(e->cameraOrtho);
    CHECK_NEAR(e->orthoSize, 20.0, 1e-5);
    CHECK_EQ(e->designWidth, 1024);
    CHECK_EQ(e->designHeight, 512);
    CHECK(!e->letterbox);
}

TEST(EnvironmentAbsentDefaultsAtRuntime) {
    const char* json = R"({"entities":[{"components":{"transform":{"pos":[0,0,0]}}}]})";
    scene::ComponentRegistry reg;
    scene::RegisterBuiltinComponents(reg);
    ecs::World world;
    scene::PrefabLibrary prefs;
    auto res = scene::SceneFile::Parse(json);
    CHECK(res.Ok());
    CHECK(scene::Instantiate(world, res.Value(), prefs, reg).Ok());
    CHECK_EQ(world.ViewAll<scene::SceneEnvironment>().Size(), 0u);
}
