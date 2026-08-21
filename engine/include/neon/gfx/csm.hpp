#pragma once
#include "neon/gfx/camera.hpp"
#include "neon/math/mat4.hpp"

namespace neon::gfx {

// Cascaded shadow mapping helpers (pure math, no GL). Kept separate from the
// renderer so the cascade split / frustum-slice / light-ortho construction can
// be unit-tested headlessly (tests run against a NullBackend with no GL).
//
// Splits are distances along the camera forward axis: splits[0] == nearPlane,
// splits[3] == farPlane, with two interior boundaries. Cascade i covers the
// view-space depth range [splits[i], splits[i+1]].

// Linear split fractions, matching the design doc's example
// {near, near+0.2*(far-near), near+0.6*(far-near), far}.
inline void ComputeCascadeSplits(float nearPlane, float farPlane, float splits[4]) {
    splits[0] = nearPlane;
    const float range = farPlane - nearPlane;
    splits[1] = nearPlane + 0.2f * range;
    splits[2] = nearPlane + 0.6f * range;
    splits[3] = farPlane;
}

// Returns the cascade index (0..2) for a positive view-space depth (distance
// along the camera forward axis). Clamped to the range of the 3-cascade setup.
inline int SelectCascade(float viewDepth, const float splits[4]) {
    if (viewDepth < splits[1]) return 0;
    if (viewDepth < splits[2]) return 1;
    return 2;
}

// Builds the orthographic light view-projection that tightly encloses the
// frustum slice between splitNear and splitFar (view-space distances along the
// camera forward axis), for a directional light travelling along lightDir.
// The resulting matrix maps every point in the slice into clip space [-1,1].
// When sceneBounds is non-null, the light frustum is additionally clamped to
// the scene's world AABB (union of shadow casters), so the shadow map is not
// wasted on empty space around a small scene.
math::Mat4 ComputeCascadeLightViewProj(const math::Vec3& lightDir, const Camera& cam,
                                       float aspect, float splitNear, float splitFar,
                                       const math::AABB* sceneBounds = nullptr);

} // namespace neon::gfx
