#include <string>
#include <vector>

#include "neon/neon.hpp"
#include "neon/scene/component_schema.hpp"
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
// Rigidbody component: invalid/empty shape falls back to sphere (editor added
// the component with an empty enum default in older builds); valid shapes pass
// through; the registered schema's first option is "sphere".
// ---------------------------------------------------------------------------

TEST(SceneRigidBodyShapeTolerance) {
    scene::ComponentRegistry reg;
    scene::RegisterBuiltinComponents(reg);
    const char* json = R"({
        "entities": [
            {
                "name": "EmptyShape",
                "components": {
                    "transform": {"pos": [0,0,0]},
                    "rigidbody": {"shape": "", "radius": 0.5}
                }
            },
            {
                "name": "BoxShape",
                "components": {
                    "transform": {"pos": [1,0,0]},
                    "rigidbody": {"shape": "box", "halfExtents": [0.5,0.5,0.5]}
                }
            }
        ]
    })";
    auto res = scene::SceneFile::Parse(json);
    CHECK(res.Ok());
    scene::PrefabLibrary prefs;
    ecs::World world;
    auto inst = scene::Instantiate(world, res.Value(), prefs, reg);
    CHECK(inst.Ok());
    CHECK_EQ(inst.Value(), 2);

    const scene::SceneRigidBody* rb0 =
        world.Get<scene::SceneRigidBody>(world.EntityAt<scene::SceneRigidBody>(0));
    CHECK(rb0 != nullptr);
    CHECK_EQ(rb0->shape, std::string("sphere")); // invalid shape defaulted
    const scene::SceneRigidBody* rb1 =
        world.Get<scene::SceneRigidBody>(world.EntityAt<scene::SceneRigidBody>(1));
    CHECK(rb1 != nullptr);
    CHECK_EQ(rb1->shape, std::string("box")); // valid shape preserved
}

// G8-3: the "audio" component parses into a SceneAudioSource (sound/volume/
// radius) and round-trips through ToJson; unknown fields are rejected.
TEST(SceneAudioSourceParse) {
    scene::ComponentRegistry reg;
    scene::RegisterBuiltinComponents(reg);
    const char* json = R"({
        "entities": [
            {
                "name": "Ambience",
                "components": {
                    "transform": {"pos": [1, 2, 3]},
                    "audio": {"sound": "waterfall", "volume": 0.8, "radius": 25}
                }
            }
        ]
    })";
    auto res = scene::SceneFile::Parse(json);
    CHECK(res.Ok());
    scene::PrefabLibrary prefs;
    ecs::World world;
    auto inst = scene::Instantiate(world, res.Value(), prefs, reg);
    CHECK(inst.Ok());
    CHECK_EQ(inst.Value(), 1);

    const scene::SceneAudioSource* a =
        world.Get<scene::SceneAudioSource>(world.EntityAt<scene::SceneAudioSource>(0));
    CHECK(a != nullptr);
    if (!a) return;
    CHECK_EQ(a->sound, std::string("waterfall"));
    CHECK_NEAR(a->volume, 0.8f, 1e-6);
    CHECK_NEAR(a->radius, 25.0f, 1e-6);

    // ToJson round-trips the component.
    core::Json out = res.Value().ToJson();
    const core::Json* au = out.Get("entities")->At(0)->Get("components")->Get("audio");
    CHECK(au != nullptr);
    CHECK_EQ(au->Get("sound")->GetString(), std::string("waterfall"));

    // Unknown field rejected at Instantiate.
    const char* bad = R"({"entities":[{"name":"B",
        "components":{"transform":{"pos":[0,0,0]},"audio":{"sound":"x","warp":9}}}]})";
    auto badRes = scene::SceneFile::Parse(bad);
    CHECK(badRes.Ok());
    ecs::World badWorld;
    auto badInst = scene::Instantiate(badWorld, badRes.Value(), prefs, reg);
    CHECK(!badInst.Ok());
}

TEST(SceneSpriteComponentRoundTrip) {
    // A 2D sprite: image texture on an XY quad. Parse -> Instantiate -> ECS
    // component -> ToJson round-trips the flip flags and tint, and invalid
    // shapes (missing texture / wrong types) are rejected.
    scene::ComponentRegistry reg;
    scene::RegisterBuiltinComponents(reg);
    const char* json = R"({
        "entities": [
            {
                "name": "Plant",
                "components": {
                    "transform": {"pos": [3,4,0], "scale": [2,2,1]},
                    "sprite": {"texture": "assets/textures/plant.png",
                               "flipX": true, "flipY": false,
                               "colorHex": "#ff00aa"}
                }
            },
            {
                "name": "Plain",
                "components": {
                    "transform": {"pos": [0,0,0]},
                    "sprite": {"texture": "assets/textures/bg.png"}
                }
            }
        ]
    })";
    auto res = scene::SceneFile::Parse(json);
    CHECK(res.Ok());
    scene::PrefabLibrary prefs;
    ecs::World world;
    auto inst = scene::Instantiate(world, res.Value(), prefs, reg);
    CHECK(inst.Ok());
    CHECK_EQ(inst.Value(), 2);

    const scene::SceneSprite* s0 =
        world.Get<scene::SceneSprite>(world.EntityAt<scene::SceneSprite>(0));
    CHECK(s0 != nullptr);
    CHECK_EQ(s0->texture, std::string("assets/textures/plant.png"));
    CHECK(s0->flipX);
    CHECK(!s0->flipY);
    CHECK_EQ(s0->colorHex, std::string("#ff00aa"));

    // ToJson keeps the sprite component intact.
    core::Json out = res.Value().ToJson();
    const core::Json* ent = out.Get("entities")->At(0);
    const core::Json* sp = ent->Get("components")->Get("sprite");
    CHECK(sp != nullptr);
    CHECK_EQ(sp->Get("texture")->GetString(), std::string("assets/textures/plant.png"));
    CHECK(sp->Get("flipX")->GetBool());

    // Missing texture / non-string texture fail at Instantiate (the factory
    // validates component content; Parse only checks the JSON shape).
    const char* badJson1 = R"({"entities":[{"name":"B",
        "components":{"transform":{"pos":[0,0,0]},"sprite":{}}}]})";
    const char* badJson2 = R"({"entities":[{"name":"B",
        "components":{"transform":{"pos":[0,0,0]},"sprite":{"texture":7}}}]})";
    for (const char* bad : {badJson1, badJson2}) {
        auto badRes = scene::SceneFile::Parse(bad);
        CHECK(badRes.Ok()); // shape parses
        scene::PrefabLibrary badPrefs;
        ecs::World badWorld;
        auto badInst = scene::Instantiate(badWorld, badRes.Value(), badPrefs, reg);
        CHECK(!badInst.Ok()); // factory rejects the component
    }
}

TEST(SceneRigidBodySchemaDefaultOption) {
    // The inspector's 添加组件 button uses the schema's first enum option as
    // the default, so a freshly added rigidbody is "sphere" and plays.
    const scene::ComponentSchema* s = scene::FindComponentSchema("rigidbody");
    CHECK(s != nullptr);
    if (!s) return;
    bool foundShape = false;
    for (const scene::FieldSchema& f : s->fields) {
        if (f.key == "shape" && f.type == scene::FieldType::Enum) {
            foundShape = true;
            CHECK(f.optionCount > 0);
            CHECK_EQ(std::string(f.options[0]), std::string("sphere"));
        }
    }
    CHECK(foundShape);
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
    CHECK_EQ(world.ViewAll<scene::SceneName>().Size(), 0u);  // auto-added name cleared
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
// Chinese (UTF-8) entity names survive Parse -> ToJson -> Write -> Parse
// ---------------------------------------------------------------------------

TEST(SceneCjkNameRoundTrip) {
    // "地面" as raw UTF-8 bytes: E5 9C B0 E9 9D A2.
    const std::string ground = std::string("\xE5\x9C\xB0\xE9\x9D\xA2");
    const std::string json =
        "{\"entities\":[{\"name\":\"" + ground + "\",\"components\":{"
        "\"transform\":{\"pos\":[0,0,0],\"scale\":[1,1,1]},"
        "\"mesh\":{\"meshKey\":\"terrain\"}}}]}";
    auto a = scene::SceneFile::Parse(json);
    CHECK(a.Ok());
    CHECK_EQ(a.Value().entities[0].name, ground);

    const std::string written = core::JsonWriter::Write(a.Value().ToJson());
    // The writer must emit the raw UTF-8 bytes, never \u-escaped Latin-1 lookalikes.
    CHECK(written.find("\xE5\x9C\xB0\xE9\x9D\xA2") != std::string::npos);

    auto b = scene::SceneFile::Parse(written);
    CHECK(b.Ok());
    CHECK_EQ(b.Value().entities[0].name, ground);
}

// ---------------------------------------------------------------------------
// Scene-level "level" data (2D levels embedded in scenes/*.json) round trips
// ---------------------------------------------------------------------------

TEST(SceneLevelDataRoundTrip) {
    const char* json = R"({
        "entities": [
            {
                "name": "PvZ",
                "components": {
                    "transform": {"pos": [0,0,0]},
                    "script": {"backend": "lua", "path": "scripts/pvz.lua"}
                }
            }
        ],
        "level": {
            "plants": [{"row": 4, "col": 1, "plant": "sunflower"}],
            "zombies": [{"row": 2, "delay": 8, "type": "basic"}]
        }
    })";
    auto a = scene::SceneFile::Parse(json);
    CHECK(a.Ok());
    CHECK(a.Value().level.IsObject());
    CHECK_EQ(a.Value().level.Get("plants")->At(0)->Get("plant")->GetString(),
             std::string("sunflower"));

    const std::string written = core::JsonWriter::Write(a.Value().ToJson());
    CHECK(written.find("\"level\"") != std::string::npos);

    auto b = scene::SceneFile::Parse(written);
    CHECK(b.Ok());
    CHECK(b.Value().level.IsObject());
    CHECK_EQ(b.Value().level.Get("plants")->Size(), 1u);
    CHECK_EQ(b.Value().level.Get("zombies")->At(0)->Get("delay")->GetNumber(), 8.0);
    CHECK_EQ(b.Value().entities[0].name, std::string("PvZ"));
}

// ---------------------------------------------------------------------------
// Component schemas (inspector metadata for arbitrary components)
// ---------------------------------------------------------------------------

TEST(ComponentSchemaRegistry) {
    scene::RegisterBuiltinComponentSchemas();
    const scene::ComponentSchema* plant = scene::FindComponentSchema("plant");
    CHECK(plant != nullptr);
    CHECK_EQ(plant->label, std::string("植物"));
    CHECK(plant->fields.size() >= 3u);
    bool hasRow = false, hasType = false;
    for (const scene::FieldSchema& f : plant->fields) {
        if (f.key == "row") {
            hasRow = true;
            CHECK(f.type == scene::FieldType::Int);
            CHECK_EQ(f.min, 0.0);
            CHECK_EQ(f.max, 4.0);
        }
        if (f.key == "type") {
            hasType = true;
            CHECK(f.type == scene::FieldType::Enum);
            CHECK(f.optionCount == 5);
        }
    }
    CHECK(hasRow);
    CHECK(hasType);

    // Unknown components have no schema (inspector falls back to raw JSON).
    CHECK(scene::FindComponentSchema("does_not_exist") == nullptr);
    // Built-ins are registered too.
    CHECK(scene::FindComponentSchema("transform") != nullptr);
    CHECK(scene::FindComponentSchema("mesh") != nullptr);
    CHECK(scene::FindComponentSchema("health") != nullptr);
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

// ---------------------------------------------------------------------------
// Prefab rejects non-object component values; stray top-level keys rejected
// ---------------------------------------------------------------------------

TEST(ScenePrefabRejectsNonObjectComponent) {
    scene::PrefabLibrary prefs;
    // "components" wrapper form
    CHECK(!prefs.Add("bad1", R"({"components": {"transform": 5}})").Ok());
    CHECK(!prefs.Has("bad1"));
    // bare component-map form
    CHECK(!prefs.Add("bad2", R"({"transform": 5})").Ok());
    CHECK(!prefs.Has("bad2"));
    // stray top-level key next to "components" (catches "Components" typos)
    CHECK(!prefs.Add("bad3", R"({"Components": {}, "components": {}})").Ok());
    CHECK(!prefs.Has("bad3"));

    // A scene referencing a prefab that failed to load errors cleanly.
    auto res = scene::SceneFile::Parse(R"({
        "entities": [
            {"prefab": "bad1", "components": {"transform": {"pos": [0,0,0]}}}
        ]
    })");
    CHECK(res.Ok());
    scene::ComponentRegistry reg;
    scene::RegisterBuiltinComponents(reg);
    ecs::World world;
    auto inst = scene::Instantiate(world, res.Value(), prefs, reg);
    CHECK(!inst.Ok());
    CHECK(!inst.Error().empty());
    CHECK_EQ(world.EntityCount(), 0u);
}

// ---------------------------------------------------------------------------
// behaviorTree + mesh defaults
// ---------------------------------------------------------------------------

TEST(SceneBehaviorTreeComponent) {
    auto res = scene::SceneFile::Parse(R"({
        "entities": [
            {"components": {
                "transform": {"pos": [0,0,0]},
                "behaviorTree": {"tree": "bt:wolf_ai"}
            }}
        ]
    })");
    CHECK(res.Ok());

    scene::ComponentRegistry reg;
    scene::RegisterBuiltinComponents(reg);
    ecs::World world;
    scene::PrefabLibrary prefs;
    auto inst = scene::Instantiate(world, res.Value(), prefs, reg);
    CHECK(inst.Ok());

    auto view = world.ViewAll<scene::SceneBehaviorTree>();
    CHECK_EQ(view.Size(), 1u);
    ecs::Entity ent = world.EntityAt<scene::SceneBehaviorTree>(0);
    const scene::SceneBehaviorTree* bt = world.Get<scene::SceneBehaviorTree>(ent);
    CHECK(bt != nullptr);
    CHECK_EQ(bt->treeJson, std::string("bt:wolf_ai"));
}

TEST(SceneMeshDefaults) {
    auto res = scene::SceneFile::Parse(R"({
        "entities": [
            {"components": {
                "transform": {"pos": [0,0,0]},
                "mesh": {"meshKey": "obj:assets/cube.obj"}
            }}
        ]
    })");
    CHECK(res.Ok());

    scene::ComponentRegistry reg;
    scene::RegisterBuiltinComponents(reg);
    ecs::World world;
    scene::PrefabLibrary prefs;
    auto inst = scene::Instantiate(world, res.Value(), prefs, reg);
    CHECK(inst.Ok());

    auto view = world.ViewAll<scene::SceneMesh>();
    CHECK_EQ(view.Size(), 1u);
    ecs::Entity ent = world.EntityAt<scene::SceneMesh>(0);
    const scene::SceneMesh* m = world.Get<scene::SceneMesh>(ent);
    CHECK(m != nullptr);
    CHECK_NEAR(m->metallic, 0.0, 1e-6);
    CHECK_NEAR(m->roughness, 1.0, 1e-6);
}

// P1-1 scene inheritance: "extends" parses, and Merge overlays parent entities
// with same-name child overrides + appends new names.
TEST(SceneInheritanceParseAndMerge) {
    const std::string parentJson = R"({
      "entities": [
        {"name": "地面", "components": {"transform": {"pos": [0,0,0]}, "mesh": {"meshKey": "terrain"}}},
        {"name": "树", "components": {"transform": {"pos": [1,0,0]}, "mesh": {"meshKey": "tree"}}}
      ],
      "gameVars": {"gold": 10}
    })";
    const std::string childJson = R"({
      "extends": "scenes/parent.json",
      "entities": [
        {"name": "地面", "components": {"transform": {"pos": [5,0,0]}, "mesh": {"meshKey": "terrain"}}},
        {"name": "英雄", "components": {"transform": {"pos": [0,1,0]}, "mesh": {"meshKey": "hero"}}}
      ],
      "gameVars": {"gold": 99}
    })";
    auto pp = scene::SceneFile::Parse(parentJson);
    auto cc = scene::SceneFile::Parse(childJson);
    CHECK(pp.Ok());
    CHECK(cc.Ok());
    CHECK_EQ(cc.Value().extends, "scenes/parent.json");

    scene::SceneFile merged = scene::SceneFile::Merge(pp.Value(), cc.Value());
    CHECK_EQ(merged.entities.size(), 3u);
    // Overridden entity keeps its position (index 0) but child data wins.
    CHECK_EQ(merged.entities[0].name, "地面");
    bool foundGround = false;
    bool foundTree = false;
    bool foundHero = false;
    for (const auto& e : merged.entities) {
        if (e.name == "地面") {
            foundGround = true;
            for (const auto& c : e.components)
                if (c.name == "transform")
                    CHECK_NEAR(c.data.Get("pos")->At(0)->GetNumber(), 5.0, 1e-6);
        } else if (e.name == "树") {
            foundTree = true;
        } else if (e.name == "英雄") {
            foundHero = true;
        }
    }
    CHECK(foundGround);
    CHECK(foundTree);
    CHECK(foundHero);
    // Child gameVars win.
    CHECK_NEAR(merged.gameVars.Get("gold")->GetNumber(), 99.0, 1e-6);
}

TEST(SceneSortOrderAndCameraComponents) {
    const std::string json = R"({
      "entities": [
        {"name": "bg", "components": {"transform": {"pos": [0,0,0]}, "sprite": {"texture": "a.png"}, "sortOrder": {"z": -5}}},
        {"name": "cam", "components": {"transform": {"pos": [0,0,10]}, "type": {"value": "Camera3D"}, "camera": {"fov": 75, "ortho": true}}}
      ]
    })";
    auto res = scene::SceneFile::Parse(json);
    CHECK(res.Ok());
    scene::ComponentRegistry reg;
    scene::RegisterBuiltinComponents(reg);
    ecs::World world;
    scene::PrefabLibrary prefs;
    auto inst = scene::Instantiate(world, res.Value(), prefs, reg);
    CHECK(inst.Ok());

    ecs::Entity bgEnt;
    world.ViewAll<scene::SceneSortOrder>().ForEach([&](ecs::Entity e, const scene::SceneSortOrder& s) {
        bgEnt = e;
        CHECK_NEAR(s.z, -5.0f, 1e-6f);
    });
    CHECK(bgEnt.IsValid());

    bool camFound = false;
    world.ViewAll<scene::SceneCamera, scene::SceneNodeType>().ForEach(
        [&](ecs::Entity, const scene::SceneCamera& c, const scene::SceneNodeType& t) {
            camFound = true;
            CHECK_NEAR(c.fov, 75.0f, 1e-6f);
            CHECK(c.ortho);
            CHECK_EQ(t.value, "Camera3D");
        });
    CHECK(camFound);
}

TEST(SceneTilemapComponent) {
    const std::string json = R"({
      "entities": [
        {"name": "ground", "components": {
          "transform": {"pos": [0,0,0]},
          "tilemap": {"cols": 2, "rows": 2, "cellSize": 64, "tiles": ["a.png", "", "b.png", ""]}
        }}
      ]
    })";
    auto res = scene::SceneFile::Parse(json);
    CHECK(res.Ok());
    scene::ComponentRegistry reg;
    scene::RegisterBuiltinComponents(reg);
    ecs::World world;
    scene::PrefabLibrary prefs;
    auto inst = scene::Instantiate(world, res.Value(), prefs, reg);
    CHECK(inst.Ok());
    bool found = false;
    world.ViewAll<scene::SceneTilemap>().ForEach(
        [&](ecs::Entity, const scene::SceneTilemap& t) {
            found = true;
            CHECK_EQ(t.cols, 2);
            CHECK_EQ(t.rows, 2);
            CHECK_NEAR(t.cellSize, 64.0f, 1e-6f);
            CHECK_EQ(t.tiles.size(), 4u);
            CHECK_EQ(t.tiles[0], "a.png");
            CHECK(t.tiles[1].empty());
            CHECK_EQ(t.tiles[2], "b.png");
        });
    CHECK(found);
}

TEST(SceneDecalComponent) {
    const std::string json = R"({
      "entities": [
        {"name": "stain", "components": {
          "transform": {"pos": [0, 0.02, 0]},
          "decal": {"texture": "assets/decals/blood.png", "size": 4, "alpha": 0.7}
        }}
      ]
    })";
    auto res = scene::SceneFile::Parse(json);
    CHECK(res.Ok());
    scene::ComponentRegistry reg;
    scene::RegisterBuiltinComponents(reg);
    ecs::World world;
    scene::PrefabLibrary prefs;
    auto inst = scene::Instantiate(world, res.Value(), prefs, reg);
    CHECK(inst.Ok());
    bool found = false;
    world.ViewAll<scene::SceneDecal>().ForEach(
        [&](ecs::Entity, const scene::SceneDecal& d) {
            found = true;
            CHECK_EQ(d.texture, "assets/decals/blood.png");
            CHECK_NEAR(d.size, 4.0f, 1e-6f);
            CHECK_NEAR(d.alpha, 0.7f, 1e-6f);
        });
    CHECK(found);
}
