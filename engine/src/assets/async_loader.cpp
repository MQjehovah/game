#include "neon/assets/async_loader.hpp"
#include "neon/platform/threading.hpp"

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <deque>

namespace neon::assets {

namespace {

// Minimal spinlock: std::mutex needs OS thread support which some toolchains
// (e.g. MinGW 8.1 win32 model) lack; atomic_flag is pure header-only. Mirrors
// the pattern already used by core/log.cpp.
struct SpinLock {
    std::atomic_flag flag = ATOMIC_FLAG_INIT;
    void Lock() {
        while (flag.test_and_set(std::memory_order_acquire)) {
        }
    }
    void Unlock() { flag.clear(std::memory_order_release); }
};

} // namespace

struct AsyncLoader::Impl {
    SpinLock lock;
    std::deque<std::function<void()>> pending;
    std::deque<std::function<void()>> ready;
    std::atomic<bool> stop{false};
    int workerCount = 2;
    bool available = false;

    std::vector<platform::Thread> threads;
    // Counting semaphore: Submit() posts one count per job; workers block in
    // Semaphore::Wait instead of polling with Sleep(1) — wakeups are immediate
    // and idle workers cost zero CPU.
    platform::Semaphore wake;
};

void AsyncLoader::WorkerLoop(Impl* impl) {
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
            continue;
        }
        // Block until Submit() posts a wake count (or a short timeout elapses,
        // which doubles as the stop-flag poll while shutting down). A spurious
        // wake with an empty queue is harmless: the loop re-locks, finds
        // nothing, and waits again.
        impl->wake.Wait(100);
    }
}

void AsyncLoader::WorkerEntryTrampoline(void* param) {
    WorkerLoop(static_cast<Impl*>(param));
}

AsyncLoader::AsyncLoader(int workerCount) : impl_(std::make_unique<Impl>()) {
    impl_->workerCount = std::max(workerCount, 0);
    if (impl_->workerCount == 0) return;
    if (!impl_->wake.Create()) return;
    impl_->available = true;

    for (int i = 0; i < impl_->workerCount; ++i) {
        platform::Thread t;
        // Start before moving into the vector: Start() runs workers that may
        // touch impl_ fields immediately, so the semaphore above must already
        // be live (it is) and the push_back below only stores the handle.
        if (!t.Start(&AsyncLoader::WorkerEntryTrampoline, impl_.get())) {
            impl_->available = false;
            break;
        }
        impl_->threads.push_back(std::move(t));
    }

    if (!impl_->available) {
        // Some workers started before the failure: stop + join them so the
        // pool shuts down cleanly (available stays false afterwards).
        impl_->stop.store(true, std::memory_order_relaxed);
        // Wake every blocked worker so they observe the stop flag now instead
        // of at the next wait timeout.
        for (size_t i = 0; i < impl_->threads.size(); ++i) impl_->wake.Post();
        for (auto& t : impl_->threads) t.Join();
        impl_->threads.clear();
    }
}

AsyncLoader::~AsyncLoader() { Shutdown(); }

bool AsyncLoader::Available() const { return impl_ && impl_->available; }

bool AsyncLoader::Submit(std::function<void()> work) {
    if (!impl_ || !impl_->available || !work) return false;
    impl_->lock.Lock();
    impl_->pending.push_back(std::move(work));
    impl_->lock.Unlock();
    impl_->wake.Post();
    return true;
}

void AsyncLoader::Deliver(std::function<void()> completion) {
    if (!impl_ || !completion) return;
    impl_->lock.Lock();
    impl_->ready.push_back(std::move(completion));
    impl_->lock.Unlock();
}

int AsyncLoader::Pump() {
    if (!impl_) return 0;
    std::deque<std::function<void()>> local;
    {
        impl_->lock.Lock();
        local.swap(impl_->ready);
        impl_->lock.Unlock();
    }
    int drained = 0;
    while (!local.empty()) {
        std::function<void()> completion = std::move(local.front());
        local.pop_front();
        completion();
        ++drained;
    }
    return drained;
}

void AsyncLoader::Shutdown() {
    if (!impl_) return;
    if (impl_->available || !impl_->threads.empty()) {
        impl_->stop.store(true, std::memory_order_relaxed);
        // Wake every blocked worker so they observe the stop flag now instead
        // of at the next wait timeout.
        for (size_t i = 0; i < impl_->threads.size(); ++i) impl_->wake.Post();
        for (auto& t : impl_->threads) t.Join();
        impl_->threads.clear();
    }
    {
        impl_->lock.Lock();
        impl_->pending.clear();
        impl_->ready.clear();
        impl_->lock.Unlock();
    }
    impl_->wake.Destroy();
    impl_->available = false;
}

} // namespace neon::assets
