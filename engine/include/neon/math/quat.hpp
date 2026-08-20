#pragma once
#include <cmath>
#include "neon/math/mat4.hpp"
#include "neon/math/vec3.hpp"

namespace neon::math {

struct Quat {
    float x = 0.0f, y = 0.0f, z = 0.0f, w = 1.0f;

    constexpr Quat() = default;
    constexpr Quat(float x_, float y_, float z_, float w_) : x(x_), y(y_), z(z_), w(w_) {}

    static Quat Identity() { return {}; }

    static Quat FromAxisAngle(const Vec3& axis, float radians) {
        float half = radians * 0.5f;
        float s = std::sin(half);
        Vec3 a = axis.Normalized();
        return {a.x * s, a.y * s, a.z * s, std::cos(half)};
    }

    static Quat FromEuler(float yaw, float pitch, float roll) {
        float cy = std::cos(yaw * 0.5f), sy = std::sin(yaw * 0.5f);
        float cp = std::cos(pitch * 0.5f), sp = std::sin(pitch * 0.5f);
        float cr = std::cos(roll * 0.5f), sr = std::sin(roll * 0.5f);
        return {
            sr * cp * cy - cr * sp * sy,
            cr * sp * cy + sr * cp * sy,
            cr * cp * sy - sr * sp * cy,
            cr * cp * cy + sr * sp * sy};
    }

    Quat Normalized() const {
        float l = std::sqrt(x * x + y * y + z * z + w * w);
        return l > 1e-6f ? Quat{x / l, y / l, z / l, w / l} : Identity();
    }

    Quat operator*(const Quat& o) const {
        return {
            w * o.x + x * o.w + y * o.z - z * o.y,
            w * o.y - x * o.z + y * o.w + z * o.x,
            w * o.z + x * o.y - y * o.x + z * o.w,
            w * o.w - x * o.x - y * o.y - z * o.z};
    }

    Vec3 Rotate(const Vec3& v) const {
        // v' = q * v * q^-1
        Quat q = Normalized();
        Quat t = q * Quat{v.x, v.y, v.z, 0.0f} * Quat{-q.x, -q.y, -q.z, q.w};
        return {t.x, t.y, t.z};
    }

    Mat4 ToMat4() const {
        Quat q = Normalized();
        float xx = q.x * q.x, yy = q.y * q.y, zz = q.z * q.z;
        float xy = q.x * q.y, xz = q.x * q.z, yz = q.y * q.z;
        float wx = q.w * q.x, wy = q.w * q.y, wz = q.w * q.z;
        Mat4 r;
        r.m[0] = 1 - 2 * (yy + zz);  r.m[1] = 2 * (xy - wz);      r.m[2] = 2 * (xz + wy);
        r.m[4] = 2 * (xy + wz);      r.m[5] = 1 - 2 * (xx + zz);  r.m[6] = 2 * (yz - wx);
        r.m[8] = 2 * (xz - wy);      r.m[9] = 2 * (yz + wx);      r.m[10] = 1 - 2 * (xx + yy);
        return r;
    }
};

inline Quat Slerp(const Quat& a, const Quat& b, float t) {
    float dot = a.x * b.x + a.y * b.y + a.z * b.z + a.w * b.w;
    Quat bb = b;
    if (dot < 0.0f) { dot = -dot; bb = {-b.x, -b.y, -b.z, -b.w}; }
    if (dot > 0.9995f) {
        Quat r = {a.x + (bb.x - a.x) * t, a.y + (bb.y - a.y) * t,
                  a.z + (bb.z - a.z) * t, a.w + (bb.w - a.w) * t};
        return r.Normalized();
    }
    float theta = std::acos(dot);
    float sinTheta = std::sin(theta);
    float wa = std::sin((1 - t) * theta) / sinTheta;
    float wb = std::sin(t * theta) / sinTheta;
    return {a.x * wa + bb.x * wb, a.y * wa + bb.y * wb,
            a.z * wa + bb.z * wb, a.w * wa + bb.w * wb};
}

} // namespace neon::math
