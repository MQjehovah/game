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
    if (radius <= 0.0f) return out;
    ecs::World& world = const_cast<ecs::World&>(world_); // ECS has no const ViewAll
    // Fast path: the broadphase only holds CURRENT poses, so a lag-compensated
    // (rewound) query still scans linearly. Almost every query rewinds 0.
    if (rewindTicks == 0) {
        spatial_.Sync(world, SimStamp());
        const float r2 = radius * radius;
        spatial_.QuerySphere(center, radius, [&](ecs::Entity ent) {
            const SceneHealth* h = world_.Get<SceneHealth>(ent);
            const SceneTransform* t = world_.Get<SceneTransform>(ent);
            if (!h || !t || h->hp <= 0.0f) return;
            if ((t->pos - center).LengthSq() <= r2) out.push_back({ent, t->pos});
        });
        return out;
    }
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
    // Exact oriented-box test on a candidate pose.
    auto test = [&](const math::Vec3& p) {
        const math::Vec3 d = p - center;
        const float lx = c * d.x - s * d.z, ly = d.y, lz = s * d.x + c * d.z;
        return std::fabs(lx) <= half.x && std::fabs(ly) <= half.y && std::fabs(lz) <= half.z;
    };
    if (rewindTicks == 0) {
        spatial_.Sync(world, SimStamp());
        // Broadphase box = the oriented box's world AABB (conservative).
        const float ex = std::fabs(c) * half.x + std::fabs(s) * half.z;
        const float ez = std::fabs(s) * half.x + std::fabs(c) * half.z;
        math::AABB q;
        q.min = {center.x - ex, center.y - half.y, center.z - ez};
        q.max = {center.x + ex, center.y + half.y, center.z + ez};
        spatial_.QueryAABB(q, [&](ecs::Entity ent) {
            const SceneHealth* h = world_.Get<SceneHealth>(ent);
            const SceneTransform* t = world_.Get<SceneTransform>(ent);
            if (!h || !t || h->hp <= 0.0f) return;
            if (test(t->pos)) out.push_back({ent, t->pos});
        });
        return out;
    }
    auto view = world.ViewAll<SceneHealth>();
    for (size_t i = 0; i < view.Size(); ++i) {
        ecs::Entity ent = world.EntityAt<SceneHealth>(i);
        const SceneHealth* h = world_.Get<SceneHealth>(ent);
        const SceneTransform* t = world_.Get<SceneTransform>(ent);
        if (!h || !t || h->hp <= 0.0f) continue;
        math::Vec3 p = t->pos;
        if (rewindTicks > 0) lagComp_.Position(ent, rewindTicks, p);
        if (test(p)) out.push_back({ent, p});
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
