#pragma once
#include <cstdint>
#include <vector>
#include "neon/gfx/backend.hpp"
#include "neon/gfx/camera.hpp"
#include "neon/gfx/color.hpp"
#include "neon/gfx/font.hpp"
#include "neon/math/math.hpp"

namespace neon::gfx {

// Immediate-mode 2D overlay + screen-space billboard subsystem (split out of
// Renderer). Owns the design-space -> screen mapping (uiScale_/uiOffset_*),
// the batched UI vertex/index buffers, the UI program and the 2D viewport
// state (Set2DViewport / Set2DViewportPixels / SetSceneViewport).
// DrawQuad/DrawText/... accumulate into the batch and Flush2D emits it with a
// full-window ortho projection, restoring any active scene sub-rect afterwards.
//
// Uses the backend pointer set by SetBackend (Renderer::ConnectSubsystems) and
// the UI shader + white texture handed to Init (Renderer::InitBuiltinResources).
// In the headless test path no Init runs: the handles stay invalid and Flush2D
// degenerates to clearing the (empty) batch, exactly like the old Renderer.
class DrawBatch2D {
public:
    DrawBatch2D() = default;
    ~DrawBatch2D() = default;

    void SetBackend(IRenderBackend* backend) { backend_ = backend; }
    // Creates the UI program (sources in draw_batch2d.cpp). `white` is the
    // renderer's shared 1x1 white texture, bound when no texture is active.
    void Init(IRenderBackend& backend, TextureHandle white);
    void Shutdown(IRenderBackend& backend);
    // Frame resize: updates the screen dims and the default full-window design
    // mapping (called from Renderer::BeginFrame with the window size). The
    // editor re-establishes a custom mapping via Set2DViewport afterwards.
    void Resize(int w, int h);

    // 2D overlay (design units: 1280x720, uniform scale, centered)
    void DrawQuad(const math::Vec2& pos, const math::Vec2& size, const Color& color,
                  TextureHandle texture = {}, const math::Vec2& uv0 = {0.0f, 1.0f},
                  const math::Vec2& uv1 = {1.0f, 0.0f}, BlendMode blend = BlendMode::Alpha);
    void DrawRect(const math::Vec2& pos, const math::Vec2& size, const Color& color);
    void DrawRectOutline(const math::Rect2& rect, float thickness, const Color& color);
    // Filled triangle in design units (same immediate-mode 2D buffer as quads).
    void DrawTriangle2D(const math::Vec2& a, const math::Vec2& b, const math::Vec2& c,
                        const Color& color);
    void DrawText(const Font& font, const std::string& text, const math::Vec2& pos, float size,
                  const Color& color, bool centerX = false, bool centerY = false);
    // Flushes the batched 2D overlay now.
    void Flush2D();

    // Full-window vertical gradient (the sky). Pushes a screen-space quad into
    // the overlay batch and flushes it; used by SceneState::DrawSky.
    void DrawFullscreenGradient(const Color& top, const Color& horizon);

    // 2D viewport mapping + 3D scene rasterization rect (see renderer.hpp for
    // the semantics of the public Renderer wrappers).
    void Set2DViewport(float x, float y, float w, float h, float zoom = 1.0f,
                       const math::Vec2& pan = {0.0f, 0.0f}, float aspect = 16.0f / 9.0f);
    void Reset2DViewport();
    void Set2DViewportPixels(float x, float y);
    void SetSceneViewport(float x, float y, float w, float h);
    void ResetSceneViewport();
    float SceneAspect() const;
    math::Vec2 ScreenToUI(const math::Vec2& screenPixels) const;
    math::Vec2 ToScreen(const math::Vec2& design) const;

    // Screen-space billboard: projects a world point into the overlay using the
    // caller's camera + view-projection (the Renderer forwards its active
    // SceneState camera). Unlike the 3D DrawBillboards instanced path this is
    // a screen-space quad, depth-unaware.
    void DrawBillboard(const math::Vec3& worldPos, float size, const Color& color,
                       TextureHandle texture, BlendMode blend, const Camera& camera,
                       const math::Mat4& viewProj);

    // Getters used by the Renderer facade (2D mapping + screen size).
    float UiScale() const { return uiScale_; }
    float UiOffsetX() const { return uiOffsetX_; }
    float UiOffsetY() const { return uiOffsetY_; }
    int ScreenWidth() const { return screenW_; }
    int ScreenHeight() const { return screenH_; }
    const math::Rect2& SceneViewport() const { return sceneViewport_; }
    // True when the 3D scene renders into a sub-rect (editor viewport dock).
    bool SceneViewportActive() const {
        return sceneViewport_.w > 0.0f && sceneViewport_.h > 0.0f;
    }

private:
    struct UIVertex {
        float x, y, u, v;
        float r, g, b, a;
    };
    void PushQuad(const math::Vec2& a, const math::Vec2& b, const math::Vec2& c,
                  const math::Vec2& d, const Color& color, const math::Vec2& uv0,
                  const math::Vec2& uv1, TextureHandle texture, BlendMode blend);
    void PushQuadColored(const math::Vec2& a, const math::Vec2& b, const math::Vec2& c,
                         const math::Vec2& d, const Color& ca, const Color& cb, const Color& cc,
                         const Color& cd, const math::Vec2& uv0, const math::Vec2& uv1,
                         TextureHandle texture, BlendMode blend);

    IRenderBackend* backend_ = nullptr;
    ShaderHandle uiShader_;
    TextureHandle white_;
    int screenW_ = 1280;
    int screenH_ = 720;
    float uiScale_ = 1.0f;
    float uiOffsetX_ = 0.0f;
    float uiOffsetY_ = 0.0f;
    // 3D scene rasterization rect (defaults to the full target).
    math::Rect2 sceneViewport_{0.0f, 0.0f, 0.0f, 0.0f};
    std::vector<UIVertex> uiVerts_;
    std::vector<uint16_t> uiIndices_;
    TextureHandle currentUITexture_;
    BlendMode currentUIBlend_ = BlendMode::Alpha;
};

} // namespace neon::gfx
