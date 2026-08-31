// C1: GameRuntime combat subsystem (status effects / projectiles /
// spatial overlap queries / lag-compensated hit tests). Split out of the
// former single game_runtime.cpp TU: these functions own the combat state
// (projectiles_ / poseSlots_) and read world_/scriptCtx_.
#include "neon/scene/game_runtime.hpp"
#include "game_runtime_priv.hpp"

#include <cmath>

#include "neon/scene/status.hpp"

namespace neon::scene {
using namespace detail; // EntityKey

void GameRuntime::SpawnProjectile(const math::Vec3& pos, const math::Vec3& dir, float speed,
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

// G3-4: the position a hit test uses for `ent` - the pose it had
// `rewindTicks` fixed ticks ago when history exists, else its CURRENT pose
// (fresh spawns / shallow history degrade gracefully to the plain path).
bool GameRuntime::LagCompPosition(ecs::Entity e, uint32_t rewindTicks,
                                  math::Vec3& out) const {
    if (rewindTicks > 0 && poseCount_ > 0) {
        // Slot layout: the OLDEST snapshot sits at (head - count) mod N; the
        // newest is one slot before head. idx counts back from the newest.
        const size_t n = poseCount_;
        const size_t idx = rewindTicks >= n ? 0 : n - 1 - rewindTicks;
        const size_t slot =
            (poseHead_ + poseSlots_.size() - poseCount_ + idx) % poseSlots_.size();
        const auto& snap = poseSlots_[slot];
        const auto it = snap.find(EntityKey(e));
        if (it != snap.end()) {
            out = it->second;
            return true;
        }
    }
    return false;
}

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
        if (rewindTicks > 0) LagCompPosition(ent, rewindTicks, p);
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
        if (rewindTicks > 0) LagCompPosition(ent, rewindTicks, p);
        const math::Vec3 d = p - center;
        const float lx = c * d.x - s * d.z, ly = d.y, lz = s * d.x + c * d.z;
        if (std::fabs(lx) <= half.x && std::fabs(ly) <= half.y && std::fabs(lz) <= half.z)
            out.push_back({ent, p});
    }
    return out;
}

void GameRuntime::TickStatuses(float dt) {
    auto view = world_.ViewAll<StatusComponent>();
    for (size_t i = 0; i < view.Size(); ++i) {
        ecs::Entity ent = world_.EntityAt<StatusComponent>(i);
        StatusComponent* c = world_.Get<StatusComponent>(ent);
        if (!c) continue;
        TickStatus(*c, dt, [this, ent](uint32_t id, float magnitude) {
            // 状态 tick 效果下沉 Lua：调用脚本全局 OnStatusTick(entity, id, magnitude)。
            // 具体掉血/回血/减速规则由基础库 Gameplay（或项目脚本覆盖）定义。
            if (auto* host = hosts_.lua.get()) {
                if (host->HasFunction("OnStatusTick")) {
                    (void)host->Call("OnStatusTick",
                        { EntityToValue(ent), script::Value::Num(static_cast<double>(id)),
                          script::Value::Num(static_cast<double>(magnitude)) });
                }
            }
        });
    }
}

bool GameRuntime::HasStatus(ecs::Entity ent, uint32_t id) const {
    const StatusComponent* c = world_.Get<StatusComponent>(ent);
    return c ? scene::HasStatus(*c, id) : false;
}

float GameRuntime::StatusMagnitude(ecs::Entity ent, uint32_t id) const {
    const StatusComponent* c = world_.Get<StatusComponent>(ent);
    return c ? scene::StatusMagnitude(*c, id) : 0.0f;
}

// Projectile trail ember: one short-lived additive particle per tick, drifting
// back along the flight path (bloom picks it up — HDR color > 1).
void GameRuntime::ProjectileTrail(const Projectile& p) {
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
    particles_.Emit(cfg);
}

// Impact burst when a projectile dies (hit, range-out or timeout).
void GameRuntime::ProjectileBurst(const Projectile& p) {
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
    particles_.Emit(cfg);
}

void GameRuntime::TickProjectiles(float dt) {
    for (auto it = projectiles_.begin(); it != projectiles_.end();) {
        Projectile& p = *it;
        const float step = p.speed * dt;
        p.pos += p.dir * step;
        p.traveled += step;
        p.life -= dt;
        ProjectileTrail(p);
        // Data-driven skills can bound a projectile by travel distance.
        if (p.range > 0.0f && p.traveled >= p.range) {
            ProjectileBurst(p);
            it = projectiles_.erase(it);
            continue;
        }
        // Damage the closest SceneHealth entity within the hit radius. Use a
        // horizontal-radius + vertical-band test (projectiles fly at chest
        // height while targets sit on the ground), so a fireball passing over
        // a grounded enemy still connects.
        float best = p.hitRadius;
        ecs::Entity target;
        auto view = world_.ViewAll<SceneHealth>();
        for (size_t i = 0; i < view.Size(); ++i) {
            ecs::Entity ent = world_.EntityAt<SceneHealth>(i);
            SceneHealth* h = world_.Get<SceneHealth>(ent);
            const SceneTransform* t = world_.Get<SceneTransform>(ent);
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
            // status effects (name resolved through the built-in status table).
            if (SceneHealth* h = world_.Get<SceneHealth>(target)) {
                h->hp = std::fmax(0.0f, h->hp - p.damage);
            }
            if (!p.statuses.empty() && world_.Alive(target)) {
                if (!world_.Has<StatusComponent>(target)) world_.Add<StatusComponent>(target);
                if (StatusComponent* c = world_.Get<StatusComponent>(target)) {
                    for (const SkillStatus& st : p.statuses) {
                        const uint32_t id = StatusIdByName(st.name);
                        if (id != 0) ApplyStatus(*c, id, st.duration, st.magnitude);
                    }
                }
            }
            ProjectileBurst(p);
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

} // namespace neon::scene
