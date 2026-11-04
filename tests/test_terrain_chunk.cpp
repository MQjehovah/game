#include <cmath>
#include <set>
#include <tuple>
#include <vector>

#include "helpers.hpp"
#include "neon/world/terrain_chunk.hpp"

TEST(TerrainChunksCoverFocusWithoutOverlap) {
    std::vector<neon::world::TerrainChunkKey> keys;
    neon::world::SelectTerrainChunks({0.0f, 0.0f}, 512.0f, 4, keys);
    CHECK(keys.size() > 8u);
    CHECK(keys.size() < 600u);
    std::set<std::tuple<int, int, int>> seen;
    for (auto& k : keys) CHECK(seen.insert({k.level, k.x, k.z}).second);
    bool coversOrigin = false;
    for (auto& k : keys) {
        float s = neon::world::TerrainChunkSize(k.level);
        float x0 = static_cast<float>(k.x) * s;
        float z0 = static_cast<float>(k.z) * s;
        if (x0 <= 0.0f && 0.0f < x0 + s && z0 <= 0.0f && 0.0f < z0 + s) coversOrigin = true;
    }
    CHECK(coversOrigin);
}

TEST(TerrainChunkMeshMatchesHeightAndHasSkirt) {
    neon::world::TerrainWorld w({1337, 1.0f / 420.0f, {}, 300.0f});
    neon::world::TerrainChunkMeshData m;
    neon::world::BuildTerrainChunkData(w, {0, 0, 0}, 8, 2.0f, m);
    CHECK(m.vertices.size() >= 81u);
    CHECK(!m.indices.empty());
    for (auto& v : m.vertices) {
        CHECK(std::isfinite(v.position.y));
        CHECK_NEAR(v.normal.Length(), 1.0f, 1e-3);
    }
    const float cell = neon::world::TerrainChunkSize(0) / 8.0f;
    for (int j = 0; j <= 8; ++j)
        for (int i = 0; i <= 8; ++i) {
            float x = static_cast<float>(i) * cell;
            float z = static_cast<float>(j) * cell;
            CHECK_NEAR(m.vertices[j * 9 + i].position.y, w.Height(x, z), 0.02);
        }
}

TEST(TerrainChunkAdjacentEdgesMatch) {
    neon::world::TerrainWorld w({1337, 1.0f / 420.0f, {}, 300.0f});
    neon::world::TerrainChunkMeshData a, b;
    neon::world::BuildTerrainChunkData(w, {0, 0, 0}, 8, 2.0f, a);
    neon::world::BuildTerrainChunkData(w, {0, 1, 0}, 8, 2.0f, b);  // east neighbour (x+1)
    for (int j = 0; j <= 8; ++j) {
        CHECK_NEAR(a.vertices[j * 9 + 8].position.y,
                   b.vertices[j * 9 + 0].position.y, 1e-3);
        CHECK_NEAR(a.vertices[j * 9 + 8].position.x,
                   b.vertices[j * 9 + 0].position.x, 1e-3);
    }
}
