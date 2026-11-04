#pragma once

#include <cstdint>

namespace neon::world {

// Deterministic integer hash: same (x, z, seed) always yields the same value.
uint32_t Hash2D(int x, int z, uint32_t seed);

// Gradient (Perlin-style) value noise in roughly [-1, 1]. Continuous,
// deterministic, no static state.
float ValueNoise2D(float x, float z, uint32_t seed);

// Fractal sum of ValueNoise2D. Each octave doubles the frequency (lacunarity)
// and scales the amplitude by gain; the result is normalized by the total
// amplitude so it stays in roughly [-1, 1].
float Fbm2D(float x, float z, uint32_t seed, int octaves, float lacunarity,
            float gain);

// Ridged multifractal: sum of (1 - |noise|) octaves, normalized to [0, 1].
// Produces sharp mountain ridges rather than rolling hills.
float Ridged2D(float x, float z, uint32_t seed, int octaves);

} // namespace neon::world
