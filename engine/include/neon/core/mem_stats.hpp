#pragma once

#include <cstddef>
#include <cstdint>

namespace neon::core {

// G6-3 global heap tracker: counts allocations/bytes across the engine's heap
// via the operator new/delete overrides compiled into neon_engine (mem_stats.cpp).
// Gives the editor's performance panel real process-side heap numbers and a
// starting point to confirm fragmentation pressure before any compacting
// allocator work. The overrides are pure counters that forward to malloc/free —
// allocation behavior is unchanged, so this is safe to always enable.
class MemStats {
public:
    struct Snapshot {
        uint64_t allocCount = 0;    // total allocations since process start
        uint64_t allocBytes = 0;    // total bytes allocated since process start
        int64_t liveBytes = 0;      // currently live bytes
        int64_t liveAllocs = 0;     // currently live allocation count
        int64_t peakLiveBytes = 0;  // high-water mark of liveBytes
    };

    static Snapshot SnapshotNow();

    // Reporting hooks used by the new/delete overrides (and unit tests).
    static void TrackAlloc(std::size_t bytes);
    static void TrackFree(std::size_t bytes);
};

} // namespace neon::core
