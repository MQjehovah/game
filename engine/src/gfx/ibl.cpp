#include "neon/gfx/ibl.hpp"

#include <algorithm>
#include <cmath>

namespace neon::gfx::ibl {
namespace {

// Low-discrepancy (0,1) sequence used for all Monte Carlo integration so the
// precompute is deterministic across runs (no RNG state).
inline float RadicalInverseVdC(uint32_t bits) {
    bits = (bits << 16u) | (bits >> 16u);
    bits = ((bits & 0x55555555u) << 1u) | ((bits & 0xAAAAAAAAu) >> 1u);
    bits = ((bits & 0x33333333u) << 2u) | ((bits & 0xCCCCCCCCu) >> 2u);
    bits = ((bits & 0x0F0F0F0Fu) << 4u) | ((bits & 0xF0F0F0F0u) >> 4u);
    bits = ((bits & 0x00FF00FFu) << 8u) | ((bits & 0xFF00FF00u) >> 8u);
    return static_cast<float>(bits) * 2.3283064365386963e-10f;
}

inline math::Vec2 Hammersley(uint32_t i, uint32_t n) {
    return {static_cast<float>(i) / static_cast<float>(n), RadicalInverseVdC(i)};
}

// Importance-samples the GGX (Trowbridge-Reitz) half-vector distribution D with
// alpha = roughness^2, in the tangent frame (t, b, n).
inline math::Vec3 ImportanceSampleGGX(const math::Vec2& xi, float alpha, const math::Vec3& t,
                                      const math::Vec3& b, const math::Vec3& n) {
    const float phi = math::kTwoPi * xi.x;
    const float cosTheta =
        std::sqrt((1.0f - xi.y) / (1.0f + (alpha * alpha - 1.0f) * xi.y));
    const float sinTheta = std::sqrt(std::max(1.0f - cosTheta * cosTheta, 0.0f));
    return (t * (std::cos(phi) * sinTheta) + b * (std::sin(phi) * sinTheta) + n * cosTheta)
        .Normalized();
}

// Schlick-GGX geometry with the IBL k (roughness^2 / 2), matching the
// split-sum BRDF integration used for the LUT.
inline float GSchlickIBL(float ndx, float k) { return ndx / (ndx * (1.0f - k) + k); }
inline float GeometrySmith(float ndv, float ndl, float roughness) {
    const float k = roughness * roughness * 0.5f;
    return GSchlickIBL(ndv, k) * GSchlickIBL(ndl, k);
}

inline uint8_t Encode(float v) {
    const int b = static_cast<int>(std::lround(math::Saturate(v) * 255.0f));
    return static_cast<uint8_t>(std::max(0, std::min(255, b)));
}

inline void Push(std::vector<uint8_t>& out, const math::Vec3& v) {
    out.push_back(Encode(v.x));
    out.push_back(Encode(v.y));
    out.push_back(Encode(v.z));
    out.push_back(255);
}

// Unit vector with the given y-component, used to evaluate the sky-dependent
// maps row by row. Passing {0, ny, 0} directly would be normalized by the
// integrators to {0, +/-1, 0}, collapsing every row onto the poles - the map
// would lose its vertical gradient entirely (the T3.8 symptom the tests
// target). The x-component is only a tie-break; the environment is
// y-symmetric so any azimuth gives the same row value.
inline math::Vec3 DirForY(float ny) {
    return math::Vec3{std::sqrt(std::max(1.0f - ny * ny, 0.0f)), ny, 0.0f};
}

} // namespace

// Right-handed orthonormal tangent frame (t, b, n) for any normal n. The
// reference is the world axis LEAST aligned with n (after normalizing), so the
// cross product can never vanish - including for n parallel to a world axis.
void TangentBasis(const math::Vec3& n, math::Vec3& t, math::Vec3& b) {
    const math::Vec3 N = n.Normalized();
    const float ax = std::fabs(N.x), ay = std::fabs(N.y), az = std::fabs(N.z);
    const math::Vec3 ref = (ax <= ay && ax <= az) ? math::Vec3{1, 0, 0}
                          : (ay <= az)            ? math::Vec3{0, 1, 0}
                                                  : math::Vec3{0, 0, 1};
    t = math::Cross(ref, N).Normalized();
    b = math::Cross(N, t);
}

Color SkyColor(const Color& top, const Color& horizon, const math::Vec3& dir,
               float gradientPower) {
    // A smooth horizon band (below it the horizon tint persists, giving a
    // ground-adjacent colour), then a power curve toward the zenith so the
    // deep sky color shows overhead while the horizon keeps its haze.
    const float t = std::pow(math::SmoothStep(-0.15f, 0.35f, dir.y), gradientPower);
    return gfx::Lerp(horizon, top, t);
}

math::Vec3 IrradianceForNormal(const Color& top, const Color& horizon, const math::Vec3& n,
                               float gradientPower) {
    const math::Vec3 N = n.Normalized();
    math::Vec3 t, b;
    TangentBasis(N, t, b);
    math::Vec3 acc{};
    for (int i = 0; i < kIrradianceSamples; ++i) {
        // Cosine-weighted hemisphere sample: with pdf = cos(theta)/pi the
        // estimator (1/pi) * E[E * cos / pdf] collapses to the plain mean of
        // the sampled radiance, so no per-sample weighting is needed and a
        // white environment integrates to exactly 1.0.
        const math::Vec2 xi = Hammersley(i, kIrradianceSamples);
        const float phi = math::kTwoPi * xi.x;
        const float cosTheta = std::sqrt(1.0f - xi.y);
        const float sinTheta = std::sqrt(xi.y);
        const math::Vec3 L =
            (t * (std::cos(phi) * sinTheta) + b * (std::sin(phi) * sinTheta) + N * cosTheta)
                .Normalized();
        const Color c = SkyColor(top, horizon, L, gradientPower);
        acc.x += c.r;
        acc.y += c.g;
        acc.z += c.b;
    }
    return acc * (1.0f / static_cast<float>(kIrradianceSamples));
}

math::Vec3 PrefilteredForReflection(const Color& top, const Color& horizon, const math::Vec3& r,
                                    float roughness, float gradientPower) {
    const math::Vec3 N = r.Normalized();
    const float rr = math::Saturate(roughness);
    const float alpha = rr * rr;
    math::Vec3 t, b;
    TangentBasis(N, t, b);
    math::Vec3 acc{};
    float weight = 0.0f;
    for (int i = 0; i < kRadianceSamples; ++i) {
        const math::Vec2 xi = Hammersley(i, kRadianceSamples);
        const math::Vec3 H = ImportanceSampleGGX(xi, alpha, t, b, N);
        // Reflect the (fixed) view == N direction about the sampled half-vector.
        const math::Vec3 L = (2.0f * math::Dot(N, H) * H - N).Normalized();
        const float ndl = math::Dot(N, L);
        if (ndl <= 0.0f) continue;
        const Color c = SkyColor(top, horizon, L, gradientPower);
        acc.x += c.r * ndl;
        acc.y += c.g * ndl;
        acc.z += c.b * ndl;
        weight += ndl;
    }
    return weight > 0.0f ? acc * (1.0f / weight) : math::Vec3{};
}

math::Vec2 BrdfLutValue(float ndv, float roughness) {
    ndv = math::Clamp(ndv, 0.0f, 1.0f);
    roughness = math::Clamp(roughness, 0.0f, 1.0f);
    const math::Vec3 N{0.0f, 0.0f, 1.0f};
    const math::Vec3 V{std::sqrt(std::max(1.0f - ndv * ndv, 0.0f)), 0.0f, ndv};
    const float alpha = roughness * roughness;
    math::Vec3 t, b;
    TangentBasis(N, t, b);
    float a = 0.0f, bias = 0.0f;
    for (int i = 0; i < kBrdfSamples; ++i) {
        const math::Vec2 xi = Hammersley(i, kBrdfSamples);
        const math::Vec3 H = ImportanceSampleGGX(xi, alpha, t, b, N);
        const math::Vec3 L = (2.0f * math::Dot(V, H) * H - V).Normalized();
        const float ndl = std::max(L.z, 0.0f);
        const float ndh = std::max(H.z, 0.0f);
        const float vdh = std::max(math::Dot(V, H), 0.0f);
        if (ndl <= 0.0f) continue;
        const float gVis = GeometrySmith(ndv, ndl, roughness) * vdh / (ndh * ndv + 1e-4f);
        const float fc = std::pow(1.0f - vdh, 5.0f);
        a += (1.0f - fc) * gVis;
        bias += fc * gVis;
    }
    return {a / static_cast<float>(kBrdfSamples), bias / static_cast<float>(kBrdfSamples)};
}

std::vector<uint8_t> BuildIrradianceMap(const Color& top, const Color& horizon,
                                        float gradientPower) {
    std::vector<uint8_t> out;
    out.reserve(static_cast<size_t>(kEnvRows) * 4);
    for (int r = 0; r < kEnvRows; ++r) {
        const float ny = -1.0f + (static_cast<float>(r) + 0.5f) / kEnvRows * 2.0f;
        Push(out, IrradianceForNormal(top, horizon, DirForY(ny), gradientPower));
    }
    return out;
}

std::vector<uint8_t> BuildPrefilteredMap(const Color& top, const Color& horizon,
                                         float gradientPower) {
    std::vector<uint8_t> out;
    out.reserve(static_cast<size_t>(kRoughnessCols) * kEnvRows * 4);
    for (int r = 0; r < kEnvRows; ++r) {
        const float ny = -1.0f + (static_cast<float>(r) + 0.5f) / kEnvRows * 2.0f;
        for (int c = 0; c < kRoughnessCols; ++c) {
            const float roughness =
                kRoughnessMin + (1.0f - kRoughnessMin) * (static_cast<float>(c) + 0.5f) /
                                    kRoughnessCols;
            Push(out, PrefilteredForReflection(top, horizon, DirForY(ny), roughness,
                                               gradientPower));
        }
    }
    return out;
}

// --- Octahedral helpers -----------------------------------------------------
namespace {

constexpr float kPi = 3.14159265358979323846f;

// Decoded equirect frame (row 0 = zenith) sampled with bilinear filtering and
// azimuth wrap. Returns 0..1 RGB (the engine is gamma-unaware, so the raw
// bytes are used exactly like the old top/horizon extraction).
math::Vec3 SampleEquirect(const uint8_t* rgba, int w, int h, const math::Vec3& dirIn) {
    if (!rgba || w <= 0 || h <= 0) return {1.0f, 1.0f, 1.0f};
    const math::Vec3 d = dirIn.Normalized();
    const float u = std::atan2(d.z, d.x) * (0.5f / kPi) + 0.5f;
    const float v = std::acos(math::Clamp(d.y, -1.0f, 1.0f)) / kPi;
    const float x = u * static_cast<float>(w) - 0.5f;
    const float y = v * static_cast<float>(h) - 0.5f;
    const int x0 = static_cast<int>(std::floor(x));
    const int y0 = static_cast<int>(std::floor(y));
    const float fx = x - static_cast<float>(x0);
    const float fy = y - static_cast<float>(y0);
    auto fetch = [&](int xi, int yi) {
        xi %= w;
        if (xi < 0) xi += w;
        yi = std::max(0, std::min(h - 1, yi));
        const uint8_t* p = rgba + (static_cast<size_t>(yi) * w + xi) * 4;
        return math::Vec3{p[0] * (1.0f / 255.0f), p[1] * (1.0f / 255.0f),
                          p[2] * (1.0f / 255.0f)};
    };
    const math::Vec3 c00 = fetch(x0, y0);
    const math::Vec3 c10 = fetch(x0 + 1, y0);
    const math::Vec3 c01 = fetch(x0, y0 + 1);
    const math::Vec3 c11 = fetch(x0 + 1, y0 + 1);
    const math::Vec3 cx0 = c00 * (1.0f - fx) + c10 * fx;
    const math::Vec3 cx1 = c01 * (1.0f - fx) + c11 * fx;
    return cx0 * (1.0f - fy) + cx1 * fy;
}

// Generic cosine-hemisphere irradiance over an arbitrary radiance function.
template <typename F>
math::Vec3 IrradianceGeneric(const math::Vec3& n, F&& radiance) {
    const math::Vec3 N = n.Normalized();
    math::Vec3 t, b;
    TangentBasis(N, t, b);
    math::Vec3 acc{};
    for (int i = 0; i < kIrradianceSamples; ++i) {
        const math::Vec2 xi = Hammersley(i, kIrradianceSamples);
        const float phi = math::kTwoPi * xi.x;
        const float cosTheta = std::sqrt(1.0f - xi.y);
        const float sinTheta = std::sqrt(xi.y);
        const math::Vec3 L =
            (t * (std::cos(phi) * sinTheta) + b * (std::sin(phi) * sinTheta) + N * cosTheta)
                .Normalized();
        acc += radiance(L);
    }
    return acc * (1.0f / static_cast<float>(kIrradianceSamples));
}

// Generic GGX-prefiltered specular over an arbitrary radiance function.
template <typename F>
math::Vec3 PrefilterGeneric(const math::Vec3& r, float roughness, F&& radiance) {
    const math::Vec3 N = r.Normalized();
    const float alpha = math::Saturate(roughness) * math::Saturate(roughness);
    math::Vec3 t, b;
    TangentBasis(N, t, b);
    math::Vec3 acc{};
    float weight = 0.0f;
    for (int i = 0; i < kRadianceSamples; ++i) {
        const math::Vec2 xi = Hammersley(i, kRadianceSamples);
        const math::Vec3 H = ImportanceSampleGGX(xi, alpha, t, b, N);
        const math::Vec3 L = (2.0f * math::Dot(N, H) * H - N).Normalized();
        const float ndl = math::Dot(N, L);
        if (ndl <= 0.0f) continue;
        acc += radiance(L) * ndl;
        weight += ndl;
    }
    return weight > 0.0f ? acc * (1.0f / weight) : math::Vec3{};
}

template <typename F>
std::vector<uint8_t> BuildOctaIrradianceGeneric(int size, F&& radiance) {
    std::vector<uint8_t> out;
    out.reserve(static_cast<size_t>(size) * size * 4);
    for (int y = 0; y < size; ++y) {
        for (int x = 0; x < size; ++x) {
            const math::Vec2 uv{(static_cast<float>(x) + 0.5f) / size,
                                (static_cast<float>(y) + 0.5f) / size};
            Push(out, IrradianceGeneric(OctahedralDecode(uv), radiance));
        }
    }
    return out;
}

template <typename F>
std::vector<uint8_t> BuildOctaPrefilteredGeneric(int size, F&& radiance) {
    std::vector<uint8_t> out;
    out.reserve(static_cast<size_t>(size) * (size * kOctaRoughnessLevels) * 4);
    for (int level = 0; level < kOctaRoughnessLevels; ++level) {
        const float t = kOctaRoughnessLevels > 1
                            ? static_cast<float>(level) / static_cast<float>(kOctaRoughnessLevels - 1)
                            : 0.0f;
        const float roughness = kRoughnessMin + (1.0f - kRoughnessMin) * t;
        for (int y = 0; y < size; ++y) {
            for (int x = 0; x < size; ++x) {
                const math::Vec2 uv{(static_cast<float>(x) + 0.5f) / size,
                                    (static_cast<float>(y) + 0.5f) / size};
                Push(out, PrefilterGeneric(OctahedralDecode(uv), roughness, radiance));
            }
        }
    }
    return out;
}

} // namespace

math::Vec2 OctahedralEncode(const math::Vec3& nIn) {
    math::Vec3 n = nIn.Normalized();
    const float inv = 1.0f / std::max(std::fabs(n.x) + std::fabs(n.y) + std::fabs(n.z), 1e-8f);
    n.x *= inv;
    n.y *= inv;
    if (n.z < 0.0f) {
        const float ox = (1.0f - std::fabs(n.y)) * (n.x >= 0.0f ? 1.0f : -1.0f);
        const float oy = (1.0f - std::fabs(n.x)) * (n.y >= 0.0f ? 1.0f : -1.0f);
        n.x = ox;
        n.y = oy;
    }
    return {n.x * 0.5f + 0.5f, n.y * 0.5f + 0.5f};
}

math::Vec3 OctahedralDecode(const math::Vec2& uv) {
    const float fx = uv.x * 2.0f - 1.0f;
    const float fy = uv.y * 2.0f - 1.0f;
    math::Vec3 n{fx, fy, 1.0f - std::fabs(fx) - std::fabs(fy)};
    if (n.z < 0.0f) {
        const float ox = (1.0f - std::fabs(n.y)) * (n.x >= 0.0f ? 1.0f : -1.0f);
        const float oy = (1.0f - std::fabs(n.x)) * (n.y >= 0.0f ? 1.0f : -1.0f);
        n.x = ox;
        n.y = oy;
    }
    return n.Normalized();
}

std::vector<uint8_t> BuildOctaIrradiance(const Color& top, const Color& horizon,
                                         float gradientPower) {
    return BuildOctaIrradianceGeneric(kOctaIrradianceSize, [&](const math::Vec3& d) {
        const Color c = SkyColor(top, horizon, d, gradientPower);
        return math::Vec3{c.r, c.g, c.b};
    });
}

std::vector<uint8_t> BuildOctaPrefiltered(const Color& top, const Color& horizon,
                                          float gradientPower) {
    return BuildOctaPrefilteredGeneric(kOctaPrefilterSize, [&](const math::Vec3& d) {
        const Color c = SkyColor(top, horizon, d, gradientPower);
        return math::Vec3{c.r, c.g, c.b};
    });
}

std::vector<uint8_t> BuildOctaIrradianceFromEquirect(const uint8_t* rgba, int width, int height) {
    return BuildOctaIrradianceGeneric(kOctaIrradianceSize,
                                      [&](const math::Vec3& d) {
                                          return SampleEquirect(rgba, width, height, d);
                                      });
}

std::vector<uint8_t> BuildOctaPrefilteredFromEquirect(const uint8_t* rgba, int width, int height) {
    return BuildOctaPrefilteredGeneric(kOctaPrefilterSize,
                                       [&](const math::Vec3& d) {
                                           return SampleEquirect(rgba, width, height, d);
                                       });
}

std::vector<uint8_t> BuildBrdfLut() {    std::vector<uint8_t> out;
    out.reserve(static_cast<size_t>(kBrdfLutSize) * kBrdfLutSize * 4);
    for (int r = 0; r < kBrdfLutSize; ++r) {
        const float roughness = (static_cast<float>(r) + 0.5f) / kBrdfLutSize;
        for (int c = 0; c < kBrdfLutSize; ++c) {
            const float ndv = (static_cast<float>(c) + 0.5f) / kBrdfLutSize;
            const math::Vec2 v = BrdfLutValue(ndv, roughness);
            out.push_back(Encode(v.x));
            out.push_back(Encode(v.y));
            out.push_back(0);
            out.push_back(255);
        }
    }
    return out;
}

void SkyDominantColors(const uint8_t* rgba, int width, int height, Color& zenith,
                       Color& horizon) {
    if (!rgba || width <= 0 || height <= 0) return;
    // Equirect: v=0 row is the zenith pole, v~0.5 the horizon. Sample the upper
    // hemisphere only (sky/terrain in the photo's lower half is ignored) so the
    // extracted colours are pure atmosphere, not ground.
    const int zenithEnd = std::max(1, static_cast<int>(height * 0.20f));
    const int horizLo = std::max(0, static_cast<int>(height * 0.42f));
    const int horizHi = std::min(height, static_cast<int>(height * 0.55f));
    if (horizHi <= horizLo) return;
    auto avg = [&](int y0, int y1, math::Vec3& acc) {
        float r = 0.0f, g = 0.0f, b = 0.0f;
        int n = 0;
        for (int y = y0; y < y1; ++y) {
            for (int x = 0; x < width; x += 2) {
                const uint8_t* p = rgba + (static_cast<size_t>(y) * width + x) * 4;
                r += p[0]; g += p[1]; b += p[2]; ++n;
            }
        }
        if (n > 0) { acc = {r / n, g / n, b / n}; }
    };
    math::Vec3 z{}, hz{};
    avg(0, zenithEnd, z);
    avg(horizLo, horizHi, hz);
    const float k = 1.0f / 255.0f;
    zenith = {z.x * k, z.y * k, z.z * k, 1.0f};
    horizon = {hz.x * k, hz.y * k, hz.z * k, 1.0f};
}

} // namespace neon::gfx::ibl
