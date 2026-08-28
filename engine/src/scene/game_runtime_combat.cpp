// C1: GameRuntime combat subsystem (skills / status effects / projectiles /
// melee & attack boxes / lag-compensated hit tests). Split out of the former
// single game_runtime.cpp TU: these functions own the combat state
// (skillCooldowns_ / projectiles_ / poseSlots_) and read world_/scriptCtx_.
#include "neon/scene/game_runtime.hpp"
#include "game_runtime_priv.hpp"

#include <cmath>

#include "neon/scene/status.hpp"

namespace neon::scene {
using namespace detail; // EntityKey

void GameRuntime::SpawnProjectile(const math::Vec3& pos, const math::Vec3& dir, float speed,
                                  float damage, float life, ecs::Entity caster) {
    Projectile p;
    p.pos = pos;
    p.dir = dir.LengthSq() > 1e-6f ? dir.Normalized() : math::Vec3{0, 0, 1};
    p.speed = speed > 0.0f ? speed : 12.0f;
    p.damage = damage;
    p.life = life > 0.0f ? life : 2.0f;
    p.caster = caster;
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

// Shared arc-hit test used by MeleeAttack (auto rewind), MeleeAttackLagComp
// (explicit rewind) and CastSkill's melee skill. Damage always lands on the
// CURRENT entity; only the position test is rewound.
int GameRuntime::MeleeAttackImpl(const math::Vec3& origin, const math::Vec3& dir, float range,
                                 float arcDeg, float damage, uint32_t rewindTicks,
                                 ecs::Entity exclude,
                                 const std::vector<SkillStatus>& statuses) {
    const math::Vec3 fwd = dir.LengthSq() > 1e-6f ? dir.Normalized() : math::Vec3{0, 0, 1};
    const float cosArc = std::cos(arcDeg * 0.5f * math::kDegToRad);
    int hits = 0;
    auto view = world_.ViewAll<SceneHealth>();
    for (size_t i = 0; i < view.Size(); ++i) {
        ecs::Entity ent = world_.EntityAt<SceneHealth>(i);
        SceneHealth* h = world_.Get<SceneHealth>(ent);
        const SceneTransform* t = world_.Get<SceneTransform>(ent);
        if (!h || !t || h->hp <= 0.0f) continue;
        if (ent == exclude) continue; // never self-hit
        math::Vec3 hitPos = t->pos;
        if (rewindTicks > 0) LagCompPosition(ent, rewindTicks, hitPos);
        const math::Vec3 to = hitPos - origin;
        // Horizontal-range + vertical-band hit test (like projectiles): the
        // attack originates at chest height while targets sit on the ground,
        // so a 3D distance would shrink the effective reach (a wolf at 1.8m
        // horizontal, 1.5m below the origin, is ~2.3m away in 3D and misses a
        // 2.2m swing).
        const float horiz = std::sqrt(to.x * to.x + to.z * to.z);
        if (horiz > range || horiz < 1e-4f) continue;
        if (std::fabs(to.y) > 2.0f) continue;
        const math::Vec3 toDir{to.x / horiz, 0.0f, to.z / horiz};
        const math::Vec3 fwdDir{fwd.x, 0.0f, fwd.z};
        if (math::Dot(toDir, fwdDir) < cosArc) continue; // outside the arc
        if (statuses.empty())
            h->hp = std::fmax(0.0f, h->hp - damage);
        else
            ApplyHit(ent, damage, statuses);
        ++hits;
    }
    return hits;
}

int GameRuntime::MeleeAttack(const math::Vec3& origin, const math::Vec3& dir, float range,
                             float arcDeg, float damage) {
    return MeleeAttackImpl(origin, dir, range, arcDeg, damage, autoRewindTicks_);
}

int GameRuntime::MeleeAttackLagComp(const math::Vec3& origin, const math::Vec3& dir, float range,
                                    float arcDeg, float damage, uint32_t rewindTicks) {
    return MeleeAttackImpl(origin, dir, range, arcDeg, damage, rewindTicks);
}

// Oriented attack box (OBB around Y): damages every SceneHealth entity whose
// position lies inside the yaw-rotated half-extents box. Returns hit count.
int GameRuntime::AttackBoxImpl(const math::Vec3& center, const math::Vec3& half, float yaw,
                               float damage, uint32_t rewindTicks) {
    const float c = std::cos(yaw);
    const float s = std::sin(yaw);
    int hits = 0;
    auto view = world_.ViewAll<SceneHealth>();
    for (size_t i = 0; i < view.Size(); ++i) {
        ecs::Entity ent = world_.EntityAt<SceneHealth>(i);
        SceneHealth* h = world_.Get<SceneHealth>(ent);
        const SceneTransform* t = world_.Get<SceneTransform>(ent);
        if (!h || !t || h->hp <= 0.0f) continue;
        math::Vec3 hitPos = t->pos;
        if (rewindTicks > 0) LagCompPosition(ent, rewindTicks, hitPos);
        const math::Vec3 d = hitPos - center;
        // Rotate the target into box-local space (rotate by -yaw around Y).
        const float lx = c * d.x - s * d.z;
        const float ly = d.y;
        const float lz = s * d.x + c * d.z;
        if (std::fabs(lx) <= half.x && std::fabs(ly) <= half.y && std::fabs(lz) <= half.z) {
            h->hp = std::fmax(0.0f, h->hp - damage);
            ++hits;
        }
    }
    return hits;
}

int GameRuntime::AttackBox(const math::Vec3& center, const math::Vec3& half, float yaw,
                           float damage) {
    return AttackBoxImpl(center, half, yaw, damage, autoRewindTicks_);
}

int GameRuntime::AttackBoxLagComp(const math::Vec3& center, const math::Vec3& half, float yaw,
                                  float damage, uint32_t rewindTicks) {
    return AttackBoxImpl(center, half, yaw, damage, rewindTicks);
}

void GameRuntime::ApplySkillStatuses(ecs::Entity target,
                                     const std::vector<SkillStatus>& statuses) {
    if (statuses.empty() || !world_.Alive(target)) return;
    if (!world_.Has<StatusComponent>(target)) world_.Add<StatusComponent>(target);
    StatusComponent* c = world_.Get<StatusComponent>(target);
    if (!c) return;
    for (const SkillStatus& st : statuses) {
        const uint32_t id = StatusIdByName(st.name);
        if (id != 0) ApplyStatus(*c, id, st.duration, st.magnitude);
    }
}

void GameRuntime::ApplyHit(ecs::Entity target, float damage,
                           const std::vector<SkillStatus>& statuses) {
    if (!world_.Alive(target)) return;
    if (SceneHealth* h = world_.Get<SceneHealth>(target)) {
        h->hp = std::fmax(0.0f, h->hp - damage);
    }
    ApplySkillStatuses(target, statuses);
}

void GameRuntime::TickStatuses(float dt) {
    auto view = world_.ViewAll<StatusComponent>();
    for (size_t i = 0; i < view.Size(); ++i) {
        ecs::Entity ent = world_.EntityAt<StatusComponent>(i);
        StatusComponent* c = world_.Get<StatusComponent>(ent);
        if (!c) continue;
        TickStatus(*c, dt, [this, ent](uint32_t id, float magnitude) {
            SceneHealth* h = world_.Get<SceneHealth>(ent);
            if (!h || h->hp <= 0.0f) return;
            if (id == kStatusRegen) {
                // Regen magnitude is a heal amount per tick.
                h->hp = std::fmin(h->maxHp, h->hp + magnitude);
            } else if (id == kStatusSlow) {
                // Slow is a movement modifier read by scripts (magnitude =
                // speed factor); it deals no tick damage.
            } else {
                h->hp = std::fmax(0.0f, h->hp - magnitude);
            }
        });
    }
}

void GameRuntime::TickSkillCooldowns(float dt) {
    for (auto it = skillCooldowns_.begin(); it != skillCooldowns_.end();) {
        // Reconstruct the entity from the stable key; prune destroyed casters.
        const uint64_t key = it->first;
        ecs::Entity e{static_cast<uint32_t>(key >> 32), static_cast<uint32_t>(key & 0xFFFFFFFFu)};
        if (!world_.Alive(e)) {
            it = skillCooldowns_.erase(it);
            continue;
        }
        bool any = false;
        for (auto& kv : it->second) {
            kv.second = std::fmax(0.0f, kv.second - dt);
            if (kv.second > 0.0f) any = true;
        }
        if (!any)
            it = skillCooldowns_.erase(it);
        else
            ++it;
    }
}

bool GameRuntime::LoadSkills(const std::string& json, std::string* err) {
    return skills_.Load(json, err);
}

int GameRuntime::CastSkill(const std::string& name, const math::Vec3& origin,
                           const math::Vec3& dir, ecs::Entity caster) {
    const SkillDef* def = skills_.Find(name);
    if (!def) return 0;

    const uint64_t key = EntityKey(caster);
    auto& cds = skillCooldowns_[key];
    const auto cdIt = cds.find(name);
    if (cdIt != cds.end() && cdIt->second > 0.0f) return 0; // on cooldown

    // Mana: when the skill has a cost, the convention is a GameVar "mana"
    // (set by the scene script). Refuse without enough mana, subtract on cast.
    if (def->manaCost > 0.0f) {
        const script::Value mana = scriptCtx_.gameVars.Get("mana");
        const float have =
            mana.type == script::Value::Type::Number ? static_cast<float>(mana.number) : 0.0f;
        if (have < def->manaCost) return 0;
        scriptCtx_.gameVars.Set("mana", script::Value::Num(have - def->manaCost));
    }
    if (def->cooldown > 0.0f) cds[name] = def->cooldown;

    if (def->kind == "projectile") {
        Projectile p;
        p.pos = origin;
        p.dir = dir.LengthSq() > 1e-6f ? dir.Normalized() : math::Vec3{0, 0, 1};
        p.speed = def->speed;
        p.damage = def->damage;
        p.life = def->life;
        p.range = def->range;
        p.caster = caster;
        p.statuses = def->statuses;
        projectiles_.push_back(p);
        return 1;
    }

    if (def->kind == "melee") {
        // G3-4: the skill hit test honours the auto lag-comp rewind set by
        // the server (targets tested at the pose they had `autoRewindTicks_`
        // ticks ago); damage lands on the current entity.
        MeleeAttackImpl(origin, dir, def->meleeRange, def->arcDeg, def->damage,
                        autoRewindTicks_, caster, def->statuses);
        return 1; // the cast happened even when no target was in the arc
    }

    // "box": oriented attack box; yaw derived from the facing dir so a
    // script passes a direction vector like every other skill.
    const float yaw = std::atan2(dir.x, dir.z);
    AttackBoxImpl(origin, {def->boxHalfX, def->boxHalfY, def->boxHalfZ}, yaw, def->damage,
                  autoRewindTicks_);
    return 1;
}

float GameRuntime::SkillCooldownLeft(const std::string& name, ecs::Entity caster) const {
    const auto it = skillCooldowns_.find(EntityKey(caster));
    if (it == skillCooldowns_.end()) return 0.0f;
    const auto cd = it->second.find(name);
    return cd == it->second.end() ? 0.0f : cd->second;
}

bool GameRuntime::HasStatus(ecs::Entity ent, uint32_t id) const {
    const StatusComponent* c = world_.Get<StatusComponent>(ent);
    return c ? scene::HasStatus(*c, id) : false;
}

float GameRuntime::StatusMagnitude(ecs::Entity ent, uint32_t id) const {
    const StatusComponent* c = world_.Get<StatusComponent>(ent);
    return c ? scene::StatusMagnitude(*c, id) : 0.0f;
}

void GameRuntime::TickProjectiles(float dt) {
    for (auto it = projectiles_.begin(); it != projectiles_.end();) {
        Projectile& p = *it;
        const float step = p.speed * dt;
        p.pos += p.dir * step;
        p.traveled += step;
        p.life -= dt;
        // Data-driven skills can bound a projectile by travel distance.
        if (p.range > 0.0f && p.traveled >= p.range) {
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
            ApplyHit(target, p.damage, p.statuses);
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
