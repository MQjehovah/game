#pragma once

// Headless gfx fixture for CPU-only asset tests (OBJ/glTF import).
//
// AssetManager::LoadMeshOBJ/LoadGLTF upload CPU data through a real
// gfx::Renderer, which normally requires a window + GL context. This fixture
// installs a NullBackend (every GPU call is a no-op returning fake-but-valid
// handles) so the full import pipeline runs deterministically on the CPU with
// no GPU, window, or driver. gfx::Mesh still records its CPU-side vertex/index
// copies and bounds, which is exactly what the import tests assert on.

#include <memory>

#include "neon/assets/asset_manager.hpp"
#include "neon/gfx/backend.hpp"
#include "neon/gfx/renderer.hpp"

namespace test {

class NullBackend : public neon::gfx::IRenderBackend {
public:
    bool Init(neon::platform::IWindow*) override { return true; }
    void Shutdown() override {}
    const char* Name() const override { return "null"; }

    neon::gfx::RenderTargetHandle CreateRenderTarget(int, int) override { return {1}; }
    void DestroyRenderTarget(neon::gfx::RenderTargetHandle) override {}
    void BindRenderTarget(neon::gfx::RenderTargetHandle) override {}
    void BindDefaultTarget() override {}
    neon::gfx::TextureHandle RenderTargetColorTexture(neon::gfx::RenderTargetHandle) const override {
        return {1};
    }
    neon::gfx::TextureHandle RenderTargetDepthTexture(neon::gfx::RenderTargetHandle) const override {
        return {1};
    }

    neon::gfx::RenderTargetHandle CreateDepthTarget(int, int) override { return {2}; }
    void BeginDepthPass(neon::gfx::RenderTargetHandle) override {}
    void EndDepthPass() override {}
    void BindShadowMap(int, neon::gfx::RenderTargetHandle) override {}
    bool ReadCurrentTargetDepth(int, int, float*) override { return false; }

    neon::gfx::ShaderHandle CreateShader(const char*, const char*, const char*) override {
        return {1};
    }
    void DestroyShader(neon::gfx::ShaderHandle) override {}

    neon::gfx::TextureHandle CreateTexture(const neon::gfx::TextureDesc&) override { return {1}; }
    void DestroyTexture(neon::gfx::TextureHandle) override {}

    neon::gfx::MeshHandle CreateMesh(const void*, uint32_t vertexCount, const uint16_t*,
                                     uint32_t indexCount) override {
        return {1, 1, 1, indexCount};
    }
    void DestroyMesh(const neon::gfx::MeshHandle&) override {}
    void UpdateMeshVertices(const neon::gfx::MeshHandle&, const void*, uint32_t) override {}

    void SetBlendMode(neon::gfx::BlendMode) override {}
    void SetDepthTest(bool, bool) override {}
    void SetCullMode(neon::gfx::CullMode) override {}
    void SetViewport(int, int) override {}
    void SetScissor(int, int, int, int, bool) override {}
    void Clear(const neon::gfx::Color&, float) override {}

    void UseShader(neon::gfx::ShaderHandle) override {}
    void SetUniformMat4(const char*, const neon::math::Mat4&) override {}
    void SetUniformMat4Array(const char*, const float*, int) override {}
    void SetUniformVec4(const char*, const neon::math::Vec4&) override {}
    void SetUniformVec3(const char*, const neon::math::Vec3&) override {}
    void SetUniformFloat(const char*, float) override {}
    void SetUniformVec2(const char*, const neon::math::Vec2&) override {}
    void SetUniformInt(const char*, int) override {}
    void BindTexture(int, neon::gfx::TextureHandle) override {}

    void DrawMesh(const neon::gfx::MeshHandle&) override {}
    void DrawMeshInstanced(const neon::gfx::MeshHandle&, const neon::math::Mat4*, uint32_t) override {}
    void DrawPrimitives(const void*, uint32_t, uint32_t, const uint16_t*, uint32_t,
                        neon::gfx::PrimitiveTopology) override {}

    void BeginFrame() override {}
    void EndFrame() override {}
    void CaptureFrame(int, int, void*) override {}
    void ReadCurrentTargetPixel(int, int, unsigned char*) override {}
    bool DepthAvailable() const override { return true; }
};

// Owns a Renderer wired to a NullBackend plus an AssetManager bound to it.
// One instance per test keeps the asset caches independent.
struct HeadlessAssetFixture {
    neon::gfx::Renderer renderer;
    neon::assets::AssetManager assets;

    HeadlessAssetFixture() {
        renderer.AttachBackendForTesting(std::make_unique<NullBackend>());
        assets.Init(&renderer);
    }
};

} // namespace test
