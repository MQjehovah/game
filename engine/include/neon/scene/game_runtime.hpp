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
#include "neon/core/localization.hpp"
#include "neon/core/result.hpp"
#include "neon/ecs/world.hpp"
#include "neon/gfx/camera.hpp"
#include "neon/gfx/font.hpp"
#include "neon/gfx/material.hpp"
#include "neon/gfx/mesh.hpp"
#include "neon/math/bvh.hpp"
#include "neon/plugin/runtime_plugin.hpp"
#include "neon/physics/physics.hpp"
#include "neon/physics/jolt_world.hpp"
#include "neon/platform/input.hpp"
#include "neon/scene/scene_file.hpp"
#include "neon/scene/skinned_model.hpp"
#include "neon/scene/skills.hpp"
#include "neon/scene/status.hpp"
#include "neon/script/bindings.hpp"
#include "neon/script/gamevars.hpp"
#include "neon/script/script.hpp"
#include "neon/ui/document.hpp"

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
    std::string localesDir;                 // locales/*.json tables for Loc()
    std::function<std::string(const std::string& path)> readScript; // optional override
    std::function<void(const std::string&)> playSfx; // optional audio sink for PlaySfx
    // P2-2 audio hooks (host-owned backend; null -> script no-ops).
    std::function<void(const std::string&, float)> playMusic;
    std::function<void(const std::string&, const math::Vec3&)> playSfx3D;
    std::function<void(const math::Vec3&, const math::Vec3&)> setAudioListener;
    std::function<void(int, float)> setBusVolume;
    platform::IInput* input = nullptr;      // optional live input for scripts
    gfx::Font font2d;                       // 2D canvas font (on_render text); invalid = skip
    uint64_t rngSeed = 20260821u;           // fixed: playtest RNG is reproducible
    bool headless = false;                  // skip draw-list build; pure simulation
    // Physics backend: "custom" (deterministic custom sphere/AABB solver) or
    // "jolt" (Jolt rigid bodies; compiled when NEON_ENABLE_JOLT). The packaged
    // player and the authoritative server default to "jolt" so client
    // prediction and server simulation run the same rigid-body code; "custom"
    // remains the cross-platform bit-exact fallback. Unknown values fall back
    // to "custom".
    std::string physicsBackend = "custom";
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
// PHYSICS: every entity with a `rigidbody` component is registered with the
// physics world at Start; Tick advances the world on a fixed 60 Hz step and
// writes the resulting positions back into the entities' transforms. Scripts
// can also spawn ad-hoc bodies (PhysicsAddSphere/PhysicsAddBox) and query
// collisions/raycasts through the bindings.
class GameRuntime {
public:
    core::Status Start(const std::string& sceneJson, GameRuntimeConfig cfg);
    void Tick(float dt);
    void Draw(gfx::Renderer& renderer, const gfx::Camera& camera);
    // Draws the data-driven UI document (UIShow / ui/*.ui.json) on top of the
    // composited frame. Call AFTER Renderer::EndScene so menus/HUD keep their
    // authored colors instead of being ACES tone-mapped with the 3D scene.
    void DrawUI(gfx::Renderer& renderer);
    void Stop();

    // Data-driven UI document (UIShow / ui/*.ui.json): rendered on top of the
    // 2D canvas every frame; button clicks are edge-triggered per frame.
    bool ShowUI(const std::string& path);
    void HideUI();
    bool UIClicked(const std::string& name) const {
        return uiClickedNames_.count(name) != 0;
    }
    void UISetText(const std::string& name, const std::string& text);
    void UISetFill(const std::string& name, float fill);
    void UISetVisible(const std::string& name, bool visible);

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
    // P1-2 debugger passthrough for the editor playtest.
    // The Lua host (the canonical debugger backend; the editor's breakpoint /
    // step UI talks to it). JS scripts route internally through the same
    // IScriptHost interface.
    script::IScriptHost* ScriptHost() { return hosts_.lua.get(); }
    script::IScriptHost* ScriptHost(const std::string& backend) {
        return hosts_.Get(backend);
    }
    // Runtime plugin manager (gameplay/system modules loaded from
    // <scriptBaseDir>/plugins). Null until Start() and after Stop().
    plugin::RuntimePluginManager* PluginManager() { return plugins_.get(); }
    const plugin::RuntimePluginManager* PluginManager() const { return plugins_.get(); }
    // Dispatches a named event to every subscribed runtime plugin (the server
    // uses this for player_join etc.). No-op when no plugins are loaded.
    bool DispatchPluginEvent(const std::string& name,
                             const std::vector<script::Value>& args = {});
    // Runs a command registered by a runtime plugin. Returns false when the
    // command is unknown or its handler raised.
    bool RunPluginCommand(const std::string& name,
                          const std::vector<script::Value>& args = {},
                          std::string* error = nullptr);
    // Instantiates a prefab (prefabs/<name>.json) at `pos` into the live
    // world, attaching its script components and running on_start like a
    // scene-placed entity. Returns the new entity (invalid on failure). The
    // SpawnPrefab binding calls this; game scripts use it for dynamic content
    // (projectiles, spawned units, pickups).
    ecs::Entity SpawnPrefab(const std::string& name, const math::Vec3& pos);
    bool DebuggerPaused() const {
        return hosts_.lua && hosts_.lua->DebuggerPaused();
    }

    // Stats for the editor profiler / debug panels.
    size_t EntityCount() const { return world_.EntityCount(); }
    size_t ScriptCount() const { return scripts_.size(); }
    size_t BehaviorTreeCount() const { return trees_.size(); }
    size_t DrawCount() const { return draws_.size(); }
    size_t PhysicsBodyCount() const { return physics_ ? physics_->BodyCount() : 0; }
    physics::World& PhysicsWorld() { return *physics_; }
    const physics::World& PhysicsWorld() const { return *physics_; }
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
        // The backend host this instance's chunk runs on (resolved from the
        // component's `backend` field: "lua" / "js"). Null never happens for
        // an attached instance.
        script::IScriptHost* host = nullptr;
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
        // The host run_script/script_bool nodes call through (the tree
        // entity's script backend, defaulting to Lua).
        script::IScriptHost* host = nullptr;
    };
    struct DrawItem {
        ecs::Entity ent;
        std::string meshKey;
        // 2D sprite: texture path + flips. When isSprite is true the item
        // draws an XY quad with an unlit texture material instead of a mesh.
        bool isSprite = false;
        std::string spriteTex;
        bool flipX = false;
        bool flipY = false;
        math::Vec3 tileOffset{};  // P1-1: per-cell offset for tilemap quads
        // P2-1 ground decal: draws a flat XZ-plane quad with the texture.
        bool isDecal = false;
        float decalSize = 2.0f;
        // LOD chain spec from the entity's SceneMesh (data-driven: distance +
        // meshKey per level). Resolved into `chain` during ResolveDrawItem.
        std::vector<LodEntry> lod;
        gfx::Mesh mesh;
        gfx::LodChain chain; // resolved levels+thresholds; empty = single mesh
        gfx::Material mat;
        // Animated skinned glTF (meshKey "gltf:...") resolved once; when set,
        // drawing uses the skinned parts + bone matrices instead of `mesh`.
        std::shared_ptr<SkinnedModel> skinned;
        bool resolved = false;
        bool failed = false;
    };
    // Snapshot of the active 2D design-space mapping (captured during Draw,
    // when the host's 2D viewport is live). InputMousePos()/UIClicked() use
    // this between renders so coordinates stay in design units even after the
    // renderer resets its 2D mapping at the end of the frame.
    float uiScale_ = 1.0f;
    math::Vec2 uiOffset_{0.0f, 0.0f};
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
    // Behavior-tree script hook: invokes the named global function on the
    // tree entity's backend host (run_script / script_bool nodes). Returns
    // Nil when the function is missing or the call failed.
    script::Value CallScriptOnTree(const BtInst& inst, const std::string& fn,
                                   uint64_t ent);
    // Advances every entity's StatusComponent (damage/heal ticks + expiry).
    void TickStatuses(float dt);
    // Advances every resolved skinned model's default clip (fixed-step
    // animation; deterministic like the rest of the simulation).
    void TickAnimations(float dt);
    // Decays per-caster skill cooldowns and prunes dead casters.
    void TickSkillCooldowns(float dt);
    // Flushes the script 2D canvas (draw2d_) into the renderer overlay.
    void FlushDraw2D(gfx::Renderer& renderer);
    // Scene-tree world transform: walks SceneParentLink ancestors (bounded
    // depth) composing local TRS. Identity for unlinked entities.
    math::Mat4 LocalToWorld(ecs::Entity e) const;
    // Applies a skill's status effects to `target` (creates the component).
    void ApplySkillStatuses(ecs::Entity target, const std::vector<SkillStatus>& statuses);
    // Damages `target` (clamped to 0) and applies the skill's statuses.
    void ApplyHit(ecs::Entity target, float damage, const std::vector<SkillStatus>& statuses);
    // Loads every prefabs/*.json under cfg_.scriptBaseDir into prefs_ (no-op
    // when the base dir is empty or the prefabs dir is absent). Scene entities
    // can then reference prefabs by name, matching how packed games ship them.
    void LoadPrefabs();
    void LoadLocales(); // locales/*.json string tables for Loc()
    void BuildDrawList();
    void ResolveDrawItem(DrawItem& item, gfx::Renderer& renderer);
    // Resolves one meshKey ("obj:"/"gltf:" file-backed or a procedural
    // primitive) through the runtime's AssetManager; invalid mesh on failure.
    gfx::Mesh ResolveMeshKey(gfx::Renderer& renderer, const std::string& key,
                             const SceneTerrain* terrain = nullptr);
    // Invokes one of the instance's captured chunk functions and logs the
    // first failure of the script instance (throttled per Start); failures
    // never abort the runtime. Sets the per-entity input routing context.
    // `fn` names the handler for the log ("on_start" / "on_update").
    void CallEntityFunctionHandle(ScriptInst& inst, uint64_t handle,
                                  const char* fn, const std::vector<script::Value>& args);
    void RegisterSceneBodies();
    void RegisterCharacters();
    void SyncSceneBodies();
    void TickTweens(float dt);
    std::string ReadScript(const std::string& path) const;
    std::string FullScriptPath(const std::string& path) const;
    // Resolves an asset reference (obj:/gltf:/texture path) against
    // cfg_.assetBaseDir; absolute paths and empty base pass through unchanged.
    std::string FullAssetPath(const std::string& path) const;

    ecs::World world_;
    std::unique_ptr<physics::World> physics_;
    float physicsAccum_ = 0.0f; // fixed-step accumulator (60 Hz)
    script::ScriptContext scriptCtx_; // owns the GameVars scripts + BT share
    PrefabLibrary prefs_;             // prefabs loaded from <scriptBaseDir>/prefabs/
    core::Localization loc_;          // string tables loaded from cfg_.localesDir
    // Dual script backends: Lua + QuickJS (ES2020). Scene scripts pick a
    // backend via the script component's `backend` field; both hosts share the
    // same bindings, RNG seed and sim clock, so one scene can mix languages.
    struct ScriptHosts {
        std::unique_ptr<script::IScriptHost> lua;
        std::unique_ptr<script::IScriptHost> js;
        script::IScriptHost* Get(const std::string& backend) {
            return backend == "js" ? js.get() : lua.get();
        }
        const script::IScriptHost* Get(const std::string& backend) const {
            return backend == "js" ? js.get() : lua.get();
        }
        script::IScriptHost* First() { return lua.get(); }
    } hosts_;
    std::vector<ScriptInst> scripts_;
    std::vector<BtInst> trees_;
    std::vector<DrawItem> draws_;
    // Instanced-batching scratch for opaque static meshes (per-frame reuse):
    // each batch groups entities with the same mesh + material so N identical
    // entities cost one instanced draw call instead of N. Flushed whenever a
    // non-batchable item (sprite / skinned / transparent / custom shader)
    // interrupts the run, preserving the original draw order.
    struct DrawBatch {
        gfx::Mesh mesh;
        gfx::Material mat;
        uint32_t start = 0;
        uint32_t count = 0;
    };
    std::vector<DrawBatch> drawBatches_;
    std::vector<math::Mat4> batchModels_;
    // G1-2 spatial index: per-frame BVH over batchable draw items, used to
    // pre-cull the camera frustum before instanced draws (id = draw index).
    math::Bvh drawBvh_;
    std::vector<uint8_t> bvhVisible_;
    // Sprite sort scratch (reused instead of a fresh allocation every frame).
    std::vector<size_t> drawOrder_;
    std::vector<Projectile> projectiles_;
    // P1-3 tweens: Lua `Tween(ent, prop, from, to, time, easing)` calls append
    // here; TickTweens advances them every frame and writes into the entity's
    // SceneTransform. prop: 0=pos 1=rot(euler degrees) 2=scale.
    struct Tween {
        ecs::Entity target;
        int prop = 0;
        math::Vec3 from{};
        math::Vec3 to{};
        float time = 1.0f;
        float elapsed = 0.0f;
        int easing = 0;  // 0=linear 1=in 2=out 3=inout
    };
    std::vector<Tween> tweens_;
    SkillTable skills_;
    std::unique_ptr<plugin::RuntimePluginManager> plugins_; // runtime plugins
    scene::ComponentRegistry compReg_; // built-in + data component factories
    std::vector<script::Draw2DCmd> draw2d_; // script 2D canvas (on_render)
    std::unique_ptr<ui::UiDocument> uiDoc_; // data-driven UI document
    std::set<std::string> uiClickedNames_;  // buttons clicked since last Draw
    std::set<uint64_t> hiddenEntities_;     // SetVisible hide list (EntityKey)
    script::InputMap inputMap_;             // Godot-style actions (input.json)
    std::string pendingScene_;              // ChangeScene deferred to next Tick
    std::vector<std::pair<std::string, uint64_t>> signalHandlers_; // Lua signals
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
