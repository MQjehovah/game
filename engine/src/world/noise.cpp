#include "neon/world/noise.hpp"

#include <cmath>

namespace neon::world {
namespace {

float Fade(float t) { return t * t * t * (t * (t * 6.0f - 15.0f) + 10.0f); }
float Lerp(float a, float b, float t) { return a + (b - a) * t; }

// One of 8 unit-ish gradient directions from the low 3 hash bits. Unit length
// keeps the noise Lipschitz-bounded and cheap.
float Grad(uint32_t hash, float x, float y) {
    static const float kGx[8] = {1.0f, -1.0f, 0.0f, 0.0f,
                                 0.70710678f, -0.70710678f, 0.70710678f, -0.70710678f};
    static const float kGy[8] = {0.0f, 0.0f, 1.0f, -1.0f,
                                 0.70710678f, 0.70710678f, -0.70710678f, -0.70710678f};
    const uint32_t i = hash & 7u;
    return kGx[i] * x + kGy[i] * y;
}

} // namespace

uint32_t Hash2D(int x, int z, uint32_t seed) {
    uint32_t h = static_cast<uint32_t>(x) * 0x8da6b343u;
    h ^= static_cast<uint32_t>(z) * 0xd8163841u;
    h ^= seed * 0xcb1ab31fu;
    h ^= h >> 16;
    h *= 0x7feb352du;
    h ^= h >> 15;
    h *= 0x846ca68bu;
    h ^= h >> 16;
    return h;
}

float ValueNoise2D(float x, float z, uint32_t seed) {
    const int xi = static_cast<int>(std::floor(x));
    const int zi = static_cast<int>(std::floor(z));
    const float tx = x - static_cast<float>(xi);
    const float tz = z - static_cast<float>(zi);
    const float u = Fade(tx);
    const float v = Fade(tz);

    const float g00 = Grad(Hash2D(xi, zi, seed), tx, tz);
    const float g10 = Grad(Hash2D(xi + 1, zi, seed), tx - 1.0f, tz);
    const float g01 = Grad(Hash2D(xi, zi + 1, seed), tx, tz - 1.0f);
    const float g11 = Grad(Hash2D(xi + 1, zi + 1, seed), tx - 1.0f, tz - 1.0f);

    return Lerp(Lerp(g00, g10, u), Lerp(g01, g11, u), v);
}

float Fbm2D(float x, float z, uint32_t seed, int octaves, float lacunarity,
            float gain) {
    if (octaves <= 0) return 0.0f;
    float sum = 0.0f;
    float amp = 1.0f;
    float total = 0.0f;
    float freq = 1.0f;
    for (int i = 0; i < octaves; ++i) {
        sum += ValueNoise2D(x * freq, z * freq, seed + static_cast<uint32_t>(i) * 7919u) * amp;
        total += amp;
        amp *= gain;
        freq *= lacunarity;
    }
    return total > 0.0f ? sum / total : 0.0f;
}

float Ridged2D(float x, float z, uint32_t seed, int octaves) {
    if (octaves <= 0) return 0.0f;
    float sum = 0.0f;
    float amp = 1.0f;
    float total = 0.0f;
    float freq = 1.0f;
    for (int i = 0; i < octaves; ++i) {
        const float n = 1.0f - std::fabs(ValueNoise2D(x * freq, z * freq,
                                                      seed + static_cast<uint32_t>(i) * 6151u));
        sum += n * n * amp;
        total += amp;
        amp *= 0.5f;
        freq *= 2.0f;
    }
    const float r = total > 0.0f ? sum / total : 0.0f;
    return r < 0.0f ? 0.0f : (r > 1.0f ? 1.0f : r);
}

} // namespace neon::world
