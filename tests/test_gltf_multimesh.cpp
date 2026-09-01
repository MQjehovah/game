#include <cstdio>
#include <fstream>
#include <string>
#ifdef _WIN32
#define NOMINMAX
#include <windows.h>
#endif
#include "neon/neon.hpp"
#include "neon/assets/asset_manager.hpp"
#include "neon/gfx/renderer.hpp"
#include "neon/scene/systems/draw_system.hpp"
#include "helpers.hpp"
#include "test_backend.hpp"
using namespace neon;

// C15 延伸（多 mesh glTF 场景，如 Sponza）：一个 mesh 含多个 primitives 时，
// 引擎必须把每个 primitive 解析为独立的 GltfMeshNode（各自材质 + 累积变换），
// 而非只取第一个。此前只取 rawMeshes[n.mesh]（第一个 primitive）。
// 测试运行时生成最小 glTF（1 mesh / 2 primitives / 2 材质）验证。
TEST(GltfMultiPrimitiveMesh) {
    const char* dir = "tmp_gltf_test";
    const std::string binPath = std::string(dir) + "/mini.bin";
    const std::string gltfPath = std::string(dir) + "/mini.gltf";
    CreateDirectoryA(dir, nullptr);

    // 数据：2 个三角形（每 primitive 一个）。POSITION 各 3 顶点 = 72 字节，
    // indices 各 3 个 uint16 = 12 字节。
    {
        std::ofstream bin(binPath, std::ios::binary);
        float posA[9] = {0,0,0, 1,0,0, 0,1,0};
        float posB[9] = {0,0,1, 1,0,1, 0,1,1};
        bin.write(reinterpret_cast<const char*>(posA), sizeof(posA));
        bin.write(reinterpret_cast<const char*>(posB), sizeof(posB));
        uint16_t idxA[3] = {0,1,2};
        uint16_t idxB[3] = {0,1,2};
        bin.write(reinterpret_cast<const char*>(idxA), sizeof(idxA));
        bin.write(reinterpret_cast<const char*>(idxB), sizeof(idxB));
    }
    {
        std::ofstream j(gltfPath);
        j << R"({
  "asset": {"version": "2.0"},
  "scene": 0,
  "scenes": [{"nodes": [0]}],
  "nodes": [{"mesh": 0, "name": "multiPrim"}],
  "meshes": [{"primitives": [
    {"attributes": {"POSITION": 0}, "indices": 2, "material": 0},
    {"attributes": {"POSITION": 1}, "indices": 3, "material": 1}
  ]}],
  "materials": [
    {"pbrMetallicRoughness": {"baseColorFactor": [1,0,0,1]}},
    {"pbrMetallicRoughness": {"baseColorFactor": [0,1,0,1]}}
  ],
  "bufferViews": [
    {"buffer": 0, "byteOffset": 0,  "byteLength": 72, "target": 34962},
    {"buffer": 0, "byteOffset": 72, "byteLength": 12, "target": 34963}
  ],
  "accessors": [
    {"bufferView": 0, "byteOffset": 0,  "componentType": 5126, "count": 3, "type": "VEC3"},
    {"bufferView": 0, "byteOffset": 36, "componentType": 5126, "count": 3, "type": "VEC3"},
    {"bufferView": 1, "byteOffset": 0,  "componentType": 5123, "count": 3, "type": "SCALAR"},
    {"bufferView": 1, "byteOffset": 6,  "componentType": 5123, "count": 3, "type": "SCALAR"}
  ],
  "buffers": [{"byteLength": 84, "uri": "mini.bin"}]
})";
    }

    test::HeadlessAssetFixture fx;
    auto g = fx.assets.LoadGLTF(gltfPath);
    printf("GltfMultiPrimitiveMesh: nodes=%zu (expected 2)\n", g.nodes.size());
    CHECK(g.nodes.size() == 2);
    if (g.nodes.size() == 2) {
        CHECK(g.nodes[0].mesh.Valid());
        CHECK(g.nodes[1].mesh.Valid());
        // 两个 primitive 材质不同（红 vs 绿），验证各自材质被保留。
        const auto& c0 = g.nodes[0].material.tint;
        const auto& c1 = g.nodes[1].material.tint;
        printf("  mat0=%f,%f,%f  mat1=%f,%f,%f\n",
               static_cast<double>(c0.r), static_cast<double>(c0.g), static_cast<double>(c0.b),
               static_cast<double>(c1.r), static_cast<double>(c1.g), static_cast<double>(c1.b));
        CHECK(c0.r > 0.5f && c0.g < 0.5f);  // 红
        CHECK(c1.g > 0.5f && c1.r < 0.5f);  // 绿
    }

    std::remove(binPath.c_str());
    std::remove(gltfPath.c_str());
    RemoveDirectoryA(dir);
}
