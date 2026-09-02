#pragma once

#include <functional>
#include <memory>

#include "neon/assets/async_loader.hpp"
#include "neon/platform/window.hpp"

namespace neon::gfx {

// Background GPU-upload worker: runs submitted work on a thread with a
// SHARED GL context current (objects built there are visible to the main
// render context), so large uploads no longer stall the render thread.
//
// This is the standard multi-threaded-GL "resource context" pattern (the
// foundation step toward a full render thread: the shared-context plumbing
// is the same one a dedicated render thread would migrate onto).
//
// Availability: Start() fails (and callers must fall back to main-thread
// uploads) when the platform lacks shared-context support. Shutdown() joins
// the worker; destroy the shared context via the window afterwards.
class UploadThread {
public:
    UploadThread() = default;
    ~UploadThread() { Shutdown(); }
    UploadThread(const UploadThread&) = delete;
    UploadThread& operator=(const UploadThread&) = delete;

    // Creates the window's shared GL context and starts the worker. Returns
    // false (and stays unavailable) when the platform has no shared-context
    // support; callers keep their synchronous path then.
    bool Start(platform::IWindow* window);

    // Joins the worker and detaches the shared context. Call before the
    // window is destroyed.
    void Shutdown();

    bool Available() const { return window_ != nullptr; }

    // Enqueues `glWork` to run on the worker with the shared context current.
    // Returns false when unavailable (caller does the work inline instead).
    bool Submit(std::function<void()> glWork);

    // Queues `completion` for the next Pump() on the main thread (where the
    // finished handles are recorded / callbacks fire — after the GL work has
    // fully completed, so no fence is needed for object visibility).
    void Deliver(std::function<void()> completion) { loader_.Deliver(std::move(completion)); }

    // Main thread: runs completed followups; returns how many ran.
    int Pump() { return loader_.Pump(); }

private:
    assets::AsyncLoader loader_{1}; // exactly one GL upload worker
    platform::IWindow* window_ = nullptr;
};

} // namespace neon::gfx
