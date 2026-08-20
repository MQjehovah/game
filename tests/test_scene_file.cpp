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

} // namespace

// ---------------------------------------------------------------------------
// Parse a valid componentized scene: entities/components/gameVars
// ---------------------------------------------------------------------------

TEST(SceneFileParseValidScene) {
    const char* json = R"({
        "entities": [
            {
                "name": "Wolf",
                "prefab": "wolf",
                "components": {
                    "transform": {"pos": [1,0,2], "rot": [0,0,0,1], "scale": [1,1,1]},
                    "mesh": {"meshKey": "gltf:assets/wolf.glb"},
                    "health": {"hp": 50, "maxHp": 50},
                    "script": {"backend": "lua", "path": "scripts/wolf.lua", "vars": {"aggro": 10}}
                }
            },
            {
                "name": "Ground",
                "components": {
                    "transform": {"pos": [0,0,0], "scale": [10,1,10]},
                    "mesh": {"meshKey": "obj:assets/ground.obj"}
                }
            }
        ],
        "gameVars": {"gold": 0}
    })";
    auto res = scene::SceneFile::Parse(json);
    CHECK(res.Ok());
    const scene::SceneFile& sf = res.Value();
    CHECK_EQ(sf.entities.size(), 2u);
    CHECK_EQ(sf.entities[0].name, std::string("Wolf"));
    CHECK_EQ(sf.entities[0].prefab, std::string("wolf"));
    CHECK_EQ(sf.entities[0].components.size(), 4u);
    const scene::ComponentDef* health = FindComp(sf.entities[0], "health");
    CHECK(health != nullptr);
    CHECK_NEAR(health->data.Get("hp")->GetNumber(), 50.0, 1e-9);
    CHECK_NEAR(sf.gameVars.Get("gold")->GetNumber(), 0.0, 1e-9);
    CHECK_EQ(sf.entities[1].prefab, std::string(""));
}

TEST(SceneFileParseValidationErrors) {
    CHECK(!scene::SceneFile::Parse(R"({"gameVars": {}})").Ok());           // entities missing
    CHECK(!scene::SceneFile::Parse(R"({"entities": {}})").Ok());           // entities not array
    CHECK(!scene::SceneFile::Parse(R"({"entities": [], "gameVars": 5})").Ok()); // gameVars wrong type
    CHECK(!scene::SceneFile::Parse(R"({"entities": [42]})").Ok());         // entity not object
    CHECK(!scene::SceneFile::Parse(R"({"entities": [{"name": "X"}]})").Ok());  // components missing
    CHECK(!scene::SceneFile::Parse(R"({"entities": [{"components": {"health": 7}}]})").Ok()); // comp data not object
    CHECK(!scene::SceneFile::Parse("this is not json").Ok());              // invalid JSON
    CHECK(!scene::SceneFile::Parse("42").Ok());                            // scalar root
}

TEST(SceneEmptyEntitiesOk) {
    auto res = scene::SceneFile::Parse(R"({"entities": []})");
    CHECK(res.Ok());
    CHECK_EQ(res.Value().entities.size(), 0u);
}

// ---------------------------------------------------------------------------
// Prefab expansion + instance override
// ---------------------------------------------------------------------------

TEST(ScenePrefabExpansionAndInstanceOverride) {
    scene::PrefabLibrary prefs;
    CHECK(prefs.Add("wolf", R"({
        "components": {
            "transform": {"pos": [1,0,2], "rot": [0,0,0,1], "scale": [1,1,1]},
            "health": {"hp": 50, "maxHp": 50},
            "mesh": {"meshKey": "gltf:assets/wolf.glb", "material": {"metallic": 0.0, "roughness": 0.8}}
        }
    })").Ok());

    auto res = scene::SceneFile::Parse(R"({
        "entities": [
            {
                "name": "Wolf",
                "prefab": "wolf",
                "components": {
                    "transform": {"scale": [2,2,2]},
                    "script": {"backend": "lua", "path": "scripts/wolf.lua", "vars": {"aggro": 10}}
                }
            }
        ]
    })");
    CHECK(res.Ok());

    scene::ComponentRegistry reg;
    scene::RegisterBuiltinComponents(reg);
    ecs::World world;
    auto inst = scene::Instantiate(world, res.Value(), prefs, reg);
    CHECK(inst.Ok());
    CHECK_EQ(inst.Value(), 1);
    CHECK_EQ(world.EntityCount(), 1u);

    auto view = world.ViewAll<scene::SceneTransform>();
    CHECK_EQ(view.Size(), 1u);
    ecs::Entity ent = world.EntityAt<scene::SceneTransform>(0);
    CHECK(ent.IsValid());

    const scene::SceneTransform* t = world.Get<scene::SceneTransform>(ent);
    CHECK_NEAR(t->pos.x, 1.0, 1e-6);
    CHECK_NEAR(t->pos.z, 2.0, 1e-6);
    CHECK_NEAR(t->scale.x, 2.0, 1e-6); // instance overrides prefab scale
    CHECK_NEAR(t->scale.z, 2.0, 1e-6);

    const scene::SceneHealth* h = world.Get<scene::SceneHealth>(ent);
    CHECK(h != nullptr);
    CHECK_NEAR(h->hp, 50.0, 1e-6);
    CHECK_NEAR(h->maxHp, 50.0, 1e-6);

    const scene::SceneMesh* m = world.Get<scene::SceneMesh>(ent);
    CHECK(m != nullptr);
    CHECK_EQ(m->meshKey, std::string("gltf:assets/wolf.glb"));
    CHECK_NEAR(m->metallic, 0.0, 1e-6);
    CHECK_NEAR(m->roughness, 0.8, 1e-6);

    const scene::SceneScript* s = world.Get<scene::SceneScript>(ent);
    CHECK(s != nullptr);
    CHECK_EQ(s->backend, std::string("lua"));
    CHECK_EQ(s->path, std::string("scripts/wolf.lua"));
    CHECK_NEAR(s->vars.Get("aggro")->GetNumber(), 10.0, 1e-9);

    const scene::SceneName* n = world.Get<scene::SceneName>(ent);
    CHECK(n != nullptr);
    CHECK_EQ(n->name, std::string("Wolf"));
}

// ---------------------------------------------------------------------------
// Deep merge of nested objects (script vars)
// ---------------------------------------------------------------------------

TEST(SceneDeepMergeNestedVars) {
    // Prefab registered as a bare component map (no "components" wrapper).
    scene::PrefabLibrary prefs;
    CHECK(prefs.Add("wolf", R"({
        "transform": {"pos": [0,0,0], "rot": [0,0,0,1], "scale": [1,1,1]},
        "script": {"backend": "lua", "path": "scripts/wolf.lua", "vars": {"aggro": 5, "color": "red"}}
    })").Ok());

    auto res = scene::SceneFile::Parse(R"({
        "entities": [
            {
                "prefab": "wolf",
                "components": {
                    "script": {"vars": {"aggro": 10}}
                }
            }
        ]
    })");
    CHECK(res.Ok());

    scene::ComponentRegistry reg;
    scene::RegisterBuiltinComponents(reg);
    ecs::World world;
    auto inst = scene::Instantiate(world, res.Value(), prefs, reg);
    CHECK(inst.Ok());

    auto view = world.ViewAll<scene::SceneScript>();
    CHECK_EQ(view.Size(), 1u);
    ecs::Entity ent = world.EntityAt<scene::SceneScript>(0);
    const scene::SceneScript* s = world.Get<scene::SceneScript>(ent);
    CHECK_NEAR(s->vars.Get("aggro")->GetNumber(), 10.0, 1e-9); // instance wins
    CHECK_EQ(s->vars.Get("color")->GetString(), std::string("red")); // prefab kept
}

// ---------------------------------------------------------------------------
// Validation failures
// ---------------------------------------------------------------------------

TEST(SceneMissingTransformErrors) {
    auto res = scene::SceneFile::Parse(R"({
        "entities": [
            {"name": "Ghost", "components": {"health": {"hp": 1, "maxHp": 1}}}
        ]
    })");
    CHECK(res.Ok()); // structural parse is fine; transform is a semantic requirement

    scene::ComponentRegistry reg;
    scene::RegisterBuiltinComponents(reg);
    ecs::World world;
    scene::PrefabLibrary prefs;
    auto inst = scene::Instantiate(world, res.Value(), prefs, reg);
    CHECK(!inst.Ok());
    CHECK(!inst.Error().empty());
    CHECK_EQ(world.EntityCount(), 0u);
}

TEST(SceneUnknownFieldInBuiltinErrors) {
    auto res = scene::SceneFile::Parse(R"({
        "entities": [
            {"components": {"transform": {"pos": [0,0,0], "foo": 1}}}
        ]
    })");
    CHECK(res.Ok());

    scene::ComponentRegistry reg;
    scene::RegisterBuiltinComponents(reg);
    ecs::World world;
    scene::PrefabLibrary prefs;
    auto inst = scene::Instantiate(world, res.Value(), prefs, reg);
    CHECK(!inst.Ok());
    CHECK(!inst.Error().empty());
    CHECK(inst.Error().find("foo") != std::string::npos);
}

TEST(SceneUnknownComponentTypeWarnsAndSkips) {
    auto res = scene::SceneFile::Parse(R"({
        "entities": [
            {
                "components": {
                    "transform": {"pos": [0,0,0]},
                    "hover": {"amplitude": 1.0},
                    "dash": {"cooldown": 2.0}
                }
            }
        ]
    })");
    CHECK(res.Ok());

    scene::ComponentRegistry reg;
    scene::RegisterBuiltinComponents(reg);
    ecs::World world;
    scene::PrefabLibrary prefs;
    auto inst = scene::Instantiate(world, res.Value(), prefs, reg);
    CHECK(inst.Ok()); // unknown component types are non-fatal
    CHECK_EQ(inst.Value(), 1);
    CHECK_EQ(world.EntityCount(), 1u);
    auto view = world.ViewAll<scene::SceneTransform>();
    CHECK_EQ(view.Size(), 1u);
    ecs::Entity ent = world.EntityAt<scene::SceneTransform>(0);
    CHECK(world.Has<scene::SceneTransform>(ent));
}

TEST(ScenePrefabNotFound) {
    auto res = scene::SceneFile::Parse(R"({
        "entities": [
            {"prefab": "ghost", "components": {"transform": {"pos": [0,0,0]}}}
        ]
    })");
    CHECK(res.Ok());

    scene::ComponentRegistry reg;
    scene::RegisterBuiltinComponents(reg);
    ecs::World world;
    scene::PrefabLibrary prefs; // empty library
    auto inst = scene::Instantiate(world, res.Value(), prefs, reg);
    CHECK(!inst.Ok());
    CHECK(!inst.Error().empty());
}

// ---------------------------------------------------------------------------
// Transactional instantiation: failure rolls back all created entities
// ---------------------------------------------------------------------------

TEST(SceneInstantiateTransactional) {
    auto res = scene::SceneFile::Parse(R"({
        "entities": [
            {"name": "Good", "components": {
                "transform": {"pos": [1,1,1]},
                "health": {"hp": 10, "maxHp": 10}
            }},
            {"name": "Bad", "components": {
                "transform": {"pos": [2,2,2], "bogus": 1}
            }}
        ]
    })");
    CHECK(res.Ok());

    scene::ComponentRegistry reg;
    scene::RegisterBuiltinComponents(reg);
    ecs::World world;
    scene::PrefabLibrary prefs;
    auto inst = scene::Instantiate(world, res.Value(), prefs, reg);
    CHECK(!inst.Ok());
    CHECK_EQ(world.EntityCount(), 0u);                       // rolled back
    CHECK_EQ(world.ViewAll<scene::SceneTransform>().Size(), 0u);
    CHECK_EQ(world.ViewAll<scene::SceneHealth>().Size(), 0u);
}

// ---------------------------------------------------------------------------
// Round trip: Parse -> ToJson -> JsonWriter -> Parse
// ---------------------------------------------------------------------------

TEST(SceneParseRoundTrip) {
    const char* json = R"({
        "entities": [
            {
                "name": "Wolf",
                "prefab": "wolf",
                "components": {
                    "transform": {"pos": [1,0,2], "rot": [0,0,0,1], "scale": [2,2,2]},
                    "health": {"hp": 50, "maxHp": 50}
                }
            }
        ],
        "gameVars": {"gold": 0}
    })";
    auto a = scene::SceneFile::Parse(json);
    CHECK(a.Ok());
    auto b = scene::SceneFile::Parse(core::JsonWriter::Write(a.Value().ToJson()));
    CHECK(b.Ok());
    const scene::SceneFile& sf = b.Value();
    CHECK_EQ(sf.entities.size(), 1u);
    CHECK_EQ(sf.entities[0].name, std::string("Wolf"));
    CHECK_EQ(sf.entities[0].prefab, std::string("wolf"));
    const scene::ComponentDef* t = FindComp(sf.entities[0], "transform");
    CHECK(t != nullptr);
    const core::Json* scale = t->data.Get("scale");
    CHECK(scale != nullptr && scale->IsArray() && scale->Size() == 3u);
    CHECK_NEAR(scale->At(0)->GetNumber(), 2.0, 1e-9);
    const scene::ComponentDef* h = FindComp(sf.entities[0], "health");
    CHECK(h != nullptr);
    CHECK_NEAR(h->data.Get("hp")->GetNumber(), 50.0, 1e-9);
    CHECK_NEAR(sf.gameVars.Get("gold")->GetNumber(), 0.0, 1e-9);
}

// ---------------------------------------------------------------------------
// PrefabLibrary basics + mesh key prefix validation (assets mode)
// ---------------------------------------------------------------------------

TEST(ScenePrefabLibraryBasics) {
    scene::PrefabLibrary prefs;
    CHECK(!prefs.Has("wolf"));
    CHECK(!prefs.Add("wolf", "not json {").Ok());
    CHECK(!prefs.Has("wolf"));
    CHECK(prefs.Add("wolf", R"({"components":{"health":{"hp":1,"maxHp":1}}})").Ok());
    CHECK(prefs.Has("wolf"));
    auto g = prefs.Get("wolf");
    CHECK(g.Ok());
    CHECK(g.Value()->Get("health") != nullptr);
    CHECK(!prefs.Get("nope").Ok());
}

TEST(SceneMeshKeyPrefixValidation) {
    scene::PrefabLibrary prefs;
    scene::ComponentRegistry reg;
    assets::AssetManager am; // non-null: mesh keys must use a known loader prefix
    scene::RegisterBuiltinComponents(reg, &am);

    auto okRes = scene::SceneFile::Parse(R"({
        "entities": [
            {"components": {
                "transform": {"pos": [0,0,0]},
                "mesh": {"meshKey": "gltf:assets/wolf.glb"}
            }}
        ]
    })");
    CHECK(okRes.Ok());
    ecs::World world;
    auto okInst = scene::Instantiate(world, okRes.Value(), prefs, reg);
    CHECK(okInst.Ok());

    auto badRes = scene::SceneFile::Parse(R"({
        "entities": [
            {"components": {
                "transform": {"pos": [0,0,0]},
                "mesh": {"meshKey": "ftp://bad"}
            }}
        ]
    })");
    CHECK(badRes.Ok());
    ecs::World world2;
    auto badInst = scene::Instantiate(world2, badRes.Value(), prefs, reg);
    CHECK(!badInst.Ok());
}
