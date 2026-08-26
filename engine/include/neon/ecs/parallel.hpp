#pragma once
#include <algorithm>
#include <atomic>
#include <cassert>
#include <cstdint>
#include <functional>
#include <memory>
#include <vector>

#include "neon/core/log.hpp"

// Deterministic parallel-for jobs over contiguous index ranges.
//
// Threading: this toolchain (MinGW 8.1 win32 model) has NO std::thread
// (__STDCPP_THREADS__ is undefined), so the pool uses the OS primitive
// directly - Win32 CreateThread here, a POSIX pthread branch for CI (the same
// pattern as neon/assets/async_loader). A platform entry trampoline is
// declared below and implemented in parallel.cpp (the header must not pull in
// windows.h; the impl does).
//
// Determinism: ParallelFor(count, fn) splits [0, count) into a FIXED number of
// contiguous, non-overlapping chunks (chunks <= workers) and hands each chunk
// to a worker. Every index is visited exactly once no matter how the OS
// schedules threads or how the pool distributes the chunks (G5-2: workers pull
// the next chunk from a shared atomic counter — dynamic work distribution — so
// a fast worker runs the next pending chunk instead of idling). For workloads
// where fn(start, end) only touches items in [start, end) and private state,
// the result is bit-identical to running the whole loop serially, and
// bit-identical across runs.
//
// Thread-safety contract: the functor MUST touch ONLY the items in the range
// it is given and its own captured state. It MUST NOT mutate the ECS World
// (Create/Destroy/Add/Remove) and MUST NOT share mutable state across chunks
// without explicit synchronization (see parallel::Reducer for a per-chunk
// reduction helper).
namespace neon::ecs {
namespace parallel {

// Per-chunk reduction accumulator. Use it when a ParallelFor needs to combine
// per-item results: call Slot(chunkStart, total) once per chunk and accumulate
// into the returned reference; call Combine() serially afterwards. Slot maps
// the chunk (identified by its first index) to a UNIQUE slot via ParallelFor's
// exact static partition, so the slot layout is identical on every run (float
// reductions are stable run-to-run) and no two chunks ever share a slot. For
// associative/commutative types (e.g. int64) the combined result also equals a
// fully serial reduction bit-for-bit.
//
// Sizing contract: `total` must equal the count passed to ParallelFor, and
// `slots` must be >= the worker count of the pool used (AvailableWorkers() for
// the global pool). When `slots` is too small, distinct chunks collapse onto
// one slot: Slot() detects the collision, logs an error (category ecs) and
// Ok() returns false so the misconfiguration is visible even in Release.
template <class T>
class Reducer {
public:
    explicit Reducer(int slots)
        : data_(static_cast<size_t>(std::max(slots, 1)), T{}), visits_(data_.size()) {
        for (std::atomic<int>& v : visits_) v.store(0, std::memory_order_relaxed);
    }

    // First index of the c-th chunk under ParallelFor's static partition.
    static uint32_t ChunkStart(uint32_t c, uint32_t total, uint32_t chunks) {
        return static_cast<uint32_t>((static_cast<uint64_t>(c) * total) / chunks);
    }

    // Deterministic accumulator slot for the chunk that starts at `chunkStart`.
    T& Slot(uint32_t chunkStart, uint32_t total) {
        const int n = static_cast<int>(data_.size());
        const int chunks = std::min(n, static_cast<int>(total));
        if (chunks <= 0) return data_[0];
        // Chunk boundaries are strictly increasing when chunks <= total, so the
        // unique c with ChunkStart(c, ...) == chunkStart is found by a search.
        uint32_t lo = 0, hi = static_cast<uint32_t>(chunks);
        while (lo + 1 < hi) {
            const uint32_t mid = lo + (hi - lo) / 2;
            if (ChunkStart(mid, total, static_cast<uint32_t>(chunks)) <= chunkStart) lo = mid;
            else hi = mid;
        }
        if (visits_[lo].fetch_add(1, std::memory_order_relaxed) != 0) {
            // slots < chunk count: two chunks collapsed onto one slot.
            assert(false && "parallel::Reducer: slots smaller than the chunk count");
            NEON_LOG_CAT(neon::core::LogCategory::Ecs, neon::core::LogLevel::Error,
                         "parallel::Reducer slot %u reused: size the Reducer with >= "
                         "the pool's worker count",
                         static_cast<unsigned>(lo));
        }
        return data_[lo];
    }

    const std::vector<T>& Slots() const { return data_; }

    // True when no slot was handed to more than one chunk. False signals a
    // sizing mistake (slots < pool worker count) that would have shared a slot.
    bool Ok() const {
        for (size_t i = 0; i < visits_.size(); ++i) {
            if (visits_[i].load(std::memory_order_relaxed) > 1) return false;
        }
        return true;
    }

    // Serial combine of every slot in fixed order. Deterministic across runs.
    T Combine() const {
        assert(Ok());
        T sum{};
        for (const T& s : data_) sum = sum + s;
        return sum;
    }

private:
    std::vector<T> data_;
    std::vector<std::atomic<int>> visits_;
};

// Bounded worker pool that runs ParallelFor chunks off the calling thread.
class ThreadPool {
public:
    // workerCount: -1 = auto (hardware concurrency); 0 = no threads, every
    // ParallelFor falls back to a serial loop (used by tests and on platforms
    // that cannot spawn threads).
    explicit ThreadPool(int workerCount = -1);
    ~ThreadPool();

    ThreadPool(const ThreadPool&) = delete;
    ThreadPool& operator=(const ThreadPool&) = delete;

    // True while the pool is running worker threads.
    bool Available() const;

    // Number of worker threads actually running (0 when unavailable).
    int WorkerCount() const;

    // Splits [0, count) into contiguous chunks and runs fn(start, end) once
    // per chunk on the workers, then joins. When the pool is unavailable or
    // count <= 1, runs fn(0, count) serially on the calling thread.
    //
    // Synchronous + single-submitter: ParallelFor blocks the calling thread
    // until every chunk has finished (it joins before returning), so it must
    // NOT be called re-entrantly from inside a chunk (that would deadlock),
    // and only one ParallelFor may be in flight on the pool at a time.
    void ParallelFor(uint32_t count, std::function<void(uint32_t start, uint32_t end)> fn);

private:
    struct Impl;
    static void WorkerLoop(Impl* impl);
#if defined(_WIN32)
    static unsigned long __stdcall WinWorkerEntry(void* param);
#else
    static void* PosixWorkerEntry(void* param);
#endif
    std::unique_ptr<Impl> impl_;
};

// Process-wide default pool used by the free functions below. Lazily created
// on first use with an automatic worker count.
ThreadPool& GlobalPool();

// Number of worker threads the global pool can run concurrently (0 when the
// build/platform cannot spawn threads or the pool has not been started).
int AvailableWorkers();

// Deterministic parallel-for over [0, count) using the global pool; falls back
// to serial when no workers are available. See the header comment for the
// determinism guarantee and the thread-safety contract.
void ParallelFor(uint32_t count, std::function<void(uint32_t start, uint32_t end)> fn);

} // namespace parallel
} // namespace neon::ecs
