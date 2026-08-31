// C1: GameRuntime combat subsystem (status effects / spatial overlap queries /
// lag-compensated hit tests). Split out of the former single game_runtime.cpp
// TU: these functions own the combat state (poseSlots_) and read
// world_/scriptCtx_. Skill projectiles moved out to ProjectileSystem
// (engine/src/scene/systems/projectile_system.cpp); the script-facing
// GameRuntime::SpawnProjectile forwarder now lives in game_runtime.cpp.
#include "neon/scene/game_runtime.hpp"
#include "game_runtime_priv.hpp"

#include <cmath>

#include "neon/scene/status.hpp"

namespace neon::scene {
using namespace detail; // EntityKey

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

} // namespace neon::scene
