#include "neon/ecs/parallel.hpp"
#include "neon/platform/threading.hpp"

#include <algorithm>
#include <deque>

namespace neon::ecs {
namespace parallel {

namespace {

// Minimal spinlock: std::mutex needs OS thread support which some toolchains
// (e.g. MinGW 8.1 win32 model) lack; atomic_flag is pure header-only. Mirrors
// the pattern already used by core/log.cpp and assets/async_loader.cpp.
struct SpinLock {
    std::atomic_flag flag = ATOMIC_FLAG_INIT;
    void Lock() {
        while (flag.test_and_set(std::memory_order_acquire)) {
        }
    }
    void Unlock() { flag.clear(std::memory_order_release); }
};

} // namespace

struct ThreadPool::Impl {
    SpinLock lock;
    std::deque<std::function<void()>> pending;
    std::atomic<bool> stop{false};
    bool available = false;

    std::vector<platform::Thread> threads;
};

void ThreadPool::WorkerLoop(Impl* impl) {
    for (;;) {
        if (impl->stop.load(std::memory_order_relaxed)) break;
        std::function<void()> job;
        bool hasJob = false;
        {
            impl->lock.Lock();
            if (!impl->pending.empty()) {
                job = std::move(impl->pending.front());
                impl->pending.pop_front();
                hasJob = true;
            }
            impl->lock.Unlock();
        }
        if (hasJob) {
            job();
        } else {
            platform::SleepMs(1);
        }
    }
}

void ThreadPool::WorkerEntryTrampoline(void* param) {
    WorkerLoop(static_cast<Impl*>(param));
}

ThreadPool::ThreadPool(int workerCount) : impl_(std::make_unique<Impl>()) {
    if (workerCount == 0) return;
    if (workerCount < 0) workerCount = platform::CpuCount();
    const int requested = std::max(workerCount, 0);
    impl_->available = true;

    for (int i = 0; i < requested; ++i) {
        platform::Thread t;
        if (!t.Start(&ThreadPool::WorkerEntryTrampoline, impl_.get())) {
            impl_->available = false;
            break;
        }
        impl_->threads.push_back(std::move(t));
    }

    if (!impl_->available) {
        // Some workers started before the failure: stop + join them so the
        // pool shuts down cleanly (available stays false afterwards).
        impl_->stop.store(true, std::memory_order_relaxed);
        for (auto& t : impl_->threads) t.Join();
        impl_->threads.clear();
    }
}

ThreadPool::~ThreadPool() {
    if (!impl_ || impl_->threads.empty()) return;
    impl_->stop.store(true, std::memory_order_relaxed);
    for (auto& t : impl_->threads) t.Join();
    impl_->threads.clear();
    impl_->lock.Lock();
    impl_->pending.clear();
    impl_->lock.Unlock();
    impl_->available = false;
}

bool ThreadPool::Available() const { return impl_ && impl_->available; }

int ThreadPool::WorkerCount() const {
    if (!impl_ || !impl_->available) return 0;
    return static_cast<int>(impl_->threads.size());
}

void ThreadPool::ParallelFor(uint32_t count, std::function<void(uint32_t, uint32_t)> fn) {
    if (count <= 1 || !impl_ || !impl_->available) {
        fn(0, count);
        return;
    }

    const int workers = static_cast<int>(impl_->threads.size());
    const uint32_t chunks = static_cast<uint32_t>(
        std::min<uint64_t>(static_cast<uint64_t>(workers), count));
    // G5-2: dynamic work distribution. Every grabber (each worker + the calling
    // thread) pulls the next chunk from a shared atomic counter instead of
    // owning a pre-assigned slice, so a fast worker runs the next pending chunk
    // rather than idling ("work stealing" in the load-balancing sense). Chunk
    // boundaries come from the same static partition as before, so every index
    // is visited exactly once and the result stays bit-identical to serial for
    // independent items.
    std::atomic<uint32_t> next{0};
    std::atomic<uint32_t> remaining{0}; // fetched-but-unfinished chunks
    std::atomic<uint32_t> doneJobs{0};
    std::atomic<bool> abort{false};
    SpinLock exLock;
    std::exception_ptr firstException;

    auto grab = [&]() {
        for (;;) {
            if (abort.load(std::memory_order_relaxed)) break;
            const uint32_t c = next.fetch_add(1, std::memory_order_relaxed);
            if (c >= chunks) break;
            const uint32_t start = static_cast<uint32_t>(
                (static_cast<uint64_t>(c) * count) / chunks);
            const uint32_t end = static_cast<uint32_t>(
                (static_cast<uint64_t>(c + 1) * count) / chunks);
            remaining.fetch_add(1, std::memory_order_release);
            try {
                fn(start, end);
            } catch (...) {
                // A4: record the first failure, unwind every grabber, keep the
                // accounting exact so the join below still terminates; the
                // exception is rethrown on the calling thread after join.
                exLock.Lock();
                if (!firstException) firstException = std::current_exception();
                exLock.Unlock();
                abort.store(true, std::memory_order_relaxed);
                next.store(chunks, std::memory_order_relaxed);
            }
            remaining.fetch_sub(1, std::memory_order_release);
            if (abort.load(std::memory_order_relaxed)) break;
        }
    };
    // Worker grab-jobs are separate from the chunks they run: the join below
    // waits for BOTH every chunk AND every grab-job to finish, so no closure on
    // the pending queue still references this call's stack when we return.
    auto workerGrab = [&]() {
        grab();
        doneJobs.fetch_add(1, std::memory_order_release);
    };

    {
        impl_->lock.Lock();
        for (int w = 0; w < workers; ++w)
            impl_->pending.push_back([&]() { workerGrab(); });
        impl_->lock.Unlock();
    }

    grab(); // the calling thread is a worker too

    // Join: every fetched chunk has decremented `remaining` before its grab
    // loop iteration ends, and `doneJobs` reaches `workers` only after every
    // worker grab-job has exited. Both together guarantee no pending closure
    // still touches this stack before this call returns.
    while (remaining.load(std::memory_order_acquire) != 0 ||
           doneJobs.load(std::memory_order_acquire) != static_cast<uint32_t>(workers)) {
        platform::SleepMs(0);
    }
    if (firstException) std::rethrow_exception(firstException);
}

ThreadPool& GlobalPool() {
    static ThreadPool pool;
    return pool;
}

int AvailableWorkers() { return GlobalPool().WorkerCount(); }

void ParallelFor(uint32_t count, std::function<void(uint32_t, uint32_t)> fn) {
    GlobalPool().ParallelFor(count, std::move(fn));
}

} // namespace parallel
} // namespace neon::ecs
