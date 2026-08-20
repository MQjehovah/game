#include <cstdint>
#include <string>
#include <vector>

#include "neon/neon.hpp"
#include "helpers.hpp"
#include "test_backend.hpp"

using namespace neon;

// ---------------------------------------------------------------------------
// assets::AssetManager::LoadGLTF skinned meshes (Task 3.1)
// ---------------------------------------------------------------------------

namespace {

void AppendBytes(std::vector<uint8_t>& out, const void* data, size_t n) {
    const uint8_t* p = static_cast<const uint8_t*>(data);
    out.insert(out.end(), p, p + n);
}

void AppendF32(std::vector<uint8_t>& out, float v) { AppendBytes(out, &v, sizeof(v)); }
void AppendU16(std::vector<uint8_t>& out, uint16_t v) { AppendBytes(out, &v, sizeof(v)); }

// Layout (242 bytes total):
//   0    3 x VEC3 positions
//   36   3 x VEC4 JOINTS_0 (u16)
//   60   3 x VEC4 WEIGHTS_0 (f32)
//   108  3 x SCALAR u16 indices
//   114  2 x MAT4 inverseBindMatrices (f32)
std::vector<uint8_t> SkinnedBin() {
    std::vector<uint8_t> out;
    const float kPos[3][3] = {{0, 0, 0}, {1, 0, 0}, {0, 1, 0}};
    for (int i = 0; i < 3; ++i)
        for (int c = 0; c < 3; ++c) AppendF32(out, kPos[i][c]);
    const uint16_t kJoints[3][4] = {{0, 1, 0, 1}, {1, 0, 1, 0}, {0, 0, 1, 1}};
    for (int i = 0; i < 3; ++i)
        for (int c = 0; c < 4; ++c) AppendU16(out, kJoints[i][c]);
    const float kWeights[3][4] = {{0.5f, 0.5f, 0, 0}, {1, 0, 0, 0}, {0.25f, 0.25f, 0.25f, 0.25f}};
    for (int i = 0; i < 3; ++i)
        for (int c = 0; c < 4; ++c) AppendF32(out, kWeights[i][c]);
    AppendU16(out, 0);
    AppendU16(out, 1);
    AppendU16(out, 2);
    const float kMat0[16] = {2, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
    const float kMat1[16] = {3, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
    AppendBytes(out, kMat0, sizeof(kMat0));
    AppendBytes(out, kMat1, sizeof(kMat1));
    return out;
}

// Skinned fixture: node 0 has mesh 0 + skin 0; nodes 1,2 are the joints.
// JOINTS_0/WEIGHTS_0 live on the mesh primitive; inverseBindMatrices in accessor 4.
const char* kGltfSkinned = R"({
  "asset": {"version": "2.0"},
  "scene": 0,
  "scenes": [{"nodes": [0]}],
  "nodes": [
    {"skin": 0, "mesh": 0},
    {},
    {}
  ],
  "skins": [{"joints": [1, 2], "inverseBindMatrices": 4}],
  "meshes": [
    {"primitives": [{"attributes": {"POSITION": 0, "JOINTS_0": 1, "WEIGHTS_0": 2}, "indices": 3}]}
  ],
  "accessors": [
    {"bufferView": 0, "componentType": 5126, "count": 3, "type": "VEC3"},
    {"bufferView": 1, "componentType": 5123, "count": 3, "type": "VEC4"},
    {"bufferView": 2, "componentType": 5126, "count": 3, "type": "VEC4"},
    {"bufferView": 3, "componentType": 5123, "count": 3, "type": "SCALAR"},
    {"bufferView": 4, "componentType": 5126, "count": 2, "type": "MAT4"}
  ],
  "bufferViews": [
    {"buffer": 0, "byteOffset": 0, "byteLength": 36},
    {"buffer": 0, "byteOffset": 36, "byteLength": 24},
    {"buffer": 0, "byteOffset": 60, "byteLength": 48},
    {"buffer": 0, "byteOffset": 108, "byteLength": 6},
    {"buffer": 0, "byteOffset": 114, "byteLength": 128}
  ],
  "buffers": [{"byteLength": 242, "uri": "scene.bin"}]
})";

// Same fixture but the skin entry is missing inverseBindMatrices.
const char* kGltfSkinnedMalformed = R"({
  "asset": {"version": "2.0"},
  "scene": 0,
  "scenes": [{"nodes": [0]}],
  "nodes": [
    {"skin": 0, "mesh": 0},
    {},
    {}
  ],
  "skins": [{"joints": [1, 2]}],
  "meshes": [
    {"primitives": [{"attributes": {"POSITION": 0, "JOINTS_0": 1, "WEIGHTS_0": 2}, "indices": 3}]}
  ],
  "accessors": [
    {"bufferView": 0, "componentType": 5126, "count": 3, "type": "VEC3"},
    {"bufferView": 1, "componentType": 5123, "count": 3, "type": "VEC4"},
    {"bufferView": 2, "componentType": 5126, "count": 3, "type": "VEC4"},
    {"bufferView": 3, "componentType": 5123, "count": 3, "type": "SCALAR"},
    {"bufferView": 4, "componentType": 5126, "count": 2, "type": "MAT4"}
  ],
  "bufferViews": [
    {"buffer": 0, "byteOffset": 0, "byteLength": 36},
    {"buffer": 0, "byteOffset": 36, "byteLength": 24},
    {"buffer": 0, "byteOffset": 60, "byteLength": 48},
    {"buffer": 0, "byteOffset": 108, "byteLength": 6},
    {"buffer": 0, "byteOffset": 114, "byteLength": 128}
  ],
  "buffers": [{"byteLength": 242, "uri": "scene.bin"}]
})";

// Static (non-skinned) triangle: 3 VEC3 positions + 3 u16 indices (42 bytes).
std::vector<uint8_t> StaticBin() {
    std::vector<uint8_t> out;
    const float kPos[3][3] = {{0, 0, 0}, {1, 0, 0}, {0, 1, 0}};
    for (int i = 0; i < 3; ++i)
        for (int c = 0; c < 3; ++c) AppendF32(out, kPos[i][c]);
    AppendU16(out, 0);
    AppendU16(out, 1);
    AppendU16(out, 2);
    return out;
}

const char* kGltfStatic = R"({
  "asset": {"version": "2.0"},
  "scene": 0,
  "scenes": [{"nodes": [0]}],
  "nodes": [{"mesh": 0}],
  "meshes": [
    {"primitives": [{"attributes": {"POSITION": 0}, "indices": 1}]}
  ],
  "accessors": [
    {"bufferView": 0, "componentType": 5126, "count": 3, "type": "VEC3"},
    {"bufferView": 1, "componentType": 5123, "count": 3, "type": "SCALAR"}
  ],
  "bufferViews": [
    {"buffer": 0, "byteOffset": 0, "byteLength": 36},
    {"buffer": 0, "byteOffset": 36, "byteLength": 6}
  ],
  "buffers": [{"byteLength": 42, "uri": "scene.bin"}]
})";

} // namespace

// JOINTS_0/WEIGHTS_0 are extracted per vertex (4 components each) and the mesh
// is flagged skinned with the skin index the node referenced.
TEST(GltfSkinJointsWeights) {
    test::TempDir tmp;
    std::vector<uint8_t> bin = SkinnedBin();
    CHECK(test::WriteFileAll(tmp.Str() + "/scene.bin", bin.data(), bin.size()));
    CHECK(test::WriteFileAll(tmp.Str() + "/scene.gltf", std::string(kGltfSkinned)));

    test::HeadlessAssetFixture fix;
    assets::GltfAsset asset = fix.assets.LoadGLTF(tmp.Str() + "/scene.gltf");
    CHECK(asset.Valid());
    CHECK_EQ(asset.nodes.size(), 1u);
    if (asset.nodes.size() != 1u) return;

    const gfx::Mesh& mesh = asset.nodes[0].mesh;
    CHECK(mesh.Valid());
    CHECK(mesh.Skinned());
    CHECK_EQ(mesh.SkinIndex(), 0);
    const std::vector<uint16_t>& joints = mesh.CpuJointIds();
    const std::vector<float>& weights = mesh.CpuJointWeights();
    CHECK_EQ(joints.size(), 12u);
    CHECK_EQ(weights.size(), 12u);
    if (joints.size() != 12u || weights.size() != 12u) return;

    const uint16_t kJoints[3][4] = {{0, 1, 0, 1}, {1, 0, 1, 0}, {0, 0, 1, 1}};
    const float kWeights[3][4] = {{0.5f, 0.5f, 0, 0}, {1, 0, 0, 0}, {0.25f, 0.25f, 0.25f, 0.25f}};
    for (int v = 0; v < 3; ++v) {
        for (int c = 0; c < 4; ++c) {
            CHECK_EQ(joints[static_cast<size_t>(v) * 4 + c], kJoints[v][c]);
            CHECK_NEAR(weights[static_cast<size_t>(v) * 4 + c], kWeights[v][c], 1e-6);
        }
    }

    // The base vertex pipeline is unchanged: positions/indices still load.
    CHECK_EQ(mesh.CpuVerts().size(), 3u);
    CHECK_EQ(mesh.CpuIndices().size(), 3u);
    CHECK_EQ(mesh.TriangleCount(), 1u);
}

// The skins array is parsed into GltfAsset: joint chain (node indices) and the
// inverseBindMatrices accessor (one Mat4 per joint).
TEST(GltfSkinInverseBindMatrices) {
    test::TempDir tmp;
    std::vector<uint8_t> bin = SkinnedBin();
    CHECK(test::WriteFileAll(tmp.Str() + "/scene.bin", bin.data(), bin.size()));
    CHECK(test::WriteFileAll(tmp.Str() + "/scene.gltf", std::string(kGltfSkinned)));

    test::HeadlessAssetFixture fix;
    assets::GltfAsset asset = fix.assets.LoadGLTF(tmp.Str() + "/scene.gltf");
    CHECK(asset.Valid());
    CHECK_EQ(asset.skins.size(), 1u);
    if (asset.skins.size() != 1u) return;

    const gfx::Skin& skin = asset.skins[0];
    CHECK_EQ(skin.joints.size(), 2u);
    CHECK_EQ(skin.joints[0], 1u);
    CHECK_EQ(skin.joints[1], 2u);
    CHECK_EQ(skin.inverseBind.size(), 2u);
    if (skin.inverseBind.size() != 2u) return;
    CHECK_NEAR(skin.inverseBind[0].m[0], 2.0, 1e-6);
    CHECK_NEAR(skin.inverseBind[0].m[5], 1.0, 1e-6);
    CHECK_NEAR(skin.inverseBind[1].m[0], 3.0, 1e-6);
    CHECK_NEAR(skin.inverseBind[1].m[5], 1.0, 1e-6);
}

// A static (non-skinned) glTF must load exactly as before: skinned == false,
// empty joint/weight vectors, no skins, unchanged vertex/index data.
TEST(GltfStaticMeshNotSkinned) {
    test::TempDir tmp;
    std::vector<uint8_t> bin = StaticBin();
    CHECK(test::WriteFileAll(tmp.Str() + "/scene.bin", bin.data(), bin.size()));
    CHECK(test::WriteFileAll(tmp.Str() + "/scene.gltf", std::string(kGltfStatic)));

    test::HeadlessAssetFixture fix;
    assets::GltfAsset asset = fix.assets.LoadGLTF(tmp.Str() + "/scene.gltf");
    CHECK(asset.Valid());
    CHECK_EQ(asset.nodes.size(), 1u);
    if (asset.nodes.size() != 1u) return;
    CHECK_EQ(asset.skins.size(), 0u);

    const gfx::Mesh& mesh = asset.nodes[0].mesh;
    CHECK(mesh.Valid());
    CHECK(!mesh.Skinned());
    CHECK_EQ(mesh.SkinIndex(), -1);
    CHECK(mesh.CpuJointIds().empty());
    CHECK(mesh.CpuJointWeights().empty());
    CHECK_EQ(mesh.CpuVerts().size(), 3u);
    CHECK_EQ(mesh.CpuIndices().size(), 3u);
}

// A skin whose inverseBindMatrices accessor is missing must degrade gracefully:
// the skin keeps its joints, inverseBind stays empty, no crash, and the mesh
// vertex skin data is unaffected.
TEST(GltfSkinMalformedMissingInverseBind) {
    test::TempDir tmp;
    std::vector<uint8_t> bin = SkinnedBin();
    CHECK(test::WriteFileAll(tmp.Str() + "/scene.bin", bin.data(), bin.size()));
    CHECK(test::WriteFileAll(tmp.Str() + "/scene.gltf", std::string(kGltfSkinnedMalformed)));

    test::HeadlessAssetFixture fix;
    assets::GltfAsset asset = fix.assets.LoadGLTF(tmp.Str() + "/scene.gltf");
    CHECK(asset.Valid());
    CHECK_EQ(asset.nodes.size(), 1u);
    if (asset.nodes.size() != 1u) return;
    CHECK_EQ(asset.skins.size(), 1u);
    if (asset.skins.size() != 1u) return;
    CHECK_EQ(asset.skins[0].joints.size(), 2u);
    CHECK(asset.skins[0].inverseBind.empty());

    const gfx::Mesh& mesh = asset.nodes[0].mesh;
    CHECK(mesh.Skinned());
    CHECK_EQ(mesh.CpuJointIds().size(), 12u);
    CHECK_EQ(mesh.CpuJointWeights().size(), 12u);
}
