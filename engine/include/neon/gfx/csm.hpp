#pragma once
#include <cmath>

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

// Practical split scheme (Zhang et al.): a `lambda` blend of the logarithmic
// and uniform distributions. Uniform splits waste most of cascade 0's texels on
// the first few metres in front of the camera and leave the far cascade with a
// huge slice, so near shadows read blocky while distant ones crawl. The log
// term equalises texel density in screen space; lambda ~0.7 is the usual
// compromise (0 = fully uniform, 1 = fully logarithmic).
inline void ComputeCascadeSplitsPSSM(float nearPlane, float farPlane, float lambda,
                                     float splits[4]) {
    lambda = lambda < 0.0f ? 0.0f : (lambda > 1.0f ? 1.0f : lambda);
    // NB: not `near`/`far` - those are legacy empty macros in the Windows SDK.
    const float zNear = nearPlane > 1e-4f ? nearPlane : 1e-4f;
    const float zFar = farPlane > zNear ? farPlane : zNear + 1e-3f;
    const float ratio = zFar / zNear;
    const int cascades = 3;
    splits[0] = zNear;
    for (int i = 1; i < cascades; ++i) {
        const float f = static_cast<float>(i) / static_cast<float>(cascades);
        const float logSplit = zNear * std::pow(ratio, f);
        const float uniformSplit = zNear + (zFar - zNear) * f;
        splits[i] = lambda * logSplit + (1.0f - lambda) * uniformSplit;
    }
    splits[cascades] = zFar;
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
//
// When shadowMapSize > 0 the ortho extents are made square and snapped to whole
// texels of that map. Snapping is what stops shadow edges from crawling: the
// texel grid then moves in discrete steps as the camera translates, instead of
// sliding continuously under the receiver. A square footprint keeps the texel
// density identical on both light axes (a non-square ortho wastes texels on the
// wider axis and makes the penumbra direction-dependent).
// When outTexelWorld is non-null it receives the world size of one shadow-map
// texel for this cascade, which the lit shader uses to scale its normal-offset
// bias (an offset must be proportional to the map resolution to work at every
// cascade level).
math::Mat4 ComputeCascadeLightViewProj(const math::Vec3& lightDir, const Camera& cam,
                                       float aspect, float splitNear, float splitFar,
                                       const math::AABB* sceneBounds = nullptr,
                                       int shadowMapSize = 0,
                                       float* outTexelWorld = nullptr);

} // namespace neon::gfx
