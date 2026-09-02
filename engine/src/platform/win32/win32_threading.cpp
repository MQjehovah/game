#include "neon/platform/threading.hpp"

#include <windows.h>

namespace neon::platform {
namespace {

// Adapts the portable ThreadEntry (void(*)(void*)) to the Win32 __stdcall
// DWORD(LPVOID) shape: the entry+param pair is heap-packed into the single
// LPVOID slot and freed by the thunk after the call.
struct ThreadArgs {
    ThreadEntry entry;
    void* param;
};

DWORD __stdcall WinThreadThunk(void* raw) {
    ThreadArgs* args = static_cast<ThreadArgs*>(raw);
    ThreadEntry entry = args->entry;
    void* param = args->param;
    delete args;
    entry(param);
    return 0;
}

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
    handle_ = ::CreateThread(nullptr, 0, &WinThreadThunk, args, 0, nullptr);
    if (!handle_) {
        delete args;
        return false;
    }
    return true;
}

void Thread::Join() {
    if (!handle_) return;
    ::WaitForSingleObject(handle_, INFINITE);
    ::CloseHandle(handle_);
    handle_ = nullptr;
}

bool Semaphore::Create() {
    if (impl_) return true;
    // Long max count: posts may exceed jobs; excess counts are harmless (a
    // woken waiter with nothing to consume just re-waits).
    impl_ = ::CreateSemaphoreW(nullptr, 0, 0x7FFFFFFF, nullptr);
    return impl_ != nullptr;
}

Semaphore::~Semaphore() { Destroy(); }

void Semaphore::Destroy() {
    if (!impl_) return;
    ::CloseHandle(static_cast<HANDLE>(impl_));
    impl_ = nullptr;
}

void Semaphore::Post() {
    if (!impl_) return;
    ::ReleaseSemaphore(static_cast<HANDLE>(impl_), 1, nullptr);
}

void Semaphore::Wait(int timeoutMs) {
    if (!impl_) return;
    ::WaitForSingleObject(static_cast<HANDLE>(impl_),
                          timeoutMs < 0 ? INFINITE : static_cast<DWORD>(timeoutMs));
}

void SleepMs(int ms) { ::Sleep(static_cast<DWORD>(ms)); }

int CpuCount() {
    SYSTEM_INFO si;
    ::GetSystemInfo(&si);
    const DWORD n = si.dwNumberOfProcessors;
    return n > 0 ? static_cast<int>(n) : 1;
}

}  // namespace neon::platform
