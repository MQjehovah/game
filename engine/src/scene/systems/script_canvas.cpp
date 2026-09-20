#include "neon/scene/systems/script_canvas.hpp"

#include <cmath>

namespace neon::scene {

namespace {

constexpr float kTau = 6.2831853072f;

// Thick line segment as two triangles (crisp at any angle, unlike rects).
void DrawThickLine(gfx::Renderer& renderer, const math::Vec2& a, const math::Vec2& b,
                   float thickness, const gfx::Color& col) {
    const math::Vec2 d = b - a;
    const float len = d.Length();
    if (len < 1e-4f || thickness <= 0.0f) return;
    const math::Vec2 n{-d.y / len, d.x / len};
    const math::Vec2 off = n * (thickness * 0.5f);
    const math::Vec2 p0 = a + off, p1 = a - off, p2 = b + off, p3 = b - off;
    renderer.DrawTriangle2D(p0, p1, p2, col);
    renderer.DrawTriangle2D(p1, p3, p2, col);
}

} // namespace

void ScriptCanvas::Begin() {
    draw2d_.clear();
}

void ScriptCanvas::Flush(gfx::Renderer& renderer, const gfx::Font& font2d) {
    for (const script::Draw2DCmd& c : draw2d_) {
        switch (c.kind) {
            case script::Draw2DCmd::Kind::Rect:
                // Textured quads use downward-v UVs (top row = v0): DrawQuad's
                // DEFAULT is the GL bottom-up convention, which drew every
                // script-canvas DrawSprite upside down.
                renderer.DrawQuad({c.x, c.y}, {c.w, c.h}, {c.r, c.g, c.b, c.a},
                                  c.texture, {0.0f, 0.0f}, {1.0f, 1.0f});
                break;
            case script::Draw2DCmd::Kind::RectOutline:
                renderer.DrawRectOutline({c.x, c.y, c.w, c.h}, c.thickness,
                                         {c.r, c.g, c.b, c.a});
                break;
            case script::Draw2DCmd::Kind::Text:
                if (font2d.Valid())
                    renderer.DrawText(font2d, c.text, {c.x, c.y}, c.size,
                                      {c.r, c.g, c.b, c.a}, c.centerX, c.centerY);
                break;
            case script::Draw2DCmd::Kind::Line:
                DrawThickLine(renderer, {c.x, c.y}, {c.x2, c.y2}, c.thickness,
                              {c.r, c.g, c.b, c.a});
                break;
            case script::Draw2DCmd::Kind::Circle: {
                const gfx::Color col{c.r, c.g, c.b, c.a};
                const math::Vec2 ctr{c.x, c.y};
                const float radius = c.w;
                const int seg = 48;
                if (radius <= 0.0f) break;
                if (c.filled) {
                    for (int k = 0; k < seg; ++k) {
                        const float a0 = static_cast<float>(k) / seg * kTau;
                        const float a1 = static_cast<float>(k + 1) / seg * kTau;
                        const math::Vec2 p0{ctr.x + std::cos(a0) * radius,
                                            ctr.y + std::sin(a0) * radius};
                        const math::Vec2 p1{ctr.x + std::cos(a1) * radius,
                                            ctr.y + std::sin(a1) * radius};
                        renderer.DrawTriangle2D(ctr, p0, p1, col);
                    }
                } else {
                    const float t = c.thickness > 0.0f ? c.thickness : 2.0f;
                    const float ri = std::fmax(0.0f, radius - t * 0.5f);
                    const float ro = radius + t * 0.5f;
                    for (int k = 0; k < seg; ++k) {
                        const float a0 = static_cast<float>(k) / seg * kTau;
                        const float a1 = static_cast<float>(k + 1) / seg * kTau;
                        const math::Vec2 in0{ctr.x + std::cos(a0) * ri,
                                             ctr.y + std::sin(a0) * ri};
                        const math::Vec2 in1{ctr.x + std::cos(a1) * ri,
                                             ctr.y + std::sin(a1) * ri};
                        const math::Vec2 ou0{ctr.x + std::cos(a0) * ro,
                                             ctr.y + std::sin(a0) * ro};
                        const math::Vec2 ou1{ctr.x + std::cos(a1) * ro,
                                             ctr.y + std::sin(a1) * ro};
                        renderer.DrawTriangle2D(in0, in1, ou0, col);
                        renderer.DrawTriangle2D(in1, ou1, ou0, col);
                    }
                }
                break;
            }
        }
    }
}

} // namespace neon::scene
