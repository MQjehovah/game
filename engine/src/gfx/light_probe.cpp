#include "neon/gfx/light_probe.hpp"

#include <algorithm>
#include <cmath>

#include "neon/math/vec3.hpp"

namespace neon::gfx {
namespace {

math::Vec3 ColorToVec3(const Color& c) { return {c.r, c.g, c.b}; }

} // namespace

size_t BuildProbeField(const math::AABB& bounds, int res, const ProbeLightInput& input,
                       std::vector<IrradianceProbe>& out) {
    res = math::IClamp(res, 1, 32);
    out.clear();
    if (bounds.max.x < bounds.min.x || bounds.max.y < bounds.min.y ||
        bounds.max.z < bounds.min.z) {
        return 0;
    }

    const math::Vec3 extent = bounds.max - bounds.min;
    const math::Vec3 sunIrradiance =
        ColorToVec3(input.sunColor) * (input.sunIntensity * 0.5f); // hemisphere avg
    const math::Vec3 sky = ColorToVec3(input.skyIrradiance);

    for (int iz = 0; iz < res; ++iz) {
        for (int iy = 0; iy < res; ++iy) {
            for (int ix = 0; ix < res; ++ix) {
                const float fx = (static_cast<float>(ix) + 0.5f) / static_cast<float>(res);
                const float fy = (static_cast<float>(iy) + 0.5f) / static_cast<float>(res);
                const float fz = (static_cast<float>(iz) + 0.5f) / static_cast<float>(res);
                math::Vec3 pos = bounds.min +
                                 math::Vec3{extent.x * fx, extent.y * fy, extent.z * fz};

                // Sky ambient + directional sun. A probe stores the average
                // indirect irradiance, so the sun term is the *hemisphere-averaged*
                // diffuse contribution (max(dot(N,L),0) averaged over normals = 0.5).
                math::Vec3 irr = sky + sunIrradiance;
                for (const ProbePointLight& pl : input.pointLights) {
                    const float d = math::Distance(pos, pl.position);
                    const float atten = pl.radius <= 0.0f
                                            ? 1.0f / (1.0f + d * d)
                                            : std::pow(std::max(1.0f - d / pl.radius, 0.0f), 2.0f);
                    irr = irr + ColorToVec3(pl.color) * (pl.intensity * atten);
                }
                out.push_back({pos, irr});
            }
        }
    }
    return out.size();
}

math::Vec3 SampleProbeField(const std::vector<IrradianceProbe>& probes, int res,
                            const math::AABB& bounds, const math::Vec3& pos) {
    if (probes.empty() || res <= 0) return {0.0f, 0.0f, 0.0f};
    if (static_cast<size_t>(res) * res * res != probes.size()) return {0.0f, 0.0f, 0.0f};

    const math::Vec3 extent = bounds.max - bounds.min;
    auto axis = [res](float mn, float span, float p) -> float {
        if (span <= 1e-5f) return 0.0f; // degenerate axis -> only grid line 0
        const float u = (p - mn) / span * static_cast<float>(res) - 0.5f;
        return math::Clamp(u, 0.0f, static_cast<float>(res - 1));
    };
    const float u = axis(bounds.min.x, extent.x, pos.x);
    const float v = axis(bounds.min.y, extent.y, pos.y);
    const float w = axis(bounds.min.z, extent.z, pos.z);

    const int i0 = std::min(static_cast<int>(u), res - 1);
    const int j0 = std::min(static_cast<int>(v), res - 1);
    const int k0 = std::min(static_cast<int>(w), res - 1);
    const int i1 = std::min(i0 + 1, res - 1);
    const int j1 = std::min(j0 + 1, res - 1);
    const int k1 = std::min(k0 + 1, res - 1);
    const float fu = u - static_cast<float>(i0);
    const float fv = v - static_cast<float>(j0);
    const float fw = w - static_cast<float>(k0);

    auto at = [&](int i, int j, int k) -> math::Vec3 {
        const size_t idx = static_cast<size_t>(k) * res * res +
                           static_cast<size_t>(j) * res + static_cast<size_t>(i);
        return probes[idx].irradiance;
    };

    const math::Vec3 c000 = at(i0, j0, k0);
    const math::Vec3 c100 = at(i1, j0, k0);
    const math::Vec3 c010 = at(i0, j1, k0);
    const math::Vec3 c110 = at(i1, j1, k0);
    const math::Vec3 c001 = at(i0, j0, k1);
    const math::Vec3 c101 = at(i1, j0, k1);
    const math::Vec3 c011 = at(i0, j1, k1);
    const math::Vec3 c111 = at(i1, j1, k1);

    const math::Vec3 x00 = math::Lerp(c000, c100, fu);
    const math::Vec3 x10 = math::Lerp(c010, c110, fu);
    const math::Vec3 x01 = math::Lerp(c001, c101, fu);
    const math::Vec3 x11 = math::Lerp(c011, c111, fu);
    const math::Vec3 y0 = math::Lerp(x00, x10, fv);
    const math::Vec3 y1 = math::Lerp(x01, x11, fv);
    return math::Lerp(y0, y1, fw);
}

size_t BakeProbeAtlas(const std::vector<IrradianceProbe>& probes, int res,
                      const math::AABB& bounds, float maxIrradiance,
                      std::vector<uint8_t>& out) {
    if (probes.empty() || res <= 0 || maxIrradiance <= 0.0f) return 0;
    if (static_cast<size_t>(res) * res * res != probes.size()) return 0;
    // Tile (i, j, k) -> texel (i, k*res + j) in a res x (res*res) atlas, one
    // row per z-slice. Irradiance is clamped to [0,1] (LDR atlas).
    const size_t tileCount = static_cast<size_t>(res) * res * res;
    out.assign(tileCount * 4, 0);
    for (int k = 0; k < res; ++k) {
        for (int j = 0; j < res; ++j) {
            for (int i = 0; i < res; ++i) {
                const size_t idx = static_cast<size_t>(k) * res * res +
                                   static_cast<size_t>(j) * res + static_cast<size_t>(i);
                const math::Vec3 irr = probes[idx].irradiance;
                const float inv = 1.0f / maxIrradiance;
                out[(static_cast<size_t>(i) + (static_cast<size_t>(k) * res + j) * res) * 4 + 0] =
                    static_cast<uint8_t>(math::Clamp(irr.x * inv, 0.0f, 1.0f) * 255.0f);
                out[(static_cast<size_t>(i) + (static_cast<size_t>(k) * res + j) * res) * 4 + 1] =
                    static_cast<uint8_t>(math::Clamp(irr.y * inv, 0.0f, 1.0f) * 255.0f);
                out[(static_cast<size_t>(i) + (static_cast<size_t>(k) * res + j) * res) * 4 + 2] =
                    static_cast<uint8_t>(math::Clamp(irr.z * inv, 0.0f, 1.0f) * 255.0f);
                out[(static_cast<size_t>(i) + (static_cast<size_t>(k) * res + j) * res) * 4 + 3] =
                    255;
            }
        }
    }
    return out.size();
}

} // namespace neon::gfx
