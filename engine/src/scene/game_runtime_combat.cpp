// C1: GameRuntime combat subsystem (status effects / spatial overlap queries /
// lag-compensated hit tests). Split out of the former single game_runtime.cpp
// TU: these functions read world_/scriptCtx_ and the lagComp_ subsystem's pose
// history. Skill projectiles moved out to ProjectileSystem
// (engine/src/scene/systems/projectile_system.cpp); the script-facing
// GameRuntime::SpawnProjectile forwarder now lives in game_runtime.cpp.
#include "neon/scene/game_runtime.hpp"
#include "game_runtime_priv.hpp"

#include <cmath>

#include "neon/scene/status.hpp"

namespace neon::scene {
using namespace detail; // EntityToValue

std::vector<GameRuntime::HealthHit> GameRuntime::OverlapSphere(
    const math::Vec3& center, float radius, uint32_t rewindTicks) const {
    std::vector<HealthHit> out;
    ecs::World& world = const_cast<ecs::World&>(world_); // ECS has no const ViewAll
    auto view = world.ViewAll<SceneHealth>();
    for (size_t i = 0; i < view.Size(); ++i) {
        ecs::Entity ent = world.EntityAt<SceneHealth>(i);
        const SceneHealth* h = world_.Get<SceneHealth>(ent);
        const SceneTransform* t = world_.Get<SceneTransform>(ent);
        if (!h || !t || h->hp <= 0.0f) continue;
        math::Vec3 p = t->pos;
        if (rewindTicks > 0) lagComp_.Position(ent, rewindTicks, p);
        if ((p - center).LengthSq() <= radius * radius) out.push_back({ent, p});
    }
    return out;
}

std::vector<GameRuntime::HealthHit> GameRuntime::OverlapBox(
    const math::Vec3& center, const math::Vec3& half, float yaw, uint32_t rewindTicks) const {
    const float c = std::cos(yaw), s = std::sin(yaw);
    std::vector<HealthHit> out;
    ecs::World& world = const_cast<ecs::World&>(world_); // ECS has no const ViewAll
    auto view = world.ViewAll<SceneHealth>();
    for (size_t i = 0; i < view.Size(); ++i) {
        ecs::Entity ent = world.EntityAt<SceneHealth>(i);
        const SceneHealth* h = world_.Get<SceneHealth>(ent);
        const SceneTransform* t = world_.Get<SceneTransform>(ent);
        if (!h || !t || h->hp <= 0.0f) continue;
        math::Vec3 p = t->pos;
        if (rewindTicks > 0) lagComp_.Position(ent, rewindTicks, p);
        const math::Vec3 d = p - center;
        const float lx = c * d.x - s * d.z, ly = d.y, lz = s * d.x + c * d.z;
        if (std::fabs(lx) <= half.x && std::fabs(ly) <= half.y && std::fabs(lz) <= half.z)
            out.push_back({ent, p});
    }
    return out;
}

bool GameRuntime::HasStatus(ecs::Entity ent, uint32_t id) const {
    return status_.Has(const_cast<ecs::World&>(world_), ent, id);
}

float GameRuntime::StatusMagnitude(ecs::Entity ent, uint32_t id) const {
    return status_.Magnitude(const_cast<ecs::World&>(world_), ent, id);
}

} // namespace neon::scene
