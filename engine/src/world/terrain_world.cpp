#include "neon/world/terrain_world.hpp"

#include <cmath>
#include <utility>

#include "neon/world/noise.hpp"

namespace neon::world {
namespace {

float Clamp01(float v) { return v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v); }

float SmoothStep(float edge0, float edge1, float x) {
    if (edge1 <= edge0) return x < edge0 ? 0.0f : 1.0f;
    const float t = Clamp01((x - edge0) / (edge1 - edge0));
    return t * t * (3.0f - 2.0f * t);
}

// Authored starting bubble: everything inside the camp is level ground so the
// existing 288 m content keeps sitting where it was authored.
constexpr float kCampInner = 55.0f;
constexpr float kCampOuter = 110.0f;

} // namespace

TerrainWorld::TerrainWorld(TerrainWorldConfig config) : config_(std::move(config)) {}

float TerrainWorld::RawHeight(float x, float z) const {
    const uint32_t s = config_.seed;
    const float bs = config_.biomeScale;
    const BiomeWeights w = SampleBiomeWeights(x, z, s);

    // Grassland: gentle rolling hills.
    const float grass = 6.0f * Fbm2D(x * bs * 3.0f, z * bs * 3.0f, s + 11u, 4, 2.0f, 0.5f);

    // Forest: taller ridges and gullies.
    const float forest = 14.0f * Fbm2D(x * bs * 4.0f, z * bs * 4.0f, s + 23u, 4, 2.0f, 0.5f);

    // Badlands: terraced mesas.
    const float badNoise = Fbm2D(x * bs * 3.5f, z * bs * 3.5f, s + 31u, 4, 2.0f, 0.5f);
    float terrace = badNoise * 0.5f + 0.5f;
    terrace = std::floor(terrace * 6.0f) / 6.0f;
    const float badlands = 22.0f * (terrace - 0.5f);

    // Alpine: a ridged mountain range that only rises north of mountainStartZ.
    // The uplift is independent of the biome blend (a mountain stays a mountain
    // even if its local climate is still grassland); the Alpine biome only adds
    // small surface detail on top.
    const float mount = SmoothStep(config_.mountainStartZ, config_.mountainStartZ + 400.0f, z);
    const float ridge = Ridged2D(x * bs * 2.0f, z * bs * 2.0f, s + 37u, 5);
    const float uplift = 100.0f * mount * (0.45f + 0.55f * ridge);
    const float alpineDetail = 12.0f * (ridge - 0.5f);

    float h = uplift + w.w[0] * grass + w.w[1] * forest + w.w[2] * badlands +
              w.w[3] * alpineDetail;

    const float dist = std::sqrt(x * x + z * z);
    const float camp = 1.0f - SmoothStep(kCampInner, kCampOuter, dist);
    h *= (1.0f - camp);
    return h;
}

float TerrainWorld::Height(float x, float z) const {
    float h = RawHeight(x, z);
    if (config_.features.roads.empty() && config_.features.pads.empty()) return h;

    const FlattenSample f = SampleFlattening(
        config_.features, x, z,
        [this](float fx, float fz) { return RawHeight(fx, fz); });
    if (f.weight > 0.0f) h = h * (1.0f - f.weight) + f.height * f.weight;
    return h;
}

math::Vec3 TerrainWorld::Normal(float x, float z) const {
    const float e = 0.5f;
    const float hl = Height(x - e, z);
    const float hr = Height(x + e, z);
    const float hd = Height(x, z - e);
    const float hu = Height(x, z + e);
    math::Vec3 n{hl - hr, 2.0f * e, hd - hu};
    const float len = n.Length();
    if (len <= 1e-6f) return math::Vec3{0.0f, 1.0f, 0.0f};
    const float inv = 1.0f / len;
    n.x *= inv;
    n.y *= inv;
    n.z *= inv;
    return n;
}

BiomeWeights TerrainWorld::Weights(float x, float z) const {
    return SampleBiomeWeights(x, z, config_.seed);
}

} // namespace neon::world
