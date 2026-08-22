#pragma once

// Client-side synchronization layer (T6.4): buffers the server's snapshot
// stream, interpolates entity transforms between adjacent snapshots for smooth
// rendering, and exposes the reconciliation query that snaps a locally
// predicted entity back to the server when it diverges.
//
// SNAPSHOT BUFFER: OnSnapshot() appends to a ring of the most recent
// snapshots (Config::maxSnapshots); stale/duplicate ticks are dropped and the
// oldest is evicted beyond the cap. Sample() interpolates between two adjacent
// snapshots S_a (tick a) and S_b (tick b): for a renderTick in (a, b) the
// entity's position is lerp(S_a, S_b, (renderTick - a) / (b - a)). An entity
// that appears in S_b but not S_a is shown at its S_b position (spawned); an
// entity in S_a but not S_b holds S_a's position until MsgDespawn arrives.
// Outside the buffer the sample clamps to the nearest snapshot (hold-latest /
// hold-oldest).
//
// TICK-SPACE RENDER CLOCK: Sample() takes the render tick in the server's
// fixed-tick space. The client's render clock trails the latest received
// snapshot by a fixed delay (100ms = 6 ticks at 60Hz, kInterpDelayTicks) so
// the interpolation buffer always brackets it:
//     renderTick = latestTick - kInterpDelayTicks
// The pure module never reads wall time; the caller drives the render tick
// (a fake clock in tests, the network-stream tick in the player).
//
// PREDICTION + RECONCILIATION (v1, snap-on-divergence): the client runs its
// own GameRuntime with the same scene + its own inputs (prediction). On each
// snapshot the caller asks NeedsReconcile(controlledKey, localPos,
// &correction) and, when the local position diverges past
// Config::reconcileThreshold, snaps the local entity to the authoritative
// server position. No multi-frame replay in v1 (T6.7 can add it).
//
// The module is pure and renderer-free so tests drive it with fake clocks and
// hand-built snapshots.

#include <cstdint>
#include <deque>
#include <set>
#include <vector>

#include "neon/core/result.hpp"
#include "neon/core/serialize.hpp"
#include "neon/math/vec3.hpp"
#include "neon/net/protocol.hpp"

namespace neon::client {

// Server fixed tick rate (matches GameServer's 60Hz step).
inline constexpr double kFixedHz = 60.0;
// Render tick trails the latest snapshot by this many ticks (~100ms), giving
// the interpolation buffer room to bracket it.
inline constexpr int kInterpDelayTicks = 6;
// Default snap-on-divergence distance for the controlled entity.
inline constexpr float kReconcileThreshold = 2.0f;

// Serializes a protocol message into the raw body bytes that
// ReliableChannel::Send expects (the transport adds its own frame header,
// protocol version and sequence number). Thin alias onto the canonical
// shared encoder (neon::net::EncodeBody) — no client-local copy anymore.
template <typename T>
std::vector<uint8_t> EncodeBody(const T& msg) {
    return net::EncodeBody(msg);
}

// Interpolated transform of a replicated entity at some render tick.
struct InterpolatedEntity {
    uint64_t id = 0;
    math::Vec3 pos;
    float yaw = 0.0f;
};

// Pure snapshot buffering + interpolation + reconciliation query. See the file
// comment for the model.
class ClientSync {
public:
    struct Config {
        uint64_t maxSnapshots = 8;       // ring depth (oldest evicted beyond this)
        float reconcileThreshold = 2.0f; // |local - server| beyond this snaps
        uint64_t controlledEntityId = 0; // convenience: the controlled entity's stable key
    };

    ClientSync() = default;
    explicit ClientSync(const Config& cfg) : cfg_(cfg) {}

    // Buffers a server snapshot (drops stale/duplicate ticks, evicts the
    // oldest beyond Config::maxSnapshots) and advances the render window.
    void OnSnapshot(const net::MsgSnapshot& snap);
    // Marks an entity despawned: Sample() then returns Err until the entity
    // reappears in a later snapshot.
    void OnDespawn(uint64_t entityId);

    // Interpolated transform of `entityId` at `renderTick` (server tick
    // space). Err when the entity is unknown or despawned.
    core::Result<InterpolatedEntity> Sample(uint64_t entityId, double renderTick) const;

    // Reconciliation query for `entityId`: compares `localPos` against the
    // LATEST snapshot. When the divergence exceeds Config::reconcileThreshold,
    // writes the authoritative (server) position into *correctionOut and
    // returns true. Below the threshold it returns false and the caller keeps
    // predicting (avoids constant snapping from jitter).
    bool NeedsReconcile(uint64_t entityId, const math::Vec3& localPos,
                        math::Vec3* correctionOut) const;
    // Convenience: NeedsReconcile for Config::controlledEntityId.
    bool CheckControlled(const math::Vec3& localPos, math::Vec3* correctionOut) const;

    // The latest received snapshot (authoritative entity set for rendering),
    // or nullptr before the first snapshot.
    const net::MsgSnapshot* Latest() const;

    void Clear();
    uint32_t BufferedSnapshots() const { return static_cast<uint32_t>(snapshots_.size()); }
    double CurrentServerTick() const { return currentServerTick_; }

private:
    static const net::SnapshotEntity* Find(const net::MsgSnapshot& snap, uint64_t entityId);
    static InterpolatedEntity ToInterp(const net::SnapshotEntity& e);
    core::Result<InterpolatedEntity> FindResult(const net::MsgSnapshot& snap,
                                                uint64_t entityId) const;

    Config cfg_;
    std::deque<net::MsgSnapshot> snapshots_; // ascending tick, oldest first
    std::set<uint64_t> despawned_;
    double currentServerTick_ = 0.0;
};

} // namespace neon::client
