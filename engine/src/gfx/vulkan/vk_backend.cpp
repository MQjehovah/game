#include "neon/gfx/backend.hpp"

#include "neon/core/log.hpp"

// Vulkan backend placeholder.
// The IRenderBackend contract is fully defined; a real Vulkan implementation
// (instance -> device -> swapchain -> render pass -> pipeline) is tracked in
// docs/ROADMAP.md. See docs/VULKAN_ROADMAP.md for the step-by-step plan.
namespace neon::gfx {
namespace {

class VulkanBackend : public IRenderBackend {
public:
    bool Init(platform::IWindow*) override {
        NEON_LOG_WARN("Vulkan backend: placeholder - not implemented yet (see docs/VULKAN_ROADMAP.md)");
        return false;
    }
    void Shutdown() override {}
    const char* Name() const override { return "Vulkan (placeholder)"; }
    RenderTargetHandle CreateRenderTarget(int, int) override { return {}; }
    void DestroyRenderTarget(RenderTargetHandle) override {}
    void BindRenderTarget(RenderTargetHandle) override {}
    void BindDefaultTarget() override {}
    TextureHandle RenderTargetColorTexture(RenderTargetHandle) const override { return {}; }
    TextureHandle RenderTargetDepthTexture(RenderTargetHandle) const override { return {}; }
    RenderTargetHandle CreateDepthTarget(int, int) override { return {}; }
    void BeginDepthPass(RenderTargetHandle) override {}
    void EndDepthPass() override {}
    void BindShadowMap(int, RenderTargetHandle) override {}
    bool ReadCurrentTargetDepth(int, int, float*) override { return false; }
    ShaderHandle CreateShader(const char*, const char*, const char*) override { return {}; }
    void DestroyShader(ShaderHandle) override {}
    TextureHandle CreateTexture(const TextureDesc&) override { return {}; }
    void DestroyTexture(TextureHandle) override {}
    MeshHandle CreateMesh(const void*, uint32_t, const uint16_t*, uint32_t) override { return {}; }
    void DestroyMesh(const MeshHandle&) override {}
    void UpdateMeshVertices(const MeshHandle&, const void*, uint32_t) override {}
    void SetBlendMode(BlendMode) override {}
    void SetDepthTest(bool, bool) override {}
    void SetCullMode(CullMode) override {}
    void SetViewport(int, int) override {}
    void SetScissor(int, int, int, int, bool) override {}
    void Clear(const Color&, float) override {}
    void UseShader(ShaderHandle) override {}
    void SetUniformMat4(const char*, const math::Mat4&) override {}
    void SetUniformMat4Array(const char*, const float*, int) override {}
    void SetUniformVec4(const char*, const math::Vec4&) override {}
    void SetUniformVec3(const char*, const math::Vec3&) override {}
    void SetUniformFloat(const char*, float) override {}
    void SetUniformVec2(const char*, const math::Vec2&) override {}
    void SetUniformInt(const char*, int) override {}
    void BindTexture(int, TextureHandle) override {}
    void DrawMesh(const MeshHandle&) override {}
    void DrawMeshInstanced(const MeshHandle&, const math::Mat4*, uint32_t) override {}
    void DrawPrimitives(const void*, uint32_t, uint32_t, const uint16_t*, uint32_t,
                        PrimitiveTopology) override {}
    void BeginFrame() override {}
    void EndFrame() override {}
    void CaptureFrame(int, int, void*) override {}
    void ReadCurrentTargetPixel(int, int, unsigned char*) override {}
    bool DepthAvailable() const override { return false; }
};

} // namespace

std::unique_ptr<IRenderBackend> CreateVulkanBackend() {
    return std::make_unique<VulkanBackend>();
}

} // namespace neon::gfx
