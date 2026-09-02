#include "neon/gfx/threaded_backend.hpp"

#include <vector>
#include <string>

#include "neon/core/log.hpp"

namespace neon::gfx {

namespace {

// Owns a reply semaphore used by Run() to block until a command executed.
struct Reply {
    platform::Semaphore done;
};

} // namespace

// ---------------------------------------------------------------------------
// Queue plumbing
// ---------------------------------------------------------------------------

void ThreadedBackend::Enqueue(std::function<void()> fn) {
    if (!fn) return;
    if (!running_.load(std::memory_order_acquire)) return;  // dropped after shutdown
    if (failed_.load(std::memory_order_acquire)) return;    // thread can't bind context
    while (lock_.test_and_set(std::memory_order_acquire)) {
    }
    queue_.push_back(std::move(fn));
    lock_.clear(std::memory_order_release);
    wake_.Post();
}

void ThreadedBackend::Run(std::function<void()> fn) {
    if (!fn) return;
    if (!running_.load(std::memory_order_acquire)) {
        // Render thread down: nothing to wait on. Callers must not trust
        // outputs; Shutdown() / RenderLoop handle the teardown path.
        return;
    }
    Reply reply;
    reply.done.Create();
    while (lock_.test_and_set(std::memory_order_acquire)) {
    }
    queue_.push_back([fn = std::move(fn), &reply]() {
        fn();
        reply.done.Post();
    });
    lock_.clear(std::memory_order_release);
    wake_.Post();
    // Wait with a bounded timeout so a failed render thread (context bind
    // error) cannot deadlock the main thread.
    if (failed_.load(std::memory_order_acquire)) {
        reply.done.Wait(10);  // give the thread a moment to drain, then give up
    } else {
        reply.done.Wait(-1);
    }
    reply.done.Destroy();
}

void ThreadedBackend::RenderLoop() {
    // Migrate the window's GL context onto this thread.
    if (window_ && !window_->MakeGLContextCurrent()) {
        NEON_LOG_ERROR("ThreadedBackend: render thread could not bind the GL context");
        failed_.store(true, std::memory_order_relaxed);
        stopping_.store(true, std::memory_order_relaxed);
        return;
    }
    std::deque<std::function<void()>> local;
    for (;;) {
        {
            while (lock_.test_and_set(std::memory_order_acquire)) {
            }
            if (!queue_.empty()) local.swap(queue_);
            lock_.clear(std::memory_order_release);
        }
        if (local.empty()) {
            if (stopping_.load(std::memory_order_relaxed)) break;
            wake_.Wait(100);
            continue;
        }
        while (!local.empty()) {
            std::function<void()> fn = std::move(local.front());
            local.pop_front();
            fn();
        }
    }
    // Shutdown command has already torn down the real backend; release context.
    if (window_) window_->MakeNoContextCurrent();
}

void ThreadedBackend::RenderThreadEntry(void* param) {
    static_cast<ThreadedBackend*>(param)->RenderLoop();
}

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------

ThreadedBackend::ThreadedBackend(std::unique_ptr<IRenderBackend> real)
    : real_(std::move(real)) {}

ThreadedBackend::~ThreadedBackend() { Shutdown(); }

bool ThreadedBackend::Init(platform::IWindow* window) {
    if (!real_ || !window) return false;
    window_ = window;
    // Main thread creates the real backend + its GL context (Init binds it).
    if (!real_->Init(window)) return false;
    // Release the context on the main thread so the render thread can bind it
    // (a context may be current on at most one thread at a time).
    window_->MakeNoContextCurrent();
    if (!wake_.Create() || !frameDone_.Create()) {
        if (frameDone_.Valid()) frameDone_.Destroy();
        if (wake_.Valid()) wake_.Destroy();
        window_->MakeGLContextCurrent();
        real_->Shutdown();
        return false;
    }
    if (!renderThread_.Start(&ThreadedBackend::RenderThreadEntry, this)) {
        frameDone_.Destroy();
        wake_.Destroy();
        window_->MakeGLContextCurrent();
        real_->Shutdown();
        return false;
    }
    running_.store(true, std::memory_order_release);
    // Wait until the render thread has actually bound the context (or failed),
    // so callers never issue commands before the context is ready.
    // The thread sets failed_ on bind failure; poll briefly.
    return true;
}

void ThreadedBackend::Shutdown() {
    if (!real_) return;
    if (running_.load(std::memory_order_acquire)) {
        // Stop accepting new work, then ask the render thread to drain the
        // queue and tear the backend down (the context is still bound there).
        running_.store(false, std::memory_order_release);
        while (lock_.test_and_set(std::memory_order_acquire)) {
        }
        queue_.push_back([this]() { real_->Shutdown(); });
        lock_.clear(std::memory_order_release);
        stopping_.store(true, std::memory_order_relaxed);
        wake_.Post();
        renderThread_.Join();
        frameDone_.Destroy();
        wake_.Destroy();
    } else {
        real_->Shutdown();
    }
    real_.reset();
}

// ---------------------------------------------------------------------------
// Queries (synchronous)
// ---------------------------------------------------------------------------

ThreadedBackend::GpuMemStats ThreadedBackend::GpuMemory() const {
    GpuMemStats result;
    const_cast<ThreadedBackend*>(this)->Run([&]() { result = real_->GpuMemory(); });
    return result;
}
bool ThreadedBackend::DepthAvailable() const {
    bool result = false;
    const_cast<ThreadedBackend*>(this)->Run([&]() { result = real_->DepthAvailable(); });
    return result;
}
bool ThreadedBackend::ReadCurrentTargetDepth(int width, int height, float* out) {
    bool ok = false;
    Run([&]() { ok = real_->ReadCurrentTargetDepth(width, height, out); });
    return ok;
}

// ---------------------------------------------------------------------------
// State / framebuffer / uniforms / drawing (fire-and-forget)
// ---------------------------------------------------------------------------

void ThreadedBackend::SetBlendMode(BlendMode mode) {
    Enqueue([this, mode]() { real_->SetBlendMode(mode); });
}
void ThreadedBackend::SetDepthTest(bool enabled, bool write) {
    Enqueue([this, enabled, write]() { real_->SetDepthTest(enabled, write); });
}
void ThreadedBackend::SetCullMode(CullMode mode) {
    Enqueue([this, mode]() { real_->SetCullMode(mode); });
}
void ThreadedBackend::SetViewport(int x, int y, int width, int height) {
    Enqueue([this, x, y, width, height]() { real_->SetViewport(x, y, width, height); });
}
void ThreadedBackend::SetScissor(int x, int y, int width, int height, bool enabled) {
    Enqueue([this, x, y, width, height, enabled]() {
        real_->SetScissor(x, y, width, height, enabled);
    });
}
void ThreadedBackend::Clear(const Color& color, float depth) {
    Enqueue([this, color, depth]() { real_->Clear(color, depth); });
}
void ThreadedBackend::BindRenderTarget(RenderTargetHandle target) {
    Enqueue([this, target]() { real_->BindRenderTarget(target); });
}
void ThreadedBackend::BindDefaultTarget() {
    Enqueue([this]() { real_->BindDefaultTarget(); });
}
void ThreadedBackend::ResolveRenderTarget(RenderTargetHandle src, RenderTargetHandle dst) {
    Enqueue([this, src, dst]() { real_->ResolveRenderTarget(src, dst); });
}
void ThreadedBackend::BeginDepthPass(RenderTargetHandle target) {
    Enqueue([this, target]() { real_->BeginDepthPass(target); });
}
void ThreadedBackend::EndDepthPass() {
    Enqueue([this]() { real_->EndDepthPass(); });
}
void ThreadedBackend::BindShadowMap(int slot, RenderTargetHandle target) {
    Enqueue([this, slot, target]() { real_->BindShadowMap(slot, target); });
}
void ThreadedBackend::UseShader(ShaderHandle shader) {
    Enqueue([this, shader]() { real_->UseShader(shader); });
}
void ThreadedBackend::SetUniformMat4(const char* name, const math::Mat4& value) {
    const std::string n = name ? name : "";
    Enqueue([this, n, value]() { real_->SetUniformMat4(n.c_str(), value); });
}
void ThreadedBackend::SetUniformMat4Array(const char* name, const float* values, int count) {
    const std::string n = name ? name : "";
    const std::vector<float> vals(values, values + static_cast<size_t>(count) * 16);
    Enqueue([this, n, vals, count]() { real_->SetUniformMat4Array(n.c_str(), vals.data(), count); });
}
void ThreadedBackend::SetUniformVec4(const char* name, const math::Vec4& value) {
    const std::string n = name ? name : "";
    Enqueue([this, n, value]() { real_->SetUniformVec4(n.c_str(), value); });
}
void ThreadedBackend::SetUniformVec3(const char* name, const math::Vec3& value) {
    const std::string n = name ? name : "";
    Enqueue([this, n, value]() { real_->SetUniformVec3(n.c_str(), value); });
}
void ThreadedBackend::SetUniformFloat(const char* name, float value) {
    const std::string n = name ? name : "";
    Enqueue([this, n, value]() { real_->SetUniformFloat(n.c_str(), value); });
}
void ThreadedBackend::SetUniformVec2(const char* name, const math::Vec2& value) {
    const std::string n = name ? name : "";
    Enqueue([this, n, value]() { real_->SetUniformVec2(n.c_str(), value); });
}
void ThreadedBackend::SetUniformInt(const char* name, int value) {
    const std::string n = name ? name : "";
    Enqueue([this, n, value]() { real_->SetUniformInt(n.c_str(), value); });
}
void ThreadedBackend::BindTexture(int slot, TextureHandle texture) {
    Enqueue([this, slot, texture]() { real_->BindTexture(slot, texture); });
}
void ThreadedBackend::DrawMesh(const MeshHandle& mesh) {
    Enqueue([this, mesh]() { real_->DrawMesh(mesh); });
}
void ThreadedBackend::DrawMeshInstanced(const MeshHandle& mesh, const math::Mat4* models,
                                        uint32_t count) {
    const std::vector<math::Mat4> modelsCopy(models, models + count);
    Enqueue([this, mesh, modelsCopy]() {
        real_->DrawMeshInstanced(mesh, modelsCopy.data(), static_cast<uint32_t>(modelsCopy.size()));
    });
}
void ThreadedBackend::DrawMeshInstancedColored(const MeshHandle& mesh, const math::Mat4* models,
                                               const math::Vec4* colors, uint32_t count) {
    const std::vector<math::Mat4> modelsCopy(models, models + count);
    const std::vector<math::Vec4> colorsCopy(colors, colors + count);
    Enqueue([this, mesh, modelsCopy, colorsCopy]() {
        real_->DrawMeshInstancedColored(mesh, modelsCopy.data(), colorsCopy.data(),
                                        static_cast<uint32_t>(modelsCopy.size()));
    });
}
void ThreadedBackend::DrawPrimitives(const void* vertices, uint32_t vertexCount,
                                     uint32_t stride, const uint16_t* indices,
                                     uint32_t indexCount, PrimitiveTopology topology) {
    const auto* vp = static_cast<const uint8_t*>(vertices);
    const std::vector<uint8_t> v(vp, vp + static_cast<size_t>(stride) * vertexCount);
    const std::vector<uint16_t> idx(indices, indices + indexCount);
    Enqueue([this, v, idx, vertexCount, stride, topology]() {
        real_->DrawPrimitives(v.data(), vertexCount, stride, idx.data(),
                              static_cast<uint32_t>(idx.size()), topology);
    });
}

void ThreadedBackend::BeginFrame() { Enqueue([this]() { real_->BeginFrame(); }); }

void ThreadedBackend::EndFrame() {
    if (!running_.load(std::memory_order_acquire) ||
        failed_.load(std::memory_order_acquire))
        return;
    // Async swap: the main thread does not wait on the GPU/vsync. The render
    // thread posts frameDone_ after each completed swap.
    while (lock_.test_and_set(std::memory_order_acquire)) {
    }
    queue_.push_back([this]() {
        real_->EndFrame();
        framesCompleted_.fetch_add(1, std::memory_order_release);
        frameDone_.Post();
    });
    lock_.clear(std::memory_order_release);
    framesSubmitted_.fetch_add(1, std::memory_order_release);
    wake_.Post();
    // Backpressure: never run more than kMaxFramesAhead frames ahead of the
    // render thread. When the render thread is slower (GPU/vsync bound) this
    // blocks the main thread here — but command/value semantics + per-frame
    // ordering keep the replay safe, and the render thread owns the GL context.
    const uint32_t submitted = framesSubmitted_.load(std::memory_order_acquire);
    const uint32_t completed = framesCompleted_.load(std::memory_order_acquire);
    const int ahead = static_cast<int>(submitted) - static_cast<int>(completed);
    while (ahead > kMaxFramesAhead) {
        frameDone_.Wait(100);
        const uint32_t c2 = framesCompleted_.load(std::memory_order_acquire);
        if (static_cast<int>(submitted) - static_cast<int>(c2) <= kMaxFramesAhead) break;
    }
}

// ---------------------------------------------------------------------------
// Handle creation / destruction / updates (synchronous round-trip)
// ---------------------------------------------------------------------------

RenderTargetHandle ThreadedBackend::CreateRenderTarget(int width, int height, bool floatColor,
                                                       int samples) {
    RenderTargetHandle result;
    Run([&]() { result = real_->CreateRenderTarget(width, height, floatColor, samples); });
    return result;
}
void ThreadedBackend::DestroyRenderTarget(RenderTargetHandle target) {
    Run([this, target]() { real_->DestroyRenderTarget(target); });
}
TextureHandle ThreadedBackend::RenderTargetColorTexture(RenderTargetHandle target) const {
    TextureHandle result;
    const_cast<ThreadedBackend*>(this)->Run([&]() { result = real_->RenderTargetColorTexture(target); });
    return result;
}
TextureHandle ThreadedBackend::RenderTargetDepthTexture(RenderTargetHandle target) const {
    TextureHandle result;
    const_cast<ThreadedBackend*>(this)->Run([&]() { result = real_->RenderTargetDepthTexture(target); });
    return result;
}
RenderTargetHandle ThreadedBackend::CreateDepthTarget(int width, int height) {
    RenderTargetHandle result;
    Run([&]() { result = real_->CreateDepthTarget(width, height); });
    return result;
}
ShaderHandle ThreadedBackend::CreateShader(const char* vertexSource, const char* fragmentSource,
                                           const char* debugName) {
    ShaderHandle result;
    const std::string vs = vertexSource ? vertexSource : "";
    const std::string fs = fragmentSource ? fragmentSource : "";
    const std::string dn = debugName ? debugName : "";
    Run([&]() { result = real_->CreateShader(vs.c_str(), fs.c_str(), dn.c_str()); });
    return result;
}
void ThreadedBackend::DestroyShader(ShaderHandle shader) {
    Run([this, shader]() { real_->DestroyShader(shader); });
}
TextureHandle ThreadedBackend::CreateTexture(const TextureDesc& desc) {
    TextureHandle result;
    const std::vector<uint8_t> pixels(desc.rgba ? desc.rgba : nullptr,
                                      desc.rgba
                                          ? desc.rgba + static_cast<size_t>(desc.width) * desc.height * 4
                                          : nullptr);
    Run([&]() {
        TextureDesc d = desc;
        d.rgba = pixels.empty() ? nullptr : pixels.data();
        result = real_->CreateTexture(d);
    });
    return result;
}
void ThreadedBackend::DestroyTexture(TextureHandle texture) {
    Run([this, texture]() { real_->DestroyTexture(texture); });
}
void ThreadedBackend::UpdateTextureRegion(TextureHandle texture, int x, int y, int w, int h,
                                          const void* rgba) {
    const auto* rp = static_cast<const uint8_t*>(rgba);
    const std::vector<uint8_t> data(rp, rp + static_cast<size_t>(w) * h * 4);
    Run([this, texture, x, y, w, h, data]() {
        real_->UpdateTextureRegion(texture, x, y, w, h, data.data());
    });
}
TextureHandle ThreadedBackend::CreateTextureCompressed(int width, int height, uint32_t format,
                                                       const void* data, size_t size) {
    TextureHandle result;
    const auto* dp = static_cast<const uint8_t*>(data);
    const std::vector<uint8_t> bytes(dp, dp + size);
    Run([&]() { result = real_->CreateTextureCompressed(width, height, format, bytes.data(), size); });
    return result;
}
MeshHandle ThreadedBackend::CreateMesh(const void* vertices, uint32_t vertexCount,
                                       const uint16_t* indices, uint32_t indexCount) {
    MeshHandle result;
    const auto* vp = static_cast<const uint8_t*>(vertices);
    const std::vector<uint8_t> verts(vp, vp + static_cast<size_t>(80) * vertexCount);
    const std::vector<uint16_t> idx(indices, indices + indexCount);
    Run([&]() { result = real_->CreateMesh(verts.data(), vertexCount, idx.data(), indexCount); });
    return result;
}
MeshHandle ThreadedBackend::CreateMeshU32(const void* vertices, uint32_t vertexCount,
                                          const uint32_t* indices, uint32_t indexCount) {
    MeshHandle result;
    const auto* vp = static_cast<const uint8_t*>(vertices);
    const std::vector<uint8_t> verts(vp, vp + static_cast<size_t>(80) * vertexCount);
    const std::vector<uint32_t> idx(indices, indices + indexCount);
    Run([&]() { result = real_->CreateMeshU32(verts.data(), vertexCount, idx.data(), indexCount); });
    return result;
}
void ThreadedBackend::DestroyMesh(const MeshHandle& mesh) {
    Run([this, mesh]() { real_->DestroyMesh(mesh); });
}
void ThreadedBackend::UpdateMeshVertices(const MeshHandle& mesh, const void* vertices,
                                         uint32_t vertexCount) {
    const auto* vp = static_cast<const uint8_t*>(vertices);
    const std::vector<uint8_t> verts(vp, vp + static_cast<size_t>(80) * vertexCount);
    Run([this, mesh, verts, vertexCount]() {
        real_->UpdateMeshVertices(mesh, verts.data(), vertexCount);
    });
}

// ---------------------------------------------------------------------------
// Readback (synchronous)
// ---------------------------------------------------------------------------

void ThreadedBackend::CaptureFrame(int width, int height, void* rgba) {
    Run([this, width, height, rgba]() { real_->CaptureFrame(width, height, rgba); });
}
void ThreadedBackend::ReadCurrentTargetPixel(int x, int y, unsigned char* rgba) {
    Run([this, x, y, rgba]() { real_->ReadCurrentTargetPixel(x, y, rgba); });
}

} // namespace neon::gfx
