#pragma once

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

#include "neon/bt/behavior_tree.hpp"
#include "neon/core/result.hpp"
#include "neon/ecs/world.hpp"
#include "neon/gfx/camera.hpp"
#include "neon/gfx/font.hpp"
#include "neon/gfx/material.hpp"
#include "neon/gfx/mesh.hpp"
#include "neon/physics/physics.hpp"
#include "neon/platform/input.hpp"
#include "neon/scene/scene_file.hpp"
#include "neon/scene/skills.hpp"
#include "neon/scene/status.hpp"
#include "neon/script/bindings.hpp"
#include "neon/script/gamevars.hpp"
#include "neon/script/script.hpp"

namespace neon::assets {
class AssetManager;
}
namespace neon::gfx {
class Renderer;
}

namespace neon::scene {

// Runtime configuration for a headless playtest / game session. Everything is
// optional: a null AssetManager makes Draw a no-op (pure simulation, which is
// what headless servers and unit tests use). Scripts are read from disk under
// scriptBaseDir unless readScript overrides the source (pack readers, tests).
// assetBaseDir prefixes file-backed mesh/texture paths (obj:/gltf:/textures)
// so a packed game's unpacked directory works as the asset root; input wires
// the platform input state into the Lua InputAxis/InputKey/InputMouse bindings
// (null in headless hosts).
//
// Headless mode (servers, sim tests): set headless = true (and leave assets
// null) to skip building the draw list entirely — Start + Tick run the full
// data-driven scene (scripts + behavior trees + physics) with no renderer,
// window, or audio. Draw() stays a safe no-op whenever assets is null.
struct GameRuntimeConfig {
    assets::AssetManager* assets = nullptr; // mesh loading; null = sim-only
    std::string scriptBaseDir;              // base dir for script paths ("" = cwd)
    std::string assetBaseDir;               // base dir for obj:/gltf:/texture paths ("" = cwd)
    std::function<std::string(const std::string& path)> readScript; // optional override
    std::function<void(const std::string&)> playSfx; // optional audio sink for PlaySfx
    platform::IInput* input = nullptr;      // optional live input for scripts
    gfx::Font font2d;                       // 2D canvas font (on_render text); invalid = skip
    uint64_t rngSeed = 20260821u;           // fixed: playtest RNG is reproducible
    bool headless = false;                  // skip draw-list build; pure simulation
};

// A self-contained, reusable game runtime. The editor embeds one instance for
// in-editor playtest (F5); the packaged neon_game player reuses the same class
// (T4.7). Lifecycle: Start(sceneJson, cfg) -> Tick/Draw... -> Stop(); Start
// again after Stop starts a fresh world.
//
// SCRIPTING MODEL — one Lua host serves the whole runtime. Every SceneScript
// with backend "lua" is loaded once per unique path, then on_start(ent) runs at
// Start and on_update(ent, dt) runs every Tick for each entity carrying the
// script. GameVars is shared by scripts and behavior trees (single global
// store).
//
// GLOBAL NAMESPACE SHARING (important): because there is exactly one host, all
// scripts share one global Lua namespace. Per-entity dispatch is now safe for
// the standard handlers: each instance CAPTURES its own chunk's on_start and
// on_update at attach time, so a later-loaded script cannot shadow an earlier
// one for other entities (the captured function handle is called, not the
// global name). Other globals still collide (a shared helper variable or a
// second `on_player_join` is last-write-wins), and per-entity `script.vars`
// are set as globals and therefore overwrite each other across entities. Keep
// non-handler globals distinct.
//
// PHYSICS: `physics_.Step` runs every Tick, but the built-in scene registry has
// no collider component yet, so the world steps with zero bodies (inert).
// Rigidbody/spawn-body wiring is a later task; this note keeps downstream
// consumers (T4.7 neon_game, T6.3) from assuming physics is already live.
class GameRuntime {
public:
    core::Status Start(const std::string& sceneJson, GameRuntimeConfig cfg);
    void Tick(float dt);
    void Draw(gfx::Renderer& renderer, const gfx::Camera& camera);
    void Stop();

    bool Running() const { return running_; }
    ecs::World& World() { return world_; }
    script::GameVars& GameVars() { return scriptCtx_.gameVars; }
    // HUD helpers: finds a named scene entity (e.g. the hero) and reads its
    // health, plus a numeric GameVar read (0 when unset / non-numeric).
    ecs::Entity FindNamedEntity(const std::string& name);
    std::pair<float, float> EntityHealth(ecs::Entity ent) const;
    float GameVar(const std::string& name) const;
    // The bindings context scripts share (entityKinds, gameVars, input...).
    // Exposed for hosts that need to inspect script-spawned entity kinds (the
    // server's AOI replication, debug panels).
    script::ScriptContext& ScriptContext() { return scriptCtx_; }
    const script::ScriptContext& ScriptContext() const { return scriptCtx_; }

    // Stats for the editor profiler / debug panels.
    size_t EntityCount() const { return world_.EntityCount(); }
    size_t ScriptCount() const { return scripts_.size(); }
    size_t BehaviorTreeCount() const { return trees_.size(); }
    size_t DrawCount() const { return draws_.size(); }
    size_t PhysicsBodyCount() const { return physics_.BodyCount(); }
    double SimTime() const { return simTime_; }

    // Observability for tests/debug: the per-entity blackboard value the
    // behavior tree of `ent` wrote under `key` (Nil when the entity has no
    // tree or the key is unset).
    script::Value EntityBlackboardValue(const ecs::Entity& ent, const std::string& key) const;

    // Debug observability: the node path id most recently ticked by the
    // behavior tree of `ent` (bt::Context::activePath after the last tick; ""
    // when the entity has no tree or none has ticked yet). Powers the editor's
    // playtest highlight.
    std::string ActiveTreePath(const ecs::Entity& ent) const;

    // Debug observability: the mesh GameRuntime::Draw would submit for `ent`
    // given `camera` — the LOD level selected by camera distance when the
    // entity's SceneMesh carries a chain, else the single resolved mesh.
    // Invalid when `ent` has no resolved draw item (call Draw once first).
    gfx::Mesh MeshForEntity(const ecs::Entity& ent, const gfx::Camera& camera) const;

    // Combat (script-facing gameplay hooks). SpawnProjectile queues a fireball
    // the runtime advances and renders; MeleeAttack damages SceneHealth entities
    // in the arc and returns how many were hit; AttackBox damages every
    // SceneHealth entity inside a yaw-oriented box (half extents) and returns
    // how many were hit.
    void SpawnProjectile(const math::Vec3& pos, const math::Vec3& dir, float speed, float damage,
                         float life, ecs::Entity caster = {});
    int MeleeAttack(const math::Vec3& origin, const math::Vec3& dir, float range, float arcDeg,
                    float damage);
    int AttackBox(const math::Vec3& center, const math::Vec3& half, float yaw, float damage);
    void TickProjectiles(float dt);

    // Data-driven skills (M2 combat core). LoadSkills replaces the table;
    // CastSkill looks the skill up, checks cooldown/mana, casts it (projectile
    // / melee arc / attack box), applies the skill's status effects to every
    // hit target and records the cooldown for `caster`. Returns 1 when the
    // skill was cast, 0 when unknown / on cooldown / out of mana.
    bool LoadSkills(const std::string& json, std::string* err);
    int CastSkill(const std::string& name, const math::Vec3& origin, const math::Vec3& dir,
                  ecs::Entity caster = {});
    // Remaining cooldown seconds for `caster`'s skill (0 = ready / unknown).
    float SkillCooldownLeft(const std::string& name, ecs::Entity caster) const;

    // Status-effect observability (tests / HUD).
    bool HasStatus(ecs::Entity ent, uint32_t id) const;
    float StatusMagnitude(ecs::Entity ent, uint32_t id) const;

    // Script-function plumbing for hosts that drive the scene from outside
    // (the server's multi-player join flow): HasScriptFunction reports whether
    // the loaded scripts define `name`; CallScriptFunction invokes it with the
    // given args (true when the call ran and succeeded). Failures are logged
    // once per function per Start, never fatal.
    bool HasScriptFunction(const std::string& name) const;
    bool CallScriptFunction(const std::string& name,
                            const std::vector<script::Value>& args);

    // Spawns an entity with CTransformBind + kind (+ optional Lua script that
    // runs on_start/on_update for it). Used by the Spawn binding's 3rd arg
    // (multi-player player controllers) and by hosts that spawn programmatic
    // entities. Returns the new entity (invalid when the world refused).
    ecs::Entity SpawnEntity(const std::string& kind, const math::Vec3& pos,
                            const std::string& scriptPath = {});

private:
    struct ScriptInst {
        ecs::Entity ent;
        std::string path; // used for error logging; source is loaded once per path
        // Captured chunk function handles (0 = this chunk defines none). Each
        // instance calls ITS OWN chunk's handlers, so a later-loaded script
        // cannot shadow an earlier one (per-entity script isolation).
        uint64_t onStart = 0;
        uint64_t onUpdate = 0;
        bool errorLogged = false; // one log per script instance per Start
    };
    struct BtInst {
        ecs::Entity ent;
        std::unique_ptr<bt::BehaviorTree> tree;
        script::Blackboard board;
        // Persistent per-entity timer state. bt::Context owns its timers map
        // but is rebuilt per tick, so the map is parked here between ticks and
        // swapped in/out of the fresh Context (a Context cannot be stored: it
        // binds a GameVars reference and the board address may move).
        std::map<uint64_t, std::map<std::string, float>> timers;
        // bt::Context::activePath captured after the last Tick (debug highlight).
        std::string activePath;
    };
    struct DrawItem {
        ecs::Entity ent;
        std::string meshKey;
        // LOD chain spec from the entity's SceneMesh (data-driven: distance +
        // meshKey per level). Resolved into `chain` during ResolveDrawItem.
        std::vector<LodEntry> lod;
        gfx::Mesh mesh;
        gfx::LodChain chain; // resolved levels+thresholds; empty = single mesh
        gfx::Material mat;
        bool resolved = false;
        bool failed = false;
    };
    // A skill projectile (fireball): moved each tick, damages the first SceneHealth
    // entity within a small radius, and expires on time/travel-distance.
    struct Projectile {
        math::Vec3 pos;
        math::Vec3 dir;
        float speed = 0.0f;
        float damage = 0.0f;
        float life = 0.0f;    // seconds remaining
        float traveled = 0.0f; // distance travelled
        float range = 0.0f;    // max travel before expiring (0 = life-bounded only)
        float hitRadius = 0.8f;
        ecs::Entity caster;   // never damaged by its own projectile
        std::vector<SkillStatus> statuses; // applied to the hit target
    };

    void AttachScripts();
    // Attaches ONE script component to an entity (shared by AttachScripts and
    // SpawnEntity/Spawn's 3rd arg). Returns false when skipped (missing file,
    // compile error, unsafe path, previous failure).
    bool AttachOneScript(ecs::Entity ent, const SceneScript& s);
    void AttachTrees();
    // Advances every entity's StatusComponent (damage/heal ticks + expiry).
    void TickStatuses(float dt);
    // Decays per-caster skill cooldowns and prunes dead casters.
    void TickSkillCooldowns(float dt);
    // Flushes the script 2D canvas (draw2d_) into the renderer overlay.
    void FlushDraw2D(gfx::Renderer& renderer);
    // Applies a skill's status effects to `target` (creates the component).
    void ApplySkillStatuses(ecs::Entity target, const std::vector<SkillStatus>& statuses);
    // Damages `target` (clamped to 0) and applies the skill's statuses.
    void ApplyHit(ecs::Entity target, float damage, const std::vector<SkillStatus>& statuses);
    // Loads every prefabs/*.json under cfg_.scriptBaseDir into prefs_ (no-op
    // when the base dir is empty or the prefabs dir is absent). Scene entities
    // can then reference prefabs by name, matching how packed games ship them.
    void LoadPrefabs();
    void BuildDrawList();
    void ResolveDrawItem(DrawItem& item, gfx::Renderer& renderer);
    // Resolves one meshKey ("obj:"/"gltf:" file-backed or a procedural
    // primitive) through the runtime's AssetManager; invalid mesh on failure.
    gfx::Mesh ResolveMeshKey(gfx::Renderer& renderer, const std::string& key);
    // Invokes one of the instance's captured chunk functions and logs the
    // first failure of the script instance (throttled per Start); failures
    // never abort the runtime. Sets the per-entity input routing context.
    // `fn` names the handler for the log ("on_start" / "on_update").
    void CallEntityFunctionHandle(ScriptInst& inst, uint64_t handle,
                                  const char* fn, const std::vector<script::Value>& args);
    std::string ReadScript(const std::string& path) const;
    std::string FullScriptPath(const std::string& path) const;
    // Resolves an asset reference (obj:/gltf:/texture path) against
    // cfg_.assetBaseDir; absolute paths and empty base pass through unchanged.
    std::string FullAssetPath(const std::string& path) const;

    ecs::World world_;
    physics::World physics_;
    script::ScriptContext scriptCtx_; // owns the GameVars scripts + BT share
    PrefabLibrary prefs_;             // prefabs loaded from <scriptBaseDir>/prefabs/
    std::unique_ptr<script::IScriptHost> host_; // one Lua host, deterministic RNG
    std::vector<ScriptInst> scripts_;
    std::vector<BtInst> trees_;
    std::vector<DrawItem> draws_;
    std::vector<Projectile> projectiles_;
    SkillTable skills_;
    std::vector<script::Draw2DCmd> draw2d_; // script 2D canvas (on_render)
    std::set<uint64_t> hiddenEntities_;     // SetVisible hide list (EntityKey)
    // Per-caster (EntityKey) skill cooldown seconds by skill name.
    std::unordered_map<uint64_t, std::map<std::string, float>> skillCooldowns_;
    gfx::Mesh fireballMesh_; // lazily built for skill-projectile rendering
    std::set<std::string> loadedScripts_; // resolved paths whose chunk ran (presence only)
    std::set<std::string> scriptFailed_;  // resolved paths that failed (skip later)
    GameRuntimeConfig cfg_;
    bool running_ = false;
    double simTime_ = 0.0;
};

} // namespace neon::scene
