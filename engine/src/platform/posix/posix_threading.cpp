#include "neon/platform/threading.hpp"

#include <pthread.h>
#include <semaphore.h>
#include <sched.h>
#include <unistd.h>

namespace neon::platform {
namespace {

struct ThreadArgs {
    ThreadEntry entry;
    void* param;
};

void* PosixThreadThunk(void* raw) {
    ThreadArgs* args = static_cast<ThreadArgs*>(raw);
    ThreadEntry entry = args->entry;
    void* param = args->param;
    delete args;
    entry(param);
    return nullptr;
}

// Heap-held unnamed semaphore keeps the public header free of <semaphore.h>.
struct PosixSemaphore {
    sem_t sem;
};

}  // namespace

Thread::~Thread() { Join(); }

Thread::Thread(Thread&& other) noexcept : handle_(other.handle_) {
    other.handle_ = nullptr;
}

Thread& Thread::operator=(Thread&& other) noexcept {
    if (this != &other) {
        Join();
        handle_ = other.handle_;
        other.handle_ = nullptr;
    }
    return *this;
}

bool Thread::Start(ThreadEntry entry, void* param) {
    if (handle_ || !entry) return false;
    ThreadArgs* args = new ThreadArgs{entry, param};
    pthread_t t;
    if (::pthread_create(&t, nullptr, &PosixThreadThunk, args) != 0) {
        delete args;
        return false;
    }
    // Stash the pthread_t in our own heap cell so the void* handle stays
    // stable while Join() needs both the cell and the pthread_t value.
    auto* cell = new pthread_t(t);
    handle_ = cell;
    return true;
}

void Thread::Join() {
    if (!handle_) return;
    auto* cell = static_cast<pthread_t*>(handle_);
    ::pthread_join(*cell, nullptr);
    delete cell;
    handle_ = nullptr;
}

bool Semaphore::Create() {
    if (impl_) return true;
    auto* s = new PosixSemaphore();
    if (::sem_init(&s->sem, 0, 0) != 0) {
        delete s;
        return false;
    }
    impl_ = s;
    return true;
}

Semaphore::~Semaphore() { Destroy(); }

void Semaphore::Destroy() {
    if (!impl_) return;
    auto* s = static_cast<PosixSemaphore*>(impl_);
    ::sem_destroy(&s->sem);
    delete s;
    impl_ = nullptr;
}

void Semaphore::Post() {
    if (!impl_) return;
    auto* s = static_cast<PosixSemaphore*>(impl_);
    ::sem_post(&s->sem);
}

void Semaphore::Wait(int timeoutMs) {
    if (!impl_) return;
    auto* s = static_cast<PosixSemaphore*>(impl_);
    if (timeoutMs < 0) {
        ::sem_wait(&s->sem);
        return;
    }
    // Bounded wait: a try-wait + tiny sleep loop keeps the implementation
    // portable (macOS' sem_timedwait history is spotty) and is fine for the
    // chunky decode work these semaphores gate.
    int waited = 0;
    while (::sem_trywait(&s->sem) != 0) {
        if (timeoutMs == 0 || waited >= timeoutMs) return;
        ::sched_yield();
        ::usleep(1000);
        waited += 1;
    }
}

void SleepMs(int ms) {
    ::sched_yield();
    ::usleep(static_cast<useconds_t>(ms) * 1000);
}

int CpuCount() {
    const long n = ::sysconf(_SC_NPROCESSORS_ONLN);
    return n > 0 ? static_cast<int>(n) : 1;
}

}  // namespace neon::platform
