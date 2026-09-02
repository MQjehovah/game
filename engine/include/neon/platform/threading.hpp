#pragma once

// Cross-platform threading primitives (threads + counting semaphore + sleep).
// One implementation per platform lives in engine/src/platform/<platform>/;
// higher layers (assets/ecs/audio) must never touch OS headers directly - the
// MinGW win32 toolchain lacks std::thread/std::mutex, so these wrappers use
// the OS API behind the abstraction while callers stay portable.

namespace neon::platform {

// Unified thread entry signature. The platform layer adapts this to the native
// shape (Win32 __stdcall DWORD(LPVOID) / POSIX void*(*)(void*)).
using ThreadEntry = void (*)(void*);

// One OS thread. Start() once; Join() before destruction (the destructor
// joins as a safety net). Move-only: moving transfers the OS handle.
class Thread {
public:
    Thread() = default;
    ~Thread();
    Thread(Thread&& other) noexcept;
    Thread& operator=(Thread&& other) noexcept;
    Thread(const Thread&) = delete;
    Thread& operator=(const Thread&) = delete;

    // Starts `entry(param)` on a new OS thread. Returns false on failure
    // (callers must fall back to a synchronous path).
    bool Start(ThreadEntry entry, void* param);
    // Blocks until the thread finishes, then releases the OS handle. No-op
    // when not joinable.
    void Join();

private:
    void* handle_ = nullptr;  // opaque: HANDLE (win32) / pthread_t (posix)
};

// Counting semaphore. Create() once; Post() releases one count; Wait()
// consumes one count, blocking up to `timeoutMs` (-1 = indefinitely).
// Spurious wakes with a re-checked queue are expected and harmless.
class Semaphore {
public:
    Semaphore() = default;
    ~Semaphore();
    Semaphore(const Semaphore&) = delete;
    Semaphore& operator=(const Semaphore&) = delete;

    bool Create();
    void Destroy();
    void Post();
    void Wait(int timeoutMs);

private:
    void* impl_ = nullptr;  // opaque: HANDLE (win32) / heap sem_t (posix)
};

// Blocks the calling thread ~`ms` milliseconds (scheduler-granularity).
void SleepMs(int ms);

// Logical-processor count for sizing worker pools. Returns >= 1.
int CpuCount();

}  // namespace neon::platform
