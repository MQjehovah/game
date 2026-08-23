#pragma once
#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <vector>

#include "neon/ecs/world.hpp"
#include "neon/math/quat.hpp"
#include "neon/math/vec3.hpp"
#include "neon/physics/physics.hpp"
#include "neon/platform/input.hpp"
#include "neon/script/gamevars.hpp"
#include "neon/script/script.hpp"

namespace neon::script {

// Scripting convenience transform component. The engine's ECS has no gameplay
// components; bindings use this so scripts can spawn and move entities. The
// data-driven scene system (T2.6) maps it onto real scene data later.
struct CTransformBind {
    math::Vec3 pos{};
    math::Quat rot{}; // heading/attitude (SetRotationY writes this)
};

// Strict ordering for std::map<ecs::Entity, ...> (entities are id+generation
// pairs and do not define operator<).
struct EntityLess {
    bool operator()(const ecs::Entity& a, const ecs::Entity& b) const {
        if (a.id != b.id) return a.id < b.id;
        return a.generation < b.generation;
    }
};

// One 2D immediate-mode drawing command issued by a script's on_render()
// handler (design units: 1280x720). The runtime owns the buffer: scripts
// append via the DrawRect/DrawRectOutline/DrawText bindings and the runtime
// flushes them into the renderer's 2D overlay every frame. This is how
// data-driven 2D games (e.g. the editor-authored PvZ project) draw without
// any C++ gameplay code.
struct Draw2DCmd {
    enum class Kind : uint8_t { Rect, RectOutline, Text };
    Kind kind = Kind::Rect;
    float x = 0.0f;
    float y = 0.0f;
    float w = 0.0f;
    float h = 0.0f;
    float r = 1.0f, g = 1.0f, b = 1.0f, a = 1.0f;
    float thickness = 1.0f;
    float size = 16.0f;
    bool centerX = false;
    bool centerY = false;
    std::string text;
};

// The state engine bindings operate on. Created by the game/demo and passed to
// RegisterEngineBindings; each native function reaches it through the Register
// user pointer. Any member may be null/empty for hosts that do not use it:
// bindings degrade to nil/no-ops instead of crashing.
struct ScriptContext {
    ecs::World* world = nullptr;
    physics::World* physics = nullptr;
    GameVars gameVars;
    std::function<void(const std::string&)> playSfx; // optional audio sink
    std::map<ecs::Entity, std::string, EntityLess> entityKinds; // entity -> kind name
    // Optional live input state for the InputAxis/InputKey/InputMouse bindings.
    // Null in headless hosts and unit tests -> every input query returns 0.
    platform::IInput* input = nullptr;
    // The entity currently being updated (set by GameRuntime before each
    // on_start/on_update call). Lets the input bindings route per-entity:
    // when inputForEntity is set and returns non-null for the current entity,
    // that input wins over `input` (multi-player: each player's script reads
    // its OWN client's input).
    ecs::Entity currentEntity;
    std::function<platform::IInput*(ecs::Entity)> inputForEntity;

    // Optional gameplay hooks registered by the scene runtime (GameRuntime).
    // When null the corresponding bindings degrade to nil/no-ops, so other
    // hosts (demo, tests) keep working without them. Entity args are the scene
    // entity handles passed into on_start/on_update.
    std::function<math::Vec3(ecs::Entity)> sceneGetPos;   // null -> CTransformBind
    std::function<void(ecs::Entity, const math::Vec3&)> sceneSetPos;
    std::function<void(ecs::Entity, float)> sceneSetYaw;  // radians, Y-up
    std::function<float(ecs::Entity)> sceneGetHp;         // -1 when no health
    std::function<void(ecs::Entity, float)> sceneSetHp;
    std::function<void(const math::Vec3& pos, const math::Vec3& dir, float speed, float damage,
                       float life, ecs::Entity caster)>
        spawnProjectile;
    std::function<int(const math::Vec3& origin, const math::Vec3& dir, float range, float arcDeg,
                      float damage)>
        meleeAttack; // returns the number of entities hit
    // Status-effect hooks (M2 combat core): names are resolved to ids by the
    // caller (the bindings) through the built-in status table before calling.
    std::function<void(ecs::Entity, uint32_t id, float duration, float magnitude)>
        sceneApplyStatus;
    std::function<bool(ecs::Entity, uint32_t id)> sceneHasStatus;
    std::function<float(ecs::Entity, uint32_t id)> sceneStatusMagnitude;
    std::function<void(ecs::Entity, uint32_t id)> sceneRemoveStatus;
    // Skill hooks (M2 combat core): data-driven CastSkill / SkillCooldown and
    // the oriented attack box.
    std::function<int(const std::string& name, const math::Vec3& origin, const math::Vec3& dir,
                      ecs::Entity caster)>
        castSkill;
    std::function<float(const std::string& name, ecs::Entity caster)> sceneSkillCooldown;
    std::function<int(const math::Vec3& center, const math::Vec3& half, float yaw, float damage)>
        attackBox;
    // Multi-player: a scene script calls BindPlayerToClient(player, clientId)
    // inside on_player_join(clientId) to own that entity; the server then
    // routes the client's input to it. The runtime itself leaves this null.
    std::function<void(ecs::Entity, double)> bindPlayerToClient;
    // 2D immediate-mode canvas (runtime-owned; set only while on_render runs).
    // Null in headless hosts -> draw bindings are no-ops.
    std::vector<Draw2DCmd>* draw2d = nullptr;
    // Screen pixels -> 2D design units (wired by the runtime when a renderer
    // exists; null -> raw pixel coordinates).
    std::function<math::Vec2(const math::Vec2&)> screenToUi;
    // Reads a text data file (levels/*.json etc.) from the project/pack.
    // Wired by GameRuntime; null -> ReadText returns "".
    std::function<std::string(const std::string&)> readData;
    // Optional: attaches a Lua script to a spawned entity (Spawn's 3rd arg) so
    // dynamically created entities (multi-player player controllers) run
    // on_start/on_update like scene-placed ones. Wired by GameRuntime.
    std::function<void(ecs::Entity, const std::string&)> spawnScript;
};

// Registers Spawn/Despawn/GetPosition/SetPosition/GetVar/SetVar/Raycast/
// PlaySfx, the namespaced Json.Parse, and the input query API (InputAxis/
// InputKey/InputMouseX/InputMouseY) on `host`. Safe to call on an initialized
// host only; missing dependencies (null world/physics/audio sink/input) yield
// nil/no-ops rather than script errors.
void RegisterEngineBindings(IScriptHost& host, ScriptContext& ctx);

} // namespace neon::script
