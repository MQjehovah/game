#include <cmath>
#include <cstdio>
#include <vector>

#include "neon/neon.hpp"
#include "neon/gfx/terrain.hpp"
#include "neon/scene/game_runtime.hpp"
#include "neon/scene/scene_file.hpp"
#include "helpers.hpp"
#include "test_backend.hpp"

using namespace neon;

// ---------------------------------------------------------------------------
// Layer blend: height/slope picks grass, dirt and rock.
// ---------------------------------------------------------------------------

TEST(TerrainLayerColorBlend) {
    const gfx::TerrainLayerConfig cfg;
    // Low, flat ground -> grass.
    math::Vec4 grass = gfx::TerrainLayerColor(0.0f, 1.0f, 0.0f, cfg);
    CHECK_NEAR(grass.x, cfg.grass.x, 1e-5);
    CHECK_NEAR(grass.y, cfg.grass.y, 1e-5);
    CHECK_NEAR(grass.z, cfg.grass.z, 1e-5);
    // High, flat ground -> rock.
    math::Vec4 rock = gfx::TerrainLayerColor(10.0f, 1.0f, 0.0f, cfg);
    CHECK_NEAR(rock.x, cfg.rock.x, 1e-5);
    CHECK_NEAR(rock.y, cfg.rock.y, 1e-5);
    CHECK_NEAR(rock.z, cfg.rock.z, 1e-5);
    // Steep slope -> rock regardless of height.
    math::Vec4 steep = gfx::TerrainLayerColor(0.0f, 1.0f, 1.0f, cfg);
    CHECK_NEAR(steep.x, cfg.rock.x, 1e-5);
    CHECK_NEAR(steep.y, cfg.rock.y, 1e-5);
    CHECK_NEAR(steep.z, cfg.rock.z, 1e-5);
    // Dirt midpoint (h=0.5): between grass and rock, distinct from both.
    math::Vec4 mid = gfx::TerrainLayerColor(0.5f, 1.0f, 0.0f, cfg);
    CHECK(mid.y < cfg.grass.y && mid.y > cfg.dirt.y);
    CHECK_NEAR(mid.w, 1.0f, 1e-6);
}

// ---------------------------------------------------------------------------
// Splat weights encode grass/dirt/rock and sum to 1.
// ---------------------------------------------------------------------------

TEST(TerrainLayerWeightsSumToOne) {
    const gfx::TerrainLayerConfig cfg;
    for (float h : {0.0f, 0.4f, 0.9f, 2.0f, 5.0f}) {
        for (float slope : {0.0f, 0.2f, 0.6f, 1.0f}) {
            math::Vec4 w = gfx::TerrainLayerWeights(h, 1.0f, slope, cfg);
            CHECK(w.x >= 0.0f && w.x <= 1.0f);
            CHECK(w.y >= 0.0f && w.y <= 1.0f);
            CHECK(w.z >= 0.0f && w.z <= 1.0f);
            CHECK_NEAR(w.x + w.y + w.z, 1.0f, 1e-3);
        }
    }
}

// ---------------------------------------------------------------------------
// Heightfield sampling: bilinear interpolation matches grid vertices and a
// straight ramp.
// ---------------------------------------------------------------------------

TEST(TerrainSampleBilinear) {
    // segments=2 -> 3x3 grid over [-2,2]; heights = x world coordinate.
    const int segments = 2;
    const float size = 4.0f;
    std::vector<float> heights(9, 0.0f);
    const float cell = size / segments; // 2
    for (int j = 0; j <= segments; ++j) {
        for (int i = 0; i <= segments; ++i) {
            const float x = -2.0f + i * cell;
            heights[static_cast<size_t>(j) * (segments + 1) + i] = x;
        }
    }
    CHECK_NEAR(gfx::SampleTerrainHeight(heights, segments, size, 0.0f, 0.0f), 0.0, 1e-5);
    CHECK_NEAR(gfx::SampleTerrainHeight(heights, segments, size, 2.0f, 0.0f), 2.0, 1e-5);
    CHECK_NEAR(gfx::SampleTerrainHeight(heights, segments, size, 1.0f, 0.0f), 1.0, 1e-5);
}

// ---------------------------------------------------------------------------
// Chunked LOD: grid subdivision, per-chunk offsets and decreasing triangle
// counts for coarser levels. Meshes are built through the headless backend,
// which records the CPU vertex/index data needed for these assertions.
// ---------------------------------------------------------------------------

TEST(TerrainChunkedLODStructure) {
    test::HeadlessAssetFixture fix;
    const int segments = 16;
    const float size = 64.0f;
    const int gridDiv = 2;
    const int lodLevels = 3;
    const int baseSubdiv = 16;
    std::vector<float> heights(static_cast<size_t>(segments + 1) * (segments + 1), 0.0f);

    auto chunks = gfx::BuildTerrainLODChunks(fix.renderer, heights, segments, size, 1.0f,
                                             gridDiv, lodLevels, baseSubdiv);
    CHECK_EQ(chunks.size(), static_cast<size_t>(gridDiv) * gridDiv);

    std::vector<uint32_t> triCount(lodLevels, 0);
    for (const auto& c : chunks) {
        CHECK_EQ(c.chain.levels.size(), static_cast<size_t>(lodLevels));
        CHECK_EQ(c.chain.thresholds.size(), static_cast<size_t>(lodLevels - 1));
        for (int L = 0; L < lodLevels; ++L) {
            CHECK(c.chain.levels[L].Valid());
            triCount[L] += c.chain.levels[L].TriangleCount();
        }
    }
    // Level 0 is the most detailed; each coarser level drops triangles.
    CHECK(triCount[0] > triCount[1]);
    CHECK(triCount[1] > triCount[2]);

    // Chunk centres: 2x2 grid over [-32,32] -> (-16,-16), (16,-16), (-16,16), (16,16).
    CHECK_NEAR(chunks[0].offset.x, -16.0f, 1e-4);
    CHECK_NEAR(chunks[0].offset.y, -16.0f, 1e-4);
    CHECK_NEAR(chunks[1].offset.x, 16.0f, 1e-4);
    CHECK_NEAR(chunks[1].offset.y, -16.0f, 1e-4);
    CHECK_NEAR(chunks[3].offset.x, 16.0f, 1e-4);
    CHECK_NEAR(chunks[3].offset.y, 16.0f, 1e-4);
    CHECK_NEAR(chunks[0].halfSize, 16.0f, 1e-4);
}

// ---------------------------------------------------------------------------
// Vegetation scatter: positions stay inside the terrain and respect the
// height/slope filter.
// ---------------------------------------------------------------------------

TEST(TerrainVegetationScatter) {
    const int segments = 8;
    const float size = 32.0f;
    std::vector<float> heights(static_cast<size_t>(segments + 1) * (segments + 1), 0.0f);
    // A steep "wall" at large z: z >= 4 has raw height 5 (world Y = 5).
    const float cell = size / segments;
    for (int j = 0; j <= segments; ++j) {
        for (int i = 0; i <= segments; ++i) {
            const float z = -16.0f + j * cell;
            heights[static_cast<size_t>(j) * (segments + 1) + i] = z >= 4.0f ? 5.0f : 0.0f;
        }
    }

    gfx::VegetationConfig cfg;
    cfg.count = 400;
    cfg.minHeight = 0.0f;
    cfg.maxHeight = 1.0f;  // only low ground qualifies; the 5.0 wall is rejected
    cfg.maxSlope = 0.5f;
    core::Rng rng(1234);
    auto pts = gfx::ScatterVegetation(heights, segments, size, 1.0f, cfg, rng);
    CHECK(!pts.empty());
    for (const auto& p : pts) {
        CHECK(p.x >= -16.0f - 1e-3f && p.x <= 16.0f + 1e-3f);
        CHECK(p.z >= -16.0f - 1e-3f && p.z <= 16.0f + 1e-3f);
        CHECK(p.y <= cfg.maxHeight + 1e-3f);
    }
}

// ---------------------------------------------------------------------------
// Impostor quad is a valid, cheap two-triangle billboard plane.
// ---------------------------------------------------------------------------

TEST(TerrainImpostorQuad) {
    test::HeadlessAssetFixture fix;
    gfx::Mesh quad = gfx::MakeImpostorQuad(fix.renderer, 2.0f, 3.0f, {0.1f, 0.5f, 0.2f, 1.0f});
    CHECK(quad.Valid());
    CHECK_EQ(quad.CpuVerts().size(), 4u);
    CHECK_EQ(quad.CpuIndices().size(), 6u);
    CHECK_EQ(quad.TriangleCount(), 2u);
    float minY = 1e9f, maxY = -1e9f;
    for (const auto& v : quad.CpuVerts()) {
        minY = std::min(minY, v.pos.y);
        maxY = std::max(maxY, v.pos.y);
    }
    CHECK_NEAR(minY, 0.0f, 1e-5);
    CHECK_NEAR(maxY, 3.0f, 1e-5);
}

// ---------------------------------------------------------------------------
// Runtime integration: a terrain with chunkGridDiv > 0 becomes grid x grid
// patch draw items (per-patch LOD), instead of one monolithic mesh.
// ---------------------------------------------------------------------------

namespace {

bool StartFlatTerrain(scene::GameRuntime& runtime, int gridDiv, int lodLevels,
                      const std::string& extra, test::HeadlessAssetFixture& fix) {
    // 5x5 flat heightfield (segments=4).
    const char* heights =
        "[0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0]";
    char buf[512];
    std::snprintf(buf, sizeof(buf),
                  R"({"entities":[{"name":"ground","components":{
                    "transform":{"pos":[0,0,0]},
                    "mesh":{"meshKey":"terrain"},
                    "terrain":{"segments":4,"size":10,"heightScale":1,"heights":%s,
                               "chunkGridDiv":%d,"chunkLodLevels":%d%s}}}]})",
                  heights, gridDiv, lodLevels, extra.c_str());
    scene::GameRuntimeConfig cfg;
    cfg.assets = &fix.assets;
    return runtime.Start(buf, cfg).Ok();
}

} // namespace

TEST(TerrainRuntimeChunkedDraw) {
    test::HeadlessAssetFixture fix;
    scene::GameRuntime runtime;
    CHECK(StartFlatTerrain(runtime, 2, 2, "", fix));
    gfx::Camera cam;
    runtime.Draw(fix.renderer, cam); // resolves + draws patch items
    CHECK_EQ(runtime.DrawCount(), 4u); // 2x2 chunks, no single-mesh item
    runtime.Stop();
}

TEST(TerrainRuntimeDefaultSingleMesh) {
    test::HeadlessAssetFixture fix;
    scene::GameRuntime runtime;
    // No chunkGridDiv -> the legacy single terrain mesh.
    CHECK(StartFlatTerrain(runtime, 0, 1, "", fix));
    gfx::Camera cam;
    runtime.Draw(fix.renderer, cam);
    CHECK_EQ(runtime.DrawCount(), 1u);
    runtime.Stop();
}

TEST(TerrainRuntimeVegetationDraw) {
    test::HeadlessAssetFixture fix;
    scene::GameRuntime runtime;
    // Chunked terrain + a bush vegetation field; Draw() must not crash and
    // must still emit the chunk draw items.
    CHECK(StartFlatTerrain(runtime, 2, 2, R"(,"vegMeshKey":"bush","vegCount":20,"vegSeed":7)", fix));
    gfx::Camera cam;
    runtime.Draw(fix.renderer, cam);
    CHECK_EQ(runtime.DrawCount(), 4u);
    runtime.Stop();
}
