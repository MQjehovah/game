#include "client_sync.hpp"

#include <cmath>

#include "neon/math/math.hpp"

namespace neon::client {
namespace {

// Shortest-arc angular interpolation: yaw is a heading, so a jump from ~+pi to
// ~-pi must interpolate across the wrap, not through 2*pi.
float AngleLerp(float a, float b, double t) {
    double d = std::fmod(static_cast<double>(b - a), 2.0 * static_cast<double>(math::kPi));
    if (d > math::kPi) d -= 2.0 * math::kPi;
    if (d < -math::kPi) d += 2.0 * math::kPi;
    return static_cast<float>(a + d * t);
}

} // namespace

InterpolatedEntity ClientSync::ToInterp(const net::SnapshotEntity& e) {
    InterpolatedEntity out;
    out.id = e.id;
    out.pos = {e.x, e.y, e.z};
    out.yaw = e.yaw;
    return out;
}

const net::SnapshotEntity* ClientSync::Find(const net::MsgSnapshot& snap, uint64_t entityId) {
    for (const net::SnapshotEntity& e : snap.entities)
        if (e.id == entityId) return &e;
    return nullptr;
}

core::Result<InterpolatedEntity> ClientSync::FindResult(const net::MsgSnapshot& snap,
                                                        uint64_t entityId) const {
    const net::SnapshotEntity* e = Find(snap, entityId);
    if (!e) return core::Result<InterpolatedEntity>::Err("client: entity not in snapshot");
    return core::Result<InterpolatedEntity>::Ok(ToInterp(*e));
}

void ClientSync::OnSnapshot(const net::MsgSnapshot& snap) {
    // B13: fragmented snapshots arrive as consecutive parts (the reliable
    // channel is ordered and dedupes retransmits by sequence number, so parts
    // never duplicate). Assemble before the stale/duplicate checks.
    if (snap.partCount > 1) {
        if (!pendingActive_ || pendingTick_ != snap.tick || pendingCount_ != snap.partCount) {
            // A new (or restarted) fragment group: begin assembly.
            pendingActive_ = true;
            pendingTick_ = snap.tick;
            pendingCount_ = snap.partCount;
            pendingEntities_.clear();
        }
        pendingEntities_.insert(pendingEntities_.end(), snap.entities.begin(),
                                snap.entities.end());
        if (snap.partIndex + 1 == snap.partCount) {
            net::MsgSnapshot merged;
            merged.tick = snap.tick;
            merged.entities = std::move(pendingEntities_);
            merged.entityCount = static_cast<uint32_t>(merged.entities.size());
            pendingActive_ = false;
            pendingEntities_.clear();
            OnSnapshot(merged); // reassembled: recurse with the unfragmented path
        }
        return; // waiting for more parts
    }
    pendingActive_ = false;
    pendingEntities_.clear();

    // Stale or duplicate ticks (e.g. a re-sent reliable frame) are dropped so
    // the buffer stays strictly ascending.
    if (!snapshots_.empty() && static_cast<double>(snap.tick) <= currentServerTick_) return;
    snapshots_.push_back(snap);
    currentServerTick_ = static_cast<double>(snap.tick);
    if (snapshots_.size() > cfg_.maxSnapshots) snapshots_.pop_front();
    // An entity that reappears in a snapshot is no longer despawned.
    for (const net::SnapshotEntity& e : snap.entities) despawned_.erase(e.id);
}

void ClientSync::OnDespawn(uint64_t entityId) { despawned_.insert(entityId); }

core::Result<InterpolatedEntity> ClientSync::Sample(uint64_t entityId,
                                                    double renderTick) const {
    if (despawned_.count(entityId) != 0)
        return core::Result<InterpolatedEntity>::Err("client: entity despawned");
    if (snapshots_.empty())
        return core::Result<InterpolatedEntity>::Err("client: no snapshots buffered");

    const net::MsgSnapshot& newest = snapshots_.back();
    const double newestTick = static_cast<double>(newest.tick);

    // Clamp to the newest state (the render clock may briefly outrun the
    // stream, e.g. right after connect before the delay builds up).
    if (renderTick >= newestTick) return FindResult(newest, entityId);

    // Find the adjacent snapshot pair bracketing renderTick. snapshots_ is
    // small (Config::maxSnapshots), so a linear scan is fine.
    for (size_t i = 0; i + 1 < snapshots_.size(); ++i) {
        const net::MsgSnapshot& a = snapshots_[i];
        const net::MsgSnapshot& b = snapshots_[i + 1];
        if (renderTick < static_cast<double>(b.tick)) {
            const net::SnapshotEntity* ea = Find(a, entityId);
            const net::SnapshotEntity* eb = Find(b, entityId);
            if (eb) {
                if (ea && b.tick > a.tick) {
                    // Present in both: interpolate.
                    const double t = (renderTick - static_cast<double>(a.tick)) /
                                     static_cast<double>(b.tick - a.tick);
                    const double bt = t < 0.0 ? 0.0 : (t > 1.0 ? 1.0 : t);
                    InterpolatedEntity out;
                    out.id = eb->id;
                    out.pos = math::Lerp({ea->x, ea->y, ea->z}, {eb->x, eb->y, eb->z},
                                         static_cast<float>(bt));
                    out.yaw = AngleLerp(ea->yaw, eb->yaw, bt);
                    return core::Result<InterpolatedEntity>::Ok(out);
                }
                // Spawned between a and b: appear at the S_b position.
                return core::Result<InterpolatedEntity>::Ok(ToInterp(*eb));
            }
            if (ea) {
                // Despawned between a and b: hold S_a until the despawn.
                return core::Result<InterpolatedEntity>::Ok(ToInterp(*ea));
            }
            return core::Result<InterpolatedEntity>::Err("client: entity not in window");
        }
    }

    // Clamp to the oldest state (render clock trailing far behind the stream).
    return FindResult(snapshots_.front(), entityId);
}

bool ClientSync::NeedsReconcile(uint64_t entityId, const math::Vec3& localPos,
                                math::Vec3* correctionOut) const {
    if (snapshots_.empty()) return false;
    const net::SnapshotEntity* se = Find(snapshots_.back(), entityId);
    if (!se) return false; // not in the latest snapshot: nothing authoritative to snap to
    const math::Vec3 serverPos{se->x, se->y, se->z};
    if (math::Distance(localPos, serverPos) > cfg_.reconcileThreshold) {
        if (correctionOut) *correctionOut = serverPos;
        return true;
    }
    return false;
}

bool ClientSync::CheckControlled(const math::Vec3& localPos, math::Vec3* correctionOut) const {
    return NeedsReconcile(cfg_.controlledEntityId, localPos, correctionOut);
}

const net::MsgSnapshot* ClientSync::Latest() const {
    return snapshots_.empty() ? nullptr : &snapshots_.back();
}

void ClientSync::Clear() {
    snapshots_.clear();
    despawned_.clear();
    currentServerTick_ = 0.0;
}

} // namespace neon::client
