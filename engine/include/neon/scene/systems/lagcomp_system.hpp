#pragma once

// C1: standalone server-authoritative lag-compensation subsystem (split out of
// GameRuntime). Every fixed tick the host records the authoritative poses into
// a ring buffer; spatial overlap queries rewind to the pose a target had
// `rewindTicks` ticks ago while testing the CURRENT entity. Owns the pose ring
// + the auto-rewind state; no GameRuntime / world_ dependency — the caller
// collects poses and passes pre-keyed {EntityKey, pos} pairs.
//
// Not installed as part of the public API surface beyond neon_scene.

#include <cstdint>
#include <unordered_map>
#include <utility>
#include <vector>

#include "neon/ecs/world.hpp"
#include "neon/math/vec3.hpp"

namespace neon::scene {

// 服务器权威模拟的延迟补偿：每固定 tick 记录实体权威姿态到环形缓冲，
// 供 Overlap 查询回滚到射手当时看到的位置。
class LagCompSystem {
public:
    static constexpr uint32_t kHistoryTicks = 64;
    // Appends one authoritative snapshot (EntityKey -> position) to the ring,
    // evicting the oldest snapshot when full. `poses` must carry pre-keyed
    // EntityKeys (the caller does the ecs::Entity -> uint64_t conversion).
    void Record(const std::vector<std::pair<uint64_t, math::Vec3>>& poses);
    // The pose `e` had `rewindTicks` fixed ticks ago when history exists; false
    // when no snapshot that old exists for it (fresh spawns / shallow history
    // degrade gracefully to the caller's current-pose path).
    bool Position(ecs::Entity e, uint32_t rewindTicks, math::Vec3& out) const;
    void SetAutoRewind(uint32_t t) { autoRewindTicks_ = t; }
    uint32_t AutoRewindTicks() const { return autoRewindTicks_; }
    size_t Size() const { return poseSlots_.size(); }
    // Drops all history + the auto-rewind (host teardown; keeps slot reuse so
    // the next tick does not re-allocate the ring).
    void Clear();

private:
    std::vector<std::unordered_map<uint64_t, math::Vec3>> poseSlots_;
    size_t poseHead_ = 0;   // slot index of the OLDEST snapshot
    size_t poseCount_ = 0;  // number of valid snapshots (<= capacity)
    uint32_t autoRewindTicks_ = 0; // rewind used by overlap queries / hit tests
};

} // namespace neon::scene
