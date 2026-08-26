#pragma once

// C-compatible API of the sample physics native plugin (G4-1). The plugin
// exports `NeonPlugin_GetInfo` (generic ABI, see neon/plugin/native.hpp) and
// `NeonPhysics_GetApi(instance)` which returns this function table. Both the
// plugin and the host test include this header — a module-specific surface on
// top of the generic plugin ABI.

#include <stdint.h>

// dllexport only while building the plugin module itself (the host just links
// the symbol via GetProcAddress/dlsym, no import needed).
#if defined(_WIN32)
#if defined(NEON_PLUGIN_PHYSICS_BUILD)
#define NEON_PHYSICS_API __declspec(dllexport)
#else
#define NEON_PHYSICS_API
#endif
#else
#define NEON_PHYSICS_API __attribute__((visibility("default")))
#endif

#ifdef __cplusplus
extern "C" {
#endif

// Minimal deterministic micro-physics: dynamic spheres fall under gravity onto
// the y=0 ground plane and rest there. Enough to prove a native module is
// loaded, driven and torn down across the ABI.
typedef struct NeonPhysicsApi {
    // Advances the world by `dt` under gravity (gx,gy,gz). Dynamic spheres
    // gain velocity and clamp to the ground (y - radius >= 0).
    void (*step)(void* world, float dt, float gx, float gy, float gz);
    // Adds a sphere; returns its id (1-based) or 0 on overflow.
    int (*add_sphere)(void* world, float x, float y, float z, float r, int dynamic);
    int (*body_count)(void* world);
    // Reads back the current y of a sphere id (for assertions). 0 on bad id.
    float (*get_y)(void* world, int id);
    void (*reset)(void* world);
} NeonPhysicsApi;

// Returns the physics API table for the plugin instance (null on mismatch).
NEON_PHYSICS_API const NeonPhysicsApi* NeonPhysics_GetApi(void* instance);

// ---------------------------------------------------------------------------
// G5-1 middleware plug-and-play: a "physics backend provider" factory. The
// plugin compiles the engine's real deterministic solver into itself and hands
// back an opaque neon::physics::World instance. The host only calls the two C
// entry points; create/destroy MUST run on the same module (the plugin owns the
// heap of the object, so destroy_world is a C call into the DLL, never a host
// `delete`). The opaque pointer is a `neon::physics::World*` and its layout is
// shared via the same headers, so the object crosses the boundary only under a
// same-toolchain build (exactly how middleware like PhysX ships binary SDKs).
// ---------------------------------------------------------------------------
typedef struct NeonPhysicsWorldApi {
    // Returns a new world instance (opaque neon::physics::World*), or null.
    void* (*create_world)(void);
    // Destroys a world created by create_world (same module).
    void (*destroy_world)(void* world);
    // Backend display name (e.g. "custom").
    const char* (*name)(void);
} NeonPhysicsWorldApi;

// Returns the world factory table (null on mismatch).
NEON_PHYSICS_API const NeonPhysicsWorldApi* NeonPhysics_GetWorldApi(void);

#ifdef __cplusplus
}
#endif
