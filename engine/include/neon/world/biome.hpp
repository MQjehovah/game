#pragma once

#include <cstdint>

namespace neon::world {

// Four climate-driven biomes. The camp starts in Grassland; the other three
// are reached by driving far enough for the low-frequency climate field to
// move into their anchor region.
enum class Biome : uint8_t { Grassland = 0, Forest = 1, Badlands = 2, Alpine = 3, Count = 4 };

struct Climate {
    float temperature = 0.0f;  // [0, 1]
    float moisture = 0.0f;     // [0, 1]
};

// Normalized biome blend weights (sum == 1).
struct BiomeWeights {
    float w[4] = {1.0f, 0.0f, 0.0f, 0.0f};

    float At(Biome b) const { return w[static_cast<int>(b)]; }
    float Sum() const { return w[0] + w[1] + w[2] + w[3]; }
    void Normalize();
};

Climate SampleClimate(float x, float z, uint32_t seed);
BiomeWeights SampleBiomeWeights(float x, float z, uint32_t seed);
Biome DominantBiome(const BiomeWeights& weights);

} // namespace neon::world
