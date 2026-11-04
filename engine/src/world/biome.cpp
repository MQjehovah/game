#include "neon/world/biome.hpp"

#include <cmath>

#include "neon/world/noise.hpp"

namespace neon::world {
namespace {

float Clamp01(float v) { return v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v); }

float SmoothStep(float edge0, float edge1, float x) {
    const float t = Clamp01((x - edge0) / (edge1 - edge0));
    return t * t * (3.0f - 2.0f * t);
}

struct Anchor {
    float temperature;
    float moisture;
};

// Grassland / Forest / Badlands / Alpine anchors in climate space.
constexpr Anchor kAnchors[4] = {
    {0.56f, 0.54f},  // Grassland: mild, moderate moisture
    {0.42f, 0.76f},  // Forest: cool, wet
    {0.82f, 0.24f},  // Badlands: hot, dry
    {0.14f, 0.44f},  // Alpine: cold
};

} // namespace

void BiomeWeights::Normalize() {
    const float sum = Sum();
    if (sum <= 1e-8f) {
        w[0] = 1.0f;
        w[1] = w[2] = w[3] = 0.0f;
        return;
    }
    const float inv = 1.0f / sum;
    for (float& v : w) v *= inv;
}

Climate SampleClimate(float x, float z, uint32_t seed) {
    // Low-frequency fields, spread over most of [0, 1] so every anchor is
    // reachable within a few kilometres of the camp.
    const float tNoise = Fbm2D(x / 900.0f, z / 900.0f, seed, 4, 2.0f, 0.5f);
    const float mNoise = Fbm2D(x / 650.0f, z / 650.0f, seed + 101u, 4, 2.0f, 0.5f);

    Climate c;
    c.temperature = Clamp01(0.5f + 0.9f * tNoise);
    c.moisture = Clamp01(0.5f + 0.9f * mNoise);

    // North gets colder as it rises into the alpine region.
    c.temperature = Clamp01(c.temperature - 0.38f * SmoothStep(200.0f, 1600.0f, z));
    return c;
}

BiomeWeights SampleBiomeWeights(float x, float z, uint32_t seed) {
    const Climate c = SampleClimate(x, z, seed);
    BiomeWeights out;
    constexpr float kSigma = 0.18f;
    constexpr float kInv2Sigma2 = 1.0f / (2.0f * kSigma * kSigma);

    for (int i = 0; i < 4; ++i) {
        const float dt = c.temperature - kAnchors[i].temperature;
        const float dm = c.moisture - kAnchors[i].moisture;
        out.w[i] = std::exp(-(dt * dt + dm * dm) * kInv2Sigma2);
    }

    // Authored starting bubble: the camp and its immediate POIs are grassland
    // regardless of the climate field, so the existing 288 m content keeps its
    // intended look.
    const float dist = std::sqrt(x * x + z * z);
    const float camp = 1.0f - SmoothStep(120.0f, 300.0f, dist);
    if (camp > 0.0f) {
        for (int i = 0; i < 4; ++i) out.w[i] *= (1.0f - camp);
        out.w[static_cast<int>(Biome::Grassland)] += camp;
    }

    out.Normalize();
    return out;
}

Biome DominantBiome(const BiomeWeights& weights) {
    int best = 0;
    for (int i = 1; i < 4; ++i)
        if (weights.w[i] > weights.w[best]) best = i;
    return static_cast<Biome>(best);
}

} // namespace neon::world
