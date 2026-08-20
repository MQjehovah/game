#pragma once
#include <algorithm>
#include <cmath>
#include "neon/math/mat4.hpp"
#include "neon/math/vec2.hpp"
#include "neon/math/vec3.hpp"

namespace neon::math {

constexpr float kPi = 3.14159265358979323846f;
constexpr float kTwoPi = 6.28318530717958647692f;
constexpr float kHalfPi = 1.57079632679489661923f;
constexpr float kDegToRad = kPi / 180.0f;
constexpr float kRadToDeg = 180.0f / kPi;

inline float Clamp(float v, float lo, float hi) { return std::fmin(std::fmax(v, lo), hi); }
inline int IClamp(int v, int lo, int hi) { return std::min(std::max(v, lo), hi); }
inline float Saturate(float v) { return Clamp(v, 0.0f, 1.0f); }
inline float Lerp(float a, float b, float t) { return a + (b - a) * t; }
inline float Approach(float current, float target, float maxDelta) {
    float diff = target - current;
    if (std::fabs(diff) <= maxDelta) return target;
    return current + (diff > 0 ? maxDelta : -maxDelta);
}
inline float SmoothStep(float edge0, float edge1, float x) {
    float t = Saturate((x - edge0) / (edge1 - edge0));
    return t * t * (3.0f - 2.0f * t);
}
inline float WrapAngle(float radians) {
    radians = std::fmod(radians, kTwoPi);
    if (radians > kPi) radians -= kTwoPi;
    if (radians < -kPi) radians += kTwoPi;
    return radians;
}

struct Rect2 {
    float x = 0.0f, y = 0.0f, w = 0.0f, h = 0.0f;
    bool Contains(const Vec2& p) const { return p.x >= x && p.x <= x + w && p.y >= y && p.y <= y + h; }
    Rect2 Inset(float d) const { return {x + d, y + d, w - 2 * d, h - 2 * d}; }
};

struct AABB {
    Vec3 min{};
    Vec3 max{};

    Vec3 Center() const { return (min + max) * 0.5f; }
    Vec3 Extents() const { return (max - min) * 0.5f; }
    bool Contains(const Vec3& p) const {
        return p.x >= min.x && p.x <= max.x && p.y >= min.y && p.y <= max.y && p.z >= min.z && p.z <= max.z;
    }
    bool Intersects(const AABB& o) const {
        return min.x <= o.max.x && max.x >= o.min.x &&
               min.y <= o.max.y && max.y >= o.min.y &&
               min.z <= o.max.z && max.z >= o.min.z;
    }
    void Expand(const Vec3& p) {
        min.x = std::fmin(min.x, p.x); min.y = std::fmin(min.y, p.y); min.z = std::fmin(min.z, p.z);
        max.x = std::fmax(max.x, p.x); max.y = std::fmax(max.y, p.y); max.z = std::fmax(max.z, p.z);
    }
};

struct Ray {
    Vec3 origin{};
    Vec3 dir{0, 0, -1};
};

struct Plane {
    Vec3 normal{0, 1, 0};
    float d = 0.0f;

    float Distance(const Vec3& p) const { return Dot(normal, p) + d; }
};

// View frustum extracted from a view-projection matrix (row-major convention).
struct Frustum {
    Plane planes[6]; // left, right, bottom, top, near, far

    static Frustum FromViewProjection(const Mat4& vp) {
        Frustum f;
        auto row = [&](int r, float out[4]) {
            for (int c = 0; c < 4; ++c) out[c] = vp.m[r * 4 + c];
        };
        float r0[4], r1[4], r2[4], r3[4];
        row(0, r0);
        row(1, r1);
        row(2, r2);
        row(3, r3);
        auto make = [](const float a[4], const float b[4], Plane& out) {
            out.normal = {a[0] + b[0], a[1] + b[1], a[2] + b[2]};
            out.d = a[3] + b[3];
            float len = out.normal.Length();
            if (len > 1e-8f) {
                out.normal = out.normal * (1.0f / len);
                out.d /= len;
            }
        };
        float neg0[4] = {-r0[0], -r0[1], -r0[2], -r0[3]};
        float neg1[4] = {-r1[0], -r1[1], -r1[2], -r1[3]};
        float neg2[4] = {-r2[0], -r2[1], -r2[2], -r2[3]};
        make(r3, r0, f.planes[0]); // left
        make(r3, neg0, f.planes[1]); // right
        make(r3, r1, f.planes[2]); // bottom
        make(r3, neg1, f.planes[3]); // top
        make(r3, r2, f.planes[4]); // near
        make(r3, neg2, f.planes[5]); // far
        return f;
    }

    bool Intersects(const AABB& box) const {
        for (const Plane& plane : planes) {
            bool anyInside = false;
            for (int i = 0; i < 8; ++i) {
                Vec3 p{
                    (i & 1) ? box.max.x : box.min.x,
                    (i & 2) ? box.max.y : box.min.y,
                    (i & 4) ? box.max.z : box.min.z};
                if (plane.Distance(p) >= 0.0f) {
                    anyInside = true;
                    break;
                }
            }
            if (!anyInside) return false;
        }
        return true;
    }
};

// Conservative AABB transform (8 corners -> new bounds).
inline AABB TransformAABB(const AABB& box, const Mat4& m) {
    AABB out;
    out.min = {1e30f, 1e30f, 1e30f};
    out.max = {-1e30f, -1e30f, -1e30f};
    for (int i = 0; i < 8; ++i) {
        Vec3 p{
            (i & 1) ? box.max.x : box.min.x,
            (i & 2) ? box.max.y : box.min.y,
            (i & 4) ? box.max.z : box.min.z};
        out.Expand(m.TransformPoint(p));
    }
    return out;
}

inline bool IntersectRayAABB(const Ray& ray, const AABB& box, float& outT) {
    float tmin = -1e30f, tmax = 1e30f;
    for (int i = 0; i < 3; ++i) {
        float o = i == 0 ? ray.origin.x : (i == 1 ? ray.origin.y : ray.origin.z);
        float d = i == 0 ? ray.dir.x : (i == 1 ? ray.dir.y : ray.dir.z);
        float lo = i == 0 ? box.min.x : (i == 1 ? box.min.y : box.min.z);
        float hi = i == 0 ? box.max.x : (i == 1 ? box.max.y : box.max.z);
        if (std::fabs(d) < 1e-8f) {
            if (o < lo || o > hi) return false;
        } else {
            float inv = 1.0f / d;
            float t1 = (lo - o) * inv;
            float t2 = (hi - o) * inv;
            if (t1 > t2) std::swap(t1, t2);
            tmin = std::fmax(tmin, t1);
            tmax = std::fmin(tmax, t2);
            if (tmin > tmax) return false;
        }
    }
    outT = tmin > 0.0f ? tmin : tmax;
    return outT > 0.0f;
}

inline bool IntersectRaySphere(const Ray& ray, const Vec3& center, float radius, float& outT) {
    Vec3 oc = ray.origin - center;
    float b = Dot(oc, ray.dir);
    float c = Dot(oc, oc) - radius * radius;
    float disc = b * b - c;
    if (disc < 0.0f) return false;
    float sq = std::sqrt(disc);
    float t = -b - sq;
    if (t < 0.0f) t = -b + sq;
    if (t < 0.0f) return false;
    outT = t;
    return true;
}

} // namespace neon::math
