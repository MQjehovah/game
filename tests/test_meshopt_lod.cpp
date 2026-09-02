#include "neon/neon.hpp"
#include "neon/gfx/renderer.hpp"
#include "neon/gfx/mesh.hpp"
#include "neon/assets/asset_manager.hpp"
#include "helpers.hpp"
#include "test_backend.hpp"
using namespace neon;
TEST(MeshoptLodSimplify) {
    test::HeadlessAssetFixture fx;
    auto g = fx.assets.LoadGLTF("projects/forest/assets/models/island_tree_01/island_tree_01.gltf");
    if (g.nodes.empty()) { CHECK(false); return; }
    size_t bigVert = 0; size_t bigIdx = 0; const gfx::Mesh* big = nullptr;
    for (const auto& n : g.nodes) {
        const size_t v = n.mesh.CpuVerts().size();
        const size_t i = !n.mesh.CpuIndicesU32().empty() ? n.mesh.CpuIndicesU32().size()
                                                          : n.mesh.CpuIndices().size();
        if (v > bigVert) { bigVert = v; bigIdx = i; big = &n.mesh; }
    }
    printf("island_tree 鏈€澶?primitive: verts=%zu idx=%zu; nodes=%zu\n", bigVert, bigIdx,
           g.nodes.size());
    CHECK(bigVert > 100000);
    if (bigVert <= 100000) return;
    gfx::Mesh lod = gfx::Mesh::CreateSimplifyLod(fx.renderer, *big, 50000, "tree_lod");
    CHECK(lod.Valid());
    if (!lod.Valid()) return;
    const size_t lv = lod.CpuVerts().size();
    printf("lod: verts=%zu (%.2f%% of %zu)\n", lv,
           100.0 * static_cast<double>(lv) / static_cast<double>(bigVert), bigVert);
    CHECK(lv > 10000);
    CHECK(lv < bigVert / 5);
}
