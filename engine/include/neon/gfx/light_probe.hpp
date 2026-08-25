#pragma once

#include <vector>

#include "neon/gfx/color.hpp"
#include "neon/math/math.hpp"
#include "neon/math/vec3.hpp"

namespace neon::gfx {

// G2-4 / G8-3 light-probe field. A grid of irradiance probes spread over a
// world AABB approximates indirect/global illumination cheaply (no ray tracing
// hardware), so it is the mobile-friendly step toward dynamic GI. Each probe
// stores the hemisphere-averaged irradiance from the scene's static lights
// (sun + sky/IBL) plus any point lights, and can be sampled at runtime.
//
// The editor visualizes the probe markers (G8-3) and a future dynamic-GI pass
// would sample the field for indirect light (G2-4).

struct ProbePointLight {
    math::Vec3 position{};
    Color color{1.0f, 1.0f, 1.0f, 1.0f};
    float intensity = 1.0f;
    float radius = 10.0f; // falloff range
};

// Scene light inputs used to bake a probe field.
struct ProbeLightInput {
    // Directional sun: direction (away from the light, i.e. the light travels
    // along -sunDir), color and intensity.
    math::Vec3 sunDir{0.4f, 1.0f, 0.3f};
    Color sunColor{1.0f, 0.95f, 0.85f, 1.0f};
    float sunIntensity = 0.45f;
    // Sky/IBL ambient irradiance (flat, applied to every probe).
    Color skyIrradiance{0.55f, 0.70f, 0.88f, 1.0f};
    std::vector<ProbePointLight> pointLights;
};

struct IrradianceProbe {
    math::Vec3 pos;
    math::Vec3 irradiance; // normalized-ish total indirect (sky + sun + points)
};

// Fills `out` with a res x res x res grid of probes spanning `bounds`. The
// probes cover the volume so moving meshes and characters can sample the nearest
// irradiance. Returns the number of probes written.
size_t BuildProbeField(const math::AABB& bounds, int res, const ProbeLightInput& input,
                       std::vector<IrradianceProbe>& out);

// Samples the probe field at `pos` (trilinear interpolation; clamps to the
// field bounds). Returns the irradiance, or black when out-of-field/empty.
math::Vec3 SampleProbeField(const std::vector<IrradianceProbe>& probes, int res,
                            const math::AABB& bounds, const math::Vec3& pos);

} // namespace neon::gfx
