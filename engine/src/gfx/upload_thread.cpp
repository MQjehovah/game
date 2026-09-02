#include "neon/gfx/upload_thread.hpp"

#include "neon/core/log.hpp"

namespace neon::gfx {

bool UploadThread::Start(platform::IWindow* window) {
    if (!window || window_) return false;
    if (!window->CreateSharedContext()) {
        NEON_LOG_WARN("UploadThread: shared GL context unavailable; "
                      "falling back to main-thread uploads");
        return false;
    }
    window_ = window;
    NEON_LOG_INFO("UploadThread: started (shared GL context)");
    return true;
}

void UploadThread::Shutdown() {
    if (!window_) return;
    // The worker still holds the shared context current: enqueue a detach as
    // its final job (queue order guarantees it runs before the join inside
    // Shutdown), then stop the pool and destroy the shared context. Callers
    // invoke this BEFORE the window's own context is destroyed (renderer
    // shutdown precedes window destroy in the app frame loop).
    loader_.Submit([w = window_]() { w->MakeNoContextCurrent(); });
    loader_.Shutdown();
    window_->DestroySharedContext();
    window_ = nullptr;
}

bool UploadThread::Submit(std::function<void()> glWork) {
    if (!window_ || !glWork) return false;
    // GL contexts are thread-affine: bind the shared context on the worker
    // before every job (a MakeCurrent on an already-current context is a
    // cheap no-op in practice, and it keeps the worker self-contained).
    return loader_.Submit([w = window_, fn = std::move(glWork)]() {
        w->MakeSharedContextCurrent();
        fn();
    });
}

} // namespace neon::gfx
