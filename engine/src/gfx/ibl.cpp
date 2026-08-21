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

std::vector<uint8_t> BuildBrdfLut() {
    std::vector<uint8_t> out;
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

} // namespace neon::gfx::ibl
