#include "neon/core/mem_stats.hpp"

#include <atomic>
#include <new>

#include <cstdlib>

#if defined(_WIN32)
#include <malloc.h>
static size_t UsableSize(const void* p) { return _msize(const_cast<void*>(p)); }
#else
#include <malloc.h>
static size_t UsableSize(const void* p) { return malloc_usable_size(const_cast<void*>(p)); }
#endif

namespace neon::core {

namespace {
struct Counter {
    std::atomic<uint64_t> allocCount{0};
    std::atomic<uint64_t> allocBytes{0};
    std::atomic<int64_t> liveBytes{0};
    std::atomic<int64_t> liveAllocs{0};
    std::atomic<int64_t> peakLiveBytes{0};
};
Counter g_counters;
} // namespace

MemStats::Snapshot MemStats::SnapshotNow() {
    Snapshot s;
    s.allocCount = g_counters.allocCount.load(std::memory_order_relaxed);
    s.allocBytes = g_counters.allocBytes.load(std::memory_order_relaxed);
    s.liveBytes = g_counters.liveBytes.load(std::memory_order_relaxed);
    s.liveAllocs = g_counters.liveAllocs.load(std::memory_order_relaxed);
    s.peakLiveBytes = g_counters.peakLiveBytes.load(std::memory_order_relaxed);
    return s;
}

void MemStats::TrackAlloc(std::size_t bytes) {
    g_counters.allocCount.fetch_add(1, std::memory_order_relaxed);
    g_counters.allocBytes.fetch_add(bytes, std::memory_order_relaxed);
    const int64_t live = g_counters.liveBytes.fetch_add(static_cast<int64_t>(bytes),
                                                        std::memory_order_relaxed) +
                         static_cast<int64_t>(bytes);
    g_counters.liveAllocs.fetch_add(1, std::memory_order_relaxed);
    int64_t peak = g_counters.peakLiveBytes.load(std::memory_order_relaxed);
    while (live > peak &&
           !g_counters.peakLiveBytes.compare_exchange_weak(peak, live,
                                                           std::memory_order_relaxed)) {
    }
}

void MemStats::TrackFree(std::size_t bytes) {
    g_counters.liveBytes.fetch_sub(static_cast<int64_t>(bytes), std::memory_order_relaxed);
    g_counters.liveAllocs.fetch_sub(1, std::memory_order_relaxed);
}

} // namespace neon::core

// ---------------------------------------------------------------------------
// G6-3: global operator new/delete overrides. Pure counting wrappers over
// malloc/free — never allocators themselves — so every heap allocation in the
// engine and its hosts is measured without changing allocation behavior.
//
// Counting is EXACT for malloc-backed blocks: allocation records the block's
// usable size and unsized delete recovers the same number from _msize/
// malloc_usable_size, so liveBytes does not drift. Sized delete passes the
// caller-known size (matches the requested size; a rounded-up block simply
// settles the same as its allocation). Over-aligned (C++17) allocations are
// intentionally left to the CRT so we never call _msize on an _aligned_malloc
// block; they are simply not counted.
// ---------------------------------------------------------------------------

using neon::core::MemStats;

void* operator new(std::size_t size) {
    void* p = std::malloc(size);
    if (!p) throw std::bad_alloc();
    MemStats::TrackAlloc(UsableSize(p));
    return p;
}
void* operator new[](std::size_t size) {
    void* p = std::malloc(size);
    if (!p) throw std::bad_alloc();
    MemStats::TrackAlloc(UsableSize(p));
    return p;
}
void operator delete(void* p) noexcept {
    if (p) MemStats::TrackFree(UsableSize(p));
    std::free(p);
}
void operator delete[](void* p) noexcept {
    if (p) MemStats::TrackFree(UsableSize(p));
    std::free(p);
}
void operator delete(void* p, std::size_t size) noexcept {
    if (p) MemStats::TrackFree(size);
    std::free(p);
}
void operator delete[](void* p, std::size_t size) noexcept {
    if (p) MemStats::TrackFree(size);
    std::free(p);
}

void* operator new(std::size_t size, const std::nothrow_t&) noexcept {
    void* p = std::malloc(size);
    if (p) MemStats::TrackAlloc(UsableSize(p));
    return p;
}
void* operator new[](std::size_t size, const std::nothrow_t&) noexcept {
    void* p = std::malloc(size);
    if (p) MemStats::TrackAlloc(UsableSize(p));
    return p;
}
void operator delete(void* p, const std::nothrow_t&) noexcept {
    if (p) MemStats::TrackFree(UsableSize(p));
    std::free(p);
}
void operator delete[](void* p, const std::nothrow_t&) noexcept {
    if (p) MemStats::TrackFree(UsableSize(p));
    std::free(p);
}
