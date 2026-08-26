// G4-1 sample native plugin: a minimal deterministic physics module built as a
// DLL/SO. Demonstrates the generic plugin ABI (NeonPlugin_GetInfo) plus a
// module-specific C surface (NeonPhysics_GetApi). Self-contained: no engine
// runtime objects cross the boundary, only the opaque instance handle.
#define NEON_PLUGIN_PHYSICS_BUILD
#include <cstdint>
#include <cstring>
#include <vector>

#include "api.h"
#include "neon/plugin/native.hpp"

// G5-1: the plugin also compiles the engine's real deterministic solver
// (engine/src/physics/physics.cpp, added to this target) into itself, turning
// the DLL into a drop-in physics *middleware*. NeonPhysics_GetWorldApi exposes
// the C factory; the created object is a full neon::physics::World.
#include "neon/physics/physics.hpp"

#if defined(_WIN32)
#define NEON_PLUGIN_EXPORT extern "C" __declspec(dllexport)
#else
#define NEON_PLUGIN_EXPORT extern "C" __attribute__((visibility("default")))
#endif

namespace {

// Opaque plugin instance: a tiny world of dynamic/static spheres on a ground
// plane. Only ever touched by the functions below, through the void* handle.
struct Sphere {
    float x = 0.0f, y = 0.0f, z = 0.0f, r = 1.0f;
    float vy = 0.0f;
    bool dynamic = true;
};

struct PhysicsWorld {
    std::vector<Sphere> spheres;
};

void Step(void* w, float dt, float gx, float gy, float gz) {
    PhysicsWorld* W = static_cast<PhysicsWorld*>(w);
    for (Sphere& s : W->spheres) {
        if (!s.dynamic) continue;
        s.vy += gy * dt;
        s.x += gx * dt;
        s.y += s.vy * dt;
        s.z += gz * dt;
        if (s.y - s.r < 0.0f) {
            s.y = s.r;
            s.vy = 0.0f;
        }
    }
}

int AddSphere(void* w, float x, float y, float z, float r, int dynamic) {
    PhysicsWorld* W = static_cast<PhysicsWorld*>(w);
    if (W->spheres.size() >= 4096) return 0;
    Sphere s;
    s.x = x;
    s.y = y;
    s.z = z;
    s.r = r > 0.0f ? r : 1.0f;
    s.dynamic = dynamic != 0;
    W->spheres.push_back(s);
    return static_cast<int>(W->spheres.size());
}

int BodyCount(void* w) {
    return static_cast<int>(static_cast<PhysicsWorld*>(w)->spheres.size());
}

float GetY(void* w, int id) {
    PhysicsWorld* W = static_cast<PhysicsWorld*>(w);
    if (id < 1 || id > static_cast<int>(W->spheres.size())) return 0.0f;
    return W->spheres[static_cast<size_t>(id - 1)].y;
}

void Reset(void* w) { static_cast<PhysicsWorld*>(w)->spheres.clear(); }

const NeonPhysicsApi kApi = {
    /*.step=*/Step,
    /*.add_sphere=*/AddSphere,
    /*.body_count=*/BodyCount,
    /*.get_y=*/GetY,
    /*.reset=*/Reset,
};

// create/destroy for the generic ABI: the instance is a fresh PhysicsWorld.
void* Create(uint32_t apiVersion) {
    if (apiVersion != neon::plugin::kNativeApiVersion) return nullptr;
    return new PhysicsWorld();
}

void Destroy(void* instance) { delete static_cast<PhysicsWorld*>(instance); }

} // namespace

NEON_PLUGIN_EXPORT bool NeonPlugin_GetInfo(neon::plugin::NativePluginInfo* out) {
    if (!out) return false;
    out->apiVersion = neon::plugin::kNativeApiVersion;
    out->size = sizeof(neon::plugin::NativePluginInfo);
    out->name = "physics_plugin";
    out->version = "1.0.0";
    out->create = &Create;
    out->destroy = &Destroy;
    return true;
}

const NeonPhysicsApi* NeonPhysics_GetApi(void* instance) {
    if (!instance) return nullptr;
    return &kApi;
}

// ---------------------------------------------------------------------------
// G5-1 world factory: create/destroy a real neon::physics::World (the engine's
// deterministic solver, compiled into this DLL). Both directions of the
// lifecycle happen here so new/delete stay on the plugin's CRT.
// ---------------------------------------------------------------------------
void* CreateWorld() { return new neon::physics::World(); }

void DestroyWorld(void* world) { delete static_cast<neon::physics::World*>(world); }

const char* WorldName() { return "custom"; }

const NeonPhysicsWorldApi kWorldApi = {
    /*.create_world=*/CreateWorld,
    /*.destroy_world=*/DestroyWorld,
    /*.name=*/WorldName,
};

const NeonPhysicsWorldApi* NeonPhysics_GetWorldApi() { return &kWorldApi; }
