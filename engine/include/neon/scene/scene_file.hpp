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

namespace neon::assets {
class AssetManager;
}

namespace neon::scene {

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
    int id = 0; // stable per-scene id (0 = none; parentId references this)
    std::string name;
    std::string prefab;
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
                                               int id = 0);
};

// Prefab library: registers prefab component templates parsed from JSON text
// (prefabs/*.json). A prefab file is either a bare component map
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

// Engine-level component types produced by the built-in factories. These are
// plain data; nothing here uploads to the GPU or touches the renderer.
struct SceneTransform {
    math::Vec3 pos;
    math::Quat rot;
    math::Vec3 scale{1, 1, 1};
    std::string parent; // scene-tree parent by entity NAME ("" = root)
    int parentId = 0;   // G1-3: scene-tree parent by stable entity id (0 = root);
                        // preferred over `parent` (legacy name fallback).
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
    float ao = 1.f;               // occlusion strength (0 = ignore AO, 1 = full)
    float emissiveIntensity = 1.f;
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
};
struct SceneScript {
    std::string backend;
    std::string path;
    core::Json vars; // object, or null when absent
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
};
// Node type table (P1-1): an explicit type for the inspector/editor (Node /
// MeshInstance3D / Camera3D / CharacterBody / Sprite / Light3D). Empty = the
// editor auto-derives the type from the mesh key / sprite.
struct SceneNodeType {
    std::string value;
};
// Camera3D component: view parameters for a camera entity (used by editors /
// tools; the runtime reads the active camera from the scene when present).
struct SceneCamera {
    float fov = 60.0f;  // vertical field of view, degrees
    bool ortho = false;
};
// The `Light` component on a light object (Unity GameObject -> Light). `type`
// is exactly one of "directional" / "point" / "ambient". Directional uses
// `sunDir` + `color`; point uses `color` + `radius` (position from the entity's
// transform); ambient uses `ambientColor` + `ambientStrength`.
struct SceneLight {
    std::string type = "directional";
    math::Vec3 sunDir{-0.4f, -1.0f, -0.3f};
    gfx::Color color{1.0f, 0.95f, 0.85f, 1.0f};
    float radius = 10.0f;               // point light falloff range
    gfx::Color ambientColor{0.55f, 0.70f, 0.88f, 1.0f};
    float ambientStrength = 0.25f;
};
// 2D sort order (P2-3): sprites draw back-to-front by this value (lower first;
// default 0 when the component is absent).
struct SceneSortOrder {
    float z = 0.0f;
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
