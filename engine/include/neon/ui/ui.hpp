#pragma once
#include <string>
#include "neon/gfx/font.hpp"
#include "neon/gfx/renderer.hpp"
#include "neon/math/math.hpp"
#include "neon/platform/input.hpp"

namespace neon::ui {

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
