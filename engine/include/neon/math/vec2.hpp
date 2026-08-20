#pragma once
#include <cmath>

namespace neon::math {

struct Vec2 {
    float x = 0.0f;
    float y = 0.0f;

    constexpr Vec2() = default;
    constexpr Vec2(float x_, float y_) : x(x_), y(y_) {}

    Vec2 operator+(const Vec2& o) const { return {x + o.x, y + o.y}; }
    Vec2 operator-(const Vec2& o) const { return {x - o.x, y - o.y}; }
    Vec2 operator*(float s) const { return {x * s, y * s}; }
    Vec2 operator/(float s) const { return {x / s, y / s}; }
    Vec2 operator-() const { return {-x, -y}; }

    Vec2& operator+=(const Vec2& o) { x += o.x; y += o.y; return *this; }
    Vec2& operator-=(const Vec2& o) { x -= o.x; y -= o.y; return *this; }
    Vec2& operator*=(float s) { x *= s; y *= s; return *this; }

    float Length() const { return std::sqrt(x * x + y * y); }
    float LengthSq() const { return x * x + y * y; }
    Vec2 Normalized() const {
        float l = Length();
        return l > 1e-6f ? Vec2{x / l, y / l} : Vec2{};
    }

    static Vec2 Zero() { return {}; }
    static Vec2 One() { return {1.0f, 1.0f}; }
};

inline float Dot(const Vec2& a, const Vec2& b) { return a.x * b.x + a.y * b.y; }
inline Vec2 operator*(float s, const Vec2& v) { return v * s; }
inline float Distance(const Vec2& a, const Vec2& b) { return (b - a).Length(); }
inline Vec2 Lerp(const Vec2& a, const Vec2& b, float t) { return a + (b - a) * t; }
inline Vec2 Rotate(const Vec2& v, float radians) {
    float c = std::cos(radians);
    float s = std::sin(radians);
    return {v.x * c - v.y * s, v.x * s + v.y * c};
}
inline float Angle(const Vec2& v) { return std::atan2(v.y, v.x); }

} // namespace neon::math
