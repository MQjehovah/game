#pragma once

#include <functional>
#include <map>
#include <string>
#include <vector>

#include "neon/core/json.hpp"
#include "neon/core/result.hpp"
#include "neon/ecs/world.hpp"
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
    std::string name;
    std::string prefab;
    std::vector<ComponentDef> components;
};

// Parsed componentized scene file. Entities reference prefabs by name; the
// runtime PrefabLibrary holds the prefab component templates.
struct SceneFile {
    std::vector<EntityDef> entities;
    core::Json gameVars; // object, or null when absent

    // Parse + structural validation (entities array, entity/component shapes,
    // gameVars type). Semantic checks (transform presence, prefab resolution,
    // built-in field validation) happen in Instantiate.
    static core::Result<SceneFile> Parse(const std::string& jsonText);

    // Re-serialize the parsed scene back to a JSON DOM (instance level; prefab
    // references are preserved, not expanded).
    core::Json ToJson() const;
};

// Prefab library: registers prefab component templates parsed from JSON text
// (prefabs/*.json). A prefab file is either a bare component map
// ({"transform": {...}, ...}) or an object with a "components" member.
class PrefabLibrary {
public:
    core::Status Add(const std::string& name, const std::string& jsonText);
    bool Has(const std::string& name) const;
    core::Result<const core::Json*> Get(const std::string& name) const;

private:
    std::map<std::string, core::Json> prefs_;
};

// Engine-level component types produced by the built-in factories. These are
// plain data; nothing here uploads to the GPU or touches the renderer.
struct SceneTransform {
    math::Vec3 pos;
    math::Quat rot;
    math::Vec3 scale{1, 1, 1};
};
struct SceneMesh {
    std::string meshKey;
    float metallic = 0.f;
    float roughness = 1.f;
    std::string colorHex;
};
struct SceneHealth {
    float hp = 0.f;
    float maxHp = 0.f;
};
struct SceneScript {
    std::string backend;
    std::string path;
    core::Json vars; // object, or null when absent
};
struct SceneBehaviorTree {
    std::string treeJson;
};
struct SceneName {
    std::string name;
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
core::Result<int> Instantiate(ecs::World& world, const SceneFile& scene,
                              const PrefabLibrary& prefs, const ComponentRegistry& reg);

} // namespace neon::scene
