#include "neon/gfx/terrain.hpp"

#include <algorithm>
#include <cmath>

#include "neon/gfx/renderer.hpp"
#include "neon/math/math.hpp"
#include "neon/math/vec3.hpp"

namespace neon::gfx {
namespace {

// Bilinear interpolate the heightfield at world (x,z). h is the raw stored
// height; the caller scales by heightScale.
float SampleRaw(const std::vector<float>& heights, int segments, float size, float x,
                float z) {
    if (heights.empty() || segments < 1) return 0.0f;
    const int cols = segments + 1;
    const float half = size * 0.5f;
    const float cell = size / static_cast<float>(segments);
    float gx = (x + half) / cell;
    float gz = (z + half) / cell;
    gx = math::Clamp(gx, 0.0f, static_cast<float>(segments));
    gz = math::Clamp(gz, 0.0f, static_cast<float>(segments));
    const int i0 = static_cast<int>(gx);
    const int j0 = static_cast<int>(gz);
    const int i1 = std::min(i0 + 1, segments);
    const int j1 = std::min(j0 + 1, segments);
    const float fx = gx - static_cast<float>(i0);
    const float fz = gz - static_cast<float>(j0);
    auto at = [&](int i, int j) {
        size_t k = static_cast<size_t>(j) * cols + static_cast<size_t>(i);
        return k < heights.size() ? heights[k] : 0.0f;
    };
    const float h00 = at(i0, j0);
    const float h10 = at(i1, j0);
    const float h01 = at(i0, j1);
    const float h11 = at(i1, j1);
    const float a = math::Lerp(h00, h10, fx);
    const float b = math::Lerp(h01, h11, fx);
    return math::Lerp(a, b, fz);
}

// Slope (1 - normal.y) from the heightfield gradient in world units.
float SlopeAt(const std::vector<float>& heights, int segments, float size, float x,
              float z, float heightScale) {
    const float cell = size / static_cast<float>(segments);
    const float e = std::max(cell * 0.5f, 1e-3f);
    const float hxC = SampleRaw(heights, segments, size, x, z);
    const float hx = (SampleRaw(heights, segments, size, x + e, z) - hxC) / e;
    const float hz = (SampleRaw(heights, segments, size, x, z + e) - hxC) / e;
    // Gradient in world Y per world unit (heightScale already applied to the
    // stored heights' effect; use the same linear factor as the normals).
    const float gx = hx * heightScale;
    const float gz = hz * heightScale;
    const float g = std::sqrt(gx * gx + gz * gz);
    return 1.0f - 1.0f / std::sqrt(1.0f + g * g);
}

struct PlaneMesh {
    std::vector<Vertex3D> verts;
    std::vector<uint16_t> indices;
};

// Build one chunk surface at `cells` x `cells` over the chunk's world rect,
// with a thin downward skirt around the perimeter to mask LOD cracks.
PlaneMesh BuildChunkSurface(const std::vector<float>& heights, int segments, float size,
                            float heightScale, float xMin, float xMax, float zMin, float zMax,
                            int cells, const TerrainLayerConfig& layer, bool skirt,
                            float skirtDrop) {
    PlaneMesh pm;
    const int n = cells + 1;
    pm.verts.reserve(static_cast<size_t>(n) * n + (skirt ? static_cast<size_t>(n) * 4 : 0));
    pm.indices.reserve(static_cast<size_t>(cells) * cells * 6 +
                       (skirt ? static_cast<size_t>(cells) * 4 * 6 : 0));

    const float cw = (xMax - xMin) / static_cast<float>(cells);
    const float cd = (zMax - zMin) / static_cast<float>(cells);
    auto gridIdx = [&](int i, int j) { return static_cast<uint16_t>(static_cast<size_t>(j) * n + i); };

    for (int j = 0; j < n; ++j) {
        for (int i = 0; i < n; ++i) {
            const float u = static_cast<float>(i) / static_cast<float>(cells);
            const float v = static_cast<float>(j) / static_cast<float>(cells);
            const float x = math::Lerp(xMin, xMax, u);
            const float z = math::Lerp(zMin, zMax, v);
            const float y = SampleRaw(heights, segments, size, x, z) * heightScale;
            const float hR = SampleRaw(heights, segments, size, x + cw, z) * heightScale;
            const float hD = SampleRaw(heights, segments, size, x, z + cd) * heightScale;
            math::Vec3 normal = math::Cross({cw, hR - y, 0.0f}, {0.0f, hD - y, cd}).Normalized();
            if (normal.y < 0.0f) normal = -normal;
            const float slope = 1.0f - normal.y;
            // Splat WEIGHTS (R=grass, G=dirt, B=rock sum to 1), not the final
            // layered color: the terrain splatmap shader mixes a grass texture
            // + dirt/rock colors by these weights. (The editor's non-chunk
            // CreateTerrain mesh uses the final TerrainLayerColor instead.)
            const math::Vec4 color = TerrainLayerWeights(y, heightScale, slope, layer);
            pm.verts.push_back({math::Vec3{x, y, z}, normal, {x / cw, z / cd}, color});
        }
    }

    for (int j = 0; j < cells; ++j) {
        for (int i = 0; i < cells; ++i) {
            const uint16_t a = gridIdx(i, j);
            const uint16_t b = gridIdx(i + 1, j);
            const uint16_t c = gridIdx(i + 1, j + 1);
            const uint16_t d = gridIdx(i, j + 1);
            pm.indices.insert(pm.indices.end(), {a, c, b, a, d, c});
        }
    }

    if (!skirt) return pm;

    // Perimeter ring in order, then a dropped bottom vertex per perimeter
    // vertex. The skirt is a wall around the chunk; the runtime material is
    // doubleSided so the wall renders regardless of winding.
    const auto pushVert = [&](const Vertex3D& v) {
        pm.verts.push_back(v);
        return static_cast<uint16_t>(pm.verts.size() - 1);
    };
    // Walk the four edges clockwise and emit bottom verts + quads.
    auto addRingEdge = [&](std::vector<std::pair<int, int>> pts) {
        for (size_t k = 0; k + 1 < pts.size(); ++k) {
            const int i0 = pts[k].first, j0 = pts[k].second;
            const int i1 = pts[k + 1].first, j1 = pts[k + 1].second;
            const Vertex3D& t0 = pm.verts[gridIdx(i0, j0)];
            const Vertex3D& t1 = pm.verts[gridIdx(i1, j1)];
            Vertex3D b0 = t0;
            Vertex3D b1 = t1;
            b0.pos.y -= skirtDrop;
            b1.pos.y -= skirtDrop;
            const uint16_t b0i = pushVert(b0);
            const uint16_t b1i = pushVert(b1);
            const uint16_t t0i = gridIdx(i0, j0);
            const uint16_t t1i = gridIdx(i1, j1);
            pm.indices.insert(pm.indices.end(), {t0i, t1i, b1i, t0i, b1i, b0i});
        }
    };
    std::vector<std::pair<int, int>> edge;
    for (int i = 0; i < cells; ++i) edge.push_back({i, 0});             // z-min edge
    for (int j = 0; j < cells; ++j) edge.push_back({cells, j});         // x-max edge
    for (int i = cells; i > 0; --i) edge.push_back({i, cells});         // z-max edge
    for (int j = cells; j > 0; --j) edge.push_back({0, j});             // x-min edge
    addRingEdge(edge);
    return pm;
}

} // namespace

math::Vec4 TerrainLayerColor(float height, float heightScale, float slope,
                             const TerrainLayerConfig& config) {
    const float h = height / std::max(heightScale, 1e-4f);
    const float tDirt =
        math::SmoothStep(config.grassMaxHeight - config.blendWidth,
                         config.grassMaxHeight + config.blendWidth, h);
    const float tRock =
        math::SmoothStep(config.dirtMaxHeight - config.blendWidth,
                         config.dirtMaxHeight + config.blendWidth, h);
    math::Vec4 color = math::Lerp(config.grass, config.dirt, tDirt);
    color = math::Lerp(color, config.rock, tRock);
    const float tSlope =
        math::SmoothStep(config.rockSlope, config.rockSlope + config.slopeBlend, slope);
    color = math::Lerp(color, config.rock, tSlope);
    color.w = 1.0f;
    return color;
}

math::Vec4 TerrainLayerWeights(float height, float heightScale, float slope,
                               const TerrainLayerConfig& config) {
    const float h = height / std::max(heightScale, 1e-4f);
    const float tDirt =
        math::SmoothStep(config.grassMaxHeight - config.blendWidth,
                         config.grassMaxHeight + config.blendWidth, h);
    const float tRock =
        math::SmoothStep(config.dirtMaxHeight - config.blendWidth,
                         config.dirtMaxHeight + config.blendWidth, h);
    const float tSlope =
        math::SmoothStep(config.rockSlope, config.rockSlope + config.slopeBlend, slope);
    // Slope pulls weight toward rock; otherwise grass->dirt->rock by height.
    float wR = tDirt;                        // rock from the height ramp
    wR = std::max(wR, tSlope);
    float wDirt = tDirt * (1.0f - tRock);    // dirt band between grass and rock
    float wGrass = 1.0f - wDirt - wR;
    wGrass = math::Clamp(wGrass, 0.0f, 1.0f);
    return {wGrass, wDirt, math::Clamp(1.0f - wGrass - wDirt, 0.0f, 1.0f), 0.0f};
}

float SampleTerrainHeight(const std::vector<float>& heights, int segments, float size,
                          float x, float z) {
    return SampleRaw(heights, segments, size, x, z);  // raw; caller scales
}

float TerrainSlope(const std::vector<float>& heights, int segments, float size, float x,
                   float z) {
    // A slope measure independent of heightScale (gradient normalized).
    return SlopeAt(heights, segments, size, x, z, 1.0f);
}

std::vector<TerrainChunkMesh> BuildTerrainLODChunks(Renderer& renderer,
                                                    const std::vector<float>& heights,
                                                    int segments, float size, float heightScale,
                                                    int gridDiv, int lodLevels, int baseSubdiv,
                                                    const TerrainLayerConfig& layer) {
    std::vector<TerrainChunkMesh> chunks;
    if (heights.empty() || segments < 1 || gridDiv < 1) return chunks;
    gridDiv = math::IClamp(gridDiv, 1, 16);
    chunks.reserve(static_cast<size_t>(gridDiv) * gridDiv);
    for (int gz = 0; gz < gridDiv; ++gz) {
        for (int gx = 0; gx < gridDiv; ++gx) {
            chunks.push_back(BuildTerrainChunk(renderer, heights, segments, size, heightScale,
                                               gridDiv, gx, gz, lodLevels, baseSubdiv, layer));
        }
    }
    return chunks;
}

TerrainChunkMesh BuildTerrainChunk(Renderer& renderer, const std::vector<float>& heights,
                                   int segments, float size, float heightScale, int gridDiv,
                                   int gridX, int gridZ, int lodLevels, int baseSubdiv,
                                   const TerrainLayerConfig& layer) {
    TerrainChunkMesh chunk;
    if (heights.empty() || segments < 1 || gridDiv < 1) return chunk;
    gridDiv = math::IClamp(gridDiv, 1, 16);
    lodLevels = math::IClamp(lodLevels, 1, 5);
    baseSubdiv = math::IClamp(baseSubdiv, 2, 64);
    gridX = math::IClamp(gridX, 0, gridDiv - 1);
    gridZ = math::IClamp(gridZ, 0, gridDiv - 1);
    const float half = size * 0.5f;
    const float chunkSize = size / static_cast<float>(gridDiv);
    const float chunkHalf = chunkSize * 0.5f;
    const float skirtDrop = std::max(heightScale * 0.25f, 0.05f);
    const float xMin = -half + gridX * chunkSize;
    const float xMax = xMin + chunkSize;
    const float zMin = -half + gridZ * chunkSize;
    const float zMax = zMin + chunkSize;
    chunk.offset = {(xMin + xMax) * 0.5f, (zMin + zMax) * 0.5f};
    chunk.halfSize = chunkHalf;
    for (int L = 0; L < lodLevels; ++L) {
        const int cells = std::max(1, baseSubdiv >> L);
        PlaneMesh pm = BuildChunkSurface(heights, segments, size, heightScale, xMin, xMax,
                                         zMin, zMax, cells, layer, /*skirt=*/true, skirtDrop);
        Mesh m = Mesh::CreateFromData(renderer, pm.verts.data(),
                                      static_cast<uint32_t>(pm.verts.size()),
                                      pm.indices.data(), static_cast<uint32_t>(pm.indices.size()),
                                      "terrain_chunk");
        chunk.chain.levels.push_back(m);
        // PickLod expects levels.size()-1 swap thresholds (the last level needs
        // no swap). Strictly increasing; the coarse step is plenty for a patch.
        if (L + 1 < lodLevels)
            chunk.chain.thresholds.push_back(40.0f + static_cast<float>(L) * 60.0f);
    }
    return chunk;
}

std::vector<math::Vec3> ScatterVegetation(const std::vector<float>& heights, int segments,
                                          float size, float heightScale,
                                          const VegetationConfig& config, core::Rng& rng) {
    std::vector<math::Vec3> result;
    if (heights.empty() || segments < 1 || config.count == 0) return result;
    result.reserve(config.count);
    const float half = size * 0.5f;
    for (uint32_t i = 0; i < config.count; ++i) {
        const float x = rng.Range(-half, half);
        const float z = rng.Range(-half, half);
        const float y = SampleRaw(heights, segments, size, x, z) * heightScale;
        const float slope = SlopeAt(heights, segments, size, x, z, heightScale);
        if (y < config.minHeight || y > config.maxHeight || slope > config.maxSlope) continue;
        result.push_back({x, y, z});
    }
    return result;
}

Mesh MakeImpostorQuad(Renderer& renderer, float width, float height,
                      const math::Vec4& color, const std::string& name) {
    std::vector<Vertex3D> verts;
    std::vector<uint16_t> indices;
    const float hw = width * 0.5f;
    const math::Vec3 n{0.0f, 0.0f, 1.0f};
    // Ground-anchored: bottom at y=0, top at y=height, facing +Z.
    verts.push_back({{-hw, 0.0f, 0.0f}, n, {0.0f, 0.0f}, color});
    verts.push_back({{hw, 0.0f, 0.0f}, n, {1.0f, 0.0f}, color});
    verts.push_back({{hw, height, 0.0f}, n, {1.0f, 1.0f}, color});
    verts.push_back({{-hw, height, 0.0f}, n, {0.0f, 1.0f}, color});
    indices = {0, 2, 1, 0, 3, 2};
    return Mesh::CreateFromData(renderer, verts.data(), static_cast<uint32_t>(verts.size()),
                                indices.data(), static_cast<uint32_t>(indices.size()), name);
}

} // namespace neon::gfx
