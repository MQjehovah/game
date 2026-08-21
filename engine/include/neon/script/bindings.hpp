#pragma once
#include <functional>
#include <map>
#include <string>

#include "neon/ecs/world.hpp"
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
};

// Strict ordering for std::map<ecs::Entity, ...> (entities are id+generation
// pairs and do not define operator<).
struct EntityLess {
    bool operator()(const ecs::Entity& a, const ecs::Entity& b) const {
        if (a.id != b.id) return a.id < b.id;
        return a.generation < b.generation;
    }
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
};

// Registers Spawn/Despawn/GetPosition/SetPosition/GetVar/SetVar/Raycast/
// PlaySfx, the namespaced Json.Parse, and the input query API (InputAxis/
// InputKey/InputMouseX/InputMouseY) on `host`. Safe to call on an initialized
// host only; missing dependencies (null world/physics/audio sink/input) yield
// nil/no-ops rather than script errors.
void RegisterEngineBindings(IScriptHost& host, ScriptContext& ctx);

} // namespace neon::script
