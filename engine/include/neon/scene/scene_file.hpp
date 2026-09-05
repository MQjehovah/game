#pragma once

#include <functional>
#include <map>
#include <string>
#include <vector>

#include "neon/core/json.hpp"
#include "neon/core/result.hpp"
#include "neon/ecs/world.hpp"
#include "neon/gfx/color.hpp"
#include "neon/math/quat.hpp"
#include "neon/math/vec3.hpp"
#include "neon/scene/component_reflect.hpp"
#include "neon/scene/component_schema.hpp"
#include "neon/scene/render_stack.hpp"

namespace neon::assets {
class AssetManager;
}

namespace neon::scene {

// Environment component: owns ambient light, atmosphere, skybox, fog and
// exposure. Kept separate from Light so scene authors can change the world
// mood without touching light objects (and vice versa).
struct SceneEnvironment {
    gfx::Color ambientColor{0.45f, 0.55f, 0.72f, 1.0f};
    float ambientStrength = 0.25f;
    std::string skyTexture;
    bool useAtmosphere = false;
    bool skybox = false;
    gfx::Color skyTop{0.28f, 0.38f, 0.58f, 1.0f};
    gfx::Color skyHorizon{0.55f, 0.65f, 0.80f, 1.0f};
    gfx::Color fogColor{0.45f, 0.55f, 0.70f, 1.0f};
    float fogNear = 60.0f;
    float fogFar = 220.0f;
    float exposure = -1.0f;
};

// A single component of an entity: the component name plus its raw JSON data.
// Component data is always a JSON object; the component factory validates its
// fields at instantiation time.
struct ComponentDef {
    std::string name;
    core::Json data;
};

// A scene entity. `prefab` names a PrefabLibrary entry whose components are
// expanded and deep-merged with `components` (instance fields win).
struct EntityDef {
    int id = 0;           // stable per-scene id (0 = none; parentId references this)
    std::string name;
    std::string prefab;
    // G5-4: scene-graph hierarchy lives at the ENTITY level (alongside id/name),
    // not inside the transform component. parentId references another entity's
    // stable id; `parent` is the legacy name-based form (resolved to parentId
    // at Instantiate; kept for old scenes).
    int parentId = 0;
    std::string parent;
    std::vector<ComponentDef> components;
};

// One level-of-detail step for a SceneMesh: beyond `distance` (camera units)
// the previous level is swapped for the lower-detail `meshKey`. `distance`
// values must be strictly increasing across the list (the mesh factory
// validates this); level 0 is always the component's base meshKey.
struct LodEntry {
    float distance = 0.f;
    std::string meshKey;
};

// Parsed componentized scene file. Entities reference prefabs by name; the
// runtime PrefabLibrary holds the prefab component templates.
struct SceneFile {
    std::vector<EntityDef> entities;
    core::Json gameVars; // object, or null when absent
    // Optional parent scene path (P1-1 scene inheritance). When set, the
    // parent scene's entities are loaded first and the child's same-named
    // entities override them (new names append). The editor resolves this at
    // load time (flattening); the runtime starts from flattened JSON.
    std::string extends;
    // Optional scene-level data (any object, e.g. a 2D game's level layout:
    // {"plants": [...], "zombies": [...]}). Editors and scripts read/write it
    // as part of the scene file, so 2D and 3D scenes live in scenes/*.json.
    core::Json level; // object, or null when absent
    SceneEnvironment environment;
    bool hasEnvironment = false;
    RenderStack renderStack;
    bool hasRenderStack = false;

    // Parse + structural validation (entities array, entity/component shapes,
    // gameVars type). Semantic checks (transform presence, prefab resolution,
    // built-in field validation) happen in Instantiate.
    static core::Result<SceneFile> Parse(const std::string& jsonText);
    // Scene inheritance merge (P1-1): parent entities first, child entities
    // replace same-name parent entries and append new ones; child gameVars /
    // level win when present.
    static SceneFile Merge(const SceneFile& parent, const SceneFile& child);

    // Re-serialize the parsed scene back to a JSON DOM (instance level; prefab
    // references are preserved, not expanded).
    core::Json ToJson() const;

    // Asset references declared by the scene's mesh components: the base
    // meshKey, every LOD level's meshKey and the PBR texture paths (albedo /
    // metallic-roughness / AO / emissive, top-level and under "material").
    // Used by the chunk streamer to fill a WorldChunk's preload list.
    std::vector<std::string> MeshKeys() const;

    // Build a single entity's JSON ({"name", "components": {transform, mesh}})
    // from raw editor-like data. The meshKey is stored verbatim: the caller
    // chooses a key the runtime can resolve (e.g. "obj:..." / "gltf:..." for
    // file-backed meshes; short keys like "terrain"/"cube" imply procedural
    // primitives). NOTE: the built-in mesh factory validates meshKey prefixes
    // only when an AssetManager is registered, so exported scenes that use
    // short procedural keys require runtime-side procedural mesh resolution
    // (T2.9 playtest / T4.7 packager) — this helper does not resolve them.
    // `color` becomes the mesh material's colorHex ("#RRGGBB"). The four
    // texture paths are optional PBR maps (albedo / metallic-roughness / AO /
    // emissive); empty strings are omitted from the JSON. Empty name or
    // meshKey returns an error.
    //
    // An optional script component is emitted when `scriptPath` is non-empty:
    // {"backend": "lua", "path": <scriptPath>, "vars": <scriptVars>} (vars
    // omitted when not a JSON object), matching the built-in `script` factory.
    // `scriptBackend` defaults to "lua" when empty.
    //
    // `lod` (optional) carries level-of-detail steps as
    // [{"distance", "meshKey"}, ...] under the mesh component; each entry must
    // have a strictly increasing distance (the factory rejects duplicates/non-
    // increasing thresholds). Empty = no LOD chain.
    static core::Result<core::Json> MakeEntity(const std::string& name,
                                               const math::Vec3& pos,
                                               const math::Quat& rot,
                                               const math::Vec3& scale,
                                               const std::string& meshKey,
                                               float metallic = 0.0f,
                                               float roughness = 1.0f,
                                               const gfx::Color& color = gfx::Color::White,
                                               const std::string& albedoTex = "",
                                               const std::string& mrTex = "",
                                               const std::string& aoTex = "",
                                               const std::string& emissiveTex = "",
                                               float ao = 1.0f,
                                               float emissiveIntensity = 1.0f,
                                                const std::string& scriptPath = "",
                                                const std::string& scriptBackend = "lua",
                                                const core::Json& scriptVars = core::Json{},
                                               const std::vector<LodEntry>& lod = {},
                                               float hp = 0.0f,
                                               float maxHp = 0.0f,
                                                 const std::string& parent = "",
                                                 int parentId = 0,
                                                 int id = 0,
                                                 float uvRepeat = 1.0f,
                                                 const std::string& normalTex = "",
                                                 float normalScale = 1.0f);
    // G2-2 scene unification: the canonical 2D sprite entity builder (mirrors
    // MakeEntity for mesh entities). Emits name + id + components: transform
    // (pos/rot/scale/parent/parentId), sprite (texture/flipX/flipY/colorHex)
    // and health (hp/maxHp when maxHp > 0). Both the editor (BuildPlaySceneJson)
    // and any host export sprites through this, so the runtime parser always
    // reads what this builder writes — no hand-written drift (the sprite branch
    // historically dropped health). `colorHex` is "#rrggbb" ("" = white).
    static core::Result<core::Json> MakeSpriteEntity(const std::string& name,
                                                     const math::Vec3& pos,
                                                     const math::Quat& rot,
                                                     const math::Vec3& scale,
                                                     const std::string& texture,
                                                     bool flipX = false,
                                                     bool flipY = false,
                                                     const std::string& colorHex = "",
                                                     float hp = 0.0f,
                                                     float maxHp = 0.0f,
                                                     const std::string& parent = "",
                                                     int parentId = 0,
                                                     int id = 0,
                                                     const std::vector<std::string>& frames = {},
                                                     float fps = 0.0f,
                                                     bool loop = true,
                                                     const std::string& sheet = "",
                                                     int sheetFrames = 0);
    // G2-2: serializes an ecs::World back to the scene-file JSON format (the
    // reverse of Instantiate). Every factory's component is emitted exactly as
    // the corresponding factory reads it, so Parse(FromWorld(w)) re-Instantiates
    // an equivalent World. Stable ids round-trip via SceneId. The editor uses
    // this to generate play/save output from the runtime World it hosts.
    static core::Result<core::Json> FromWorld(ecs::World& world);
    // G5-4-4(项4): serialize ONE World entity (by SceneId) back to its scene
    // JSON — the per-entity counterpart of FromWorld (same component output).
    // The editor's World-backed inspector reads use it so panels are driven by
    // the runtime representation. Err when the entity or its transform is
    // absent.
    static core::Result<core::Json> EntityToJson(ecs::World& world, int entityId);
};

// Prefab library: registers prefab component templates parsed from JSON text
// (assets/prefabs/*.json). A prefab file is either a bare component map
// ({"transform": {...}, ...}) or an object with a "components" member.
class PrefabLibrary {
public:
    core::Status Add(const std::string& name, const std::string& jsonText);
    bool Has(const std::string& name) const;
    core::Result<const core::Json*> Get(const std::string& name) const;
    size_t Size() const { return prefs_.size(); }

private:
    std::map<std::string, core::Json> prefs_;
};

// G5-4-4(项1) prefab instance overrides: the field-level diff/mix pair used by
// the editor to store ONLY what an instance changed from its prefab template
// (Unity-style instance variants). DeepMerge already gives "template expanded
// then instance wins"; these two make it round-trip without storing the whole
// expansion.
//
// ComputePrefabOverrides(template, instance) -> override component map:
//   - a component absent from the template is emitted wholesale;
//   - a component present in both is diffed FIELD-WISE (equal fields omitted,
//     only differing fields emitted);
//   - a component identical to the template is omitted entirely.
// MergePrefabOverrides(template, overrides) -> DeepMerge(template, overrides),
// the inverse for the emitted values (instance == the original).
core::Json ComputePrefabOverrides(const core::Json& templateComps,
                                  const core::Json& instanceComps);
core::Json MergePrefabOverrides(const core::Json& templateComps,
                                const core::Json& overrides);

// Engine-level component types produced by the built-in factories. These are
// plain data; nothing here uploads to the GPU or touches the renderer.
struct SceneTransform {
    math::Vec3 pos;
    math::Quat rot;
    math::Vec3 scale{1, 1, 1};

    // G2-1: reflection drives the editor schema + JSON + script field access.
    // The `rot` member is a Quat (the runtime's real representation); the editor
    // edits it as a 4-component quaternion, matching how the serialized JSON
    // stores it ([x,y,z,w]) — "reflect the struct, don't invent a display type".
    inline static const auto kFields = ReflectFields(
        Field("pos", "位置", FieldType::Vec3, &SceneTransform::pos, 0, -100000, 100000, 0.1),
        Field("rot", "旋转 (四元数 x,y,z,w)", FieldType::Vec4, &SceneTransform::rot,
              0, -1, 1, 0.01),
        Field("scale", "缩放", FieldType::Vec3, &SceneTransform::scale, 1, 0.01, 1000, 0.05));
};
// Resolved scene-tree link (set by Instantiate after names resolve): the
// parent entity whose world transform this entity inherits at render time.
struct SceneParentLink {
    ecs::Entity parent;
};
struct SceneMesh {
    std::string meshKey;
    // Level-of-detail chain (data-driven): each entry swaps the previous mesh
    // for a lower-detail one beyond `distance`. The runtime resolves these
    // into a gfx::LodChain and picks a level per frame by camera distance.
    // Empty = single-mesh entity (level 0 = meshKey).
    std::vector<LodEntry> lod;
    float metallic = 0.f;
    float roughness = 1.f;
    std::string colorHex;
    // Optional PBR texture paths (empty = none). albedoTex is the base-color
    // map, mrTex the metallic-roughness map (G=roughness, B=metallic), aoTex
    // the ambient-occlusion map (R channel) and emissiveTex the emissive map.
    std::string albedoTex;
    std::string mrTex;
    std::string aoTex;
    std::string emissiveTex;
    // A2: tangent-space normal map (reconstructed via screen-space derivatives
    // in the lit shader — no per-vertex tangents needed). Empty = none.
    std::string normalTex;
    float normalScale = 1.0f;
    // G4 terrain splatmap: per-layer colors for the dirt and rock bands (grass
    // uses albedoTex as its realistic texture). Empty = engine default color.
    std::string dirtColorHex;
    std::string rockColorHex;
    float ao = 1.f;               // occlusion strength (0 = ignore AO, 1 = full)
    float emissiveIntensity = 1.f;
    // UV tiling multiplier applied to the material's base UVs (default 1 = no
    // repeat). For a large ground plane set this to the world-size so a small
    // texture tiles instead of stretching across the whole surface.
    float uvRepeat = 1.0f;
    // Casts a directional-light (CSM) shadow. Large receivers such as a ground
    // plane must NOT be a caster: a big plane projecting onto itself lands the
    // cascade depth within the bias band and blackens the whole surface (the
    // "half the map is shadowed" bug). Default true so trees/props cast; set
    // false on ground/water/terrain to keep it receiver-only.
    bool castShadow = true;
    bool receiveShadow = true;
};
// 2D sprite component: an image texture drawn on an XY quad facing +Z (the
// front-ortho camera). Display size comes from SceneTransform::scale, so the
// gizmo and inspector scale edits work like any other entity. flips mirror
// the quad around its center (rendered as negative scale, no UV changes).
struct SceneSprite {
    std::string texture; // asset path, e.g. "assets/textures/plant.png"
    bool flipX = false;
    bool flipY = false;
    std::string colorHex; // tint, "#rrggbb" (empty = white)
    // Sequence-frame animation: when `frames` is non-empty the entity plays
    // them in order at `fps` (looping), swapping the drawn texture each frame.
    // The static `texture` is used as the first frame if frames[0] is absent.
    std::vector<std::string> frames;
    float fps = 0.0f; // frames per second (0 = static texture)
    bool loop = true;
    // Spritesheet variant: `sheet` is a horizontal atlas of `sheetFrames`
    // equal-sized frames (one texture instead of N files -- no per-frame .meta
    // clutter). Mutually exclusive with `frames`; when both are set, `sheet`
    // wins.
    std::string sheet;
    int sheetFrames = 0;
    // 3D billboard mode: when true the quad is re-oriented every frame to face
    // the camera (glow particles, world-space VFX). 2D front-ortho games are
    // unaffected — the camera-facing basis degenerates to the identity there.
    bool billboard = false;
};
// Data-driven zombie spawn: which row it attacks, when it appears (scene
// time) and its armor type. Kept as a component so per-entity scripts can
// read their own parameters (scripts/zombie.lua etc.).
struct SceneZombie {
    int row = 0;           // 0-based lawn row (matches plant/zombie data)
    float delay = 0.0f;    // seconds after scene start before it appears
    std::string type;      // "basic" | "cone" | "bucket"
};
struct SceneHealth {
    float hp = 0.f;
    float maxHp = 0.f;

    // C6: schema + JSON are reflected from this single field list (editor
    // properties and the scene file serializer share one source of truth).
    inline static const auto kFields = scene::ReflectFields(
        scene::Field("hp", "血量", FieldType::Number, &SceneHealth::hp, 0, 0, 1e9, 1),
        scene::Field("maxHp", "最大血量", FieldType::Number, &SceneHealth::maxHp, 0, 0, 1e9, 1));
    static scene::ComponentSchema Schema() { return {"health", "生命", kFields.Schemas()}; }
    core::Json ToJson() const { return kFields.ToJson(*this); }
    bool FromJson(const core::Json& json, std::string* err = nullptr) {
        return kFields.FromJson(json, *this, err);
    }
};
// Per-entity animation override for skinned models (M1): when present on an
// entity whose mesh is "gltf:..." with animation clips, the runtime plays
// `clip` (name substring, case-insensitive) instead of the model's default
// loop. Fields map 1:1 to the PlayAnimation/animation script binding.
struct SceneAnimOverride {
    std::string clip;        // clip name (substring match, "" = default loop)
    bool loop = true;
    float speed = 1.0f;
    float crossFade = 0.2f;
    // Runtime slot: 1 while the override is live (set on PlayAnimation,
    // cleared by AnimFinished polling); lets scripts re-issue the same clip.
    bool active = false;

    // G2-1: reflection drives the editor schema + JSON + script field access.
    inline static const auto kFields = ReflectFields(
        Field("clip", "动画片段", FieldType::String, &SceneAnimOverride::clip),
        Field("loop", "循环", FieldType::Bool, &SceneAnimOverride::loop),
        Field("speed", "速度", FieldType::Number, &SceneAnimOverride::speed, 1, 0, 10, 0.05),
        Field("crossFade", "过渡", FieldType::Number, &SceneAnimOverride::crossFade, 0.2, 0, 5, 0.05),
        Field("active", "激活", FieldType::Bool, &SceneAnimOverride::active,
              FieldMeta{FieldCategory::Transient}));
};
// Rigid body component (physics::World): describes one collider attached to
// the entity's transform. GameRuntime::Start registers the body with the
// physics world and writes `bodyId`; every fixed physics step moves the body
// and the resulting position is written back into SceneTransform so the mesh
// follows. AABB-only boxes (axis aligned), no rotation support yet.
struct SceneRigidBody {
    std::string shape = "sphere";       // "sphere" | "box"
    float radius = 0.5f;
    math::Vec3 halfExtents{0.5f, 0.5f, 0.5f};
    bool dynamic = true;
    float mass = 0.0f;                  // <=0 -> auto from volume
    float restitution = 0.0f;
    float friction = 0.4f;
    float linearDamping = 0.0f;
    float gravityScale = 1.0f;
    uint32_t layer = 1;                 // collision group (0..255 on Jolt)
    uint32_t mask = 0xFFFFFFFFu;        // collision mask (which groups to hit)
    uint32_t bodyId = 0;                // physics::World::BodyId (runtime state)

    // G2-1: reflection drives the editor schema + JSON + script field access.
    // NOTE: the scene-file key for the damping field is "damping" (the member is
    // `linearDamping`); Field lets us map a serialized key to a differently-named
    // member so the reflection schema matches what the factory reads.
    inline static const char* const kShapeOptions[2] = {"sphere", "box"};
    inline static const auto kFields = ReflectFields(
        Field("shape", "形状", FieldType::Enum, &SceneRigidBody::shape, 0, 0, 0, 0,
              FieldMeta{FieldCategory::Serialize, nullptr, nullptr, nullptr, nullptr,
                        kShapeOptions, 2}),
        Field("radius", "半径", FieldType::Number, &SceneRigidBody::radius, 0.5, 0.01, 100, 0.1),
        Field("halfExtents", "半尺寸", FieldType::Vec3, &SceneRigidBody::halfExtents,
              0.5, 0.01, 100, 0.1),
        Field("dynamic", "动态", FieldType::Bool, &SceneRigidBody::dynamic, 1, 0, 1, 0),
        Field("mass", "质量 (0=自动)", FieldType::Number, &SceneRigidBody::mass, 0, 0, 1e6, 0.5),
        Field("restitution", "弹性", FieldType::Number, &SceneRigidBody::restitution, 0, 0, 1, 0.01),
        Field("friction", "摩擦", FieldType::Number, &SceneRigidBody::friction, 0.4, 0, 1, 0.01),
        Field("damping", "线性阻尼", FieldType::Number, &SceneRigidBody::linearDamping, 0, 0, 10, 0.01),
        Field("gravityScale", "重力缩放", FieldType::Number, &SceneRigidBody::gravityScale, 1, 0, 10, 0.1),
        Field("layer", "碰撞层", FieldType::Int, &SceneRigidBody::layer, 1, 0, 255, 1),
        Field("mask", "碰撞掩码", FieldType::Int, &SceneRigidBody::mask, 1, 0, 0xFFFFFFFF, 1),
        Field("bodyId", "物理体", FieldType::Int, &SceneRigidBody::bodyId,
              FieldMeta{FieldCategory::Transient}));
};
// Character body component (Jolt CharacterVirtual when the Jolt backend is
// active). A capsule-shaped kinematic controller that moves with the desired
// velocity, lands on geometry and reports ground state - the player/NPC
// movement primitive. The custom (deterministic server) world does not
// implement characters; entities with this component are skipped there.
struct SceneCharacter {
    float radius = 0.4f;
    float halfHeight = 0.9f;            // capsule half height (total height 2*h+r*2)
    uint32_t layer = 1;
    uint32_t mask = 0xFFFFFFFFu;
    uint32_t bodyId = 0;                // JoltWorld character handle (runtime state)

    // G2-1: reflection drives the editor schema + JSON + script field access.
    inline static const auto kFields = ReflectFields(
        Field("radius", "半径", FieldType::Number, &SceneCharacter::radius, 0.4, 0.01, 10, 0.05),
        Field("halfHeight", "半高", FieldType::Number, &SceneCharacter::halfHeight, 0.9, 0.1, 50, 0.1),
        Field("layer", "碰撞层", FieldType::Int, &SceneCharacter::layer, 1, 0, 255, 1),
        Field("mask", "碰撞掩码", FieldType::Int, &SceneCharacter::mask, 0xFFFFFFFF, 0, 0xFFFFFFFF, 1),
        Field("bodyId", "物理体", FieldType::Int, &SceneCharacter::bodyId,
              FieldMeta{FieldCategory::Transient}));
};
struct SceneScript {
    std::string backend;
    std::string path;
    core::Json vars; // object, or null when absent

    // G2-1: reflection drives the editor schema + JSON + script field access.
    // The editor's script panel reads schema->fields (backend Enum / path
    // Resource / vars Json), so keep exactly these three, in this order.
    inline static const char* const kScriptBackends[2] = {"lua", "js"};
    inline static const auto kFields = ReflectFields(
        Field("backend", "后端", FieldType::Enum, &SceneScript::backend, 0, 0, 0, 0,
              FieldMeta{FieldCategory::Serialize, nullptr, nullptr, nullptr, nullptr,
                        kScriptBackends, 2}),
        Field("path", "脚本路径", FieldType::Resource, &SceneScript::path, 0, 0, 0, 0,
              FieldMeta{FieldCategory::Serialize, nullptr, nullptr, "script"}),
        Field("vars", "变量", FieldType::Json, &SceneScript::vars));
    core::Json ToJson() const { return kFields.ToJson(*this); }
};
// Audio source component (G8-3): a positioned sound. GameRuntime::Start plays
// it once at the entity's position through the playSfx3D hook; the editor shows
// its attenuation sphere in the debug overlay. Looping/ambient playback needs a
// loop-3D hook (current playSfx3D is one-shot) and is a documented follow-up.
// G2-1: schema + JSON are reflected from `kFields` (one source of truth).
struct SceneAudioSource {
    std::string sound;   // SoundFx name (PlaySfx convention)
    float volume = 1.0f; // 0..1
    float radius = 10.0f; // attenuation distance, drives the debug sphere

    // G2-1: schema + JSON are reflected from this single field list.
    inline static const auto kFields = scene::ReflectFields(
        scene::Field("sound", "声音名", FieldType::String, &SceneAudioSource::sound, 0, 0, 0),
        scene::Field("volume", "音量", FieldType::Number, &SceneAudioSource::volume, 1, 0, 1, 0.01),
        scene::Field("radius", "衰减半径", FieldType::Number, &SceneAudioSource::radius, 10, 0.1,
                     100, 0.5));
    static scene::ComponentSchema Schema() { return {"audio", "音频源", kFields.Schemas()}; }
    core::Json ToJson() const { return kFields.ToJson(*this); }
    bool FromJson(const core::Json& j, std::string* err) { return kFields.FromJson(j, *this, err); }
};
// Multiple script components on one entity (Unity-style): a scene entity can
// carry several behaviors. Each entry attaches like a single SceneScript.
struct SceneScripts {
    std::vector<SceneScript> items;
};
struct SceneBehaviorTree {
    std::string treeJson;
};
struct SceneName {
    std::string name;
};
// G2-2: the scene file's stable entity id, preserved through Instantiate so a
// World can be serialized back (SceneFile::FromWorld) with identical ids and
// parentId links. 0 = none.
struct SceneId {
    int id = 0;
};
// Generic storage for scene components WITHOUT a registered factory (plugin
// or game-data components such as "inventory" / "plant"). Instantiate appends
// one SceneData per entity carrying such components so runtime scripts and
// plugins can read custom component data via the EntityComponent binding.
struct SceneData {
    std::vector<std::pair<std::string, core::Json>> components;
};
// Group membership (P1-1): an entity can belong to any number of named groups
// ("enemy", "player", "respawn"). Scripts query them at runtime via
// GetEntitiesInGroup(name); the editor edits them as a comma-separated list.
struct SceneGroups {
    std::vector<std::string> groups;

    // G2-1: reflection (generic "groups" data component; the factory accepts
    // either a JSON array or a comma-separated string, and the Array editor
    // emits an array).
    inline static const auto kFields = ReflectFields(
        Field("groups", "组 (逗号分隔)", FieldType::Array, &SceneGroups::groups));
};
// Node type table (P1-1): an explicit type for the inspector/editor (Node /
// MeshInstance3D / Camera3D / CharacterBody / Sprite / Light3D). Empty = the
// editor auto-derives the type from the mesh key / sprite.
struct SceneNodeType {
    std::string value;

    // G2-1: reflection (scene-file key "value"; the editor shows it as an enum).
    inline static const char* const kNodeTypeOptions[6] = {
        "Node", "MeshInstance3D", "Camera3D", "CharacterBody", "Sprite", "Light3D"};
    inline static const auto kFields = ReflectFields(
        Field("value", "类型", FieldType::Enum, &SceneNodeType::value, 0, 0, 0, 0,
              FieldMeta{FieldCategory::Serialize, nullptr, nullptr, nullptr, nullptr,
                        kNodeTypeOptions, 6}));
};
// Camera3D component: view parameters for a camera entity (used by editors /
// tools; the runtime reads the active camera from the scene when present).
struct SceneCamera {
    float fov = 60.0f;  // vertical field of view, degrees
    bool ortho = false;
    float orthoSize = 10.0f; // Unity orthographic camera "Size" (half view height)
    // View aspect (width/height) the game runs at - the play viewport
    // letterboxes to THIS, whatever the editor dock looks like. 0 = the
    // 16:9 design default (1280x720).
    float aspect = 0.0f;

    // G2-1: reflection (scene-file keys fov/ortho/orthoSize/aspect, all of which
    // the camera factory reads — the serializer writes them too).
    inline static const auto kFields = ReflectFields(
        Field("fov", "视野 (度)", FieldType::Number, &SceneCamera::fov, 60, 20, 120, 1),
        Field("ortho", "正交", FieldType::Bool, &SceneCamera::ortho),
        Field("orthoSize", "正交尺寸", FieldType::Number, &SceneCamera::orthoSize, 10, 0, 1e4, 0.5),
        Field("aspect", "宽高比", FieldType::Number, &SceneCamera::aspect, 0, 0, 4, 0.01));
};
// The `Light` component on a light object (Unity GameObject -> Light). `type`
// is exactly one of "directional" / "point" / "ambient". Directional uses
// `sunDir` + `color`; point uses `color` + `radius` (position from the entity's
// transform); ambient uses the shared `color` + `ambientStrength`.
struct SceneLight {
    std::string type = "directional";
    math::Vec3 sunDir{-0.4f, -1.0f, -0.3f};
    gfx::Color color{1.0f, 0.95f, 0.85f, 1.0f};
    float intensity = 1.0f;        // per-light strength (multiplies color at render)
    float radius = 10.0f;               // point light falloff range
    float ambientStrength = 0.25f;
    // 写实天空贴图（HDRI tonemapped JPG 的虚拟路径）。非空时 DrawSystem 加载
    // 并作为全屏天空背景替代纯色渐变。
    std::string skyTexture;
    // 自定义氛围覆盖（showcase 场景整场色调）：useAtmosphere=true 时两个渲染宿主
    // （编辑器 edit 视图 + runtime draw_system）用 skyTop/skyHorizon 渐变、
    // fogColor/fogNear/fogFar 替代默认白天天空 + 灰蓝雾，使黄昏/雪地/极光等
    // 场景能在 scene JSON 里数据驱动地表达整场氛围。默认值 = 现有硬编码
    // （不覆盖，保回归等价）；fog 默认取 runtime 的 60-220。
    bool useAtmosphere = false;
    // A4 程序化天空盒：useAtmosphere 场景可额外开启 view-ray 太阳/月亮/云。
    bool skybox = false;
    gfx::Color skyTop{0.28f, 0.38f, 0.58f, 1.0f};
    gfx::Color skyHorizon{0.55f, 0.65f, 0.80f, 1.0f};
    gfx::Color fogColor{0.45f, 0.55f, 0.70f, 1.0f};
    float fogNear = 60.0f;
    float fogFar = 220.0f;
    // 曝光（tonemap 前乘子）：<0 表示未设置（宿主保留自身曝光）。夜晚场景
    // 需要 >1 提亮月光/火光氛围，黄昏可微调避免死黑/泛白。
    float exposure = -1.0f;

    // G2-1: reflection drives the editor schema + JSON + script field access.
    // `color` is a gfx::Color (serialized as [r,g,b,a]); the editor's Color
    // control reads BOTH forms (hex string for mesh material, array for light).
    inline static const char* const kLightTypes[3] = {"directional", "point", "ambient"};
    inline static const auto kFields = ReflectFields(
        Field("type", "类型", FieldType::Enum, &SceneLight::type, 0, 0, 0, 0,
              FieldMeta{FieldCategory::Serialize, nullptr, nullptr, nullptr, nullptr,
                        kLightTypes, 3}),
        Field("sunDir", "太阳方向", FieldType::Vec3, &SceneLight::sunDir),
        Field("color", "颜色", FieldType::Color, &SceneLight::color),
        Field("intensity", "强度", FieldType::Number, &SceneLight::intensity, 1, 0, 100, 0.05),
        Field("radius", "范围", FieldType::Number, &SceneLight::radius, 10, 0, 1e4, 0.5),
        Field("ambientStrength", "环境强度", FieldType::Number, &SceneLight::ambientStrength,
              0.25, 0, 1, 0.01),
        Field("skyTexture", "天空贴图", FieldType::Resource, &SceneLight::skyTexture,
              0, 0, 0, 0, FieldMeta{FieldCategory::Serialize, nullptr, nullptr, "texture"}),
        Field("useAtmosphere", "自定义氛围", FieldType::Bool, &SceneLight::useAtmosphere),
        Field("skybox", "天空盒(程序化)", FieldType::Bool, &SceneLight::skybox),
        Field("skyTop", "天空顶部", FieldType::Color, &SceneLight::skyTop),
        Field("skyHorizon", "天空地平", FieldType::Color, &SceneLight::skyHorizon),
        Field("fogColor", "雾颜色", FieldType::Color, &SceneLight::fogColor),
        Field("fogNear", "雾起点", FieldType::Number, &SceneLight::fogNear, 60, 0, 1e4, 1),
        Field("fogFar", "雾终点", FieldType::Number, &SceneLight::fogFar, 220, 0, 1e5, 1),
        Field("exposure", "曝光", FieldType::Number, &SceneLight::exposure, -1, -1, 100, 0.05));
};

// 2D sort order (P2-3): sprites draw back-to-front by this value (lower first;
// default 0 when the component is absent).
struct SceneSortOrder {
    float z = 0.0f;

    // G2-1: this single list drives the editor schema + JSON + script field
    // access (reflection). Registered into the TypeRegistry (see
    // RegisterBuiltinReflectedTypes) so the editor edits it from reflection.
    inline static const auto kFields = ReflectFields(
        Field("z", "Z 排序 (小在前)", FieldType::Number, &SceneSortOrder::z, 0, -10000, 10000, 0.1));
    core::Json ToJson() const { return kFields.ToJson(*this); }
};
// Authorable terrain (P1-1 world editor): a (segments+1)^2 heightmap over a
// size x size area. The runtime builds the terrain mesh from these heights;
// the editor paints them with a brush.
struct SceneTerrain {
    int segments = 48;
    float size = 60.0f;
    float heightScale = 1.0f;
    std::vector<float> heights;
    // G2-3 chunked LOD: when chunkGridDiv > 0 the runtime renders the terrain
    // as gridDiv x gridDiv patches, each with its own LodChain (near = dense,
    // far = coarse) instead of one monolithic mesh. 0 keeps the old single-mesh
    // path. chunkLodLevels is the LOD depth per patch.
    int chunkGridDiv = 0;
    int chunkLodLevels = 3;
    int chunkBaseSubdiv = 16;
    // G2-3 vegetation: deterministic scatter of a prop over flat/low ground.
    // vegMeshKey (e.g. "tree"/"bush"/"rock") is resolved for each instance;
    // beyond vegImpostorDistance an instance draws a cheap billboard card.
    std::string vegMeshKey;
    uint32_t vegCount = 0;
    uint32_t vegSeed = 1;
    float vegSize = 1.0f;
    float vegImpostorDistance = 60.0f;
    float vegMinHeight = 0.0f;  // world Y floor for plantable ground
    float vegMaxHeight = 3.0f;  // world Y ceiling
    float vegMaxSlope = 0.30f;  // slope (1 - normal.y) plants tolerate
    // G2-3 exclusion: a cleared radius (XZ) around vegExcludeCenter where no
    // vegetation spawns — keeps gameplay zones (village, roads) plant-free.
    float vegExcludeRadius = -1.0f;
    math::Vec3 vegExcludeCenter{0.0f, 0.0f, 0.0f};
};
// 2D tilemap (P1-1): cols x rows of texture paths ("" = empty cell), each
// cell rendered as a `cellSize` design-unit quad at the entity's position.
struct SceneTilemap {
    int cols = 8;
    int rows = 5;
    float cellSize = 80.0f;
    std::vector<std::string> tiles;  // cols*rows, row-major
};
// Ground-projected decal (P2-1): a textured quad lying on the XZ plane at the
// entity's position (road markings, stains, blood splats). `size` is the world
// edge length; alpha fades the whole decal.
struct SceneDecal {
    std::string texture;
    float size = 2.0f;
    float alpha = 1.0f;

    // G2-1: reflection drives the editor schema + JSON + script field access.
    inline static const auto kFields = ReflectFields(
        Field("texture", "贴图", FieldType::Resource, &SceneDecal::texture, 0, 0, 0, 0,
              FieldMeta{FieldCategory::Serialize, nullptr, nullptr, "texture"}),
        Field("size", "尺寸", FieldType::Number, &SceneDecal::size, 2, 0.1, 100, 0.1),
        Field("alpha", "不透明度", FieldType::Number, &SceneDecal::alpha, 1, 0, 1, 0.01));
    core::Json ToJson() const { return kFields.ToJson(*this); }
};

// A component factory builds a component onto `ent` from its effective JSON
// data. Args: world, entity, merged component data (prefab already applied),
// raw instance data (null when only the prefab provides it), error out-param.
// Returns false + *error on failure (the whole Instantiate call rolls back).
using ComponentFactory = std::function<bool(ecs::World&, ecs::Entity,
                                            const core::Json&, const core::Json&,
                                            std::string*)>;

// Registry of named component factories. Component types without a factory are
// skipped with a warning during Instantiate (non-fatal).
class ComponentRegistry {
public:
    void Register(const std::string& name, ComponentFactory fn);
    bool Has(const std::string& name) const;
    const std::map<std::string, ComponentFactory>& All() const;

private:
    std::map<std::string, ComponentFactory> factories_;
};

// Registers the engine's built-in factories: transform, mesh, health, script,
// behaviorTree, name. When `assets` is non-null, mesh keys are additionally
// validated against the known loader prefixes ("obj:" / "gltf:").
void RegisterBuiltinComponents(ComponentRegistry& reg, assets::AssetManager* assets = nullptr);

// Instantiate a whole scene into `world`. Prefabs expand via `prefs`; every
// component goes through `reg`. Effective components apply in alphabetical
// order (std::map order, deterministic). An entity's `name` field produces a
// SceneName; an explicit prefab or instance `name` component overrides that
// field value. Returns the created entity count, or an error; on error all
// entities created by this call are destroyed (transactional).
//
// When `outEntities` is non-null it is cleared and filled with the created
// entities in creation order (on success only) - the chunk streamer uses this
// to track which entities a chunk owns for unload.
core::Result<int> Instantiate(ecs::World& world, const SceneFile& scene,
                              const PrefabLibrary& prefs, const ComponentRegistry& reg,
                              std::vector<ecs::Entity>* outEntities = nullptr);

} // namespace neon::scene
