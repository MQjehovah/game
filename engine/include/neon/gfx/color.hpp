#pragma once
#include <cstdint>

namespace neon::gfx {

struct Color {
    float r = 1.0f, g = 1.0f, b = 1.0f, a = 1.0f;

    constexpr Color() = default;
    constexpr Color(float r_, float g_, float b_, float a_ = 1.0f) : r(r_), g(g_), b(b_), a(a_) {}

    static const Color White;
    static const Color Black;
    static const Color Transparent;
    static const Color Red;
    static const Color Green;
    static const Color Blue;
    static const Color Yellow;
    static const Color Cyan;
    static const Color Magenta;
    static const Color Orange;
    static const Color Gray;
    static const Color DarkGray;

    Color WithAlpha(float a_) const { return {r, g, b, a_}; }
    Color Multiplied(float s) const { return {r * s, g * s, b * s, a}; }
};

inline Color Rgba(float r, float g, float b, float a = 1.0f) { return {r, g, b, a}; }
inline Color Lerp(const Color& a, const Color& b, float t) {
    return {a.r + (b.r - a.r) * t, a.g + (b.g - a.g) * t, a.b + (b.b - a.b) * t,
            a.a + (b.a - a.a) * t};
}

} // namespace neon::gfx
