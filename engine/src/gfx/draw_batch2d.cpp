#include "neon/gfx/draw_batch2d.hpp"

#include <cmath>

#include "neon/gfx/font.hpp"

namespace neon::gfx {
namespace {

constexpr uint32_t kMaxQuads = 4096;
constexpr uint32_t kMaxUIVertices = kMaxQuads * 4;
constexpr float kDesignWidth = 1280.0f;
constexpr float kDesignHeight = 720.0f;

const char* kUIVertexShader = R"(
#version 330 core
layout(location = 0) in vec2 aPos;
layout(location = 1) in vec2 aUV;
layout(location = 2) in vec4 aColor;
uniform mat4 uMVP;
out vec2 vUV;
out vec4 vColor;
void main() {
    vUV = aUV;
    vColor = aColor;
    gl_Position = uMVP * vec4(aPos, 0.0, 1.0);
}
)";

const char* kUIFragmentShader = R"(
#version 330 core
in vec2 vUV;
in vec4 vColor;
out vec4 FragColor;
uniform sampler2D uTex;
void main() {
    FragColor = vColor * texture(uTex, vUV);
}
)";

} // namespace

void DrawBatch2D::Init(IRenderBackend& backend, TextureHandle white) {
    white_ = white;
    uiIndices_.reserve(kMaxQuads * 6);
    uiVerts_.reserve(kMaxUIVertices);
    uiShader_ = backend.CreateShader(kUIVertexShader, kUIFragmentShader, "ui");
}

void DrawBatch2D::Shutdown(IRenderBackend& backend) {
    if (uiShader_.Valid()) backend.DestroyShader(uiShader_);
    uiShader_ = {};
    uiVerts_.clear();
    uiIndices_.clear();
}

void DrawBatch2D::Resize(int w, int h) {
    screenW_ = w;
    screenH_ = h;
    uiScale_ = static_cast<float>(screenH_) / kDesignHeight;
    uiOffsetX_ = (static_cast<float>(screenW_) - kDesignWidth * uiScale_) * 0.5f;
    uiOffsetY_ = 0.0f;
}

void DrawBatch2D::DrawQuad(const math::Vec2& pos, const math::Vec2& size, const Color& color,
                           TextureHandle texture, const math::Vec2& uv0, const math::Vec2& uv1,
                           BlendMode blend) {
    PushQuad(pos, {pos.x + size.x, pos.y}, pos + size, {pos.x, pos.y + size.y},
             color, uv0, uv1, texture, blend);
}

void DrawBatch2D::DrawRect(const math::Vec2& pos, const math::Vec2& size, const Color& color) {
    DrawQuad(pos, size, color, {}, {0, 0}, {1, 1}, BlendMode::Alpha);
}

void DrawBatch2D::DrawRectOutline(const math::Rect2& rect, float thickness, const Color& color) {
    DrawRect({rect.x, rect.y}, {rect.w, thickness}, color);
    DrawRect({rect.x, rect.y + rect.h - thickness}, {rect.w, thickness}, color);
    DrawRect({rect.x, rect.y}, {thickness, rect.h}, color);
    DrawRect({rect.x + rect.w - thickness, rect.y}, {thickness, rect.h}, color);
}

void DrawBatch2D::DrawTriangle2D(const math::Vec2& a, const math::Vec2& b, const math::Vec2& c,
                                 const Color& color) {
    if (currentUITexture_.Valid() || currentUIBlend_ != BlendMode::Alpha) Flush2D();
    if (uiVerts_.size() + 3 > kMaxUIVertices) Flush2D();
    currentUITexture_ = {};
    currentUIBlend_ = BlendMode::Alpha;

    const math::Vec2 s[3] = {ToScreen(a), ToScreen(b), ToScreen(c)};
    uint16_t base = static_cast<uint16_t>(uiVerts_.size());
    for (int i = 0; i < 3; ++i) {
        UIVertex v;
        v.x = s[i].x;
        v.y = s[i].y;
        v.u = 0.0f;
        v.v = 0.0f;
        v.r = color.r;
        v.g = color.g;
        v.b = color.b;
        v.a = color.a;
        uiVerts_.push_back(v);
    }
    uiIndices_.push_back(base + 0);
    uiIndices_.push_back(base + 1);
    uiIndices_.push_back(base + 2);
}

void DrawBatch2D::DrawText(const Font& font, const std::string& text, const math::Vec2& pos,
                           float size, const Color& color, bool centerX, bool centerY) {
    if (!font.Valid() || text.empty()) return;
    math::Vec2 p = pos;
    if (centerX || centerY) {
        math::Vec2 m = font.Measure(text, size);
        if (centerX) p.x -= m.x * 0.5f;
        if (centerY) p.y -= m.y * 0.5f;
    }
    float scale = size / static_cast<float>(font.bakedSize_);
    float cursorX = 0.0f;
    float cursorY = 0.0f;
    const char* it = text.data();
    const char* end = it + text.size();
    while (it < end) {
        int32_t cp = DecodeUTF8Next(it, end);
        if (cp == 0) continue;
        if (cp == '\n') {
            cursorX = 0.0f;
            cursorY += font.LineHeight(size);
            continue;
        }
        const Font::Glyph* g = font.FindGlyph(cp);
        if (!g) {
            // Dynamic glyphs: rasterize the missing codepoint into the atlas.
            const_cast<Font&>(font).EnsureGlyph(cp);
            g = font.FindGlyph(cp);
        }
        if (!g) continue;
        math::Vec2 a{p.x + cursorX + g->xoff * scale, p.y + cursorY + g->yoff * scale};
        math::Vec2 b{p.x + cursorX + g->xoff2 * scale, p.y + cursorY + g->yoff2 * scale};
        PushQuad(a, {b.x, a.y}, b, {a.x, b.y}, color, {g->u0, g->v0}, {g->u1, g->v1},
                 font.Atlas(), BlendMode::Alpha);
        cursorX += g->advance * scale;
    }
}

void DrawBatch2D::DrawBillboard(const math::Vec3& worldPos, float size, const Color& color,
                                TextureHandle texture, BlendMode blend, const Camera& camera,
                                const math::Mat4& viewProj) {
    math::Vec4 clip = viewProj.TransformVec4(math::Vec4(worldPos.x, worldPos.y, worldPos.z, 1.0f));
    if (clip.w <= 0.1f) return;
    const float ndcX = clip.x / clip.w;
    const float ndcY = clip.y / clip.w;
    const math::Rect2& r = sceneViewport_.w > 0.0f
                               ? sceneViewport_
                               : math::Rect2{0.0f, 0.0f, static_cast<float>(screenW_),
                                             static_cast<float>(screenH_)};
    const float px = r.x + (ndcX * 0.5f + 0.5f) * r.w;
    const float py = r.y + (0.5f - ndcY * 0.5f) * r.h;
    float pixelSize = size * r.h * 0.5f / (std::tan(camera.fovY * 0.5f) * clip.w);
    math::Vec2 design = ScreenToUI({px, py});
    float designSize = pixelSize / uiScale_;
    DrawQuad(design - math::Vec2{designSize * 0.5f, designSize * 0.5f},
             {designSize, designSize}, color, texture, {0, 1}, {1, 0}, blend);
}

void DrawBatch2D::DrawFullscreenGradient(const Color& top, const Color& horizon) {
    // Full-screen gradient in screen pixels (depth already cleared).
    if (uiVerts_.size() >= kMaxUIVertices) Flush2D();
    auto push = [&](float x, float y, const Color& c) {
        UIVertex v;
        v.x = x;
        v.y = y;
        v.u = 0.0f;
        v.v = 0.0f;
        v.r = c.r;
        v.g = c.g;
        v.b = c.b;
        v.a = 1.0f;
        uiVerts_.push_back(v);
    };
    float w = static_cast<float>(screenW_);
    float h = static_cast<float>(screenH_);
    push(0, 0, top);
    push(w, 0, top);
    push(w, h, horizon);
    push(0, h, horizon);
    uint16_t base = static_cast<uint16_t>(uiVerts_.size() - 4);
    uiIndices_.push_back(base + 0);
    uiIndices_.push_back(base + 1);
    uiIndices_.push_back(base + 2);
    uiIndices_.push_back(base + 2);
    uiIndices_.push_back(base + 3);
    uiIndices_.push_back(base + 0);
    Flush2D();
}

math::Vec2 DrawBatch2D::ScreenToUI(const math::Vec2& screenPixels) const {
    return {(screenPixels.x - uiOffsetX_) / uiScale_,
            (screenPixels.y - uiOffsetY_) / uiScale_};
}

math::Vec2 DrawBatch2D::ToScreen(const math::Vec2& design) const {
    return {design.x * uiScale_ + uiOffsetX_, design.y * uiScale_ + uiOffsetY_};
}

void DrawBatch2D::Set2DViewport(float x, float y, float w, float h, float zoom,
                                const math::Vec2& pan, float aspect) {
    if (w <= 0.0f || h <= 0.0f || zoom <= 0.0f) {
        Reset2DViewport();
        return;
    }
    if (aspect <= 0.01f)
        aspect = kDesignWidth / kDesignHeight;
    // GAME AREA mapping: the camera's aspect (default 16:9) letterboxed
    // inside the rect; the UI unit base is 720 design units of game-area
    // HEIGHT, the design width follows the aspect (720 * aspect). The
    // editor's blue frame, the play view and every overlay share this rect.
    const float gaW = std::fmin(w, h * aspect);
    const float gaH = gaW / aspect;
    const float gaX = x + (w - gaW) * 0.5f;
    const float gaY = y + (h - gaH) * 0.5f;
    uiScale_ = (gaH / kDesignHeight) * zoom;
    const float designW = kDesignHeight * aspect;
    const float designCx = designW * 0.5f + pan.x;
    const float designCy = kDesignHeight * 0.5f + pan.y;
    uiOffsetX_ = gaX + gaW * 0.5f - designCx * uiScale_;
    uiOffsetY_ = gaY + gaH * 0.5f - designCy * uiScale_;
}

void DrawBatch2D::Reset2DViewport() {
    uiScale_ = static_cast<float>(screenH_) / kDesignHeight;
    uiOffsetX_ = (static_cast<float>(screenW_) - kDesignWidth * uiScale_) * 0.5f;
    uiOffsetY_ = 0.0f;
}

void DrawBatch2D::Set2DViewportPixels(float x, float y) {
    uiScale_ = 1.0f;
    uiOffsetX_ = x;
    uiOffsetY_ = y;
}

void DrawBatch2D::SetSceneViewport(float x, float y, float w, float h) {
    if (w <= 0.0f || h <= 0.0f) {
        ResetSceneViewport();
        return;
    }
    sceneViewport_ = {x, y, w, h};
    backend_->SetViewport(static_cast<int>(x), static_cast<int>(y),
                          static_cast<int>(w), static_cast<int>(h));
}

void DrawBatch2D::ResetSceneViewport() {
    sceneViewport_ = {0.0f, 0.0f, static_cast<float>(screenW_), static_cast<float>(screenH_)};
    backend_->SetViewport(0, 0, screenW_, screenH_);
}

float DrawBatch2D::SceneAspect() const {
    if (sceneViewport_.w > 0.0f && sceneViewport_.h > 0.0f) {
        return sceneViewport_.w / sceneViewport_.h;
    }
    return screenH_ > 0 ? static_cast<float>(screenW_) / static_cast<float>(screenH_) : 1.0f;
}

void DrawBatch2D::PushQuad(const math::Vec2& a, const math::Vec2& b, const math::Vec2& c,
                           const math::Vec2& d, const Color& color, const math::Vec2& uv0,
                           const math::Vec2& uv1, TextureHandle texture, BlendMode blend) {
    PushQuadColored(a, b, c, d, color, color, color, color, uv0, uv1, texture, blend);
}

void DrawBatch2D::PushQuadColored(const math::Vec2& a, const math::Vec2& b, const math::Vec2& c,
                                  const math::Vec2& d, const Color& ca, const Color& cb,
                                  const Color& cc, const Color& cd, const math::Vec2& uv0,
                                  const math::Vec2& uv1, TextureHandle texture, BlendMode blend) {
    if (texture.id != currentUITexture_.id || blend != currentUIBlend_) Flush2D();
    if (uiVerts_.size() + 4 > kMaxUIVertices) Flush2D();
    currentUITexture_ = texture;
    currentUIBlend_ = blend;

    math::Vec2 s[4] = {ToScreen(a), ToScreen(b), ToScreen(c), ToScreen(d)};
    const Color cols[4] = {ca, cb, cc, cd};
    // a -> uv0, b -> (u1, v0), c -> uv1, d -> (u0, v1)
    const math::Vec2 uvs[4] = {uv0, {uv1.x, uv0.y}, uv1, {uv0.x, uv1.y}};
    uint16_t base = static_cast<uint16_t>(uiVerts_.size());
    for (int i = 0; i < 4; ++i) {
        UIVertex v;
        v.x = s[i].x;
        v.y = s[i].y;
        v.u = uvs[i].x;
        v.v = uvs[i].y;
        v.r = cols[i].r;
        v.g = cols[i].g;
        v.b = cols[i].b;
        v.a = cols[i].a;
        uiVerts_.push_back(v);
    }
    uiIndices_.push_back(base + 0);
    uiIndices_.push_back(base + 1);
    uiIndices_.push_back(base + 2);
    uiIndices_.push_back(base + 2);
    uiIndices_.push_back(base + 3);
    uiIndices_.push_back(base + 0);
}

void DrawBatch2D::Flush2D() {
    if (uiVerts_.empty()) return;
    // The 2D overlay uses full-window pixel coordinates with a full-screen
    // ortho, so it must always rasterize with the FULL backend viewport even
    // when the 3D scene is rendering into a sub-rect (editor viewport dock).
    // Otherwise an early flush (e.g. ApplyMaterial switching to the 3D shader)
    // would squash the overlay into the scene rect. Restore the scene viewport
    // afterwards so the next 3D draw is unaffected.
    const bool sceneVpActive = sceneViewport_.w > 0.0f && sceneViewport_.h > 0.0f &&
                               (sceneViewport_.x != 0.0f || sceneViewport_.y != 0.0f ||
                                sceneViewport_.w != static_cast<float>(screenW_) ||
                                sceneViewport_.h != static_cast<float>(screenH_));
    if (sceneVpActive) backend_->SetViewport(0, 0, screenW_, screenH_);
    backend_->SetBlendMode(currentUIBlend_);
    backend_->SetDepthTest(false, false);
    backend_->SetCullMode(CullMode::None);
    backend_->UseShader(uiShader_);
    backend_->SetUniformMat4("uMVP",
                             math::Mat4::Ortho(0, static_cast<float>(screenW_),
                                               static_cast<float>(screenH_), 0, -1, 1));
    backend_->BindTexture(0, currentUITexture_.Valid() ? currentUITexture_ : white_);
    backend_->SetUniformInt("uTex", 0);
    backend_->DrawPrimitives(uiVerts_.data(), static_cast<uint32_t>(uiVerts_.size()), 32,
                             uiIndices_.data(), static_cast<uint32_t>(uiIndices_.size()),
                             PrimitiveTopology::Triangles);
    if (sceneVpActive) {
        backend_->SetViewport(static_cast<int>(sceneViewport_.x),
                              static_cast<int>(sceneViewport_.y),
                              static_cast<int>(sceneViewport_.w),
                              static_cast<int>(sceneViewport_.h));
    }
    uiVerts_.clear();
    uiIndices_.clear();
    currentUITexture_ = {};
    currentUIBlend_ = BlendMode::Alpha;
}

} // namespace neon::gfx
