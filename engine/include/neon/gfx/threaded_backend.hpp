#pragma once

#include <atomic>
#include <deque>
#include <functional>
#include <memory>

#include "neon/gfx/backend.hpp"
#include "neon/platform/threading.hpp"

namespace neon::gfx {

// Threaded render backend: wraps a real IRenderBackend and replays every
// command on a dedicated render thread. The main thread records commands as
// value-semantic closures (arguments copied), the render thread executes them
// against the real backend in order — so replay never reads the main thread's
// mutable state and the sequence of GL state changes is identical to a
// single-threaded run (bit-identical output).
//
// Command classes:
//   * Enqueue (async, fire-and-forget): Draw/Set*/Clear/UseShader/Bind*/Update*.
//   * Run (synchronous round-trip): Create*/Destroy*/query/readback/swap —
//     blocks until the render thread has executed it, so a returned handle is
//     fully created before the caller references it in a later command.
//
// Frame pacing (phase 1 = frame-lock): EndFrame is a Run, so the main thread
// cannot outpace the render thread; the queue holds at most one frame. True
// async overlap (phase 4) will relax this.
//
// The render thread owns the window's GL context (contexts are thread-affine):
// Init() migrates the main-thread-created context to the render thread. The
// main thread never touches GL directly after Init.
class ThreadedBackend : public IRenderBackend {
public:
    explicit ThreadedBackend(std::unique_ptr<IRenderBackend> real);
    ~ThreadedBackend() override;

    bool Init(platform::IWindow* window) override;
    void Shutdown() override;
    const char* Name() const override { return "threaded"; }
    GpuMemStats GpuMemory() const override;

    // Resources
    RenderTargetHandle CreateRenderTarget(int width, int height, bool floatColor,
                                          int samples) override;
    void DestroyRenderTarget(RenderTargetHandle target) override;
    void BindRenderTarget(RenderTargetHandle target) override;
    void BindDefaultTarget() override;
    void ResolveRenderTarget(RenderTargetHandle src, RenderTargetHandle dst) override;
    TextureHandle RenderTargetColorTexture(RenderTargetHandle target) const override;
    TextureHandle RenderTargetDepthTexture(RenderTargetHandle target) const override;
    RenderTargetHandle CreateDepthTarget(int width, int height) override;
    void BeginDepthPass(RenderTargetHandle target) override;
    void EndDepthPass() override;
    void BindShadowMap(int slot, RenderTargetHandle target) override;
    bool ReadCurrentTargetDepth(int width, int height, float* out) override;

    ShaderHandle CreateShader(const char* vertexSource, const char* fragmentSource,
                              const char* debugName) override;
    void DestroyShader(ShaderHandle shader) override;
    TextureHandle CreateTexture(const TextureDesc& desc) override;
    void DestroyTexture(TextureHandle texture) override;
    void UpdateTextureRegion(TextureHandle texture, int x, int y, int w, int h,
                             const void* rgba) override;
    TextureHandle CreateTextureCompressed(int width, int height, uint32_t format,
                                          const void* data, size_t size) override;
    MeshHandle CreateMesh(const void* vertices, uint32_t vertexCount,
                          const uint16_t* indices, uint32_t indexCount) override;
    MeshHandle CreateMeshU32(const void* vertices, uint32_t vertexCount,
                             const uint32_t* indices, uint32_t indexCount) override;
    void DestroyMesh(const MeshHandle& mesh) override;
    void UpdateMeshVertices(const MeshHandle& mesh, const void* vertices,
                            uint32_t vertexCount) override;

    // State
    void SetBlendMode(BlendMode mode) override;
    void SetDepthTest(bool enabled, bool write) override;
    void SetCullMode(CullMode mode) override;
    void SetViewport(int x, int y, int width, int height) override;
    void SetScissor(int x, int y, int width, int height, bool enabled) override;
    void Clear(const Color& color, float depth) override;

    void UseShader(ShaderHandle shader) override;
    void SetUniformMat4(const char* name, const math::Mat4& value) override;
    void SetUniformMat4Array(const char* name, const float* values, int count) override;
    void SetUniformVec4(const char* name, const math::Vec4& value) override;
    void SetUniformVec3(const char* name, const math::Vec3& value) override;
    void SetUniformFloat(const char* name, float value) override;
    void SetUniformVec2(const char* name, const math::Vec2& value) override;
    void SetUniformInt(const char* name, int value) override;
    void BindTexture(int slot, TextureHandle texture) override;

    void DrawMesh(const MeshHandle& mesh) override;
    void DrawMeshInstanced(const MeshHandle& mesh, const math::Mat4* models,
                           uint32_t count) override;
    void DrawMeshInstancedColored(const MeshHandle& mesh, const math::Mat4* models,
                                  const math::Vec4* colors, uint32_t count) override;
    void DrawPrimitives(const void* vertices, uint32_t vertexCount, uint32_t stride,
                        const uint16_t* indices, uint32_t indexCount,
                        PrimitiveTopology topology) override;

    void BeginFrame() override;
    // Submits the frame's swap asynchronously; the main thread does NOT block
    // on the GPU/vsync here. Backpressure (bounded frames-ahead) keeps the
    // command queue from growing unboundedly when the render thread is slower.
    void EndFrame() override;
    void CaptureFrame(int width, int height, void* rgba) override;
    void ReadCurrentTargetPixel(int x, int y, unsigned char* rgba) override;
    bool DepthAvailable() const override;

private:
    // Async fire-and-forget command.
    void Enqueue(std::function<void()> fn);
    // Synchronous command: enqueue, then block until the render thread ran it.
    void Run(std::function<void()> fn);

    static void RenderThreadEntry(void* param);
    void RenderLoop();

    std::unique_ptr<IRenderBackend> real_;
    platform::IWindow* window_ = nullptr;
    std::atomic<bool> running_{false};
    std::atomic<bool> stopping_{false};
    std::atomic<bool> failed_{false};  // render thread could not bind context
    // Frames ahead backpressure: the main thread records at most this many
    // frames ahead of the render thread before blocking in EndFrame.
    static constexpr int kMaxFramesAhead = 2;
    std::atomic<uint32_t> framesSubmitted_{0};
    std::atomic<uint32_t> framesCompleted_{0};
    platform::Semaphore frameDone_;  // posted once per completed frame

    // Command queue + render-thread wake.
    std::atomic_flag lock_ = ATOMIC_FLAG_INIT;
    std::deque<std::function<void()>> queue_;
    platform::Semaphore wake_;
    platform::Thread renderThread_;
};

} // namespace neon::gfx
