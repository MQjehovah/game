#include "neon/neon.hpp"
#include "neon/gfx/renderer.hpp"
#include "neon/gfx/mesh.hpp"
#include "neon/assets/asset_manager.hpp"
#include "helpers.hpp"
#include "test_backend.hpp"
#include <cmath>
using namespace neon;
TEST(MeshoptLodSimplify) {
    test::HeadlessAssetFixture fx;
    // A dense grid mesh (u32 indices) gives a large mesh without an external
    // binary asset (the original test used a 58MB glTF that was never committed,
    // so it failed on fresh checkouts). ~430x430 grid => >180k vertices.
    const int segs = 430;
    const int cols = segs + 1;
    std::vector<gfx::Vertex3D> verts;
    verts.reserve(static_cast<size_t>(cols) * cols);
    for (int r = 0; r < cols; ++r) {
        for (int c = 0; c < cols; ++c) {
            const float x = static_cast<float>(c) * 0.1f;
            const float y = 0.0f;
            const float z = static_cast<float>(r) * 0.1f;
            // Ridged height so the simplification has geometry to preserve.
            const float h = std::sin(x * 3.0f) * std::cos(z * 3.0f) * 0.5f;
            verts.push_back({{x, h, z}, {0.0f, 1.0f, 0.0f}, {0, 1}, {1, 1, 1, 1}});
        }
    }
    std::vector<uint32_t> indices;
    indices.reserve(static_cast<size_t>(segs) * segs * 6);
    for (int r = 0; r < segs; ++r) {
        for (int c = 0; c < segs; ++c) {
            const uint32_t i0 = static_cast<uint32_t>(r) * cols + c;
            const uint32_t i1 = i0 + 1;
            const uint32_t i2 = i0 + cols;
            const uint32_t i3 = i2 + 1;
            indices.insert(indices.end(), {i0, i2, i1, i1, i2, i3});
        }
    }
    gfx::Mesh big =
        gfx::Mesh::CreateFromDataU32(fx.renderer, verts.data(), static_cast<uint32_t>(verts.size()),
                                     indices.data(), static_cast<uint32_t>(indices.size()), "lod_src");
    CHECK(big.Valid());
    if (!big.Valid()) return;
    const size_t bigVert = big.CpuVerts().size();
    printf("lod_src: verts=%zu idx=%zu\n", bigVert, big.CpuIndicesU32().size());
    CHECK(bigVert > 100000);

    gfx::Mesh lod = gfx::Mesh::CreateSimplifyLod(fx.renderer, big, 50000, "tree_lod");
    CHECK(lod.Valid());
    if (!lod.Valid()) return;
    const size_t lv = lod.CpuVerts().size();
    printf("lod: verts=%zu (%.2f%% of %zu)\n", lv,
           100.0 * static_cast<double>(lv) / static_cast<double>(bigVert), bigVert);
    CHECK(lv > 10000);
    CHECK(lv < bigVert / 5);
}
