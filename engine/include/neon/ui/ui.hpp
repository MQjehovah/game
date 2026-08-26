#pragma once
#include <string>
#include "neon/gfx/font.hpp"
#include "neon/gfx/renderer.hpp"
#include "neon/math/math.hpp"
#include "neon/platform/input.hpp"

namespace neon::ui {

// G3-5 9-slice: a UI texture region split into 9 quads — the 4 corners keep
// their source size, the 4 edges stretch along one axis, and the center fills
// the remainder. This is how a rounded/framed panel background scales without
// distortion.
struct NineSliceQuad {
    math::Rect2 dest;    // destination rect in design space
    math::Vec2 uv0;      // source UV bottom-left (matches DrawQuad convention)
    math::Vec2 uv1;      // source UV top-right
};

// Computes the 9 quads for `rect` given a border `slice` (design px) and the
// texture's pixel size (texW/texH), which converts the slice to UV units.
// Returns false when the rect cannot fit two slices on either axis.
inline bool ComputeNineSlice(const math::Rect2& rect, float slice, float texW, float texH,
                             NineSliceQuad out[9]) {
    const float su = (texW > 0.0f) ? slice / texW : 0.0f;
    const float sv = (texH > 0.0f) ? slice / texH : 0.0f;
    const float ix = rect.w - 2.0f * slice;
    const float iy = rect.h - 2.0f * slice;
    if (ix < 0.0f || iy < 0.0f) return false;
    const float x0 = rect.x, y0 = rect.y;
    const float x1 = x0 + slice, y1 = y0 + slice;
    const float x2 = x1 + ix, y2 = y1 + iy;
    auto q = [&](int i, float dx, float dy, float dw, float dh, float u0, float v0,
                 float u1, float v1) {
        out[i].dest = {dx, dy, dw, dh};
        out[i].uv0 = {u0, v0};
        out[i].uv1 = {u1, v1};
    };
    q(0, x0, y0, slice, slice, 0.0f, 1.0f - sv, su, 1.0f);        // top-left
    q(1, x1, y0, ix, slice, su, 1.0f - sv, 1.0f - su, 1.0f);      // top edge
    q(2, x2, y0, slice, slice, 1.0f - su, 1.0f - sv, 1.0f, 1.0f); // top-right
    q(3, x0, y1, slice, iy, 0.0f, sv, su, 1.0f - sv);             // left edge
    q(4, x1, y1, ix, iy, su, sv, 1.0f - su, 1.0f - sv);           // center
    q(5, x2, y1, slice, iy, 1.0f - su, sv, 1.0f, 1.0f - sv);      // right edge
    q(6, x0, y2, slice, slice, 0.0f, 0.0f, su, sv);               // bottom-left
    q(7, x1, y2, ix, slice, su, 0.0f, 1.0f - su, sv);             // bottom edge
    q(8, x2, y2, slice, slice, 1.0f - su, 0.0f, 1.0f, sv);        // bottom-right
    return true;
}

struct Theme {
    gfx::Font font;
    gfx::Color panel{0.03f, 0.04f, 0.09f, 0.92f};
    gfx::Color panelBg{0.10f, 0.11f, 0.16f, 1.0f};
    gfx::Color windowBg{0.075f, 0.085f, 0.12f, 0.97f};
    gfx::Color border{0.25f, 0.55f, 1.0f, 1.0f};
    gfx::Color text{0.9f, 0.95f, 1.0f, 1.0f};
    gfx::Color accent{0.1f, 0.8f, 1.0f, 1.0f};
    gfx::Color hover{0.12f, 0.2f, 0.4f, 0.95f};
    gfx::Color disabled{0.25f, 0.28f, 0.35f, 1.0f};
    gfx::Color pressed{0.10f, 0.14f, 0.22f, 1.0f};
    gfx::Color dim{0.52f, 0.57f, 0.68f, 1.0f};
    gfx::Color inputBg{0.05f, 0.06f, 0.09f, 1.0f};
    float fontSize = 14.0f;
    float spacing = 6.0f;
    float padding = 8.0f;
};

math::Vec2 MeasureText(const gfx::Font& font, const std::string& text, float size);
void DrawLabel(gfx::Renderer& renderer, const Theme& theme, const std::string& text,
               const math::Vec2& pos, float size, const gfx::Color& color,
               bool centerX = false, bool centerY = false);
void DrawPanel(gfx::Renderer& renderer, const Theme& theme, const math::Rect2& rect);
bool DrawButton(gfx::Renderer& renderer, const Theme& theme, const std::string& label,
                const math::Rect2& rect, const platform::IInput& input, bool enabled = true);
void DrawBar(gfx::Renderer& renderer, const Theme& theme, const math::Rect2& rect,
             float fraction, const gfx::Color& fillColor);

} // namespace neon::ui
