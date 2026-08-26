#include "neon/ui/ui.hpp"

#include <algorithm>
#include <cstdlib>

namespace neon::ui {

math::Vec2 MeasureText(const gfx::Font& font, const std::string& text, float size) {
    return font.Measure(text, size);
}

std::vector<RichSpan> ParseRichText(const std::string& text, const gfx::Color& baseColor) {
    std::vector<RichSpan> out;
    std::string plain;
    gfx::Color current = baseColor;
    auto flush = [&]() {
        if (plain.empty()) return;
        out.push_back({plain, current});
        plain.clear();
    };
    size_t i = 0;
    const size_t n = text.size();
    while (i < n) {
        if (text[i] == '[') {
            const size_t close = text.find(']', i);
            if (close != std::string::npos) {
                const std::string tag = text.substr(i + 1, close - i - 1);
                if (tag.rfind("color:", 0) == 0 || tag.rfind("color=", 0) == 0) {
                    std::string hex = tag.substr(6);
                    if (hex.size() == 7 && hex[0] == '#') {
                        const unsigned long rgb =
                            std::strtoul(hex.c_str() + 1, nullptr, 16);
                        flush();
                        current = gfx::Color{static_cast<float>((rgb >> 16) & 0xFF) / 255.0f,
                                             static_cast<float>((rgb >> 8) & 0xFF) / 255.0f,
                                             static_cast<float>(rgb & 0xFF) / 255.0f, 1.0f};
                        i = close + 1;
                        continue;
                    }
                } else if (tag == "/color") {
                    flush();
                    current = baseColor;
                    i = close + 1;
                    continue;
                }
            }
        }
        plain += text[i];
        ++i;
    }
    flush();
    if (out.empty()) out.push_back({plain, current});
    return out;
}

void DrawRichLabel(gfx::Renderer& renderer, const gfx::Font& font, const std::string& text,
                   const math::Vec2& pos, float size, const gfx::Color& baseColor,
                   bool centerX) {
    const std::vector<RichSpan> spans = ParseRichText(text, baseColor);
    float totalW = 0.0f;
    for (const RichSpan& s : spans) totalW += font.Measure(s.text, size).x;
    float x = pos.x - (centerX ? totalW * 0.5f : 0.0f);
    for (const RichSpan& s : spans) {
        const float w = font.Measure(s.text, size).x;
        renderer.DrawText(font, s.text, {x, pos.y}, size, s.color, false, false);
        x += w;
    }
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
