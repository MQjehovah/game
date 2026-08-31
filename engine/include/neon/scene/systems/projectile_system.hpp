#pragma once

// C1: standalone skill-projectile subsystem (split out of GameRuntime).
// Fixed-step movement + closest-SceneHealth hit detection + trail/impact VFX.
// Fully data-driven (range / hitRadius / statuses); the caller owns the
// ecs::World and gfx::ParticleSystem it ticks against. No GameRuntime
// dependency, so hosts can run projectiles without the full runtime.

#include <cstdint>
#include <vector>

#include "neon/ecs/world.hpp"
#include "neon/gfx/mesh.hpp"
#include "neon/gfx/particles.hpp"
#include "neon/math/vec3.hpp"
#include "neon/scene/status.hpp"

namespace neon::scene {

// 通用投射物系统：fixed-step 移动 + 命中检测 + VFX。数据驱动（range/hitRadius/statuses）。
class ProjectileSystem {
public:
    // One in-flight projectile. Public so hosts can declare it in ECS-style
    // dependency graphs (typeid(ProjectileSystem::Projectile) as a marker).
    struct Projectile {
        math::Vec3 pos;
        math::Vec3 dir;
        float speed = 0.0f;
        float damage = 0.0f;
        float life = 0.0f;     // seconds remaining
        float traveled = 0.0f; // distance travelled
        float range = 0.0f;    // max travel before expiring (0 = life-bounded only)
        float hitRadius = 0.8f;
        ecs::Entity caster;    // never damaged by its own projectile
        std::vector<SkillStatus> statuses; // applied to the hit target
    };

    void Spawn(const math::Vec3& pos, const math::Vec3& dir, float speed, float damage,
               float life, ecs::Entity caster, float range, float hitRadius,
               const std::vector<SkillStatus>& statuses);
    void Tick(float dt, ecs::World& world, gfx::ParticleSystem& particles);
    // Renders every in-flight projectile (lazily builds the shared fireball
    // mesh); no-op when none are active.
    void Draw(gfx::Renderer& renderer);
    // Drops every in-flight projectile (host teardown).
    void Clear() { projectiles_.clear(); }
    size_t Count() const { return projectiles_.size(); }

private:
    void Trail(const Projectile& p, gfx::ParticleSystem& particles);
    void Burst(const Projectile& p, gfx::ParticleSystem& particles);
    std::vector<Projectile> projectiles_;
    gfx::Mesh fireballMesh_; // lazily built for skill-projectile rendering
};

} // namespace neon::scene
