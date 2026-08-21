#include <string>
#include <vector>

#include "neon/neon.hpp"
#include "neon/scene/game_runtime.hpp"
#include "neon/scene/scene_file.hpp"
#include "helpers.hpp"
#include "test_backend.hpp"

using namespace neon;

namespace {

const scene::ComponentDef* FindComp(const scene::EntityDef& e, const std::string& name) {
    for (const auto& c : e.components)
        if (c.name == name) return &c;
    return nullptr;
}

} // namespace

// ---------------------------------------------------------------------------
// Pure PickLod math: level selection across threshold boundaries
// ---------------------------------------------------------------------------

TEST(LodPickLodThresholdSelection) {
    // Three levels (hi/mid/lo), two thresholds: level 0 < 80, level 1 in
    // [80, 200), level 2 at/after 200.
    gfx::LodChain chain;
    chain.levels.push_back(gfx::Mesh{});
    chain.levels.push_back(gfx::Mesh{});
    chain.levels.push_back(gfx::Mesh{});
    chain.thresholds = {80.0f, 200.0f};

    CHECK_EQ(gfx::PickLod(chain, 0.0f), 0);
    CHECK_EQ(gfx::PickLod(chain, 79.999f), 0); // just below the first threshold
    CHECK_EQ(gfx::PickLod(chain, 80.0f), 1);   // exactly on the threshold -> next level
    CHECK_EQ(gfx::PickLod(chain, 100.0f), 1);
    CHECK_EQ(gfx::PickLod(chain, 199.999f), 1);
    CHECK_EQ(gfx::PickLod(chain, 200.0f), 2); // exactly on the final threshold
    CHECK_EQ(gfx::PickLod(chain, 5000.0f), 2);

    // Single level, no thresholds: every distance maps to level 0.
    gfx::LodChain single;
    single.levels.push_back(gfx::Mesh{});
    CHECK_EQ(gfx::PickLod(single, 0.0f), 0);
    CHECK_EQ(gfx::PickLod(single, 1e9f), 0);

    // Degraded chain (one level missing after the first): still safe.
    gfx::LodChain degraded;
    degraded.levels.push_back(gfx::Mesh{});
    degraded.levels.push_back(gfx::Mesh{});
    degraded.thresholds = {80.0f};
    CHECK_EQ(gfx::PickLod(degraded, 10.0f), 0);
    CHECK_EQ(gfx::PickLod(degraded, 500.0f), 1); // last level beyond the threshold

    // Empty chain -> -1 (nothing to draw).
    gfx::LodChain empty;
    CHECK_EQ(gfx::PickLod(empty, 10.0f), -1);
}

// ---------------------------------------------------------------------------
// Scene factory: the mesh component parses a "lod" list into SceneMesh::lod
// ---------------------------------------------------------------------------

TEST(LodSceneFactoryResolvesChainSpec) {
    const char* json = R"({
        "entities": [
            {"components": {
                "transform": {"pos": [0,0,0]},
                "mesh": {
                    "meshKey": "gltf:assets/tree_hi.glb",
                    "lod": [
                        {"distance": 80,  "meshKey": "gltf:assets/tree_mid.glb"},
                        {"distance": 200, "meshKey": "gltf:assets/tree_lo.glb"}
                    ]
                }
            }}
        ]
    })";
    auto res = scene::SceneFile::Parse(json);
    CHECK(res.Ok());

    scene::ComponentRegistry reg;
    scene::RegisterBuiltinComponents(reg); // no assets: any key accepted
    ecs::World world;
    scene::PrefabLibrary prefs;
    auto inst = scene::Instantiate(world, res.Value(), prefs, reg);
    CHECK(inst.Ok());

    auto view = world.ViewAll<scene::SceneMesh>();
    CHECK_EQ(view.Size(), 1u);
    ecs::Entity ent = world.EntityAt<scene::SceneMesh>(0);
    const scene::SceneMesh* m = world.Get<scene::SceneMesh>(ent);
    CHECK(m != nullptr);
    CHECK_EQ(m->meshKey, std::string("gltf:assets/tree_hi.glb"));
    CHECK_EQ(m->lod.size(), 2u);
    CHECK_NEAR(m->lod[0].distance, 80.0, 1e-6);
    CHECK_EQ(m->lod[0].meshKey, std::string("gltf:assets/tree_mid.glb"));
    CHECK_NEAR(m->lod[1].distance, 200.0, 1e-6);
    CHECK_EQ(m->lod[1].meshKey, std::string("gltf:assets/tree_lo.glb"));

    // No lod member -> empty chain (single-mesh entity).
    auto plain = scene::SceneFile::Parse(R"({
        "entities": [
            {"components": {"transform": {"pos": [0,0,0]},
                            "mesh": {"meshKey": "gltf:assets/tree_hi.glb"}}}
        ]
    })");
    CHECK(plain.Ok());
    ecs::World world2;
    auto inst2 = scene::Instantiate(world2, plain.Value(), prefs, reg);
    CHECK(inst2.Ok());
    const scene::SceneMesh* m2 = world2.Get<scene::SceneMesh>(world2.EntityAt<scene::SceneMesh>(0));
    CHECK(m2 != nullptr);
    CHECK_EQ(m2->lod.size(), 0u);
}

TEST(LodSceneFactoryRejectsBadChain) {
    scene::ComponentRegistry reg;
    scene::RegisterBuiltinComponents(reg);
    scene::PrefabLibrary prefs;

    auto instantiate = [&](const std::string& lodJson) {
        std::string json = std::string(R"({"entities":[{"components":{
            "transform":{"pos":[0,0,0]},
            "mesh":{"meshKey":"gltf:assets/tree_hi.glb", "lod": )") + lodJson + R"(}}}]})";
        auto res = scene::SceneFile::Parse(json);
        if (!res.Ok()) return false;
        ecs::World world;
        return scene::Instantiate(world, res.Value(), prefs, reg).Ok();
    };

    // Non-increasing distances (200 then 80) -> rejected.
    CHECK(!instantiate(R"([{"distance":200,"meshKey":"gltf:a.glb"},
                           {"distance":80,"meshKey":"gltf:b.glb"}])"));
    // Duplicate distances -> rejected.
    CHECK(!instantiate(R"([{"distance":80,"meshKey":"gltf:a.glb"},
                           {"distance":80,"meshKey":"gltf:b.glb"}])"));
    // Missing distance -> rejected.
    CHECK(!instantiate(R"([{"meshKey":"gltf:a.glb"}])"));
    // Missing/empty meshKey -> rejected.
    CHECK(!instantiate(R"([{"distance":80,"meshKey":""}])"));
    // Non-object entry -> rejected.
    CHECK(!instantiate(R"([5])"));
    // lod not an array -> rejected.
    CHECK(!instantiate(R"({"distance":80})"));

    // A strictly increasing chain still instantiates.
    CHECK(instantiate(R"([{"distance":80,"meshKey":"gltf:a.glb"},
                          {"distance":200,"meshKey":"gltf:b.glb"}])"));
}

TEST(LodSceneFactoryValidatesLodPrefixesWithAssets) {
    scene::ComponentRegistry reg;
    assets::AssetManager am; // non-null: LOD meshKeys must use a known prefix
    scene::RegisterBuiltinComponents(reg, &am);
    scene::PrefabLibrary prefs;

    auto parse = [&](const char* meshKey, const char* lodKey) {
        std::string json = std::string(R"({"entities":[{"components":{
            "transform":{"pos":[0,0,0]},
            "mesh":{"meshKey":")") + meshKey + R"(", "lod":[{"distance":80,"meshKey":")" +
                           lodKey + R"("}]}}}]})";
        auto res = scene::SceneFile::Parse(json);
        if (!res.Ok()) return false;
        ecs::World world;
        return scene::Instantiate(world, res.Value(), prefs, reg).Ok();
    };

    CHECK(parse("gltf:a.glb", "obj:b.obj"));
    CHECK(!parse("gltf:a.glb", "ftp://bad"));
    CHECK(!parse("gltf:a.glb", "cube"));
}

// ---------------------------------------------------------------------------
// Export round-trip: MakeEntity emits "lod", Parse/Instantiate restores it
// ---------------------------------------------------------------------------

TEST(LodMakeEntityRoundTrip) {
    std::vector<scene::LodEntry> lod;
    lod.push_back({80.0f, "gltf:assets/tree_mid.glb"});
    lod.push_back({200.0f, "gltf:assets/tree_lo.glb"});
    auto res = scene::SceneFile::MakeEntity("Pine", {1, 2, 3}, {}, {1, 1, 1},
                                            "gltf:assets/tree_hi.glb", 0.0f, 1.0f,
                                            gfx::Color::White, "", "", "", "", 1.0f, 1.0f,
                                            "", "lua", core::Json{}, lod);
    CHECK(res.Ok());
    const core::Json& e = res.Value();
    const core::Json* mesh = e.Get("components")->Get("mesh");
    CHECK(mesh != nullptr);
    const core::Json* lodArr = mesh->Get("lod");
    CHECK(lodArr != nullptr && lodArr->IsArray() && lodArr->Size() == 2u);
    CHECK_NEAR(lodArr->At(0)->Get("distance")->GetNumber(), 80.0, 1e-9);
    CHECK_EQ(lodArr->At(0)->Get("meshKey")->GetString(), std::string("gltf:assets/tree_mid.glb"));
    CHECK_NEAR(lodArr->At(1)->Get("distance")->GetNumber(), 200.0, 1e-9);
    CHECK_EQ(lodArr->At(1)->Get("meshKey")->GetString(), std::string("gltf:assets/tree_lo.glb"));

    // Round-trip through Parse -> Instantiate.
    core::Json root;
    root.type_ = core::Json::Type::Object;
    core::Json arr;
    arr.type_ = core::Json::Type::Array;
    arr.array_.push_back(e);
    root.object_["entities"] = std::move(arr);
    std::string json = core::JsonWriter::Write(root);

    auto parsed = scene::SceneFile::Parse(json);
    CHECK(parsed.Ok());
    const scene::ComponentDef* comp = FindComp(parsed.Value().entities[0], "mesh");
    CHECK(comp != nullptr);
    CHECK(comp->data.Get("lod") != nullptr);

    scene::ComponentRegistry reg;
    scene::RegisterBuiltinComponents(reg);
    scene::PrefabLibrary prefs;
    ecs::World world;
    auto inst = scene::Instantiate(world, parsed.Value(), prefs, reg);
    CHECK(inst.Ok());
    const scene::SceneMesh* m = world.Get<scene::SceneMesh>(world.EntityAt<scene::SceneMesh>(0));
    CHECK(m != nullptr);
    CHECK_EQ(m->meshKey, std::string("gltf:assets/tree_hi.glb"));
    CHECK_EQ(m->lod.size(), 2u);
    CHECK_NEAR(m->lod[0].distance, 80.0, 1e-6);
    CHECK_EQ(m->lod[0].meshKey, std::string("gltf:assets/tree_mid.glb"));
    CHECK_NEAR(m->lod[1].distance, 200.0, 1e-6);
    CHECK_EQ(m->lod[1].meshKey, std::string("gltf:assets/tree_lo.glb"));

    // No lod argument -> no "lod" member emitted.
    auto plain = scene::SceneFile::MakeEntity("Pine", {0, 0, 0}, {}, {1, 1, 1},
                                              "gltf:assets/tree_hi.glb");
    CHECK(plain.Ok());
    CHECK(plain.Value().Get("components")->Get("mesh")->Get("lod") == nullptr);
}

// ---------------------------------------------------------------------------
// GameRuntime: LOD chain resolution + per-frame distance picking (headless)
// ---------------------------------------------------------------------------

TEST(LodGameRuntimeResolvesAndPicksChain) {
    // Base "cube" with two LOD levels: "sphere" past 10 units, "plane" past 50.
    const char* json = R"({"entities":[
        {"name":"LODTree","components":{
            "transform":{"pos":[5,0,0]},
            "mesh":{"meshKey":"cube",
                    "lod":[{"distance":10,"meshKey":"sphere"},
                           {"distance":50,"meshKey":"plane"}]}}}]})";
    test::HeadlessAssetFixture fix;
    scene::GameRuntime runtime;
    scene::GameRuntimeConfig cfg;
    cfg.assets = &fix.assets;
    CHECK(runtime.Start(json, cfg).Ok());
    CHECK_EQ(runtime.DrawCount(), 1u);

    auto view = runtime.World().ViewAll<scene::SceneMesh>();
    CHECK_EQ(view.Size(), 1u);
    ecs::Entity ent = runtime.World().EntityAt<scene::SceneMesh>(0);

    // Resolve the chain once (Draw submits; the NullBackend records nothing).
    gfx::Camera cam;
    runtime.Draw(fix.renderer, cam);

    // Camera at the origin: entity at (5,0,0) is 5 units away -> level 0 (cube).
    cam.position = {0, 0, 0};
    gfx::Mesh nearMesh = runtime.MeshForEntity(ent, cam);
    CHECK(nearMesh.Valid());
    CHECK_EQ(nearMesh.Name(), std::string("cube"));

    // 25 units away: [10, 50) -> level 1 (sphere).
    cam.position = {30, 0, 0};
    gfx::Mesh midMesh = runtime.MeshForEntity(ent, cam);
    CHECK(midMesh.Valid());
    CHECK_EQ(midMesh.Name(), std::string("sphere"));

    // 95 units away: past 50 -> level 2 (plane).
    cam.position = {100, 0, 0};
    gfx::Mesh farMesh = runtime.MeshForEntity(ent, cam);
    CHECK(farMesh.Valid());
    CHECK_EQ(farMesh.Name(), std::string("plane"));

    // Exactly on the threshold: 10 units -> level 1 (sphere).
    cam.position = {15, 0, 0};
    gfx::Mesh boundaryMesh = runtime.MeshForEntity(ent, cam);
    CHECK(boundaryMesh.Valid());
    CHECK_EQ(boundaryMesh.Name(), std::string("sphere"));

    // A single-mesh entity (no lod) always resolves to its base mesh.
    auto runtime2 = scene::GameRuntime();
    CHECK(runtime2.Start(R"({"entities":[{"components":{
        "transform":{"pos":[5,0,0]},"mesh":{"meshKey":"sphere"}}}]})",
                         cfg).Ok());
    runtime2.Draw(fix.renderer, cam);
    auto view2 = runtime2.World().ViewAll<scene::SceneMesh>();
    CHECK_EQ(view2.Size(), 1u);
    ecs::Entity ent2 = runtime2.World().EntityAt<scene::SceneMesh>(0);
    cam.position = {100, 0, 0};
    CHECK_EQ(runtime2.MeshForEntity(ent2, cam).Name(), std::string("sphere"));

    runtime.Stop();
    runtime2.Stop();
}

TEST(LodGameRuntimeDegradesGracefully) {
    // The second LOD level ("bogus") cannot be resolved: the chain degrades to
    // [cube, sphere] with a single threshold, so the sphere still covers far
    // distances instead of failing the entity.
    const char* json = R"({"entities":[
        {"components":{
            "transform":{"pos":[5,0,0]},
            "mesh":{"meshKey":"cube",
                    "lod":[{"distance":10,"meshKey":"sphere"},
                           {"distance":50,"meshKey":"bogus"}]}}}]})";
    test::HeadlessAssetFixture fix;
    scene::GameRuntime runtime;
    scene::GameRuntimeConfig cfg;
    cfg.assets = &fix.assets;
    CHECK(runtime.Start(json, cfg).Ok());

    gfx::Camera cam;
    runtime.Draw(fix.renderer, cam); // resolves; bogus level logs + is dropped
    auto view = runtime.World().ViewAll<scene::SceneMesh>();
    CHECK_EQ(view.Size(), 1u);
    ecs::Entity ent = runtime.World().EntityAt<scene::SceneMesh>(0);

    cam.position = {0, 0, 0}; // 5 units -> level 0
    CHECK_EQ(runtime.MeshForEntity(ent, cam).Name(), std::string("cube"));
    cam.position = {100, 0, 0}; // 95 units -> last surviving level (sphere)
    CHECK_EQ(runtime.MeshForEntity(ent, cam).Name(), std::string("sphere"));
    runtime.Stop();
}
