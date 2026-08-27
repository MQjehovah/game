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

    // Euler angles (yaw/pitch/roll, radians) inverse of FromEuler. Matches the
    // FromEuler composition R = Rz(yaw) * Ry(pitch) * Rx(roll).
    Vec3 ToEulerRad() const {
        Quat q = Normalized();
        const float sqy = q.y * q.y;
        const float sqz = q.z * q.z;
        const float pitch = std::asin(-2.0f * (q.z * q.x - q.w * q.y));
        if (std::fabs(pitch) < 1.5707964f) {
            const float yaw = std::atan2(2.0f * (q.x * q.y + q.w * q.z),
                                         1.0f - 2.0f * (sqy + sqz));
            const float roll = std::atan2(2.0f * (q.y * q.z + q.w * q.x),
                                          1.0f - 2.0f * (q.x * q.x + sqy));
            return {yaw, pitch, roll};
        }
        // Gimbal lock (pitch = +-90 deg): zero out roll, derive yaw.
        const float yaw = std::atan2(2.0f * (q.x * q.z - q.w * q.y),
                                     1.0f - 2.0f * (sqy + sqz));
        return {yaw, pitch, 0.0f};
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

// Extracts the rotation part of a row-major T*R*S matrix as a unit quaternion
// (Shepperd's method). Row lengths (scale) are normalized away; reflection or
// shear matrices yield the closest rotation. Round-trips with Quat::ToMat4
// for pure rotation matrices.
inline Quat Mat4ToQuat(const Mat4& m) {
    auto row = [&](int r) {
        return Vec3{m.m[r * 4 + 0], m.m[r * 4 + 1], m.m[r * 4 + 2]}.Normalized();
    };
    Vec3 r0 = row(0), r1 = row(1), r2 = row(2);
    float trace = r0.x + r1.y + r2.z;
    Quat q;
    if (trace > 0.0f) {
        float s = std::sqrt(trace + 1.0f) * 2.0f;
        q.w = 0.25f * s;
        q.x = (r2.y - r1.z) / s;
        q.y = (r0.z - r2.x) / s;
        q.z = (r1.x - r0.y) / s;
    } else if (r0.x > r1.y && r0.x > r2.z) {
        float s = std::sqrt(1.0f + r0.x - r1.y - r2.z) * 2.0f;
        q.w = (r2.y - r1.z) / s;
        q.x = 0.25f * s;
        q.y = (r0.y + r1.x) / s;
        q.z = (r2.x + r0.z) / s;
    } else if (r1.y > r2.z) {
        float s = std::sqrt(1.0f + r1.y - r0.x - r2.z) * 2.0f;
        q.w = (r0.z - r2.x) / s;
        q.x = (r0.y + r1.x) / s;
        q.y = 0.25f * s;
        q.z = (r1.z + r2.y) / s;
    } else {
        float s = std::sqrt(1.0f + r2.z - r0.x - r1.y) * 2.0f;
        q.w = (r1.x - r0.y) / s;
        q.x = (r2.x + r0.z) / s;
        q.y = (r1.z + r2.y) / s;
        q.z = 0.25f * s;
    }
    return q.Normalized();
}

} // namespace neon::math
