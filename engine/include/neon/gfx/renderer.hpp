#pragma once
#include <cstdint>
#include <memory>
#include <string>
#include <vector>
#include "neon/gfx/backend.hpp"
#include "neon/gfx/camera.hpp"
#include "neon/gfx/color.hpp"
#include "neon/gfx/font.hpp"
#include "neon/gfx/material.hpp"
#include "neon/gfx/mesh.hpp"
#include "neon/gfx/shader.hpp"
#include "neon/gfx/texture.hpp"
#include "neon/math/math.hpp"

namespace neon::gfx {

// High-level renderer: owns the backend, built-in shaders, lights, fog,
// 3D camera pass and a 2D immediate-mode overlay in "design units".
class Renderer {
public:
    static constexpr int kDesignWidth = 1280;
    static constexpr int kDesignHeight = 720;
    static constexpr int kMaxPointLights = 8;

    Renderer() = default;
    ~Renderer();

    bool Init(platform::IWindow* window);
    void Shutdown();

    // Headless hook used by unit tests and tooling: installs a backend
    // directly, bypassing window/GL-context creation so the CPU-side asset
    // pipeline (mesh/texture upload via CreateMesh/CreateTexture) can run
    // without a GPU. Takes ownership; InitBuiltinResources is not run.
    void AttachBackendForTesting(std::unique_ptr<IRenderBackend> backend);

    IRenderBackend* Backend() { return backend_.get(); }

    // Frame
    void BeginFrame(const Color& clearColor, float clearDepth = 1.0f);
    void EndFrame();

    // 3D camera
    void SetCamera(const Camera& camera, float aspect);
    const math::Mat4& ViewProjection() const { return viewProj_; }
    const math::Vec3& CameraPosition() const { return camPos_; }

    // Atmosphere / lights
    void SetSky(const Color& top, const Color& horizon);
    void DrawSky();
    void SetFog(const Color& color, float start, float end);
    void SetDirectionalLight(const math::Vec3& direction, const Color& color, float ambientStrength);
    void SetPointLight(int index, const math::Vec3& position, const Color& color, float radius);
    void SetPlayerLight(const math::Vec3& position, const Color& color, float radius);

    // 3D drawing
    // Directional-light shadow mapping. Call BeginShadowPass, draw every
    // shadow-casting mesh with DrawShadow, then EndShadowPass before the
    // main pass. Uses an offscreen depth FBO, independent of the (possibly
    // broken) window depth buffer.
    void BeginShadowPass(const math::Vec3& lightDir, const math::Vec3& center, float orthoSize);
    void DrawShadow(const Mesh& mesh, const math::Mat4& model);
    void EndShadowPass();
    bool ShadowsEnabled() const { return shadowRT_.Valid(); }
    TextureHandle ShadowColorTexture() const { return shadowColorTex_; }

    void DrawMesh(const Mesh& mesh, const Material& material, const math::Mat4& model);
    void DrawMeshInstanced(const Mesh& mesh, const Material& material, const math::Mat4* models,
                           uint32_t count, bool frustumCull = true);
    // CPU-side projected shadow: projects the mesh onto the ground plane
    // (y=0) along lightDir. Works without any depth buffer or FBO.
    void DrawProjectedShadow(const Mesh& mesh, const math::Mat4& model,
                             const math::Vec3& lightDir, const Color& color);

    struct LineVertex {
        math::Vec3 pos;
        Color color;
    };
    void DrawLines(const LineVertex* vertices, uint32_t count, const math::Mat4& model);
    void DrawBox(const math::AABB& box, const Color& color);
    void DrawSphere(const math::Vec3& center, float radius, const Color& color, int segments = 20);

    struct RenderStats {
        uint32_t drawCalls = 0;
        uint32_t instances = 0;
        uint32_t triangles = 0;
    };
    const RenderStats& Stats() const { return stats_; }

    // Resources
    Texture CreateTexture(const TextureDesc& desc);
    Shader CreateShader(const char* vertexSource, const char* fragmentSource, const char* name);
    Font CreateFontFromMemory(const uint8_t* data, size_t size, int pixelHeight);
    Font CreateFontFromMemoryWithCodepoints(const uint8_t* data, size_t size, int pixelHeight,
                                            const int32_t* codepoints, int codepointCount);

    // 2D overlay (design units: 1280x720, uniform scale, centered)
    void DrawQuad(const math::Vec2& pos, const math::Vec2& size, const Color& color,
                  TextureHandle texture = {}, const math::Vec2& uv0 = {0.0f, 1.0f},
                  const math::Vec2& uv1 = {1.0f, 0.0f},
                  BlendMode blend = BlendMode::Alpha);
    void DrawRect(const math::Vec2& pos, const math::Vec2& size, const Color& color);
    void DrawRectOutline(const math::Rect2& rect, float thickness, const Color& color);
    void DrawText(const Font& font, const std::string& text, const math::Vec2& pos, float size,
                  const Color& color, bool centerX = false, bool centerY = false);
    void DrawBillboard(const math::Vec3& worldPos, float size, const Color& color,
                       TextureHandle texture, BlendMode blend = BlendMode::Additive);

    // Flushes the batched 2D overlay now (useful to order custom UI before
    // other passes such as a tool overlay rendered directly on the backend).
    void Flush2D();

    math::Vec2 ScreenToUI(const math::Vec2& screenPixels) const;
    // Copies the current back buffer (RGBA8, top-down) into out.
    bool CaptureFrame(std::vector<uint8_t>& out);
    float UIScale() const { return uiScale_; }
    int ScreenWidth() const { return screenW_; }
    int ScreenHeight() const { return screenH_; }

private:
    void InitBuiltinResources();
    void ApplyMaterial(const Material& material, const math::Mat4& mvp, const math::Mat4& model,
                       const math::Mat4& normalMat, ShaderHandle shader);
    math::Vec2 ToScreen(const math::Vec2& design) const;
    void PushQuad(const math::Vec2& a, const math::Vec2& b, const math::Vec2& c, const math::Vec2& d,
                  const Color& color, const math::Vec2& uv0, const math::Vec2& uv1,
                  TextureHandle texture, BlendMode blend);
    void PushQuadColored(const math::Vec2& a, const math::Vec2& b, const math::Vec2& c,
                         const math::Vec2& d, const Color& ca, const Color& cb, const Color& cc,
                         const Color& cd, const math::Vec2& uv0, const math::Vec2& uv1,
                         TextureHandle texture, BlendMode blend);

    std::unique_ptr<IRenderBackend> backend_;

    ShaderHandle litShader_;
    ShaderHandle unlitShader_;
    ShaderHandle uiShader_;
    ShaderHandle linesShader_;
    ShaderHandle litInstancedShader_;
    ShaderHandle unlitInstancedShader_;
    ShaderHandle depthShader_;
    TextureHandle white_;
    RenderTargetHandle shadowRT_;
    int shadowSize_ = 2048;
    math::Mat4 shadowVP_;
    TextureHandle shadowColorTex_;
    bool shadowEnabled_ = false;

    Camera camera_;
    math::Mat4 viewProj_;
    math::Vec3 camPos_;
    math::Frustum frustum_;
    bool frustumValid_ = false;
    bool depthAvailable_ = true;
    RenderStats stats_;

    Color skyTop_{0.05f, 0.07f, 0.12f, 1.0f};
    Color skyHorizon_{0.2f, 0.3f, 0.45f, 1.0f};
    Color fogColor_{0.2f, 0.3f, 0.45f, 1.0f};
    float fogStart_ = 60.0f;
    float fogEnd_ = 200.0f;

    math::Vec3 sunDir_{-0.4f, -1.0f, -0.3f};
    Color sunColor_{1.0f, 0.95f, 0.85f, 1.0f};
    float ambient_ = 0.25f;
    math::Vec3 pointPos_[kMaxPointLights];
    Color pointColor_[kMaxPointLights];
    float pointRadius_[kMaxPointLights];
    int pointCount_ = 0;
    math::Vec3 playerLightPos_{};
    Color playerLightColor_{1.0f, 0.8f, 0.6f, 1.0f};
    float playerLightRadius_ = 14.0f;
    bool playerLightEnabled_ = false;

    int screenW_ = 1280;
    int screenH_ = 720;
    float uiScale_ = 1.0f;
    float uiOffsetX_ = 0.0f;

    struct UIVertex {
        float x, y, u, v;
        float r, g, b, a;
    };
    std::vector<UIVertex> uiVerts_;
    std::vector<uint16_t> uiIndices_;
    TextureHandle currentUITexture_;
    BlendMode currentUIBlend_ = BlendMode::Alpha;
    platform::IWindow* window_ = nullptr;
};

} // namespace neon::gfx
