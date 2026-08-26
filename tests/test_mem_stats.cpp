#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "neon/neon.hpp"
#include "neon/core/mem_stats.hpp"
#include "helpers.hpp"

using namespace neon;

// G6-3: the global operator new/delete overrides count every heap allocation,
// so liveBytes moves with real allocations and peakLiveBytes is monotonic.
TEST(MemStatsTracksHeapAllocations) {
    const core::MemStats::Snapshot before = core::MemStats::SnapshotNow();

    std::vector<char> big(1u << 20, 0); // ~1 MB via operator new
    const core::MemStats::Snapshot mid = core::MemStats::SnapshotNow();

    // The block must be reflected in the counters (vector may allocate a
    // little more than requested, never less than the payload).
    CHECK(mid.liveBytes >= before.liveBytes + static_cast<int64_t>(1u << 20) - 64);
    CHECK(mid.liveAllocs >= before.liveAllocs + 1);
    CHECK(mid.allocCount >= before.allocCount + 1);
    CHECK(mid.peakLiveBytes >= mid.liveBytes); // peak >= current

    big.clear();
    big.shrink_to_fit(); // release the buffer back to the heap
    const core::MemStats::Snapshot after = core::MemStats::SnapshotNow();
    CHECK(after.liveBytes <= before.liveBytes + 4096); // buffer freed
    CHECK(after.liveAllocs <= before.liveAllocs + 2);
    CHECK(after.peakLiveBytes >= after.liveBytes); // peak stays at high-water

    // A fresh allocation after the release must be counted again.
    {
        std::string payload(2000, 'x');
        const core::MemStats::Snapshot s2 = core::MemStats::SnapshotNow();
        CHECK(s2.liveBytes >= after.liveBytes + 1500);
    }
}

// G6-3: the direct hooks are what the new/delete overrides call — verify the
// counters update and the peak is monotonic without any heap traffic.
TEST(MemStatsDirectHooks) {
    const core::MemStats::Snapshot before = core::MemStats::SnapshotNow();

    core::MemStats::TrackAlloc(1234);
    core::MemStats::TrackAlloc(999);
    const core::MemStats::Snapshot mid = core::MemStats::SnapshotNow();
    CHECK_EQ(mid.liveBytes, before.liveBytes + 1234 + 999);
    CHECK_EQ(mid.liveAllocs, before.liveAllocs + 2);
    CHECK_EQ(mid.allocCount, before.allocCount + 2);

    core::MemStats::TrackFree(1234);
    const core::MemStats::Snapshot after = core::MemStats::SnapshotNow();
    CHECK_EQ(after.liveBytes, before.liveBytes + 999);
    CHECK_EQ(after.liveAllocs, before.liveAllocs + 1);
    CHECK(after.peakLiveBytes >= before.peakLiveBytes);
}
