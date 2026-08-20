#pragma once
#include <cmath>

namespace neon::math {

struct Vec3 {
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;

    constexpr Vec3() = default;
    constexpr Vec3(float x_, float y_, float z_) : x(x_), y(y_), z(z_) {}

    Vec3 operator+(const Vec3& o) const { return {x + o.x, y + o.y, z + o.z}; }
    Vec3 operator-(const Vec3& o) const { return {x - o.x, y - o.y, z - o.z}; }
    Vec3 operator*(float s) const { return {x * s, y * s, z * s}; }
    Vec3 operator/(float s) const { return {x / s, y / s, z / s}; }
    Vec3 operator-() const { return {-x, -y, -z}; }

    Vec3& operator+=(const Vec3& o) { x += o.x; y += o.y; z += o.z; return *this; }
    Vec3& operator-=(const Vec3& o) { x -= o.x; y -= o.y; z -= o.z; return *this; }
    Vec3& operator*=(float s) { x *= s; y *= s; z *= s; return *this; }

    float Length() const { return std::sqrt(x * x + y * y + z * z); }
    float LengthSq() const { return x * x + y * y + z * z; }
    Vec3 Normalized() const {
        float l = Length();
        return l > 1e-6f ? Vec3{x / l, y / l, z / l} : Vec3{};
    }

    static Vec3 Zero() { return {}; }
    static Vec3 One() { return {1.0f, 1.0f, 1.0f}; }
    static Vec3 Up() { return {0.0f, 1.0f, 0.0f}; }
    static Vec3 Forward() { return {0.0f, 0.0f, -1.0f}; } // OpenGL-style -Z forward
    static Vec3 Right() { return {1.0f, 0.0f, 0.0f}; }
};

inline float Dot(const Vec3& a, const Vec3& b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
inline Vec3 Cross(const Vec3& a, const Vec3& b) {
    return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}
inline Vec3 operator*(float s, const Vec3& v) { return v * s; }
inline float Distance(const Vec3& a, const Vec3& b) { return (b - a).Length(); }
inline Vec3 Lerp(const Vec3& a, const Vec3& b, float t) { return a + (b - a) * t; }
inline Vec3 Clamp(const Vec3& v, const Vec3& lo, const Vec3& hi) {
    return {std::fmin(std::fmax(v.x, lo.x), hi.x),
            std::fmin(std::fmax(v.y, lo.y), hi.y),
            std::fmin(std::fmax(v.z, lo.z), hi.z)};
}

} // namespace neon::math
