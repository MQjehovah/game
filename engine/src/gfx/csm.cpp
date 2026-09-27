#include "neon/gfx/csm.hpp"

#include <cmath>

namespace neon::gfx {

math::Mat4 ComputeCascadeLightViewProj(const math::Vec3& lightDir, const Camera& cam,
                                       float aspect, float splitNear, float splitFar,
                                       const math::AABB* sceneBounds, int shadowMapSize,
                                       float* outTexelWorld) {
    // Camera basis (matches Camera::View convention).
    const math::Vec3 forward = (cam.target - cam.position).Normalized();
    const float tanHalf = std::tan(cam.fovY * 0.5f);

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

    // Bounding SPHERE of the frustum slice, rather than the tight corner AABB.
    // The slice's light-space AABB changes shape as the camera ROTATES (the
    // slice is a frustum, and its orientation relative to the light changes),
    // so its extent - and with it the ortho size and the shadow texel grid -
    // rescales every frame. Snapping only the centre then cannot stop the
    // shadows sliding/crawling when the view turns. A sphere's radius depends
    // only on fov / aspect / split distances, so the light-space box is a
    // constant-size cube: rotating the camera only translates it, and the
    // texel snap below keeps that translation in whole-texel steps.
    const float mid = (splitNear + splitFar) * 0.5f;
    const math::Vec3 sphereCenter = cam.position + forward * mid;
    // Ortho cameras (editor top/front views, thumbnails, the 2D game camera)
    // have a CONSTANT half-height: sizing the sphere from fovY ignored the
    // actual ortho frustum and left the cascades wildly oversized / insensitive
    // to zoom.
    const float halfHf = cam.ortho ? cam.orthoSize : tanHalf * splitFar;
    const float halfWf = halfHf * aspect;
    const float dz = (splitFar - splitNear) * 0.5f;
    const float radius = std::sqrt(halfWf * halfWf + halfHf * halfHf + dz * dz);
    const math::Vec3 lc = lightView.TransformPoint(sphereCenter);
    math::AABB aabb;
    aabb.min = {lc.x - radius, lc.y - radius, lc.z - radius};
    aabb.max = {lc.x + radius, lc.y + radius, lc.z + radius};

    // Extend the light-space DEPTH range to cover the shadow-casting scene, so a
    // caster lying outside the camera slice (e.g. above it, along the light
    // beam) still casts into view. The XY footprint stays the camera slice.
    //
    // An earlier revision also *intersected* XY with the caster AABB (to keep a
    // small scene from being squished into a corner of the map). That is wrong:
    // any receiver outside the casters' AABB - the ground plane is normally a
    // non-caster - projects outside the ortho box, and the lit shader treats
    // outside-box samples as unshadowed. The result is exactly "some places
    // have shadows, some don't", ending in a hard cut at the AABB edge, and the
    // footprint jumped as the visible caster set changed while the camera moved.
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
        aabb.min.z = std::fmin(aabb.min.z, sceneLight.min.z);
        aabb.max.z = std::fmax(aabb.max.z, sceneLight.max.z);
    }

    // Pad the map a little so receivers at the slice edge stay inside the map.
    const float pad = 1.0f;
    aabb.min.x -= pad; aabb.min.y -= pad; aabb.min.z -= pad;
    aabb.max.x += pad; aabb.max.y += pad; aabb.max.z += pad;

    // Square + texel-snapped footprint. Without this the ortho extents follow
    // the camera continuously, so every frame samples a slightly different
    // sub-texel offset: fine shadow detail then shimmers/crawls as the camera
    // moves. Snapping the centre to the map's texel grid makes the grid move in
    // whole-texel steps instead.
    if (shadowMapSize > 0) {
        const float side =
            std::fmax(aabb.max.x - aabb.min.x, aabb.max.y - aabb.min.y);
        const float texel = side / static_cast<float>(shadowMapSize);
        if (texel > 0.0f) {
            const float cx = (aabb.min.x + aabb.max.x) * 0.5f;
            const float cy = (aabb.min.y + aabb.max.y) * 0.5f;
            const float sx = std::floor(cx / texel + 0.5f) * texel;
            const float sy = std::floor(cy / texel + 0.5f) * texel;
            const float half = side * 0.5f;
            aabb.min.x = sx - half; aabb.max.x = sx + half;
            aabb.min.y = sy - half; aabb.max.y = sy + half;
        }
        if (outTexelWorld) *outTexelWorld = texel;
    } else if (outTexelWorld) {
        *outTexelWorld = (aabb.max.x - aabb.min.x) /
                         std::fmax(static_cast<float>(shadowMapSize), 1.0f);
    }

    // Light-space z is negative in front of the light; Ortho maps
    // view-z in [-far, -near] -> NDC [-1, 1], so pass distances -maxZ / -minZ.
    return math::Mat4::Ortho(aabb.min.x, aabb.max.x, aabb.min.y, aabb.max.y,
                             -aabb.max.z, -aabb.min.z) *
           lightView;
}

} // namespace neon::gfx
