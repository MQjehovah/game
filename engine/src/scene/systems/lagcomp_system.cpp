// C1: LagCompSystem implementation. Migrated verbatim from GameRuntime's
// lag-comp subsystem (LagCompPosition + the Tick-end pose ring): the ring
// buffer and auto-rewind state are now owned here instead of by GameRuntime.
// Pure code movement, no semantic change.
#include "neon/scene/systems/lagcomp_system.hpp"

namespace neon::scene {
namespace {

// Stable 64-bit key for per-entity lag-comp scoping: id occupies the high half
// so an id reused across generations still keys uniquely (matches GameRuntime's
// EntityKey layout, so Position() pairs up with the caller's pre-keyed poses).
uint64_t EntityKey(const ecs::Entity& e) {
    return (static_cast<uint64_t>(e.id) << 32) | static_cast<uint64_t>(e.generation);
}

} // namespace

void LagCompSystem::Record(const std::vector<std::pair<uint64_t, math::Vec3>>& poses) {
    // The ring reuses its snapshot maps (B10): no per-tick heap allocation, and
    // eviction is a head advance, not a vector front-erase.
    if (poseSlots_.empty()) poseSlots_.resize(kHistoryTicks);
    std::unordered_map<uint64_t, math::Vec3>& snap = poseSlots_[poseHead_];
    snap.clear();
    // operator[] (not emplace) so a duplicated EntityKey keeps the LAST entry,
    // matching the original snapshot build (CTransformBind won over
    // SceneTransform for an entity carrying both).
    for (const auto& kv : poses) snap[kv.first] = kv.second;
    poseHead_ = (poseHead_ + 1) % poseSlots_.size();
    if (poseCount_ < poseSlots_.size()) ++poseCount_;
}

// G3-4: the position a hit test uses for `e` - the pose it had `rewindTicks`
// fixed ticks ago when history exists, else false (caller falls back to the
// entity's CURRENT pose; fresh spawns / shallow history degrade gracefully).
bool LagCompSystem::Position(ecs::Entity e, uint32_t rewindTicks, math::Vec3& out) const {
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

void LagCompSystem::Clear() {
    for (auto& s : poseSlots_) s.clear();
    poseHead_ = 0;
    poseCount_ = 0;
    autoRewindTicks_ = 0;
}

} // namespace neon::scene
