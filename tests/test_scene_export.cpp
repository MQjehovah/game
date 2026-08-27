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
                                            "gltf:assets/cube.gltf", 0.3f, 0.7f,
                                            gfx::Color{1.0f, 0.0f, 0.0f});
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
    CHECK_EQ(mat->Get("colorHex")->GetString(), std::string("#FF0000")); // tint -> #RRGGBB
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
                                           0.0f, 0.9f, gfx::Color::White);
    auto r2 = scene::SceneFile::MakeEntity(
        "头盔", {3, 0.9f, -2}, math::Quat::FromEuler(0, 1.0f, 0), {1, 1, 1},
        "gltf:assets/models/DamagedHelmet/DamagedHelmet.gltf", 1.0f, 0.2f,
        gfx::Color{1.0f, 0.0f, 0.0f});
    auto r3 = scene::SceneFile::MakeEntity(
        "松树", {-5, 0, -3}, {}, {1.6f, 1.6f, 1.6f},
        "obj:assets/kenney_nature/Models/OBJ format/tree_pineTallA.obj", 0.0f, 0.8f,
        gfx::Color::White);
    auto r4 = scene::SceneFile::MakeEntity("方块", {-3, 0.6f, 1}, {}, {1, 1, 1}, "cube",
                                           0.5f, 0.3f, gfx::Color{0.0f, 0.0f, 1.0f});
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
    // Color round-trips into material.colorHex at the JSON level.
    const scene::ComponentDef* mat1 = FindComp(sf.entities[0], "mesh");
    CHECK(mat1 != nullptr);
    CHECK_EQ(mat1->data.Get("material")->Get("colorHex")->GetString(),
             std::string("#FFFFFF"));
    const scene::ComponentDef* mat2 = FindComp(sf.entities[1], "mesh");
    CHECK(mat2 != nullptr);
    CHECK_EQ(mat2->data.Get("material")->Get("colorHex")->GetString(),
             std::string("#FF0000"));
    const scene::ComponentDef* mat4 = FindComp(sf.entities[3], "mesh");
    CHECK(mat4 != nullptr);
    CHECK_EQ(mat4->data.Get("material")->Get("colorHex")->GetString(),
             std::string("#0000FF"));

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
    CHECK_EQ(m1->colorHex, std::string("#FFFFFF"));

    const scene::SceneName* n2 = world.Get<scene::SceneName>(e2);
    CHECK(n2 != nullptr && n2->name == "头盔");
    const scene::SceneTransform* t2 = world.Get<scene::SceneTransform>(e2);
    CHECK_NEAR(t2->pos.x, 3.0, 1e-6);
    CHECK_NEAR(t2->pos.y, 0.9, 1e-6);
    CHECK_NEAR(t2->pos.z, -2.0, 1e-6);
    // Non-identity FromEuler rotation round-trips onto the instantiated quat.
    CHECK_NEAR(t2->rot.y, std::sin(0.5), 1e-4);
    CHECK_NEAR(t2->rot.w, std::cos(0.5), 1e-4);
    CHECK_NEAR(t2->rot.x, 0.0, 1e-4);
    CHECK_NEAR(t2->rot.z, 0.0, 1e-4);
    const scene::SceneMesh* m2 = world.Get<scene::SceneMesh>(e2);
    CHECK_EQ(m2->meshKey, std::string("gltf:assets/models/DamagedHelmet/DamagedHelmet.gltf"));
    CHECK_NEAR(m2->metallic, 1.0, 1e-6);
    CHECK_NEAR(m2->roughness, 0.2, 1e-6);
    CHECK_EQ(m2->colorHex, std::string("#FF0000"));

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
    CHECK_EQ(m3->colorHex, std::string("#FFFFFF"));

    const scene::SceneName* n4 = world.Get<scene::SceneName>(e4);
    CHECK(n4 != nullptr && n4->name == "方块");
    const scene::SceneTransform* t4 = world.Get<scene::SceneTransform>(e4);
    CHECK_NEAR(t4->pos.z, 1.0, 1e-6);
    const scene::SceneMesh* m4 = world.Get<scene::SceneMesh>(e4);
    CHECK_EQ(m4->meshKey, std::string("cube"));
    CHECK_NEAR(m4->metallic, 0.5, 1e-6);
    CHECK_NEAR(m4->roughness, 0.3, 1e-6);
    CHECK_EQ(m4->colorHex, std::string("#0000FF"));
}

// ---------------------------------------------------------------------------
// File-level round trip: exported JSON written to disk loads back identically
// (mirrors the editor export → playtest load path)
// ---------------------------------------------------------------------------

TEST(SceneExportFileRoundTrip) {
    auto r1 = scene::SceneFile::MakeEntity("Hero", {1, 2, 3}, math::Quat{0, 0, 0, 1}, {1, 1, 1},
                                           "gltf:assets/hero.gltf", 0.4f, 0.6f,
                                           gfx::Color{0.0f, 1.0f, 0.0f});
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
    CHECK_EQ(mesh->data.Get("material")->Get("colorHex")->GetString(),
             std::string("#00FF00"));

    scene::ComponentRegistry reg;
    scene::RegisterBuiltinComponents(reg);
    ecs::World world;
    scene::PrefabLibrary prefs;
    auto inst = scene::Instantiate(world, parsed.Value(), prefs, reg);
    CHECK(inst.Ok());
    CHECK_EQ(inst.Value(), 2);
    CHECK_EQ(world.ViewAll<scene::SceneTransform>().Size(), 2u);
    ecs::Entity hero = world.EntityAt<scene::SceneTransform>(0);
    const scene::SceneMesh* hm = world.Get<scene::SceneMesh>(hero);
    CHECK(hm != nullptr);
    CHECK_EQ(hm->colorHex, std::string("#00FF00"));
}

// ---------------------------------------------------------------------------
// Material editor textures: the four PBR texture paths + AO / emissive
// intensity persist through MakeEntity -> Parse -> Instantiate
// ---------------------------------------------------------------------------

TEST(SceneMaterialTextureRoundTrip) {
    auto res = scene::SceneFile::MakeEntity(
        "Hero", {0, 0, 0}, {}, {1, 1, 1}, "cube", 0.5f, 0.4f, gfx::Color::White,
        "assets/albedo.png", "assets/mr.png", "assets/ao.png", "assets/emissive.png",
        0.7f, 2.0f);
    CHECK(res.Ok());

    const core::Json* mat = res.Value().Get("components")->Get("mesh")->Get("material");
    CHECK(mat != nullptr);
    const core::Json* alb = mat->Get("albedoTex");
    const core::Json* mr = mat->Get("mrTex");
    const core::Json* aoTex = mat->Get("aoTex");
    const core::Json* emiTex = mat->Get("emissiveTex");
    CHECK(alb != nullptr && mr != nullptr && aoTex != nullptr && emiTex != nullptr);
    if (alb) CHECK_EQ(alb->GetString(), std::string("assets/albedo.png"));
    if (mr) CHECK_EQ(mr->GetString(), std::string("assets/mr.png"));
    if (aoTex) CHECK_EQ(aoTex->GetString(), std::string("assets/ao.png"));
    if (emiTex) CHECK_EQ(emiTex->GetString(), std::string("assets/emissive.png"));
    CHECK_NEAR(mat->Get("ao")->GetNumber(), 0.7, 1e-6);
    CHECK_NEAR(mat->Get("emissiveIntensity")->GetNumber(), 2.0, 1e-6);
    CHECK_NEAR(mat->Get("metallic")->GetNumber(), 0.5, 1e-6);
    CHECK_NEAR(mat->Get("roughness")->GetNumber(), 0.4, 1e-6);

    // Empty texture paths are omitted from the JSON (optional fields).
    auto noTex = scene::SceneFile::MakeEntity("Plain", {0, 0, 0}, {}, {1, 1, 1}, "cube");
    CHECK(noTex.Ok());
    const core::Json* mat2 = noTex.Value().Get("components")->Get("mesh")->Get("material");
    CHECK(mat2 != nullptr);
    CHECK(mat2->Get("albedoTex") == nullptr);
    CHECK(mat2->Get("mrTex") == nullptr);
    CHECK_NEAR(mat2->Get("ao")->GetNumber(), 1.0, 1e-6); // defaults still written

    // Full round trip through the componentized JSON.
    std::vector<core::Json> ents;
    ents.push_back(res.Value());
    std::string json = core::JsonWriter::Write(MakeSceneRoot(ents));
    auto parsed = scene::SceneFile::Parse(json);
    CHECK(parsed.Ok());

    scene::ComponentRegistry reg;
    scene::RegisterBuiltinComponents(reg); // no assets: any meshKey accepted
    ecs::World world;
    scene::PrefabLibrary prefs;
    auto inst = scene::Instantiate(world, parsed.Value(), prefs, reg);
    CHECK(inst.Ok());
    CHECK_EQ(inst.Value(), 1);
    auto view = world.ViewAll<scene::SceneMesh>();
    CHECK_EQ(view.Size(), 1u);
    ecs::Entity ent = world.EntityAt<scene::SceneMesh>(0);
    const scene::SceneMesh* m = world.Get<scene::SceneMesh>(ent);
    CHECK(m != nullptr);
    CHECK_EQ(m->albedoTex, std::string("assets/albedo.png"));
    CHECK_EQ(m->mrTex, std::string("assets/mr.png"));
    CHECK_EQ(m->aoTex, std::string("assets/ao.png"));
    CHECK_EQ(m->emissiveTex, std::string("assets/emissive.png"));
    CHECK_NEAR(m->ao, 0.7, 1e-6);
    CHECK_NEAR(m->emissiveIntensity, 2.0, 1e-6);
    CHECK_NEAR(m->metallic, 0.5, 1e-6);
    CHECK_NEAR(m->roughness, 0.4, 1e-6);
    CHECK_EQ(m->colorHex, std::string("#FFFFFF"));
}

// ---------------------------------------------------------------------------
// Script component (T4.5): MakeEntity emits {"backend","path","vars"} when a
// script is attached, and the round trip restores a SceneScript with them.
// ---------------------------------------------------------------------------

TEST(SceneMakeEntityScriptComponent) {
    core::Json vars;
    vars.type_ = core::Json::Type::Object;
    core::Json aggro;
    aggro.type_ = core::Json::Type::Number;
    aggro.number_ = 10.0;
    vars.object_["aggro"] = aggro;

    // With an attached script: the entity JSON carries the script component.
    auto res = scene::SceneFile::MakeEntity(
        "狼", {0, 0, 0}, {}, {1, 1, 1}, "cube", 0.0f, 0.8f, gfx::Color::White,
        "", "", "", "", 1.0f, 1.0f, "scripts/wolf.lua", "lua", vars);
    CHECK(res.Ok());
    const core::Json* comps = res.Value().Get("components");
    CHECK(comps != nullptr);
    const core::Json* script = comps->Get("script");
    CHECK(script != nullptr && script->IsObject());
    CHECK_EQ(script->Get("backend")->GetString(), std::string("lua"));
    CHECK_EQ(script->Get("path")->GetString(), std::string("scripts/wolf.lua"));
    CHECK_NEAR(script->Get("vars")->Get("aggro")->GetNumber(), 10.0, 1e-9);

    // Without a script path: no script component is emitted.
    auto none = scene::SceneFile::MakeEntity("方块", {0, 0, 0}, {}, {1, 1, 1}, "cube");
    CHECK(none.Ok());
    CHECK(none.Value().Get("components")->Get("script") == nullptr);

    // An empty backend defaults to "lua" (the factory's contract).
    auto defBackend = scene::SceneFile::MakeEntity("空", {0, 0, 0}, {}, {1, 1, 1}, "cube",
                                                   0.0f, 0.8f, gfx::Color::White, "", "", "",
                                                   "", 1.0f, 1.0f, "scripts/x.lua");
    CHECK(defBackend.Ok());
    CHECK_EQ(defBackend.Value().Get("components")->Get("script")->Get("backend")->GetString(),
             std::string("lua"));
    // A non-object vars value is omitted from the JSON (null default).
    auto nullVars = scene::SceneFile::MakeEntity("零", {0, 0, 0}, {}, {1, 1, 1}, "cube",
                                                 0.0f, 0.8f, gfx::Color::White, "", "", "",
                                                 "", 1.0f, 1.0f, "scripts/y.lua", "lua");
    CHECK(nullVars.Ok());
    CHECK(nullVars.Value().Get("components")->Get("script")->Get("vars") == nullptr);

    // Full round trip: export JSON -> Parse -> Instantiate -> SceneScript.
    std::vector<core::Json> ents;
    ents.push_back(res.Value());
    std::string json = core::JsonWriter::Write(MakeSceneRoot(ents));
    auto parsed = scene::SceneFile::Parse(json);
    CHECK(parsed.Ok());
    const scene::ComponentDef* sc = FindComp(parsed.Value().entities[0], "script");
    CHECK(sc != nullptr);
    CHECK_EQ(sc->data.Get("backend")->GetString(), std::string("lua"));
    CHECK_EQ(sc->data.Get("path")->GetString(), std::string("scripts/wolf.lua"));

    scene::ComponentRegistry reg;
    scene::RegisterBuiltinComponents(reg);
    ecs::World world;
    scene::PrefabLibrary prefs;
    auto inst = scene::Instantiate(world, parsed.Value(), prefs, reg);
    CHECK(inst.Ok());
    CHECK_EQ(inst.Value(), 1);
    auto sview = world.ViewAll<scene::SceneScript>();
    CHECK_EQ(sview.Size(), 1u);
    ecs::Entity ent = world.EntityAt<scene::SceneScript>(0);
    const scene::SceneScript* s = world.Get<scene::SceneScript>(ent);
    CHECK(s != nullptr);
    CHECK_EQ(s->backend, std::string("lua"));
    CHECK_EQ(s->path, std::string("scripts/wolf.lua"));
    CHECK(s->vars.IsObject());
    CHECK_NEAR(s->vars.Get("aggro")->GetNumber(), 10.0, 1e-9);
}

// G2-2 scene unification: the canonical sprite builder (used by the editor's
// BuildPlaySceneJson for 2D entities) round-trips through Parse + Instantiate
// with sprite, transform, parent and health all preserved — the regression net
// for the historical "sprite export drops health" drift.
TEST(SceneSpriteMakeEntityRoundTrip) {
    auto r = scene::SceneFile::MakeSpriteEntity(
        "Plant", {3, 4, 0}, math::Quat{0, 0, 0, 1}, {2, 2, 1},
        "assets/sprites/sunflower.png", /*flipX=*/true, /*flipY=*/false, "#ff00aa",
        /*hp=*/80, /*maxHp=*/80, /*parent=*/"", /*parentId=*/0, /*id=*/7);
    CHECK(r.Ok());
    if (!r.Ok()) return;

    // Byte shape: sprite + health components present.
    const core::Json& e = r.Value();
    CHECK_EQ(e.Get("name")->GetString(), std::string("Plant"));
    CHECK_EQ(e.Get("id")->GetNumber(), 7.0);
    const core::Json* comps = e.Get("components");
    CHECK(comps->Get("sprite") != nullptr);
    CHECK(comps->Get("sprite")->Get("flipX")->GetBool());
    CHECK(comps->Get("sprite")->Get("colorHex")->GetString() == "#ff00aa");
    CHECK(comps->Get("health") != nullptr);
    CHECK_NEAR(comps->Get("health")->Get("maxHp")->GetNumber(), 80.0, 1e-9);

    std::vector<core::Json> entities;
    entities.push_back(r.Value());
    auto parsed = scene::SceneFile::Parse(core::JsonWriter::Write(MakeSceneRoot(entities)));
    CHECK(parsed.Ok());
    CHECK_EQ(parsed.Value().entities.size(), 1u);
    CHECK_EQ(parsed.Value().entities[0].name, std::string("Plant"));

    // Instantiate -> the ECS carries SceneSprite + SceneHealth + SceneTransform.
    scene::ComponentRegistry reg;
    scene::RegisterBuiltinComponents(reg);
    ecs::World world;
    scene::PrefabLibrary prefs;
    auto inst = scene::Instantiate(world, parsed.Value(), prefs, reg);
    CHECK(inst.Ok());
    CHECK_EQ(inst.Value(), 1);
    auto sview = world.ViewAll<scene::SceneSprite>();
    CHECK_EQ(sview.Size(), 1u);
    ecs::Entity ent = world.EntityAt<scene::SceneSprite>(0);
    const scene::SceneSprite* s = world.Get<scene::SceneSprite>(ent);
    CHECK(s != nullptr);
    CHECK_EQ(s->texture, std::string("assets/sprites/sunflower.png"));
    CHECK(s->flipX);
    CHECK(!s->flipY);
    CHECK_EQ(s->colorHex, std::string("#ff00aa"));
    const scene::SceneHealth* h = world.Get<scene::SceneHealth>(ent);
    CHECK(h != nullptr);
    if (h) CHECK_NEAR(h->maxHp, 80.0f, 1e-5f);
    const scene::SceneTransform* t = world.Get<scene::SceneTransform>(ent);
    CHECK(t != nullptr);
    if (t) {
        CHECK_NEAR(t->pos.x, 3.0f, 1e-5f);
        CHECK_NEAR(t->pos.y, 4.0f, 1e-5f);
    }

    // No health -> no SceneHealth component (optional field).
    auto plain = scene::SceneFile::MakeSpriteEntity("Plain", {0, 0, 0}, {}, {1, 1, 1},
                                                    "assets/sprites/a.png");
    CHECK(plain.Ok());
    std::vector<core::Json> plainEnts;
    plainEnts.push_back(plain.Value());
    auto plainParsed =
        scene::SceneFile::Parse(core::JsonWriter::Write(MakeSceneRoot(plainEnts)));
    CHECK(plainParsed.Ok());
    scene::ComponentRegistry reg2;
    scene::RegisterBuiltinComponents(reg2);
    ecs::World world2;
    auto inst2 = scene::Instantiate(world2, plainParsed.Value(), prefs, reg2);
    CHECK(inst2.Ok());
    CHECK_EQ(world2.ViewAll<scene::SceneHealth>().Size(), 0u);
}

// G2-2: SceneFile::FromWorld is the reverse of Instantiate — a World
// serializes back to the scene-file format, and re-parsing + re-instantiating
// yields an equivalent World (components + stable ids preserved). This is what
// lets the editor generate output from the runtime World it hosts.
TEST(SceneFromWorldRoundTrip) {
    // A scene exercising many factories + a generic component + a parent link.
    const char* json = R"({"entities":[
      {"name":"Root","id":1,"components":{
        "transform":{"pos":[1,2,3],"rot":[0,0,0,1],"scale":[1,1,1]}}},
      {"name":"Plant","id":2,"components":{
        "transform":{"pos":[4,0,0],"parentId":1},
        "sprite":{"texture":"assets/a.png","flipX":true,"colorHex":"#00ff88"},
        "health":{"hp":80,"maxHp":80},
        "plant":{"type":"sunflower","row":2,"cost":50},
        "audio":{"sound":"waterfall","volume":0.5,"radius":20},
        "script":{"backend":"lua","path":"scripts/a.lua","vars":{"x":1}}}},
      {"name":"Hero","id":3,"components":{
        "transform":{"pos":[0,5,0]},
        "mesh":{"meshKey":"obj:a.obj","material":{"metallic":0.5,"roughness":0.2}},
        "rigidbody":{"shape":"box","halfExtents":[0.5,1,0.5],"dynamic":true},
        "groups":{"groups":["player","respawn"]},
        "type":{"value":"CharacterBody"}}},
      {"name":"Cam","id":4,"components":{
        "transform":{"pos":[0,0,10]},
        "camera":{"fov":70,"ortho":true,"orthoSize":12},
        "sortOrder":{"z":5}}},
      {"name":"L","id":5,"components":{
        "transform":{"pos":[0,0,0]},
        "light":{"type":"ambient","color":[1,0.9,0.8,1],"ambientStrength":1.5},
        "decal":{"texture":"assets/d.png","size":3,"alpha":0.6}}}
    ]})";
    auto parsed = scene::SceneFile::Parse(json);
    if (!parsed.Ok()) {
        std::printf("FromWorld test parse error: %s\n", parsed.Error().c_str());
        CHECK(false);
        return;
    }

    scene::ComponentRegistry reg;
    scene::RegisterBuiltinComponents(reg);
    ecs::World w1;
    scene::PrefabLibrary prefs;
    CHECK(scene::Instantiate(w1, parsed.Value(), prefs, reg).Ok());

    // World -> JSON -> Parse -> World.
    auto out = scene::SceneFile::FromWorld(w1);
    CHECK(out.Ok());
    if (!out.Ok()) return;
    const std::string outText = core::JsonWriter::Write(out.Value());
    auto reparsed = scene::SceneFile::Parse(outText);
    if (!reparsed.Ok()) {
        std::printf("FromWorld output reparse error: %s\n---- output ----\n%s\n---- end ----\n",
                    reparsed.Error().c_str(), outText.c_str());
        CHECK(false);
        return;
    }
    CHECK_EQ(reparsed.Value().entities.size(), parsed.Value().entities.size());

    ecs::World w2;
    CHECK(scene::Instantiate(w2, reparsed.Value(), prefs, reg).Ok());
    CHECK_EQ(w2.ViewAll<scene::SceneTransform>().Size(), 5u);
    CHECK_EQ(w2.ViewAll<scene::SceneSprite>().Size(), 1u);
    CHECK_EQ(w2.ViewAll<scene::SceneHealth>().Size(), 1u);
    CHECK_EQ(w2.ViewAll<scene::SceneRigidBody>().Size(), 1u);
    CHECK_EQ(w2.ViewAll<scene::SceneCamera>().Size(), 1u);
    CHECK_EQ(w2.ViewAll<scene::SceneLight>().Size(), 1u);
    CHECK_EQ(w2.ViewAll<scene::SceneSortOrder>().Size(), 1u);
    CHECK_EQ(w2.ViewAll<scene::SceneDecal>().Size(), 1u);
    CHECK_EQ(w2.ViewAll<scene::SceneAudioSource>().Size(), 1u);

    // Sprite entity: sprite + health + generic plant data all survive.
    auto sview = w2.ViewAll<scene::SceneSprite>();
    ecs::Entity plant = w2.EntityAt<scene::SceneSprite>(0);
    const scene::SceneSprite* sp = w2.Get<scene::SceneSprite>(plant);
    CHECK(sp && sp->flipX && sp->colorHex == "#00ff88");
    const scene::SceneHealth* h = w2.Get<scene::SceneHealth>(plant);
    CHECK(h && h->maxHp == 80.0f);
    const scene::SceneData* sd = w2.Get<scene::SceneData>(plant);
    bool plantData = false;
    if (sd)
        for (const auto& kv : sd->components)
            if (kv.first == "plant" && kv.second.Get("type") &&
                kv.second.Get("type")->GetString() == "sunflower")
                plantData = true;
    CHECK(plantData);

    // Stable ids: the original ids survive the round-trip.
    bool sawIds = false;
    auto nview = w2.ViewAll<scene::SceneId>();
    CHECK_EQ(nview.Size(), 5u);
    for (size_t i = 0; i < nview.Size(); ++i) {
        const scene::SceneId* id = w2.Get<scene::SceneId>(w2.EntityAt<scene::SceneId>(i));
        if (id && id->id == 1) sawIds = true;
    }
    CHECK(sawIds);
}

// G5-4: hierarchy is ENTITY-LEVEL (parentId beside id/name, not in the
// transform component). The new format round-trips through Instantiate ->
// SceneParentLink -> FromWorld (top-level parentId) -> re-Instantiate with the
// parent link intact.
TEST(SceneEntityLevelHierarchyRoundTrip) {
    const char* json = R"({"entities":[
      {"name":"Root","id":1,"components":{"transform":{"pos":[0,0,0]}}},
      {"name":"Child","id":2,"parentId":1,
       "components":{"transform":{"pos":[5,0,0]}}},
      {"name":"Grand","id":3,"parentId":2,
       "components":{"transform":{"pos":[10,0,0]}}}
    ]})";
    auto parsed = scene::SceneFile::Parse(json);
    CHECK(parsed.Ok());
    if (!parsed.Ok()) return;

    scene::ComponentRegistry reg;
    scene::RegisterBuiltinComponents(reg);
    ecs::World w1;
    scene::PrefabLibrary prefs;
    CHECK(scene::Instantiate(w1, parsed.Value(), prefs, reg).Ok());
    // Parent links resolved from the entity-level parentId.
    CHECK_EQ(w1.ViewAll<scene::SceneParentLink>().Size(), 2u);
    CHECK_EQ(w1.ViewAll<scene::SceneTransform>().Size(), 3u);

    // FromWorld emits parentId at the TOP level (derived from SceneParentLink).
    auto out = scene::SceneFile::FromWorld(w1);
    CHECK(out.Ok());
    if (!out.Ok()) return;
    const core::Json* outText = out.Value().Get("entities");
    bool sawTopLevelParent = false;
    for (const core::Json& e : outText->Items()) {
        if (e.Get("parentId") && e.Get("parentId")->GetNumber() == 1.0) sawTopLevelParent = true;
        // No parentId inside the transform component.
        if (const core::Json* comps = e.Get("components"))
            if (comps->Get("transform") && comps->Get("transform")->Get("parentId"))
                sawTopLevelParent = false;
    }
    CHECK(sawTopLevelParent);

    // Re-parse + re-instantiate: the parent chain survives.
    auto reparsed = scene::SceneFile::Parse(core::JsonWriter::Write(out.Value()));
    CHECK(reparsed.Ok());
    ecs::World w2;
    CHECK(scene::Instantiate(w2, reparsed.Value(), prefs, reg).Ok());
    CHECK_EQ(w2.ViewAll<scene::SceneParentLink>().Size(), 2u);
}
