#pragma once

#include <cstdint>
#include <vector>

#include "neon/math/vec2.hpp"
#include "neon/math/vec3.hpp"
#include "neon/math/vec4.hpp"
#include "neon/world/terrain_world.hpp"

namespace neon::world {

struct TerrainVertex {
    math::Vec3 position;
    math::Vec3 normal;
    math::Vec2 uv;
    math::Vec4 weights;  // x=soil, y=rock, z=snow, w=wet
    math::Vec4 tint;     // biome tint blended into the albedo
};

struct TerrainChunkKey {
    int level = 0;
    int x = 0;
    int z = 0;

    bool operator==(const TerrainChunkKey& other) const {
        return level == other.level && x == other.x && z == other.z;
    }
};

// A level-N chunk is 32 * 2^N metres across.
float TerrainChunkSize(int level);

// Non-overlapping quadtree leaves covering the square of `viewDistance` around
// `focus`. Leaves subdivide while the camera is within two tile widths, so the
// ground near the camera is dense and far tiles stay cheap.
void SelectTerrainChunks(math::Vec2 focus, float viewDistance, int maxLevel,
                         std::vector<TerrainChunkKey>& out);

struct TerrainChunkMeshData {
    std::vector<TerrainVertex> vertices;
    std::vector<uint32_t> indices;
    // Dominant-biome tint at the chunk centre; the streamer applies it as the
    // per-chunk material tint so grassland/forest/badlands read as different
    // places even before per-biome layer sets land.
    math::Vec4 tint{1.0f, 1.0f, 1.0f, 1.0f};
};

// CPU mesh for one chunk: (cells+1)^2 grid vertices plus a downward skirt on
// all four edges so neighbouring LOD levels cannot show cracks.
void BuildTerrainChunkData(const TerrainWorld& world, const TerrainChunkKey& key,
                           int cells, float skirtDepth, TerrainChunkMeshData& out);

} // namespace neon::world
