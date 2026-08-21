#include "neon/gfx/csm.hpp"

#include <cmath>

namespace neon::gfx {

math::Mat4 ComputeCascadeLightViewProj(const math::Vec3& lightDir, const Camera& cam,
                                       float aspect, float splitNear, float splitFar,
                                       const math::AABB* sceneBounds) {
    // Camera basis (matches Camera::View convention).
    const math::Vec3 forward = (cam.target - cam.position).Normalized();
    const math::Vec3 right = math::Cross(forward, cam.up).Normalized();
    const math::Vec3 up = math::Cross(right, forward);
    const float tanHalf = std::tan(cam.fovY * 0.5f);

    // 8 world-space corners of the frustum slice.
    math::Vec3 corners[8];
    int ci = 0;
    const float distances[2] = {splitNear, splitFar};
    for (int d = 0; d < 2; ++d) {
        const float halfH = tanHalf * distances[d];
        const float halfW = halfH * aspect;
        const math::Vec3 center = cam.position + forward * distances[d];
        for (int sx = -1; sx <= 1; sx += 2) {
            for (int sy = -1; sy <= 1; sy += 2) {
                corners[ci++] = center + right * (halfW * static_cast<float>(sx)) +
                                up * (halfH * static_cast<float>(sy));
            }
        }
    }

    // Light view basis: light looks along lightDir; ortho covers the slice.
    math::Vec3 lf = lightDir.Normalized();
    math::Vec3 lr = math::Cross(lf, math::Vec3{0, 1, 0});
    if (lr.LengthSq() < 1e-6f) lr = math::Cross(lf, math::Vec3{1, 0, 0});
    lr = lr.Normalized();
    const math::Vec3 lu = math::Cross(lr, lf);
    const math::Vec3 sliceCenter = cam.position + forward * (splitNear + splitFar) * 0.5f;

    math::Mat4 lightView;
    lightView.m[0] = lr.x; lightView.m[1] = lr.y; lightView.m[2] = lr.z;
    lightView.m[4] = lu.x; lightView.m[5] = lu.y; lightView.m[6] = lu.z;
    lightView.m[8] = -lf.x; lightView.m[9] = -lf.y; lightView.m[10] = -lf.z;
    lightView.m[3] = -math::Dot(lr, sliceCenter);
    lightView.m[7] = -math::Dot(lu, sliceCenter);
    lightView.m[11] = math::Dot(lf, sliceCenter);

    // Light-space AABB of the slice corners.
    math::AABB aabb;
    aabb.min = {1e30f, 1e30f, 1e30f};
    aabb.max = {-1e30f, -1e30f, -1e30f};
    for (int i = 0; i < 8; ++i) aabb.Expand(lightView.TransformPoint(corners[i]));

    // Tighten the light frustum to the shadow-casting scene (union of caster
    // AABBs) so a small scene does not get squished into a corner of the map.
    // Intersect the slice AABB with the scene's light-space AABB.
    if (sceneBounds) {
        math::AABB sceneLight;
        sceneLight.min = {1e30f, 1e30f, 1e30f};
        sceneLight.max = {-1e30f, -1e30f, -1e30f};
        for (int i = 0; i < 8; ++i) {
            math::Vec3 c{(i & 1) ? sceneBounds->max.x : sceneBounds->min.x,
                         (i & 2) ? sceneBounds->max.y : sceneBounds->min.y,
                         (i & 4) ? sceneBounds->max.z : sceneBounds->min.z};
            sceneLight.Expand(lightView.TransformPoint(c));
        }
        math::AABB tight;
        tight.min = {std::fmax(aabb.min.x, sceneLight.min.x),
                     std::fmax(aabb.min.y, sceneLight.min.y),
                     std::fmax(aabb.min.z, sceneLight.min.z)};
        tight.max = {std::fmin(aabb.max.x, sceneLight.max.x),
                     std::fmin(aabb.max.y, sceneLight.max.y),
                     std::fmin(aabb.max.z, sceneLight.max.z)};
        // Fall back to the slice AABB if the intersection is degenerate.
        if (tight.min.x < tight.max.x && tight.min.y < tight.max.y &&
            tight.min.z < tight.max.z) {
            aabb = tight;
        }
    }

    // Pad the map a little so receivers at the slice edge stay inside the map.
    const float pad = 1.0f;
    aabb.min.x -= pad; aabb.min.y -= pad; aabb.min.z -= pad;
    aabb.max.x += pad; aabb.max.y += pad; aabb.max.z += pad;

    // Light-space z is negative in front of the light; Ortho maps
    // view-z in [-far, -near] -> NDC [-1, 1], so pass distances -maxZ / -minZ.
    return math::Mat4::Ortho(aabb.min.x, aabb.max.x, aabb.min.y, aabb.max.y,
                             -aabb.max.z, -aabb.min.z) *
           lightView;
}

} // namespace neon::gfx
