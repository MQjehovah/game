#include <string>
#include <vector>

#include "neon/neon.hpp"
#include "neon/scene/scene_file.hpp"
#include "helpers.hpp"

using namespace neon;

namespace {

const scene::ComponentDef* FindComp(const scene::EntityDef& e, const std::string& name) {
    for (const auto& c : e.components)
        if (c.name == name) return &c;
    return nullptr;
}

// Assemble a componentized scene root from a list of entity JSONs, exactly as
// the editor export does ("entities" array + optional gameVars).
core::Json MakeSceneRoot(const std::vector<core::Json>& entities) {
    core::Json root;
    root.type_ = core::Json::Type::Object;
    core::Json arr;
    arr.type_ = core::Json::Type::Array;
    arr.array_ = entities;
    root.object_["entities"] = std::move(arr);
    return root;
}

} // namespace

// ---------------------------------------------------------------------------
// SceneFile::MakeEntity builds the componentized entity JSON
// ---------------------------------------------------------------------------

TEST(SceneMakeEntityStructure) {
    auto res = scene::SceneFile::MakeEntity("Cube", {1.5f, 2.0f, -3.0f},
                                            math::Quat{0.0f, 0.5f, 0.0f, 0.866f}, {2, 2, 2},
                                            "gltf:assets/cube.gltf", 0.3f, 0.7f);
    CHECK(res.Ok());
    const core::Json& e = res.Value();
    CHECK(e.IsObject());
    CHECK_EQ(e.Get("name")->GetString(), std::string("Cube"));
    CHECK(e.Get("prefab") == nullptr); // plain entity, not a prefab instance

    const core::Json* comps = e.Get("components");
    CHECK(comps != nullptr && comps->IsObject());
    // The entity name lives in the top-level "name" field only; no duplicate
    // "name" component is emitted.
    CHECK(comps->Get("name") == nullptr);

    const core::Json* tf = comps->Get("transform");
    CHECK(tf != nullptr);
    const core::Json* pos = tf->Get("pos");
    CHECK(pos != nullptr && pos->IsArray() && pos->Size() == 3u);
    CHECK_NEAR(pos->At(0)->GetNumber(), 1.5, 1e-9);
    CHECK_NEAR(pos->At(1)->GetNumber(), 2.0, 1e-9);
    CHECK_NEAR(pos->At(2)->GetNumber(), -3.0, 1e-9);
    const core::Json* rot = tf->Get("rot");
    CHECK(rot != nullptr && rot->IsArray() && rot->Size() == 4u);
    CHECK_NEAR(rot->At(0)->GetNumber(), 0.0, 1e-6);
    CHECK_NEAR(rot->At(1)->GetNumber(), 0.5, 1e-6);
    CHECK_NEAR(rot->At(2)->GetNumber(), 0.0, 1e-6);
    CHECK_NEAR(rot->At(3)->GetNumber(), 0.866, 1e-6);
    const core::Json* scale = tf->Get("scale");
    CHECK(scale != nullptr && scale->IsArray() && scale->Size() == 3u);
    CHECK_NEAR(scale->At(0)->GetNumber(), 2.0, 1e-9);

    const core::Json* mesh = comps->Get("mesh");
    CHECK(mesh != nullptr);
    CHECK_EQ(mesh->Get("meshKey")->GetString(), std::string("gltf:assets/cube.gltf"));
    const core::Json* mat = mesh->Get("material");
    CHECK(mat != nullptr && mat->IsObject());
    CHECK_NEAR(mat->Get("metallic")->GetNumber(), 0.3, 1e-6);
    CHECK_NEAR(mat->Get("roughness")->GetNumber(), 0.7, 1e-6);
}

TEST(SceneMakeEntityErrors) {
    auto noName = scene::SceneFile::MakeEntity("", {0, 0, 0}, {}, {1, 1, 1}, "obj:a.obj", 0, 1);
    CHECK(!noName.Ok());
    CHECK(!noName.Error().empty());

    auto noMesh = scene::SceneFile::MakeEntity("Ghost", {0, 0, 0}, {}, {1, 1, 1}, "", 0, 1);
    CHECK(!noMesh.Ok());
    CHECK(noMesh.Error().find("Ghost") != std::string::npos);
}

// ---------------------------------------------------------------------------
// Round trip: editor-like entities -> MakeEntity -> {"entities": [...]} ->
// JsonWriter -> file -> SceneFile::Parse -> Instantiate -> assert values
// ---------------------------------------------------------------------------

TEST(SceneExportRoundTrip) {
    // Editor-like entities with mixed mesh keys: procedural short keys and
    // file-backed "obj:"/"gltf:" keys (as the editor export produces).
    auto r1 = scene::SceneFile::MakeEntity("地面", {0, 0, 0}, {}, {1, 1, 1}, "terrain",
                                           0.0f, 0.9f);
    auto r2 = scene::SceneFile::MakeEntity(
        "头盔", {3, 0.9f, -2}, math::Quat::FromEuler(0, 1.0f, 0), {1, 1, 1},
        "gltf:assets/models/DamagedHelmet/DamagedHelmet.gltf", 1.0f, 0.2f);
    auto r3 = scene::SceneFile::MakeEntity(
        "松树", {-5, 0, -3}, {}, {1.6f, 1.6f, 1.6f},
        "obj:assets/kenney_nature/Models/OBJ format/tree_pineTallA.obj", 0.0f, 0.8f);
    auto r4 = scene::SceneFile::MakeEntity("方块", {-3, 0.6f, 1}, {}, {1, 1, 1}, "cube",
                                           0.5f, 0.3f);
    CHECK(r1.Ok() && r2.Ok() && r3.Ok() && r4.Ok());

    std::vector<core::Json> entities;
    entities.push_back(r1.Value());
    entities.push_back(r2.Value());
    entities.push_back(r3.Value());
    entities.push_back(r4.Value());

    std::string json = core::JsonWriter::Write(MakeSceneRoot(entities));

    auto parsed = scene::SceneFile::Parse(json);
    CHECK(parsed.Ok());
    const scene::SceneFile& sf = parsed.Value();
    CHECK_EQ(sf.entities.size(), 4u);
    CHECK_EQ(sf.entities[0].name, std::string("地面"));
    CHECK_EQ(sf.entities[1].name, std::string("头盔"));
    CHECK_EQ(sf.entities[2].name, std::string("松树"));
    CHECK_EQ(sf.entities[3].name, std::string("方块"));
    // No "name" component: entity name is the top-level field only.
    CHECK(FindComp(sf.entities[0], "name") == nullptr);

    scene::ComponentRegistry reg;
    scene::RegisterBuiltinComponents(reg); // no assets: any meshKey accepted
    ecs::World world;
    scene::PrefabLibrary prefs;
    auto inst = scene::Instantiate(world, sf, prefs, reg);
    CHECK(inst.Ok());
    CHECK_EQ(inst.Value(), 4);
    CHECK_EQ(world.EntityCount(), 4u);

    auto names = world.ViewAll<scene::SceneName>();
    CHECK_EQ(names.Size(), 4u);
    auto transforms = world.ViewAll<scene::SceneTransform>();
    CHECK_EQ(transforms.Size(), 4u);
    auto meshes = world.ViewAll<scene::SceneMesh>();
    CHECK_EQ(meshes.Size(), 4u);

    // Entities instantiate in scene order; pool order follows creation order.
    ecs::Entity e1 = world.EntityAt<scene::SceneTransform>(0);
    ecs::Entity e2 = world.EntityAt<scene::SceneTransform>(1);
    ecs::Entity e3 = world.EntityAt<scene::SceneTransform>(2);
    ecs::Entity e4 = world.EntityAt<scene::SceneTransform>(3);
    CHECK(e1.IsValid() && e2.IsValid() && e3.IsValid() && e4.IsValid());

    const scene::SceneName* n1 = world.Get<scene::SceneName>(e1);
    CHECK(n1 != nullptr && n1->name == "地面");
    const scene::SceneTransform* t1 = world.Get<scene::SceneTransform>(e1);
    CHECK(t1 != nullptr);
    CHECK_NEAR(t1->pos.x, 0.0, 1e-6);
    CHECK_NEAR(t1->pos.z, 0.0, 1e-6);
    CHECK_NEAR(t1->rot.x, 0.0, 1e-6);
    CHECK_NEAR(t1->rot.w, 1.0, 1e-6);
    CHECK_NEAR(t1->scale.y, 1.0, 1e-6);
    const scene::SceneMesh* m1 = world.Get<scene::SceneMesh>(e1);
    CHECK(m1 != nullptr);
    CHECK_EQ(m1->meshKey, std::string("terrain"));
    CHECK_NEAR(m1->metallic, 0.0, 1e-6);
    CHECK_NEAR(m1->roughness, 0.9, 1e-6);

    const scene::SceneName* n2 = world.Get<scene::SceneName>(e2);
    CHECK(n2 != nullptr && n2->name == "头盔");
    const scene::SceneTransform* t2 = world.Get<scene::SceneTransform>(e2);
    CHECK_NEAR(t2->pos.x, 3.0, 1e-6);
    CHECK_NEAR(t2->pos.y, 0.9, 1e-6);
    CHECK_NEAR(t2->pos.z, -2.0, 1e-6);
    const scene::SceneMesh* m2 = world.Get<scene::SceneMesh>(e2);
    CHECK_EQ(m2->meshKey, std::string("gltf:assets/models/DamagedHelmet/DamagedHelmet.gltf"));
    CHECK_NEAR(m2->metallic, 1.0, 1e-6);
    CHECK_NEAR(m2->roughness, 0.2, 1e-6);

    const scene::SceneName* n3 = world.Get<scene::SceneName>(e3);
    CHECK(n3 != nullptr && n3->name == "松树");
    const scene::SceneTransform* t3 = world.Get<scene::SceneTransform>(e3);
    CHECK_NEAR(t3->pos.x, -5.0, 1e-6);
    CHECK_NEAR(t3->scale.x, 1.6, 1e-6);
    CHECK_NEAR(t3->scale.z, 1.6, 1e-6);
    const scene::SceneMesh* m3 = world.Get<scene::SceneMesh>(e3);
    CHECK_EQ(m3->meshKey,
             std::string("obj:assets/kenney_nature/Models/OBJ format/tree_pineTallA.obj"));
    CHECK_NEAR(m3->roughness, 0.8, 1e-6);

    const scene::SceneName* n4 = world.Get<scene::SceneName>(e4);
    CHECK(n4 != nullptr && n4->name == "方块");
    const scene::SceneTransform* t4 = world.Get<scene::SceneTransform>(e4);
    CHECK_NEAR(t4->pos.z, 1.0, 1e-6);
    const scene::SceneMesh* m4 = world.Get<scene::SceneMesh>(e4);
    CHECK_EQ(m4->meshKey, std::string("cube"));
    CHECK_NEAR(m4->metallic, 0.5, 1e-6);
    CHECK_NEAR(m4->roughness, 0.3, 1e-6);
}

// ---------------------------------------------------------------------------
// File-level round trip: exported JSON written to disk loads back identically
// (mirrors the editor export → playtest load path)
// ---------------------------------------------------------------------------

TEST(SceneExportFileRoundTrip) {
    auto r1 = scene::SceneFile::MakeEntity("Hero", {1, 2, 3}, math::Quat{0, 0, 0, 1}, {1, 1, 1},
                                           "gltf:assets/hero.gltf", 0.4f, 0.6f);
    auto r2 = scene::SceneFile::MakeEntity("Ground", {0, 0, 0}, {}, {10, 1, 10},
                                           "obj:assets/ground.obj", 0.0f, 1.0f);
    CHECK(r1.Ok() && r2.Ok());

    std::vector<core::Json> entities;
    entities.push_back(r1.Value());
    entities.push_back(r2.Value());
    std::string json = core::JsonWriter::Write(MakeSceneRoot(entities));

    test::TempDir dir;
    std::string path = dir.Str() + "\\exported_scene.json";
    CHECK(test::WriteFileAll(path, json));

    std::string loaded;
    CHECK(test::ReadFileAll(path, loaded));
    CHECK_EQ(loaded, json); // byte-identical on disk

    auto parsed = scene::SceneFile::Parse(loaded);
    CHECK(parsed.Ok());
    CHECK_EQ(parsed.Value().entities.size(), 2u);
    CHECK_EQ(parsed.Value().entities[0].name, std::string("Hero"));
    const scene::ComponentDef* tf = FindComp(parsed.Value().entities[0], "transform");
    CHECK(tf != nullptr);
    CHECK_NEAR(tf->data.Get("pos")->At(1)->GetNumber(), 2.0, 1e-9);
    const scene::ComponentDef* mesh = FindComp(parsed.Value().entities[0], "mesh");
    CHECK(mesh != nullptr);
    CHECK_NEAR(mesh->data.Get("material")->Get("metallic")->GetNumber(), 0.4, 1e-6);

    scene::ComponentRegistry reg;
    scene::RegisterBuiltinComponents(reg);
    ecs::World world;
    scene::PrefabLibrary prefs;
    auto inst = scene::Instantiate(world, parsed.Value(), prefs, reg);
    CHECK(inst.Ok());
    CHECK_EQ(inst.Value(), 2);
    CHECK_EQ(world.ViewAll<scene::SceneTransform>().Size(), 2u);
}
