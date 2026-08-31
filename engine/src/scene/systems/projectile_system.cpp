// C1: ProjectileSystem implementation. Migrated verbatim from GameRuntime's
// combat TU (SpawnProjectile / TickProjectiles / ProjectileTrail /
// ProjectileBurst + the fireball draw path): the world and particle system are
// now parameters instead of GameRuntime members. Pure code movement, no
// semantic change.
#include "neon/scene/systems/projectile_system.hpp"

#include <cmath>

#include "neon/gfx/material.hpp"
#include "neon/gfx/renderer.hpp"
#include "neon/gfx/scene_props.hpp"
#include "neon/scene/scene_file.hpp"
#include "neon/scene/status.hpp"

namespace neon::scene {

void ProjectileSystem::Spawn(const math::Vec3& pos, const math::Vec3& dir, float speed,
                             float damage, float life, ecs::Entity caster, float range,
                             float hitRadius, const std::vector<SkillStatus>& statuses) {
    Projectile p;
    p.pos = pos;
    p.dir = dir.LengthSq() > 1e-6f ? dir.Normalized() : math::Vec3{0, 0, 1};
    p.speed = speed > 0.0f ? speed : 12.0f;
    p.damage = damage;
    p.life = life > 0.0f ? life : 2.0f;
    p.caster = caster;
    p.range = range;
    p.hitRadius = hitRadius;
    p.statuses = statuses;
    projectiles_.push_back(p);
}

// Projectile trail ember: one short-lived additive particle per tick, drifting
// back along the flight path (bloom picks it up — HDR color > 1).
void ProjectileSystem::Trail(const Projectile& p, gfx::ParticleSystem& particles) {
    gfx::EmitterConfig cfg;
    cfg.count = 1;
    cfg.position = p.pos;
    cfg.baseVelocity = math::Vec3{-p.dir.x * 2.0f, 0.6f, -p.dir.z * 2.0f};
    cfg.speedMin = 0.2f;
    cfg.speedMax = 1.2f;
    cfg.lifeMin = 0.16f;
    cfg.lifeMax = 0.32f;
    cfg.sizeStart = 0.45f;
    cfg.sizeEnd = 0.02f;
    cfg.colorStart = gfx::Color{1.0f, 0.72f, 0.25f, 1.0f};
    cfg.colorEnd = gfx::Color{0.9f, 0.2f, 0.05f, 0.0f};
    cfg.gravity = -2.0f;
    cfg.additive = true;
    particles.Emit(cfg);
}

// Impact burst when a projectile dies (hit, range-out or timeout).
void ProjectileSystem::Burst(const Projectile& p, gfx::ParticleSystem& particles) {
    gfx::EmitterConfig cfg;
    cfg.count = 14;
    cfg.position = p.pos;
    cfg.speedMin = 2.0f;
    cfg.speedMax = 6.5f;
    cfg.lifeMin = 0.25f;
    cfg.lifeMax = 0.5f;
    cfg.sizeStart = 0.6f;
    cfg.sizeEnd = 0.03f;
    cfg.colorStart = gfx::Color{1.0f, 0.8f, 0.3f, 1.0f};
    cfg.colorEnd = gfx::Color{0.9f, 0.25f, 0.05f, 0.0f};
    cfg.gravity = -5.0f;
    cfg.additive = true;
    particles.Emit(cfg);
}

void ProjectileSystem::Tick(float dt, ecs::World& world, gfx::ParticleSystem& particles) {
    for (auto it = projectiles_.begin(); it != projectiles_.end();) {
        Projectile& p = *it;
        const float step = p.speed * dt;
        p.pos += p.dir * step;
        p.traveled += step;
        p.life -= dt;
        Trail(p, particles);
        // Data-driven skills can bound a projectile by travel distance.
        if (p.range > 0.0f && p.traveled >= p.range) {
            Burst(p, particles);
            it = projectiles_.erase(it);
            continue;
        }
        // Damage the closest SceneHealth entity within the hit radius. Use a
        // horizontal-radius + vertical-band test (projectiles fly at chest
        // height while targets sit on the ground), so a fireball passing over
        // a grounded enemy still connects.
        float best = p.hitRadius;
        ecs::Entity target;
        auto view = world.ViewAll<SceneHealth>();
        for (size_t i = 0; i < view.Size(); ++i) {
            ecs::Entity ent = world.EntityAt<SceneHealth>(i);
            SceneHealth* h = world.Get<SceneHealth>(ent);
            const SceneTransform* t = world.Get<SceneTransform>(ent);
            if (!h || !t || h->hp <= 0.0f) continue;
            if (ent == p.caster) continue; // never self-hit
            const math::Vec3 to = t->pos - p.pos;
            const float horiz = std::sqrt(to.x * to.x + to.z * to.z);
            if (horiz < best && std::fabs(to.y) < 2.0f) {
                best = horiz;
                target = ent;
            }
        }
        if (target.IsValid()) {
            // Damage the hit entity (clamped to 0) and apply the projectile's
            // status effects (numeric id resolved by the caller).
            if (SceneHealth* h = world.Get<SceneHealth>(target)) {
                h->hp = std::fmax(0.0f, h->hp - p.damage);
            }
            if (!p.statuses.empty() && world.Alive(target)) {
                if (!world.Has<StatusComponent>(target)) world.Add<StatusComponent>(target);
                if (StatusComponent* c = world.Get<StatusComponent>(target)) {
                    for (const SkillStatus& st : p.statuses) {
                        ApplyStatus(*c, st.id, st.duration, st.magnitude, st.tickInterval);
                    }
                }
            }
            Burst(p, particles);
            it = projectiles_.erase(it);
            continue;
        }
        if (p.life <= 0.0f) {
            it = projectiles_.erase(it);
            continue;
        }
        ++it;
    }
}

// Skill projectiles (fireballs): bright glowing orbs. Lazy mesh build (the
// first Draw with at least one active projectile resolves it) — identical to
// the pre-split GameRuntime::Draw path.
void ProjectileSystem::Draw(gfx::Renderer& renderer) {
    if (projectiles_.empty()) return;
    if (!fireballMesh_.Valid()) fireballMesh_ = gfx::MakeFireballMesh(renderer);
    gfx::Material fmat = gfx::Material::Lit({}, gfx::Color::White, 8.0f);
    fmat.emissiveIntensity = 2.5f;
    for (const Projectile& p : projectiles_) {
        renderer.DrawMesh(fireballMesh_, fmat, math::Mat4::Translation(p.pos));
    }
}

} // namespace neon::scene
