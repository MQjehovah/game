#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include "neon/neon.hpp"
#include "helpers.hpp"
#include "test_backend.hpp"

using namespace neon;

// ---------------------------------------------------------------------------
// assets::AssetManager::LoadGLTF
// ---------------------------------------------------------------------------

namespace {

void AppendBytes(std::vector<uint8_t>& out, const void* data, size_t n) {
    const uint8_t* p = static_cast<const uint8_t*>(data);
    out.insert(out.end(), p, p + n);
}

void AppendF32(std::vector<uint8_t>& out, float v) { AppendBytes(out, &v, sizeof(v)); }
void AppendU16(std::vector<uint8_t>& out, uint16_t v) { AppendBytes(out, &v, sizeof(v)); }
void AppendU32(std::vector<uint8_t>& out, uint32_t v) { AppendBytes(out, &v, sizeof(v)); }

// 3 interleaved vertices, 32-byte stride:
//   pos(3f) + normal(3f) + pad(2f).
// Vertex 0 (0,0,0), vertex 1 (1,0,0), vertex 2 (0,1,0), all normal +Z.
std::vector<uint8_t> InterleavedVerts() {
    std::vector<uint8_t> out;
    const float kPos[3][3] = {{0, 0, 0}, {1, 0, 0}, {0, 1, 0}};
    for (int i = 0; i < 3; ++i) {
        AppendF32(out, kPos[i][0]);
        AppendF32(out, kPos[i][1]);
        AppendF32(out, kPos[i][2]);
        AppendF32(out, 0.0f);
        AppendF32(out, 0.0f);
        AppendF32(out, 1.0f);
        AppendF32(out, 0.0f); // padding
        AppendF32(out, 0.0f);
    }
    return out; // 96 bytes
}

const char* kGltfInterleaved = R"({
  "asset": {"version": "2.0"},
  "scene": 0,
  "scenes": [{"nodes": [0]}],
  "nodes": [
    {"mesh": 0, "translation": [1, 2, 3],
     "rotation": [0, 0, 0.70710678, 0.70710678], "scale": [2, 2, 2]}
  ],
  "meshes": [
    {"primitives": [{"attributes": {"POSITION": 0, "NORMAL": 1}, "indices": 2, "material": 0}]}
  ],
  "materials": [
    {"pbrMetallicRoughness": {
       "baseColorFactor": [0.2, 0.4, 0.6, 1.0],
       "metallicFactor": 0.5, "roughnessFactor": 0.25}}
  ],
  "accessors": [
    {"bufferView": 0, "componentType": 5126, "count": 3, "type": "VEC3"},
    {"bufferView": 0, "byteOffset": 12, "componentType": 5126, "count": 3, "type": "VEC3"},
    {"bufferView": 1, "componentType": 5123, "count": 3, "type": "SCALAR"}
  ],
  "bufferViews": [
    {"buffer": 0, "byteOffset": 0, "byteLength": 96, "byteStride": 32},
    {"buffer": 0, "byteOffset": 96, "byteLength": 6}
  ],
  "buffers": [{"byteLength": 102, "uri": "scene.bin"}]
})";

// 3 verts (interleaved) + 4 x u32 indices [7, 0, 1, 2]; the index accessor
// uses byteOffset 4 to read only [0, 1, 2] (covers UNSIGNED_INT + accessor
// offset). The node has no TRS, so its world transform is identity.
const char* kGltfUint32Indices = R"({
  "asset": {"version": "2.0"},
  "scene": 0,
  "scenes": [{"nodes": [0]}],
  "nodes": [{"mesh": 0}],
  "meshes": [
    {"primitives": [{"attributes": {"POSITION": 0, "NORMAL": 1}, "indices": 2, "material": 0}]}
  ],
  "materials": [
    {"pbrMetallicRoughness": {"baseColorFactor": [0.9, 0.1, 0.1, 1.0]}}
  ],
  "accessors": [
    {"bufferView": 0, "componentType": 5126, "count": 3, "type": "VEC3"},
    {"bufferView": 0, "componentType": 5126, "count": 3, "type": "VEC3"},
    {"bufferView": 1, "componentType": 5125, "count": 3, "type": "SCALAR", "byteOffset": 4}
  ],
  "bufferViews": [
    {"buffer": 0, "byteOffset": 0, "byteLength": 96, "byteStride": 32},
    {"buffer": 0, "byteOffset": 96, "byteLength": 16}
  ],
  "buffers": [{"byteLength": 112, "uri": "scene.bin"}]
})";

// Parent transform node (no mesh, just a translation) with a mesh child.
// A real scene graph commonly has such hierarchy nodes.
const char* kGltfParentTransform = R"({
  "asset": {"version": "2.0"},
  "scene": 0,
  "scenes": [{"nodes": [0]}],
  "nodes": [
    {"translation": [5, 0, 0], "children": [1]},
    {"mesh": 0}
  ],
  "meshes": [
    {"primitives": [{"attributes": {"POSITION": 0, "NORMAL": 1}, "indices": 2, "material": 0}]}
  ],
  "materials": [{}],
  "accessors": [
    {"bufferView": 0, "componentType": 5126, "count": 3, "type": "VEC3"},
    {"bufferView": 0, "componentType": 5126, "count": 3, "type": "VEC3"},
    {"bufferView": 1, "componentType": 5123, "count": 3, "type": "SCALAR"}
  ],
  "bufferViews": [
    {"buffer": 0, "byteOffset": 0, "byteLength": 96, "byteStride": 32},
    {"buffer": 0, "byteOffset": 96, "byteLength": 6}
  ],
  "buffers": [{"byteLength": 102, "uri": "scene.bin"}]
})";

} // namespace

// Hand-written glTF with an interleaved byteStride bufferView, 16-bit indices
// and a TRS node. Verifies accessor reads honor byteStride, material factors
// and the T*R*S node composition.
TEST(GltfInterleavedByteStrideTrs) {
    test::TempDir tmp;
    std::vector<uint8_t> bin = InterleavedVerts();
    AppendU16(bin, 0);
    AppendU16(bin, 1);
    AppendU16(bin, 2);
    CHECK(test::WriteFileAll(tmp.Str() + "/scene.bin", bin.data(), bin.size()));
    CHECK(test::WriteFileAll(tmp.Str() + "/scene.gltf", std::string(kGltfInterleaved)));

    test::HeadlessAssetFixture fix;
    assets::GltfAsset asset = fix.assets.LoadGLTF(tmp.Str() + "/scene.gltf");
    CHECK(asset.Valid());
    CHECK_EQ(asset.nodes.size(), 1u);
    if (asset.nodes.size() != 1u) return;

    const gfx::Mesh& mesh = asset.nodes[0].mesh;
    CHECK(mesh.Valid());
    CHECK_EQ(mesh.CpuVerts().size(), 3u);
    CHECK_EQ(mesh.CpuIndices().size(), 3u);
    CHECK_EQ(mesh.TriangleCount(), 1u);

    // byteStride 32 plus accessor byteOffset 12 must be honored: a tight
    // 12-byte stride or a zero accessor offset would misread these values.
    CHECK_NEAR(mesh.CpuVerts()[0].pos.x, 0.0, 1e-5);
    CHECK_NEAR(mesh.CpuVerts()[1].pos.x, 1.0, 1e-5);
    CHECK_NEAR(mesh.CpuVerts()[1].pos.y, 0.0, 1e-5);
    CHECK_NEAR(mesh.CpuVerts()[2].pos.y, 1.0, 1e-5);
    CHECK_NEAR(mesh.CpuVerts()[0].normal.z, 1.0, 1e-5);
    CHECK_EQ(mesh.CpuIndices()[0], 0u);
    CHECK_EQ(mesh.CpuIndices()[1], 1u);
    CHECK_EQ(mesh.CpuIndices()[2], 2u);

    // PBR factors.
    CHECK_NEAR(asset.nodes[0].material.tint.r, 0.2, 1e-5);
    CHECK_NEAR(asset.nodes[0].material.tint.g, 0.4, 1e-5);
    CHECK_NEAR(asset.nodes[0].material.tint.b, 0.6, 1e-5);
    CHECK_NEAR(asset.nodes[0].material.tint.a, 1.0, 1e-5);
    CHECK_NEAR(asset.nodes[0].material.metallic, 0.5, 1e-5);
    CHECK_NEAR(asset.nodes[0].material.roughness, 0.25, 1e-5);

    // TRS = Translation * RotationZ(90) * Scale(2) applied in that order.
    const math::Mat4& m = asset.nodes[0].transform;
    CHECK_NEAR(m.TransformPoint({1, 0, 0}).x, 1.0, 1e-4);
    CHECK_NEAR(m.TransformPoint({1, 0, 0}).y, 4.0, 1e-4);
    CHECK_NEAR(m.TransformPoint({1, 0, 0}).z, 3.0, 1e-4);
    CHECK_NEAR(m.TransformPoint({0, 1, 0}).x, -1.0, 1e-4);
    CHECK_NEAR(m.TransformPoint({0, 1, 0}).y, 2.0, 1e-4);
    CHECK_NEAR(m.TransformPoint({0, 0, 1}).z, 5.0, 1e-4);
    CHECK_NEAR(m.TransformDir({1, 0, 0}).x, 0.0, 1e-4);
    CHECK_NEAR(m.TransformDir({1, 0, 0}).y, 2.0, 1e-4);
}

// UNSIGNED_INT indices plus a nonzero accessor byteOffset (base = bufferView
// offset + accessor offset). Node without TRS -> identity transform.
TEST(GltfUint32IndicesWithAccessorOffset) {
    test::TempDir tmp;
    std::vector<uint8_t> bin = InterleavedVerts();
    AppendU32(bin, 7); // junk before the accessor window
    AppendU32(bin, 0);
    AppendU32(bin, 1);
    AppendU32(bin, 2);
    CHECK(test::WriteFileAll(tmp.Str() + "/scene.bin", bin.data(), bin.size()));
    CHECK(test::WriteFileAll(tmp.Str() + "/scene.gltf", std::string(kGltfUint32Indices)));

    test::HeadlessAssetFixture fix;
    assets::GltfAsset asset = fix.assets.LoadGLTF(tmp.Str() + "/scene.gltf");
    CHECK(asset.Valid());
    CHECK_EQ(asset.nodes.size(), 1u);
    if (asset.nodes.size() != 1u) return;

    const gfx::Mesh& mesh = asset.nodes[0].mesh;
    CHECK(mesh.Valid());
    CHECK_EQ(mesh.CpuVerts().size(), 3u);
    CHECK_EQ(mesh.CpuIndices().size(), 3u);
    CHECK_EQ(mesh.CpuIndices()[0], 0u);
    CHECK_EQ(mesh.CpuIndices()[1], 1u);
    CHECK_EQ(mesh.CpuIndices()[2], 2u);

    // Identity node transform, but baseColorFactor applied.
    CHECK_NEAR(asset.nodes[0].material.tint.r, 0.9, 1e-5);
    CHECK_NEAR(asset.nodes[0].material.tint.g, 0.1, 1e-5);
    math::Vec3 p = asset.nodes[0].transform.TransformPoint({1, 0, 0});
    CHECK_NEAR(p.x, 1.0, 1e-6);
    CHECK_NEAR(p.y, 0.0, 1e-6);
    CHECK_NEAR(p.z, 0.0, 1e-6);
}

// A transform-only parent node (no mesh) with a mesh child: world transforms
// must accumulate down the hierarchy and the parent must not crash the loader.
TEST(GltfTransformParentNodes) {
    test::TempDir tmp;
    std::vector<uint8_t> bin = InterleavedVerts();
    AppendU16(bin, 0);
    AppendU16(bin, 1);
    AppendU16(bin, 2);
    CHECK(test::WriteFileAll(tmp.Str() + "/scene.bin", bin.data(), bin.size()));
    CHECK(test::WriteFileAll(tmp.Str() + "/scene.gltf", std::string(kGltfParentTransform)));

    test::HeadlessAssetFixture fix;
    assets::GltfAsset asset = fix.assets.LoadGLTF(tmp.Str() + "/scene.gltf");
    CHECK(asset.Valid());
    CHECK_EQ(asset.nodes.size(), 1u); // only the mesh node is emitted
    if (asset.nodes.size() != 1u) return;

    const math::Mat4& m = asset.nodes[0].transform;
    CHECK_NEAR(m.TransformPoint({1, 0, 0}).x, 6.0, 1e-4); // parent translation applied
    CHECK_NEAR(m.TransformPoint({1, 0, 0}).y, 0.0, 1e-4);
    CHECK_NEAR(m.TransformDir({1, 0, 0}).x, 1.0, 1e-4); // child has no rotation/scale
}

// Khronos DamagedHelmet sample: binary buffers, PBR metallic-roughness
// material with four textures, 16-bit indices. End-to-end real asset load.
TEST(GltfDamagedHelmetEndToEnd) {
    const char* helmet = "projects/default/assets/models/DamagedHelmet/DamagedHelmet.gltf";
    test::HeadlessAssetFixture fix;
    assets::GltfAsset asset = fix.assets.LoadGLTF(helmet);
    CHECK(asset.Valid());
    CHECK_EQ(asset.nodes.size(), 1u);
    if (asset.nodes.size() != 1u) return;

    const gfx::Mesh& mesh = asset.nodes[0].mesh;
    CHECK(mesh.Valid());
    CHECK_EQ(mesh.CpuVerts().size(), 14556u);
    CHECK_EQ(mesh.CpuIndices().size(), 46356u);
    CHECK_EQ(mesh.TriangleCount(), 15452u);

    // Mesh bounds must match the POSITION accessor min/max from the file.
    CHECK_NEAR(mesh.Bounds().min.x, -0.9474585652351379, 1e-4);
    CHECK_NEAR(mesh.Bounds().min.y, -1.18715500831604, 1e-4);
    CHECK_NEAR(mesh.Bounds().min.z, -0.9009949564933777, 1e-4);
    CHECK_NEAR(mesh.Bounds().max.x, 0.9424954056739807, 1e-4);
    CHECK_NEAR(mesh.Bounds().max.y, 0.8128451108932495, 1e-4);
    CHECK_NEAR(mesh.Bounds().max.z, 0.900973916053772, 1e-4);

    // PBR material: baseColor/MR/AO/emissive textures, default factors
    // (the file defines no metallicFactor/roughnessFactor).
    const gfx::Material& mat = asset.nodes[0].material;
    CHECK(mat.albedo.Valid());
    CHECK(mat.metallicRoughness.Valid());
    CHECK(mat.occlusion.Valid());
    CHECK(mat.emissive.Valid());
    CHECK_NEAR(mat.metallic, 0.0, 1e-6);
    CHECK_NEAR(mat.roughness, 1.0, 1e-6);
    CHECK_NEAR(mat.tint.r, 1.0, 1e-6);

    // Sampler-driven wrapping: the file's samplers are empty (glTF default =
    // REPEAT) and its UVs run v in [1,2], so every texture must be requested
    // with Repeat wrapping - not the engine's default CLAMP_TO_EDGE, which
    // would collapse the whole model onto one texel row.
    {
        neon::assets::TextureLoadOptions repeatOpts;
        repeatOpts.wrap = neon::gfx::Wrap::Repeat;
        const std::string albedoKey = assets::AssetManager::TextureCacheKey(
            "projects/default/assets/models/DamagedHelmet/Default_albedo.jpg", repeatOpts);
        CHECK_EQ(fix.assets.Textures().count(albedoKey), 1u);
        const std::string mrKey = assets::AssetManager::TextureCacheKey(
            "projects/default/assets/models/DamagedHelmet/Default_metalRoughness.jpg", repeatOpts);
        CHECK_EQ(fix.assets.Textures().count(mrKey), 1u);
    }

    // Node rotation = 90 degrees about +X (quat [0.7071, 0, -0, 0.7071]).
    math::Vec3 y = asset.nodes[0].transform.TransformDir({0, 1, 0});
    CHECK_NEAR(y.x, 0.0, 1e-4);
    CHECK_NEAR(y.y, 0.0, 1e-4);
    CHECK_NEAR(y.z, 1.0, 1e-4);
    math::Vec3 z = asset.nodes[0].transform.TransformDir({0, 0, 1});
    CHECK_NEAR(z.y, -1.0, 1e-4);
}
