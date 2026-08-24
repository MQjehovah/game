#pragma once
#include <cmath>
#include <cstring>
#include "neon/math/vec3.hpp"
#include "neon/math/vec4.hpp"

namespace neon::math {

// Row-major 4x4 matrix. Data layout: m[row * 4 + col].
struct Mat4 {
    float m[16] = {1, 0, 0, 0,
                   0, 1, 0, 0,
                   0, 0, 1, 0,
                   0, 0, 0, 1};

    static Mat4 Identity() { return Mat4{}; }

    static Mat4 Ortho(float left, float right, float bottom, float top, float near_, float far_) {
        Mat4 r;
        float rl = right - left, tb = top - bottom, fn = far_ - near_;
        r.m[0] = 2.0f / rl;          r.m[5] = 2.0f / tb;       r.m[10] = -2.0f / fn;
        r.m[3] = -(right + left) / rl;
        r.m[7] = -(top + bottom) / tb;
        r.m[11] = -(far_ + near_) / fn;
        return r;
    }

    static Mat4 Perspective(float fovYRadians, float aspect, float near_, float far_) {
        Mat4 r;
        float f = 1.0f / std::tan(fovYRadians * 0.5f);
        r.m[0] = f / aspect;
        r.m[5] = f;
        r.m[10] = (far_ + near_) / (near_ - far_);
        r.m[11] = (2.0f * far_ * near_) / (near_ - far_);
        r.m[14] = -1.0f;
        r.m[15] = 0.0f;
        return r;
    }

    static Mat4 Translation(const Vec3& t) {
        Mat4 r;
        r.m[3] = t.x;
        r.m[7] = t.y;
        r.m[11] = t.z;
        return r;
    }

    static Mat4 Scale(const Vec3& s) {
        Mat4 r;
        r.m[0] = s.x;
        r.m[5] = s.y;
        r.m[10] = s.z;
        return r;
    }

    static Mat4 RotationX(float radians) {
        Mat4 r;
        float c = std::cos(radians), s = std::sin(radians);
        r.m[5] = c;  r.m[6] = -s;
        r.m[9] = s;  r.m[10] = c;
        return r;
    }

    static Mat4 RotationY(float radians) {
        Mat4 r;
        float c = std::cos(radians), s = std::sin(radians);
        r.m[0] = c;  r.m[2] = s;
        r.m[8] = -s; r.m[10] = c;
        return r;
    }

    static Mat4 RotationZ(float radians) {
        Mat4 r;
        float c = std::cos(radians), s = std::sin(radians);
        r.m[0] = c;  r.m[1] = -s;
        r.m[4] = s;  r.m[5] = c;
        return r;
    }

    Mat4 operator*(const Mat4& o) const {
        Mat4 r;
        for (int row = 0; row < 4; ++row) {
            for (int col = 0; col < 4; ++col) {
                float sum = 0.0f;
                for (int k = 0; k < 4; ++k) sum += m[row * 4 + k] * o.m[k * 4 + col];
                r.m[row * 4 + col] = sum;
            }
        }
        return r;
    }

    Vec3 TransformPoint(const Vec3& p) const {
        return {m[0] * p.x + m[1] * p.y + m[2] * p.z + m[3],
                m[4] * p.x + m[5] * p.y + m[6] * p.z + m[7],
                m[8] * p.x + m[9] * p.y + m[10] * p.z + m[11]};
    }

    Vec3 TransformDir(const Vec3& v) const {
        return {m[0] * v.x + m[1] * v.y + m[2] * v.z,
                m[4] * v.x + m[5] * v.y + m[6] * v.z,
                m[8] * v.x + m[9] * v.y + m[10] * v.z};
    }

    // Inverse of an affine transform (3x3 + translation; the only kind a
    // TRS/skin matrix needs). Returns Identity for a singular matrix.
    Mat4 Inverted() const {
        const float a0 = m[0], a1 = m[1], a2 = m[2];
        const float a3 = m[4], a4 = m[5], a5 = m[6];
        const float a6 = m[8], a7 = m[9], a8 = m[10];
        const float det = a0 * (a4 * a8 - a5 * a7) - a1 * (a3 * a8 - a5 * a6) +
                          a2 * (a3 * a7 - a4 * a6);
        if (std::fabs(det) < 1e-12f) return Mat4{};
        const float inv = 1.0f / det;
        Mat4 r;
        r.m[0] = (a4 * a8 - a5 * a7) * inv;
        r.m[1] = (a2 * a7 - a1 * a8) * inv;
        r.m[2] = (a1 * a5 - a2 * a4) * inv;
        r.m[4] = (a5 * a6 - a3 * a8) * inv;
        r.m[5] = (a0 * a8 - a2 * a6) * inv;
        r.m[6] = (a2 * a3 - a0 * a5) * inv;
        r.m[8] = (a3 * a7 - a4 * a6) * inv;
        r.m[9] = (a1 * a6 - a0 * a7) * inv;
        r.m[10] = (a0 * a4 - a1 * a3) * inv;
        // T' = -Rinv * T. Row-major: Rinv row i is (r.m[i*4+0], r.m[i*4+1],
        // r.m[i*4+2]); T is the last column (m[3], m[7], m[11]).
        r.m[3] = -(r.m[0] * m[3] + r.m[1] * m[7] + r.m[2] * m[11]);
        r.m[7] = -(r.m[4] * m[3] + r.m[5] * m[7] + r.m[6] * m[11]);
        r.m[11] = -(r.m[8] * m[3] + r.m[9] * m[7] + r.m[10] * m[11]);
        r.m[12] = r.m[13] = r.m[14] = 0.0f;
        r.m[15] = 1.0f;
        return r;
    }

    Vec4 TransformVec4(const Vec4& v) const {
        return {m[0] * v.x + m[1] * v.y + m[2] * v.z + m[3] * v.w,
                m[4] * v.x + m[5] * v.y + m[6] * v.z + m[7] * v.w,
                m[8] * v.x + m[9] * v.y + m[10] * v.z + m[11] * v.w,
                m[12] * v.x + m[13] * v.y + m[14] * v.z + m[15] * v.w};
    }

    float* Data() { return m; }
    const float* Data() const { return m; }
};

} // namespace neon::math
