#include "neon/assets/async_loader.hpp"

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <deque>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <pthread.h>
#include <sched.h>
#include <unistd.h>
#endif

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

void SleepMs(int ms) {
#if defined(_WIN32)
    ::Sleep(static_cast<DWORD>(ms));
#else
    // sched_yield then a short sleep so an idle worker never hammers the CPU.
    ::sched_yield();
    ::usleep(static_cast<useconds_t>(ms) * 1000);
#endif
}

} // namespace

struct AsyncLoader::Impl {
    SpinLock lock;
    std::deque<std::function<void()>> pending;
    std::deque<std::function<void()>> ready;
    std::atomic<bool> stop{false};
    int workerCount = 2;
    bool available = false;

#if defined(_WIN32)
    std::vector<HANDLE> threads;
#else
    std::vector<pthread_t> threads;
#endif
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
        } else {
            SleepMs(1);
        }
    }
}

#if defined(_WIN32)
unsigned long __stdcall AsyncLoader::WinWorkerEntry(void* param) {
    WorkerLoop(static_cast<Impl*>(param));
    return 0;
}
#else
void* AsyncLoader::PosixWorkerEntry(void* param) {
    WorkerLoop(static_cast<Impl*>(param));
    return nullptr;
}
#endif

AsyncLoader::AsyncLoader(int workerCount) : impl_(std::make_unique<Impl>()) {
    impl_->workerCount = std::max(workerCount, 0);
    if (impl_->workerCount == 0) return;
    impl_->available = true;

    for (int i = 0; i < impl_->workerCount; ++i) {
#if defined(_WIN32)
        HANDLE h = ::CreateThread(nullptr, 0, &AsyncLoader::WinWorkerEntry, impl_.get(), 0, nullptr);
        if (!h) {
            impl_->available = false;
            break;
        }
        impl_->threads.push_back(h);
#else
        pthread_t t;
        if (::pthread_create(&t, nullptr, &AsyncLoader::PosixWorkerEntry, impl_.get()) != 0) {
            impl_->available = false;
            break;
        }
        impl_->threads.push_back(t);
#endif
    }

    if (!impl_->available) {
        // Some workers started before the failure: stop + join them so the
        // pool shuts down cleanly (available stays false afterwards).
        impl_->stop.store(true, std::memory_order_relaxed);
        for (auto& t : impl_->threads) {
#if defined(_WIN32)
            ::WaitForSingleObject(t, INFINITE);
            ::CloseHandle(t);
#else
            ::pthread_join(t, nullptr);
#endif
        }
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
        for (auto& t : impl_->threads) {
#if defined(_WIN32)
            ::WaitForSingleObject(t, INFINITE);
            ::CloseHandle(t);
#else
            ::pthread_join(t, nullptr);
#endif
        }
        impl_->threads.clear();
    }
    {
        impl_->lock.Lock();
        impl_->pending.clear();
        impl_->ready.clear();
        impl_->lock.Unlock();
    }
    impl_->available = false;
}

} // namespace neon::assets
