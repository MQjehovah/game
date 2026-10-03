#pragma once
#include <cstdint>
#include <vector>
#include "neon/gfx/color.hpp"
#include "neon/math/math.hpp"

namespace neon::gfx::ibl {

// Image-based lighting (IBL) environment precompute - pure CPU math with no
// GL, so the whole pipeline is unit-testable headlessly (tests run against a
// NullBackend). The renderer uploads the byte maps as plain RGBA8 2D textures
// and the lit shader samples them (SceneState::RecomputeIbl).
//
// The live maps are octahedral: an irradiance tile (kOctaIrradianceSize^2) and
// a vertical roughness atlas of prefiltered specular tiles
// (kOctaPrefilterSize x kOctaPrefilterSize*kOctaRoughnessLevels). The shader
// derives the tile count from textureSize() and blends the two nearest tiles,
// so reflections carry full azimuth (a mirror facing east differs from one
// facing west). The environment source is the decoded equirect HDRI when the
// scene has one, otherwise the procedural skyTop/skyHorizon gradient.
//
// The older Y-symmetric BuildIrradianceMap/BuildPrefilteredMap builders and
// SkyColor/IrradianceForNormal/PrefilteredForReflection are retained (they are
// still unit-tested and used as the procedural fallback path).

constexpr int kEnvRows = 128;        // Y resolution of the irradiance + prefiltered maps
constexpr int kRoughnessCols = 24;   // roughness steps (columns) of the prefiltered map
constexpr int kBrdfLutSize = 128;    // BRDF LUT edge length (u = NoV, v = roughness)
constexpr float kRoughnessMin = 0.045f; // matches the lit shader's roughness clamp
constexpr int kIrradianceSamples = 256; // cosine-weighted hemisphere samples per irradiance texel
constexpr int kRadianceSamples = 48;    // GGX importance samples per prefiltered texel
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

// --- HDRI sky extraction (photographed atmosphere drives the scene IBL) -----
// Samples a decoded equirect RGBA8 sky frame (row 0 = top = zenith, mid rows =
// horizon) and returns two representative colours: `zenith` = average of the
// upper sky rows (the dome above the camera), `horizon` = average of a band
// just above the horizon line (the low-angle haze/sunset tint). These feed
// SceneState's vertical sky gradient so the flat ambient/IBL follows the
// photographed sky's actual colours instead of a hardcoded blue. Pure CPU.
void SkyDominantColors(const uint8_t* rgba, int width, int height, Color& zenith,
                       Color& horizon);

// --- Octahedral (azimuth-aware) environment maps ---------------------------
// The original maps only vary with a direction's Y component, so reflections
// never changed with azimuth (a mirror facing east equalled one facing west).
// These variants bake the environment into an octahedral 2D projection whose
// visible hemisphere spans [0,1]^2, so both the diffuse (normal) and specular
// (reflection) lookups carry full 3D direction. The environment source is the
// decoded equirect HDRI when the scene has one, otherwise the procedural
// vertical gradient - both go through the same integrators.
//
// The specular map is a vertical atlas of kOctaRoughnessLevels octahedral
// tiles (tile l occupies v in [l/L, (l+1)/L]); the lit shader derives L from
// textureSize() and blends the two nearest tiles so roughness stays continuous.
constexpr int kOctaIrradianceSize = 24;     // octahedral irradiance edge
constexpr int kOctaPrefilterSize = 48;      // octahedral prefilter tile edge
constexpr int kOctaRoughnessLevels = 5;     // roughness tiles in the atlas

// Direction (unit) -> octahedral uv in [0,1]^2 and back.
math::Vec2 OctahedralEncode(const math::Vec3& n);
math::Vec3 OctahedralDecode(const math::Vec2& uv);

// Procedural-gradient octahedral maps (used when the scene has no HDRI).
std::vector<uint8_t> BuildOctaIrradiance(const Color& top, const Color& horizon,
                                         float gradientPower);
std::vector<uint8_t> BuildOctaPrefiltered(const Color& top, const Color& horizon,
                                          float gradientPower);
// HDRI octahedral maps: sample the decoded equirect frame (row 0 = zenith).
std::vector<uint8_t> BuildOctaIrradianceFromEquirect(const uint8_t* rgba, int width, int height);
std::vector<uint8_t> BuildOctaPrefilteredFromEquirect(const uint8_t* rgba, int width, int height);

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
