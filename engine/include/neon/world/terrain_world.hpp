#pragma once

#include <cstdint>

#include "neon/math/vec3.hpp"
#include "neon/world/biome.hpp"
#include "neon/world/world_features.hpp"

namespace neon::world {

struct TerrainWorldConfig {
    uint32_t seed = 1337;
    float biomeScale = 1.0f / 420.0f;
    WorldFeatures features;
    float mountainStartZ = 300.0f;
};

// Deterministic procedural terrain: biome blend -> raw height -> authored camp
// bubble -> road/POI flattening. Same config gives bit-identical heights.
class TerrainWorld {
public:
    explicit TerrainWorld(TerrainWorldConfig config);

    float Height(float x, float z) const;
    math::Vec3 Normal(float x, float z) const;
    BiomeWeights Weights(float x, float z) const;
    const TerrainWorldConfig& Config() const { return config_; }

private:
    float RawHeight(float x, float z) const;

    TerrainWorldConfig config_;
};

} // namespace neon::world
