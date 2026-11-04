#include "neon/world/terrain_chunk.hpp"

#include <algorithm>
#include <cmath>
#include <functional>

namespace neon::world {
namespace {

float Clamp01(float v) { return v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v); }

float SmoothStep(float edge0, float edge1, float x) {
    if (edge1 <= edge0) return x < edge0 ? 0.0f : 1.0f;
    const float t = Clamp01((x - edge0) / (edge1 - edge0));
    return t * t * (3.0f - 2.0f * t);
}

math::Vec4 BiomeTint(const BiomeWeights& w) {
    static const math::Vec4 kTint[4] = {
        {0.34f, 0.48f, 0.24f, 1.0f},  // Grassland
        {0.22f, 0.34f, 0.18f, 1.0f},  // Forest
        {0.62f, 0.52f, 0.34f, 1.0f},  // Badlands
        {0.78f, 0.82f, 0.86f, 1.0f},  // Alpine
    };
    math::Vec4 out{0.0f, 0.0f, 0.0f, 1.0f};
    for (int i = 0; i < 4; ++i) {
        out.x += kTint[i].x * w.w[i];
        out.y += kTint[i].y * w.w[i];
        out.z += kTint[i].z * w.w[i];
    }
    return out;
}

} // namespace

float TerrainChunkSize(int level) {
    if (level < 0) level = 0;
    return 32.0f * static_cast<float>(1 << level);
}

void SelectTerrainChunks(math::Vec2 focus, float viewDistance, int maxLevel,
                         std::vector<TerrainChunkKey>& out) {
    out.clear();
    if (maxLevel < 0) maxLevel = 0;
    if (viewDistance <= 0.0f) return;

    const float rootSize = TerrainChunkSize(maxLevel);
    const int minX = static_cast<int>(std::floor((focus.x - viewDistance) / rootSize));
    const int maxX = static_cast<int>(std::floor((focus.x + viewDistance) / rootSize));
    const int minZ = static_cast<int>(std::floor((focus.y - viewDistance) / rootSize));
    const int maxZ = static_cast<int>(std::floor((focus.y + viewDistance) / rootSize));

    std::function<void(TerrainChunkKey)> visit = [&](TerrainChunkKey key) {
        const float size = TerrainChunkSize(key.level);
        const float x0 = static_cast<float>(key.x) * size;
        const float z0 = static_cast<float>(key.z) * size;
        const float dx = std::max(std::max(x0 - focus.x, 0.0f),
                                  focus.x - (x0 + size));
        const float dz = std::max(std::max(z0 - focus.y, 0.0f),
                                  focus.y - (z0 + size));
        const float dist = std::sqrt(dx * dx + dz * dz);

        // Small margin (4 m) reduces boundary flapping when the camera jitters
        // on a ring edge; the streamer additionally keeps a parent alive until
        // its replacement children are resident.
        if (key.level > 0 && dist < size * 2.0f + 4.0f) {
            const int l = key.level - 1;
            const int cx = key.x * 2;
            const int cz = key.z * 2;
            visit({l, cx, cz});
            visit({l, cx + 1, cz});
            visit({l, cx, cz + 1});
            visit({l, cx + 1, cz + 1});
            return;
        }
        out.push_back(key);
    };

    for (int z = minZ; z <= maxZ; ++z)
        for (int x = minX; x <= maxX; ++x) visit({maxLevel, x, z});
}

void BuildTerrainChunkData(const TerrainWorld& world, const TerrainChunkKey& key,
                           int cells, float skirtDepth, TerrainChunkMeshData& out) {
    out.vertices.clear();
    out.indices.clear();
    if (cells <= 0) return;

    const float size = TerrainChunkSize(key.level);
    const float cell = size / static_cast<float>(cells);
    const float ox = static_cast<float>(key.x) * size;
    const float oz = static_cast<float>(key.z) * size;
    const int stride = cells + 1;
    out.tint = BiomeTint(world.Weights(ox + size * 0.5f, oz + size * 0.5f));

    out.vertices.reserve(static_cast<size_t>(stride * stride) + 4u * stride);

    for (int j = 0; j <= cells; ++j) {
        for (int i = 0; i <= cells; ++i) {
            const float x = ox + static_cast<float>(i) * cell;
            const float z = oz + static_cast<float>(j) * cell;
            TerrainVertex v;
            v.position = {x, world.Height(x, z), z};
            v.normal = world.Normal(x, z);
            v.uv = {x * 0.25f, z * 0.25f};

            const float slope = 1.0f - v.normal.y;
            const float rock = SmoothStep(0.30f, 0.62f, slope);
            const float altSnow = SmoothStep(55.0f, 85.0f, v.position.y);
            const float snow = altSnow * (1.0f - SmoothStep(0.45f, 0.70f, slope));
            const float soil = Clamp01(1.0f - rock - snow);
            v.weights = {soil, rock, snow, 0.0f};
            v.tint = BiomeTint(world.Weights(x, z));
            out.vertices.push_back(v);
        }
    }

    for (int j = 0; j < cells; ++j) {
        for (int i = 0; i < cells; ++i) {
            const uint32_t a = static_cast<uint32_t>(j * stride + i);
            const uint32_t b = a + 1;
            const uint32_t c = static_cast<uint32_t>((j + 1) * stride + i);
            const uint32_t d = c + 1;
            out.indices.push_back(a);
            out.indices.push_back(c);
            out.indices.push_back(b);
            out.indices.push_back(b);
            out.indices.push_back(c);
            out.indices.push_back(d);
        }
    }

    // Skirt: duplicate each edge and drop it by skirtDepth, then connect the
    // copies with a vertical strip. The top vertices keep their exact sampled
    // height so adjacent chunks line up; the skirt only hides the gap that a
    // coarser neighbour can leave under a finer edge.
    auto addEdge = [&](int i0, int j0, int di, int dj) {
        const uint32_t base = static_cast<uint32_t>(out.vertices.size());
        std::vector<uint32_t> top(static_cast<size_t>(cells + 1));
        for (int k = 0; k <= cells; ++k) {
            const int i = i0 + di * k;
            const int j = j0 + dj * k;
            const uint32_t src = static_cast<uint32_t>(j * stride + i);
            top[static_cast<size_t>(k)] = src;
            TerrainVertex v = out.vertices[src];
            v.position.y -= skirtDepth;
            out.vertices.push_back(v);
        }
        for (int k = 0; k < cells; ++k) {
            const uint32_t t0 = top[static_cast<size_t>(k)];
            const uint32_t t1 = top[static_cast<size_t>(k + 1)];
            const uint32_t b0 = base + static_cast<uint32_t>(k);
            const uint32_t b1 = base + static_cast<uint32_t>(k + 1);
            out.indices.push_back(t0);
            out.indices.push_back(b0);
            out.indices.push_back(t1);
            out.indices.push_back(t1);
            out.indices.push_back(b0);
            out.indices.push_back(b1);
        }
    };

    addEdge(0, 0, 1, 0);            // south edge
    addEdge(0, cells, 1, 0);        // north edge
    addEdge(0, 0, 0, 1);            // west edge
    addEdge(cells, 0, 0, 1);        // east edge
}

} // namespace neon::world
