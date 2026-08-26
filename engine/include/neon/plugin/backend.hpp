#pragma once

#include <functional>
#include <memory>
#include <string>

#include "neon/plugin/native.hpp"
#include "neon/physics/physics.hpp"

namespace neon::plugin {

// ---------------------------------------------------------------------------
// G5-1 backend providers: a native plugin can ship a concrete subsystem
// backend (physics, later audio / rendering) as a DLL/SO, and the host picks
// one at runtime without relinking. The ABI is kind-scoped: a plugin exports a
// module-specific C getter (e.g. NeonPhysics_GetWorldApi); the host discovers
// it by scanning the loaded native plugins. The returned object is opaque and
// its lifetime is owned by the module that allocated it (destroy_world), so a
// host-side `delete` never crosses a CRT boundary.
// ---------------------------------------------------------------------------

// C function table mirror of NeonPhysicsWorldApi (plugins/physics_plugin/api.h),
// kept engine-side so the host does not depend on a specific plugin's header.
struct PhysicsWorldApi {
    void* (*create_world)(void) = nullptr;
    void (*destroy_world)(void* world) = nullptr;
    const char* (*name)(void) = nullptr;
};

// A loaded physics backend from a native plugin. Owns the NativePlugin (so the
// library stays resident) and the resolved factory table. It must outlive any
// world it created — destroying it unloads the DLL.
struct PhysicsBackend {
    std::unique_ptr<NativePlugin> plugin;
    PhysicsWorldApi api{};

    // Creates a world owned by the plugin. The unique_ptr's deleter calls the
    // plugin's destroy_world so memory is freed on the allocating module.
    // Returns null when the plugin has no factory.
    std::unique_ptr<physics::World, std::function<void(physics::World*)>> CreateWorld() const;

    std::string Name() const { return api.name ? api.name() : "?"; }
};

// Loads a native plugin providing a physics backend from <baseDir>/plugins and
// resolves its world factory. `backendName` matches (case-insensitive) the
// plugin's reported name or its library filename stem; empty / "*" accepts any
// provider. Returns null (errors logged, never fatal) when none is found.
std::unique_ptr<PhysicsBackend> LoadNativePhysicsBackend(const std::string& backendName,
                                                         const std::string& baseDir);

} // namespace neon::plugin
