#pragma once

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "neon/bt/behavior_tree.hpp"
#include "neon/core/localization.hpp"
#include "neon/core/result.hpp"
#include "neon/ecs/system_scheduler.hpp"
#include "neon/ecs/world.hpp"
#include "neon/gfx/camera.hpp"
#include "neon/gfx/font.hpp"
#include "neon/gfx/material.hpp"
#include "neon/gfx/mesh.hpp"
#include "neon/math/bvh.hpp"
#include "neon/plugin/runtime_plugin.hpp"
#include "neon/plugin/backend.hpp"
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
#include "neon/ui/ui_system.hpp"

namespace neon::assets {
class AssetManager;
class AssetVariantTable;
}
namespace neon::gfx {
class Renderer;
}
namespace neon::io {
class IFileSystem;
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
    // Replaceable UI system (IUiSystem): the engine talks to game UI ONLY
    // through this seam. Null = the default document-backed system
    // (ui/*.ui.json + pluggable layout solver). Inject a custom implementation
    // to swap the whole UI stack without touching scripts or the editor.
    // shared_ptr keeps the config copyable (ChangeScene restarts reuse it).
    std::shared_ptr<ui::IUiSystem> uiSystem;
    uint64_t rngSeed = 20260821u;           // fixed: playtest RNG is reproducible
    bool headless = false;                  // skip draw-list build; pure simulation
    // Physics backend: "custom" (deterministic custom sphere/AABB solver) or
    // "jolt" (Jolt rigid bodies; compiled when NEON_ENABLE_JOLT). The packaged
    // player and the authoritative server default to "jolt" so client
    // prediction and server simulation run the same rigid-body code; "custom"
    // remains the cross-platform bit-exact fallback. Unknown values fall back
    // to "custom". A "plugin:<name>" value (G5-1) loads the physics backend
    // from a native plugin found under pluginBaseDir/plugins (see
    // plugin::LoadNativePhysicsBackend); the plugin ships the solver as
    // middleware DLL/SO, so the backend can be swapped without relinking.
    std::string physicsBackend = "custom";
    // Base directory scanned for native backend plugins (G5-1). Empty = no
    // plugin dir, so "plugin:*" backends simply fall back to custom.
    std::string pluginBaseDir;
    // G7-1: optional virtual file system (pack + Mod mount stack). When set,
    // script-family reads (scripts/behaviors/prefabs/locales/input.json)
    // resolve through it with virtual paths (the scriptBaseDir prefix is
    // stripped), so a packed game reads scripts straight from the pack and
    // Mod layers override them — no unpacked-dir copy needed.
    neon::io::IFileSystem* fileSystem = nullptr;
    // G6-1: optional platform/LOD asset variant table. When set, every asset
    // path is resolved through it (logical -> concrete file) before loading,
    // so "mobile"/"pc" variants are pure data (see neon/assets/asset_variants.hpp).
    const assets::AssetVariantTable* variantTable = nullptr;
    // G6-2: async mesh streaming. When set, file-backed mesh entities (obj:/gltf:)
    // load off the main thread (LoadMeshOBJAsync/LoadGLTFAsync); the draw item
    // resolves from the cache the frame it becomes ready and is skipped until
    // then — no per-draw hitch. The host must pump the async loader each frame.
    bool asyncMeshLoad = false;
    // G5-4-4(项1): run the per-frame component sub-tasks (tweens/animations/
    // statuses/skill cooldowns/projectiles) through the ecs::SystemScheduler
    // in parallel mode. Default false = serial in registration order (the
    // deterministic reference path). True lets independent systems overlap on
    // the shared worker pool; conflict edges come from their declared component
    // reads/writes, so the parallel result is bit-identical to serial.
    bool parallelSystems = false;
    // Optional skills.json text (data-driven CastSkill table). The hosts that
    // know their project dir load <dir>/skills.json and pass it here; empty
    // leaves the table empty and CastSkill logs "unknown skill".
    std::string skillsJson;
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
    // `previewZoom` lets an editor host zoom the WHOLE runtime view (sprites +
    // UI) by dividing the scene camera's orthographic size, matching what the
    // host passes to its 2D overlay (default 1 = no zoom).
    void Draw(gfx::Renderer& renderer, const gfx::Camera& camera,
              float previewZoom = 1.0f);
    // Draws the data-driven UI document (UIShow / ui/*.ui.json) on top of the
    // composited frame. Call AFTER Renderer::EndScene so menus/HUD keep their
    // authored colors instead of being ACES tone-mapped with the 3D scene.
    void DrawUI(gfx::Renderer& renderer);
    void Stop();

    // Data-driven UI document (UIShow / ui/*.ui.json): rendered on top of the
    // 2D canvas every frame; button clicks are edge-triggered per frame.
    // All calls forward to the replaceable IUiSystem (cfg_.uiSystem).
    bool ShowUI(const std::string& path);
    void HideUI();
    bool UIClicked(const std::string& name) const {
        return ui_ && ui_->Clicked(name);
    }
    void UISetText(const std::string& name, const std::string& text);
    void UISetFill(const std::string& name, float fill);
    void UISetVisible(const std::string& name, bool visible);
    void UISetColor(const std::string& name, float r, float g, float b, float a);

    // --- M1: per-entity animation + world-screen HUD anchors ---------------
    // Plays a named clip (substring, case-insensitive) on a skinned entity's
    // OWN instance state (the shared SkinnedModel is untouched, so many
    // entities can run different clips from one file). Returns false when
    // the entity has no skinned model or no clip matches.
    bool PlayAnimation(ecs::Entity e, const std::string& clip, bool loop = true,
                       float crossFade = 0.2f, float speed = 1.0f);
    // Normalized [0,1] progress of the entity's override clip (0 when none,
    // 1 when a one-shot finished). Poll from on_update to chain attacks.
    float AnimationProgress(ecs::Entity e) const;
    // True while an override clip exists AND has finished (one-shots only).
    bool AnimationFinished(ecs::Entity e) const;
    // Projects a world position into the CURRENT 2D design space (the same
    // mapping on_render draws in). Returns false when the point is behind
    // the camera. Used by scripts for overhead HP bars / nameplates.
    bool WorldToScreen(const math::Vec3& world, float& outX, float& outY) const;
    // Inverse mapping for the axis-aligned ortho 2D camera (viewport pixels ->
    // world XY). Returns false for perspective cameras / before the first Draw.
    bool ScreenToWorld(const math::Vec2& screen, float& outX, float& outY) const;
    // Live viewport width in pixels (GetViewportSize; kept for convenience).
    float DesignWidth() const;
    // Spawns a floating combat-text particle anchored to a world position:
    // the runtime tracks it (rise + fade over `life` seconds) and exposes it
    // to on_render via FloatTexts(). crit scales the text and tints it.
    void SpawnFloatText(const math::Vec3& world, const std::string& text, bool crit = false,
                        float life = 1.2f);
    struct FloatText {
        math::Vec3 world;
        std::string text;
        bool crit = false;
        float life = 1.0f;      // total lifetime
        float age = 0.0f;       // elapsed
    };
    const std::vector<FloatText>& FloatTexts() const { return floatTexts_; }
    // Entity screen anchors cached during the LAST Draw: {entityKey, x, y,
    // onscreen} in design units. Scripts iterate it to draw overhead bars.
    struct ScreenAnchor {
        uint64_t entity;
        float x = 0.0f, y = 0.0f;
        bool onscreen = false;
        math::Vec3 world;
    };
    const std::vector<ScreenAnchor>& ScreenAnchors() const { return screenAnchors_; }
    // Per-entity overhead metadata scripts can stamp when anchoring bars:
    // name + hp fraction (0..1). Set via SetEntityPlate(e, name, hpFrac).
    void SetEntityPlate(ecs::Entity e, const std::string& name, float hpFrac);
    struct EntityPlate {
        std::string name;
        float hpFrac = -1.0f; // <0 = hidden
    };
    const std::map<uint64_t, EntityPlate>& EntityPlates() const { return plates_; }

    bool Running() const { return running_; }
    ecs::World& World() { return world_; }
    script::GameVars& GameVars() { return scriptCtx_.gameVars; }
    // G1-3 scene-tree API: direct children / all descendants of an entity
    // (via the resolved SceneParentLink graph). O(n) per call; intended for
    // tooling, queries and tests, not per-frame hot paths.
    std::vector<ecs::Entity> GetChildren(ecs::Entity parent);
    std::vector<ecs::Entity> GetDescendants(ecs::Entity root);
    // G1-3 world-transform cache: rebuilds the parent-before-child pass from
    // the current tree (arbitrary depth, no 8-level cap). Call after any
    // transform-affecting mutation and before reads that need world matrices;
    // GameRuntime::Draw does this automatically every frame.
    void RebuildWorldTransforms();
    // World matrix from the cache (identity when unknown / no transform).
    // Prefer over LocalToWorld inside a frame after RebuildWorldTransforms().
    math::Mat4 CachedLocalToWorld(ecs::Entity e) const;
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

    // G5-4-4(项2) data-driven animation state machine (.asm.json). AttachStateMachine
    // loads the asset (VFS-aware), binds each state's clip name to the entity's
    // skinned model, and TickAnimations advances it — the current state's clip
    // plays through the existing override path. SetAnimParam drives transitions.
    bool AttachStateMachine(ecs::Entity e, const std::string& path);
    void SetAnimParam(ecs::Entity e, const std::string& name, float value);

    // G5-4-4: flushes the script 2D canvas (on_render) into the renderer's 2D
    // overlay. The host MUST call this AFTER EndScene (the scene composited) so
    // the HUD keeps its authored colors instead of being tone-mapped with the
    // 3D scene. No-op when on_render produced nothing.
    void FlushCanvas(gfx::Renderer& renderer);

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

    // G3-4 lag compensation (authoritative server). The runtime records every
    // fixed tick's authoritative poses into a ring buffer
    // (kLagCompHistoryTicks deep). MeleeAttackLagComp / AttackBoxLagComp test
    // targets at the pose they had `rewindTicks` fixed ticks ago (while
    // damaging the CURRENT entity), so a fast-moving target is hit where the
    // shooter actually SAW it instead of where it is now. SetAutoLagComp makes
    // the plain MeleeAttack/AttackBox/CastSkill use that rewind - the server
    // sets it per tick from the most latent active client's measured RTT.
    // LagCompPosition queries a single entity's historical pose (false when no
    // snapshot that old exists for it).
    static constexpr uint32_t kLagCompHistoryTicks = 64;
    void SetAutoLagComp(uint32_t rewindTicks) { autoRewindTicks_ = rewindTicks; }
    uint32_t AutoLagCompTicks() const { return autoRewindTicks_; }
    bool LagCompPosition(ecs::Entity e, uint32_t rewindTicks, math::Vec3& out) const;
    int MeleeAttackLagComp(const math::Vec3& origin, const math::Vec3& dir, float range,
                           float arcDeg, float damage, uint32_t rewindTicks);
    int AttackBoxLagComp(const math::Vec3& center, const math::Vec3& half, float yaw,
                         float damage, uint32_t rewindTicks);

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
        // A6: per-instance snapshot of the component's declared `vars`. The
        // values are injected into the host globals right before each call and
        // read back after, so two entities with the same script no longer
        // overwrite each other's declared vars ("last attach wins" bug).
        script::Value vars;
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
        // Sequence-frame sprite animation: frames/fps/loop copied from the
        // SceneSprite component; Draw advances the clock and swaps spriteTex.
        std::vector<std::string> spriteFrames;
        float spriteFps = 0.0f;
        bool spriteLoop = true;
        float spriteAnimTime = 0.0f;
        int spriteFrame = -1;
        // Spritesheet variant: one horizontal atlas texture, `sheetFrames`
        // equal sub-rects; the quad's UV window is rebuilt per frame.
        std::string sheetTex;
        int sheetFrames = 0;
        math::Vec3 tileOffset{};  // P1-1: per-cell offset for tilemap quads
        // P2-1 ground decal: draws a flat XZ-plane quad with the texture.
        bool isDecal = false;
        float decalSize = 2.0f;
        // LOD chain spec from the entity's SceneMesh (data-driven: distance +
        // meshKey per level). Resolved into `chain` during ResolveDrawItem.
        std::vector<LodEntry> lod;
        // G2-3 chunked-LOD terrain: this item is one patch of a grid-split
        // terrain. The mesh carries its own local position (verts already span
        // the patch), so the entity transform places it; `chunkCenterLocal` is
        // used only for camera-distance LOD selection (per-patch, not per
        // entity). Off for ordinary draw items.
        bool isTerrainChunk = false;
        int chunkGridX = 0;
        int chunkGridZ = 0;
        int chunkGridDiv = 0;
        math::Vec3 chunkCenterLocal{0.0f, 0.0f, 0.0f};
        gfx::Mesh mesh;
        gfx::LodChain chain; // resolved levels+thresholds; empty = single mesh
        gfx::Material mat;
        // Animated skinned glTF (meshKey "gltf:...") resolved once; when set,
        // drawing uses the skinned parts + bone matrices instead of `mesh`.
        std::shared_ptr<SkinnedModel> skinned;
        // M1 per-entity animation state (from SceneAnimOverride): plays
        // `animClip` on this item instead of the model's shared default loop.
        // The shared SkinnedModel is never mutated - two wolves can play
        // different clips from one loaded file.
        const anim::AnimationClip* animClip = nullptr; // resolved clip ptr
        std::string animName;                          // requested name (substring)
        bool animLoop = true;
        float animSpeed = 1.0f;
        float animTime = 0.0f;
        float animFade = 0.0f;       // remaining cross-fade seconds
        float animFadeTotal = 0.0f;
        bool animHasOverride = false;
        anim::Pose animFromPose;     // fade source (captured at switch)
        bool resolved = false;
        bool failed = false;
        bool asyncPending = false;   // G6-2: mesh load kicked, waiting on cache
        // G5-4-4(项2): data-driven animation state machine (AttachStateMachine).
        // TickAnimations advances it and maps the current state's clip onto the
        // existing animClip/animTime override path (no pose surgery).
        std::shared_ptr<anim::AnimationStateMachine> animSM;
        std::string animSMState;                 // last played state
        bool animSMBound = false;                // clips resolved
        std::map<std::string, float> animSMParams; // script-set params
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
    // G2-3 vegetation field attached to a terrain entity: deterministic scatter
    // positions plus lazily-resolved plant + impostor meshes. Built once per
    // entity per Start (terrain heights are static during play).
    struct VegField {
        ecs::Entity ent;
        std::vector<math::Vec3> positions;
        gfx::Mesh mesh;     // full plant/bush/rock mesh (near instances)
        gfx::Mesh impostor; // billboard card (far instances)
        gfx::Material mat;
        gfx::Material impostorMat;
        float size = 1.0f;
        float impostorDistance = 60.0f;
        bool built = false;
        bool failed = false;
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
    // B6: rebuild drawKeys_ from draws_ (called at the end of BuildDrawList).
    void SyncDrawKeys();
    void ResolveDrawItem(DrawItem& item, gfx::Renderer& renderer);
    // G6-2: async-aware item resolution — retries an asyncPending item from the
    // cache when its mesh is ready, else skips it; non-async items resolve
    // synchronously. Called from the draw passes.
    void ResolveOrSkip(DrawItem& item, gfx::Renderer& renderer);
    // Resolves one meshKey ("obj:"/"gltf:" file-backed or a procedural
    // primitive) through the runtime's AssetManager; invalid mesh on failure.
    gfx::Mesh ResolveMeshKey(gfx::Renderer& renderer, const std::string& key,
                             const SceneTerrain* terrain = nullptr);
    // Renders every terrain entity's vegetation field: instanced plant meshes
    // for near instances plus yaw-billboard impostors past the swap distance.
    void DrawVegetation(gfx::Renderer& renderer, const gfx::Camera& camera);
    // Resolves a vegetation meshKey ("tree"/"bush"/"rock"/obj:/gltf:) to a mesh.
    gfx::Mesh VegetationMesh(gfx::Renderer& renderer, const std::string& meshKey);
    // Invokes one of the instance's captured chunk functions and logs the
    // first failure of the script instance (throttled per Start); failures
    // never abort the runtime. Sets the per-entity input routing context.
    // `fn` names the handler for the log ("on_start" / "on_update").
    void CallEntityFunctionHandle(ScriptInst& inst, uint64_t handle,
                                  const char* fn, const std::vector<script::Value>& args);
    void RegisterSceneBodies();
    void RegisterCharacters();
    void RegisterAudioSources(); // G8-3: play SceneAudioSource components once
    void SyncSceneBodies();
    void TickTweens(float dt);
    // G3-4 shared hit-test cores: `rewindTicks` selects the pose used for the
    // distance/arc/box test (0 = the entity's current pose). Damage always
    // lands on the current entity. `exclude` is skipped (CastSkill's never
    // self-hit rule); non-empty `statuses` are applied via ApplyHit.
    int MeleeAttackImpl(const math::Vec3& origin, const math::Vec3& dir, float range,
                        float arcDeg, float damage, uint32_t rewindTicks,
                        ecs::Entity exclude = {},
                        const std::vector<SkillStatus>& statuses = {});
    int AttackBoxImpl(const math::Vec3& center, const math::Vec3& half, float yaw,
                      float damage, uint32_t rewindTicks);
    std::string ReadScript(const std::string& path) const;
    std::string FullScriptPath(const std::string& path) const;
    // Resolves an asset reference (obj:/gltf:/texture path) against
    // cfg_.assetBaseDir; absolute paths and empty base pass through unchanged.
    std::string FullAssetPath(const std::string& path) const;

    ecs::World world_;
    std::unique_ptr<plugin::PhysicsBackend> pluginPhysics_;   // native backend owner (G5-1)
    std::unique_ptr<physics::World, std::function<void(physics::World*)>> physics_;
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
    // B6: alive-entity index over draws_ (EntityKey -> tracked). BuildDrawList
    // used to linear-scan draws_ per candidate entity (O(N*M) per frame).
    std::unordered_set<uint64_t> drawKeys_;
    // M1 HUD state: floating combat texts (world-anchored) + per-frame
    // entity screen anchors + script-stamped overhead plates.
    std::vector<FloatText> floatTexts_;
    std::vector<ScreenAnchor> screenAnchors_;
    std::map<uint64_t, EntityPlate> plates_;
    math::Mat4 lastViewProj_;   // captured in Draw for WorldToScreen
    bool lastViewProjValid_ = false;
    gfx::Camera lastCam_;       // resolved camera + viewport snapshot (Draw)
    bool lastCamValid_ = false;
    float lastAspect_ = 16.0f / 9.0f;
    float lastVpW_ = 1280.0f;   // scene viewport pixels (UI/world screen space)
    float lastVpH_ = 720.0f;
    // G2-3 vegetation cache (EntityKey -> VegField); rebuilt lazily per Start.
    std::unordered_map<uint64_t, VegField> vegCache_;
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
    // G1-3 world-transform cache (EntityKey -> world matrix), rebuilt by
    // RebuildWorldTransforms() and consumed by CachedLocalToWorld / Draw.
    std::unordered_map<uint64_t, math::Mat4> worldTransforms_;
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
    std::shared_ptr<ui::IUiSystem> ui_;     // replaceable game UI system
    std::set<uint64_t> hiddenEntities_;     // SetVisible hide list (EntityKey)
    script::InputMap inputMap_;             // Godot-style actions (input.json)
    std::string pendingScene_;              // ChangeScene deferred to next Tick
    std::vector<std::pair<std::string, uint64_t>> signalHandlers_; // Lua signals
    // Per-caster (EntityKey) skill cooldown seconds by skill name.
    std::unordered_map<uint64_t, std::map<std::string, float>> skillCooldowns_;
    // G3-4 pose history for lag compensation: one snapshot per fixed tick
    // (EntityKey -> authoritative position), index 0 = oldest. The newest
    // snapshot is appended at the end of every Tick and the ring is capped at
    // kLagCompHistoryTicks (~1 second at 60Hz).
    // G3-4/B10: lag-comp pose history as a fixed-capacity RING of reusable
    // snapshot maps (one per history tick). The old vector<unordered_map>
    // heap-allocated a new map every tick and moved the whole ring on eviction.
    std::vector<std::unordered_map<uint64_t, math::Vec3>> poseSlots_;
    size_t poseHead_ = 0;   // slot index of the OLDEST snapshot
    size_t poseCount_ = 0;  // number of valid snapshots (<= capacity)
    uint32_t autoRewindTicks_ = 0; // rewind used by MeleeAttack/AttackBox/CastSkill
    gfx::Mesh fireballMesh_; // lazily built for skill-projectile rendering
    std::set<std::string> loadedScripts_; // resolved paths whose chunk ran (presence only)
    std::set<std::string> scriptFailed_;  // resolved paths that failed (skip later)
    // Captured handler handles per loaded chunk (keyed like loadedScripts_:
    // backend + "|" + full path). Lua/JS hosts share ONE global environment,
    // so a later-loaded chunk overwrites the on_start/on_update globals; a
    // captured FUNCTION handle keeps pointing at the original function value
    // regardless. Caching the handles at first load lets every later attach
    // reuse ITS OWN chunk's handlers instead of re-capturing whatever chunk
    // happens to own the globals at that moment (the old re-capture made
    // spawned entities run the WRONG script - peas falling like suns).
    struct ChunkHandlers {
        uint64_t onStart = 0;
        uint64_t onUpdate = 0;
    };
    std::map<std::string, ChunkHandlers> chunkHandlers_;
    GameRuntimeConfig cfg_;
    bool running_ = false;
    double simTime_ = 0.0;
    // G5-4-4(项1): dependency-graph scheduler for the per-frame component
    // sub-tasks. Registered once in InitSystemGraph() (lazily on first Start).
    ecs::SystemScheduler systems_;
    bool systemsReady_ = false;
    void InitSystemGraph();
};

} // namespace neon::scene
