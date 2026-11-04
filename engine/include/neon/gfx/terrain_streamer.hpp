#pragma once

#include <cstddef>
#include <unordered_map>
#include <vector>

#include "neon/gfx/material.hpp"
#include "neon/gfx/mesh.hpp"
#include "neon/gfx/renderer.hpp"
#include "neon/world/terrain_chunk.hpp"
#include "neon/world/terrain_world.hpp"

namespace neon::gfx {

struct TerrainStreamerConfig {
    int maxLevel = 5;
    int cells = 16;
    float viewDistance = 2048.0f;
    size_t maxResident = 256;
    int buildsPerFrame = 2;
    float skirtDepth = 4.0f;
};

// Keeps a quadtree LOD window of terrain chunk meshes alive around the camera.
// Meshes are built on the main thread, at most `buildsPerFrame` per update, so
// a fast camera never stalls on a full rebuild.
class TerrainStreamer {
public:
    void Configure(const world::TerrainWorld* world, const TerrainStreamerConfig& config);
    void Update(Renderer& renderer, const math::Vec3& focus, float dt);
    void Draw(Renderer& renderer, const math::Mat4& viewProj, const Material& terrainMaterial);
    void Clear(Renderer& renderer);

    size_t Resident() const { return chunks_.size(); }
    const TerrainStreamerConfig& Config() const { return config_; }

private:
    struct Chunk {
        Mesh mesh;
        int level = 0;
        int x = 0;
        int z = 0;
    };
    struct KeyHash {
        size_t operator()(const world::TerrainChunkKey& key) const;
    };

    const world::TerrainWorld* world_ = nullptr;
    TerrainStreamerConfig config_;
    std::unordered_map<world::TerrainChunkKey, Chunk, KeyHash> chunks_;
    std::vector<world::TerrainChunkKey> selected_;
    std::vector<world::TerrainChunkKey> missing_;
};

} // namespace neon::gfx
