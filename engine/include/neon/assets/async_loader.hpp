#pragma once
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace neon::assets {

// A decoded image ready for main-thread GPU upload. Produced off the main
// thread (image decode is pure CPU, no GL) and consumed on the main thread
// (upload + caching + callback). `channels` is 0 when the decode failed.
// `rgba` always holds RGBA8 when the decode succeeded; `bc1` holds BC1 block
// data when the texture was compressed for upload (empty otherwise). Keeping
// `rgba` alongside `bc1` lets the upload fall back to RGBA8 if the driver
// rejects compressed uploads; the RGBA buffer is freed right after upload.
struct DecodedImage {
    int width = 0;
    int height = 0;
    int channels = 0;
    std::vector<uint8_t> rgba; // RGBA8 row-major (always when channels > 0)
    std::vector<uint8_t> bc1;  // BC1 blocks when compressed (may be empty)
};

// Pure-CPU image decode (stbi_load in NATIVE channel count) + optional BC1
// compression. Never touches GL, so it is safe to run on a worker thread.
// `compressBc1` only applies to opaque images (1/3 native channels); alpha-
// bearing images stay RGBA8 (BC1 has no alpha). `channels` is 0 on decode
// failure. The RGBA8 output is byte-identical to the legacy stbi_load(path,
// &w, &h, &ch, 4) path. Public so tests can assert the channel-expansion rules
// directly; AssetManager::DecodeImage wraps it for hook injection.
DecodedImage DecodeImageFile(const std::string& path, bool compressBc1,
                             bool flipVertically = false);

// Bounded worker pool that runs pure-CPU decode work off the main thread.
//
// Threading: this toolchain (MinGW 8.1 win32 model) has NO std::thread
// (__STDCPP_THREADS__ is undefined), so the pool uses the OS primitive
// directly - Win32 CreateThread here, a POSIX pthread branch for CI. The
// pending/ready queues are guarded by the engine's atomic_flag spinlock
// pattern (see log.cpp); there is no condition variable. Workers poll the
// pending queue and sleep ~1 ms when idle, which is fine because decode work
// is chunky (image load) rather than latency-critical.
//
// Ownership model: work submitted via Submit() runs on a worker thread, and
// typically ends by calling Deliver() to enqueue a completion. completions
// run ONLY inside Pump() on the calling (main) thread, so GPU uploads never
// touch a GL context from a worker. Shutdown() joins all workers before
// discarding pending work, so a destroyed pool never leaves a closure running.
class AsyncLoader {
public:
    // workerCount == 0 creates no threads (Available() returns false, Submit
    // returns false) - used by tests to exercise the synchronous fallback.
    explicit AsyncLoader(int workerCount = 2);
    ~AsyncLoader();

    AsyncLoader(const AsyncLoader&) = delete;
    AsyncLoader& operator=(const AsyncLoader&) = delete;

    // True while the pool is running and can accept work. False when thread
    // creation failed, Shutdown() ran, or workerCount == 0; callers must then
    // fall back to a synchronous path.
    bool Available() const;

    // Enqueues `work` to run on a worker thread. Returns false (and runs
    // nothing) when the pool is unavailable.
    bool Submit(std::function<void()> work);

    // Enqueues `completion` to be run by the next Pump() on the calling
    // (main) thread. Safe to call from any worker.
    void Deliver(std::function<void()> completion);

    // Runs every completed item on the calling thread; returns how many ran.
    // The app calls this once per frame (AssetManager::PumpAsync).
    int Pump();

    // Stops the pool and joins all workers. Pending work and undelivered
    // completions are discarded (their destructors run; nothing executes).
    // No-op when already stopped.
    void Shutdown();

private:
    struct Impl;
    // Platform thread entry trampolines (async_loader.cpp): each forwards the
    // Impl pointer to WorkerLoop. Per-platform because the thread API entry
    // signatures differ (Win32 __stdcall vs POSIX void*).
#if defined(_WIN32)
    static unsigned long __stdcall WinWorkerEntry(void* param);
#else
    static void* PosixWorkerEntry(void* param);
#endif
    // Worker body shared by the platform entry points.
    static void WorkerLoop(Impl* impl);

    std::unique_ptr<Impl> impl_;
};

} // namespace neon::assets
