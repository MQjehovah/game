#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "neon/core/rng.hpp"
#include "neon/gfx/mesh.hpp"
#include "neon/math/vec2.hpp"
#include "neon/math/vec3.hpp"
#include "neon/math/vec4.hpp"

namespace neon::gfx {

// G2-3 height/slope multi-layer terrain material. `height` (world Y, already
// scaled by heightScale) and `slope` (1 - normal.y; 0 = flat, 1 = vertical)
// drive a grass -> dirt -> rock blend. The defaults read as a rolling grass
// field with dirt mid-slopes and rocky high/steep ground, an improvement over
// the legacy 2-colour height ramp.
struct TerrainLayerConfig {
    math::Vec4 grass{0.35f, 0.62f, 0.30f, 1.0f};
    math::Vec4 dirt{0.50f, 0.42f, 0.28f, 1.0f};
    math::Vec4 rock{0.55f, 0.52f, 0.48f, 1.0f};
    // Snow cap (programmatic snowfield): above snowStartHeight the surface
    // fades from rock to pure snow over snowBlend world-units of height.
    // `snow` == rock (or disabled via snowBlend <= 0) turns the cap off, so a
    // grassland or desert terrain needs no extra config.
    math::Vec4 snow{0.92f, 0.94f, 0.97f, 1.0f};
    float snowStartHeight = 0.0f;   // world Y where the snow cap begins (0 = off)
    float snowBlend = 3.0f;         // height band over which rock -> snow (0 = off)
    float grassMaxHeight = 6.0f;   // world Y where grass fades to dirt
    float dirtMaxHeight = 12.0f;   // world Y where dirt fades to rock
    float rockSlope = 0.35f;       // slope (1 - normal.y) where rock starts
    float blendWidth = 2.5f;       // smoothstep band (world Y) for the height ramp
    float slopeBlend = 0.15f;      // smoothstep band for the slope ramp (0..1)
};

// Albedo colour of a terrain vertex after the layered height/slope blend.
math::Vec4 TerrainLayerColor(float height, float heightScale, float slope,
                             const TerrainLayerConfig& config = TerrainLayerConfig());

// Per-vertex splat weights (R=grass, G=dirt, B=rock) that sum to 1. This is a
// splatmap-style encoding usable later for per-pixel terrain splatting; alpha
// stays 0 (the layer ramp never modulates it).
math::Vec4 TerrainLayerWeights(float height, float heightScale, float slope,
                               const TerrainLayerConfig& config = TerrainLayerConfig());

// Bilinear heightfield sample: world (x,z) in [-size/2, size/2] -> world Y
// (already scaled). Returns 0 for an empty/out-of-range heightfield.
float SampleTerrainHeight(const std::vector<float>& heights, int segments, float size,
                          float x, float z);

// Slope (1 - normal.y) at a world (x,z) sampled from the heightfield.
float TerrainSlope(const std::vector<float>& heights, int segments, float size,
                   float x, float z);

// One chunk of a chunked-LOD terrain: a LodChain (levels[0] most detailed) plus
// the chunk's world-space centre (x,z) and half-size. The runtime draws a
// chunk per DrawItem; the camera-distance LOD selector picks the level per
// chunk, so near patches stay dense while far patches drop triangles.
struct TerrainChunkMesh {
    gfx::LodChain chain;
    math::Vec2 offset;    // world-space (x,z) centre of the chunk
    float halfSize = 0.0f;
};

// Subdivide a heightfield into gridDiv x gridDiv chunks. Each chunk builds
// `lodLevels` meshes: level 0 has `baseSubdiv` cells per side, each higher
// level halves the cell count (so far chunks render fewer triangles). A thin
// downward skirt on each chunk edge hides T-junction cracks where neighbours
// render at different LOD levels. Returns all chunks row-major.
std::vector<TerrainChunkMesh> BuildTerrainLODChunks(
    Renderer& renderer, const std::vector<float>& heights, int segments, float size,
    float heightScale, int gridDiv, int lodLevels, int baseSubdiv,
    const TerrainLayerConfig& layer = TerrainLayerConfig());

// Build a single chunk (gridX, gridZ) of a gridDiv x gridDiv chunked terrain,
// with its own LodChain of `lodLevels` levels. Used by the runtime to resolve
// one patch per draw item.
TerrainChunkMesh BuildTerrainChunk(Renderer& renderer, const std::vector<float>& heights,
                                   int segments, float size, float heightScale, int gridDiv,
                                   int gridX, int gridZ, int lodLevels, int baseSubdiv,
                                   const TerrainLayerConfig& layer = TerrainLayerConfig());

// Vegetation scatter config: deterministic plant/rock positions on flat-ish,
// height-bounded ground. `size` is the planted scale; `impostorDistance` is the
// camera distance where the caller swaps the full mesh for a billboard card.
struct VegetationConfig {
    uint32_t count = 200;
    float minHeight = 0.0f;      // world Y floor for plantable ground
    float maxHeight = 3.0f;      // world Y ceiling
    float maxSlope = 0.30f;      // slope (1 - normal.y) plants tolerate
    float size = 1.0f;
    float impostorDistance = 60.0f;
};

// World-space positions (x, ground Y, z) for vegetation that passes the
// height/slope filter, drawn deterministically from `rng`.
std::vector<math::Vec3> ScatterVegetation(const std::vector<float>& heights, int segments,
                                          float size, float heightScale,
                                          const VegetationConfig& config, core::Rng& rng);

// Ground-anchored, camera-facing (Y-yaw billboard) quad used as a distant
// vegetation impostor: a cheap two-triangle card the lit shader tints.
Mesh MakeImpostorQuad(Renderer& renderer, float width, float height,
                      const math::Vec4& color = {0.16f, 0.48f, 0.16f, 1.0f},
                      const std::string& name = "impostor");

} // namespace neon::gfx
