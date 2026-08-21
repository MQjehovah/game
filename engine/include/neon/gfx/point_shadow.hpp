#pragma once
#include <cmath>
#include "neon/math/mat4.hpp"
#include "neon/math/math.hpp"

namespace neon::gfx {

// Point-light (omnidirectional) shadow helpers (pure math, no GL). Kept
// separate from the renderer so the cube-face selection / projection math can
// be unit-tested headlessly (tests run against a NullBackend with no GL).
//
// Convention: the OpenGL cube-map face numbering and projection (spec Table
// 8.19, "Cube map selection and transformations"), so a face depth-rendered
// with ComputePointLightFaceViewProj and sampled with CubemapFaceAndUV lines
// up exactly like a hardware GL_TEXTURE_CUBE_MAP would:
//   face 0 = +X, 1 = -X, 2 = +Y, 3 = -Y, 4 = +Z, 5 = -Z

// Maps a direction (light -> fragment, need not be normalized) to the cube
// face index plus per-face texture coordinates in [0,1] (v=0 at the bottom of
// the rendered face, matching the GL 2D texture origin). The dominant-axis
// tie-break is `>=`, mirroring the GLSL helper used in the lit shader.
inline int CubemapFaceAndUV(const math::Vec3& dir, float& u, float& v) {
    const float ax = std::fabs(dir.x), ay = std::fabs(dir.y), az = std::fabs(dir.z);
    float ma;
    int face;
    float su, tv; // projected face coordinates in [-1,1]
    if (ax >= ay && ax >= az) {
        ma = ax;
        if (dir.x >= 0.0f) { face = 0; su = -dir.z; tv = -dir.y; }
        else               { face = 1; su =  dir.z; tv = -dir.y; }
    } else if (ay >= ax && ay >= az) {
        ma = ay;
        if (dir.y >= 0.0f) { face = 2; su =  dir.x; tv =  dir.z; }
        else               { face = 3; su =  dir.x; tv = -dir.z; }
    } else {
        ma = az;
        if (dir.z >= 0.0f) { face = 4; su =  dir.x; tv = -dir.y; }
        else               { face = 5; su = -dir.x; tv = -dir.y; }
    }
    u = su / ma * 0.5f + 0.5f;
    v = tv / ma * 0.5f + 0.5f;
    return face;
}

// View matrix for a point-light cube face: a camera sitting at lightPos looking
// along the face axis with the standard cube-map up vectors, built with the
// same right/up/f basis as Camera::View.
inline math::Mat4 PointLightFaceView(int face, const math::Vec3& lightPos) {
    static const math::Vec3 kDir[6] = {
        {1, 0, 0}, {-1, 0, 0}, {0, 1, 0}, {0, -1, 0}, {0, 0, 1}, {0, 0, -1}};
    static const math::Vec3 kUp[6] = {
        {0, -1, 0}, {0, -1, 0}, {0, 0, 1}, {0, 0, -1}, {0, -1, 0}, {0, -1, 0}};
    const math::Vec3& f = kDir[face];
    const math::Vec3 s = math::Cross(f, kUp[face]).Normalized();
    const math::Vec3 u = math::Cross(s, f);
    math::Mat4 m;
    m.m[0] = s.x; m.m[1] = s.y; m.m[2] = s.z;
    m.m[4] = u.x; m.m[5] = u.y; m.m[6] = u.z;
    m.m[8] = -f.x; m.m[9] = -f.y; m.m[10] = -f.z;
    m.m[3] = -math::Dot(s, lightPos);
    m.m[7] = -math::Dot(u, lightPos);
    m.m[11] = math::Dot(f, lightPos);
    return m;
}

// View-projection for one face of a point-light depth cubemap: a 90-degree
// (horizontal and vertical) perspective that covers the whole face, matching
// the CubemapFaceAndUV projection. The rendered depth (gl_FragCoord.z) is
// linearized by the point-light shadow shader into dist/range in [0,1].
inline math::Mat4 ComputePointLightFaceViewProj(const math::Vec3& lightPos, int face,
                                                float nearZ, float farZ) {
    return math::Mat4::Perspective(math::kHalfPi, 1.0f, nearZ, farZ) *
           PointLightFaceView(face, lightPos);
}

// Pure depth-compare used by the point-light shadow sampling: the stored depth
// was written as dist/range in [0,1], so the current fragment is lit only when
// the nearest surface seen from the light is farther than this fragment (minus
// a bias). Mirrors the manual comparison in the lit shader.
inline float PointLightShadowFactor(float storedDepth, float distFromLight, float range,
                                    float bias) {
    if (range <= 0.0f) return 1.0f;
    const float current = distFromLight / range;
    return storedDepth > current - bias ? 1.0f : 0.0f;
}

} // namespace neon::gfx
