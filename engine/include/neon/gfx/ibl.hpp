#pragma once
#include <cstdint>
#include <vector>
#include "neon/gfx/color.hpp"
#include "neon/math/math.hpp"

namespace neon::gfx::ibl {

// Image-based lighting (IBL) environment precompute - pure CPU math with no
// GL, so the whole pipeline is unit-testable headlessly (tests run against a
// NullBackend). The renderer uploads the three byte maps as plain RGBA8 2D
// textures and the lit shader samples them (Renderer::RecomputeIbl).
//
// Environment source: a procedural vertical sky gradient (skyTop/skyHorizon),
// so radiance depends only on a direction's Y component. That rotational
// symmetry is exploited for the two sky-dependent maps: irradiance (diffuse)
// is a function of N.y alone and prefiltered specular of (roughness, R.y)
// alone, so each is baked into a small 2D texture instead of a 6-face cubemap
// with a mip chain. That keeps the recompute cheap (the demo's day/night cycle
// calls SetSky every frame) and avoids cubemaps / textureLod / mip uploads -
// all of which have proven driver problems on the tested Intel GPU.

constexpr int kEnvRows = 128;        // Y resolution of the irradiance + prefiltered maps
constexpr int kRoughnessCols = 24;   // roughness steps (columns) of the prefiltered map
constexpr int kBrdfLutSize = 128;    // BRDF LUT edge length (u = NoV, v = roughness)
constexpr float kRoughnessMin = 0.045f; // matches the lit shader's roughness clamp
constexpr int kIrradianceSamples = 256; // cosine-weighted hemisphere samples per irradiance texel
constexpr int kRadianceSamples = 64;    // GGX importance samples per prefiltered texel
constexpr int kBrdfSamples = 64;        // samples per BRDF LUT texel

// Sky gradient: direction (normalized, Y-up) -> radiance. Deterministic.
Color SkyColor(const Color& top, const Color& horizon, const math::Vec3& dir,
               float gradientPower);

// Builds a right-handed orthonormal tangent frame (t, b, n) for any normal n
// (any length is tolerated - the input is normalized internally). Uses the
// world axis least aligned with n as the reference so the cross product never
// degenerates, even for n parallel to a world axis (e.g. the pole normals the
// map builders evaluate at the map's top/bottom rows). Cross(t, b) == n.
void TangentBasis(const math::Vec3& n, math::Vec3& t, math::Vec3& b);

// Diffuse irradiance for a surface normal: cosine-weighted hemisphere integral
// of the sky with 1/pi baked in, so a white environment yields irradiance = 1
// and the lit-shader term is `kd * irradiance * albedo`. Because the sky is a
// vertical gradient this is effectively a function of n.y alone. Inputs are
// expected normalized (any length is tolerated but the result is defined for
// unit vectors; TangentBasis is robust to axis-aligned normals).
math::Vec3 IrradianceForNormal(const Color& top, const Color& horizon, const math::Vec3& n,
                               float gradientPower);

// Prefiltered specular for a reflection direction + roughness: GGX-weighted
// hemisphere convolution, normalized so a white environment yields (1,1,1)
// (the split-sum BRDF LUT carries the Fresnel/geometry term). r is expected
// normalized and roughness in [0,1] (both clamped defensively).
math::Vec3 PrefilteredForReflection(const Color& top, const Color& horizon,
                                    const math::Vec3& r, float roughness, float gradientPower);

// Split-sum BRDF LUT value for (NoV, roughness): x = F0 scale, y = bias.
// roughness in [0,1]. Pure model term - independent of the sky.
math::Vec2 BrdfLutValue(float ndv, float roughness);

// --- RGBA8 byte encoders (row 0 = first uploaded row = texture v=0) --------
// All deterministic: identical inputs produce identical bytes.
//
// Irradiance map: 1 x kEnvRows. Sampled at u = 0.5, v = n.y*0.5+0.5.
std::vector<uint8_t> BuildIrradianceMap(const Color& top, const Color& horizon,
                                        float gradientPower);
// Prefiltered map: kRoughnessCols x kEnvRows. Sampled at
// u = (roughness - kRoughnessMin) / (1 - kRoughnessMin), v = r.y*0.5+0.5.
std::vector<uint8_t> BuildPrefilteredMap(const Color& top, const Color& horizon,
                                         float gradientPower);
// BRDF LUT: kBrdfLutSize x kBrdfLutSize. Sampled at u = NoV, v = roughness.
std::vector<uint8_t> BuildBrdfLut();

} // namespace neon::gfx::ibl
