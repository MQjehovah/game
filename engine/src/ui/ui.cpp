#include "neon/ui/ui.hpp"

#include <algorithm>

namespace neon::ui {

math::Vec2 MeasureText(const gfx::Font& font, const std::string& text, float size) {
    return font.Measure(text, size);
}

void DrawLabel(gfx::Renderer& renderer, const Theme& theme, const std::string& text,
               const math::Vec2& pos, float size, const gfx::Color& color,
               bool centerX, bool centerY) {
    renderer.DrawText(theme.font, text, pos, size, color, centerX, centerY);
}

void DrawPanel(gfx::Renderer& renderer, const Theme& theme, const math::Rect2& rect) {
    renderer.DrawRect({rect.x, rect.y}, {rect.w, rect.h}, theme.panel);
    renderer.DrawRectOutline(rect, 2.0f, theme.border);
}

bool DrawButton(gfx::Renderer& renderer, const Theme& theme, const std::string& label,
                const math::Rect2& rect, const platform::IInput& input, bool enabled) {
    math::Vec2 uiMouse = renderer.ScreenToUI(input.MousePos());
    bool hovered = rect.Contains(uiMouse);
    gfx::Color background = !enabled ? theme.disabled : (hovered ? theme.hover : theme.panel);
    renderer.DrawRect({rect.x, rect.y}, {rect.w, rect.h}, background);
    renderer.DrawRectOutline(rect, 2.0f, enabled ? theme.border : theme.disabled);
    float textSize = std::min(rect.h * 0.42f, 22.0f);
    renderer.DrawText(theme.font, label, {rect.x + rect.w * 0.5f, rect.y + rect.h * 0.5f},
                      textSize, enabled ? theme.text : theme.disabled, true, true);
    return enabled && hovered && input.MousePressed(platform::MouseButton::Left);
}

void DrawBar(gfx::Renderer& renderer, const Theme& theme, const math::Rect2& rect,
             float fraction, const gfx::Color& fillColor) {
    renderer.DrawRect({rect.x, rect.y}, {rect.w, rect.h}, theme.panel);
    float fill = std::max(0.0f, std::min(1.0f, fraction));
    if (fill > 0.0f) {
        renderer.DrawRect({rect.x + 2, rect.y + 2}, {std::max(0.0f, (rect.w - 4) * fill), rect.h - 4},
                          fillColor);
    }
    renderer.DrawRectOutline(rect, 2.0f, theme.border);
}

} // namespace neon::ui
