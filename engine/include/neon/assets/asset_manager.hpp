#pragma once
#include <cstdint>
#include <map>
#include <string>
#include <vector>
#include "neon/core/result.hpp"
#include "neon/gfx/font.hpp"
#include "neon/gfx/mesh.hpp"
#include "neon/gfx/renderer.hpp"
#include "neon/gfx/texture.hpp"
#include "neon/math/quat.hpp"

namespace neon::assets {

// Full glTF scene-graph node (every node in the glTF "nodes" array: mesh
// nodes, joints, and transform-only nodes), in glTF node index order. Skins
// reference joints and animation channels reference targets by raw glTF node
// index, so this table lets the animator address any node directly.
struct GltfNode {
    int parent = -1;
    math::Vec3 t{0, 0, 0};
    math::Quat r{0, 0, 0, 1};
    math::Vec3 s{1, 1, 1};
    std::string name;
};

// A mesh-bearing node as exposed to the renderer (accumulated world transform
// plus the uploaded GPU mesh and its material).
struct GltfMeshNode {
    math::Mat4 transform;
    gfx::Mesh mesh;
    gfx::Material material;
};

// Raw glTF buffer layout (bufferViews / accessors arrays), kept alongside the
// binary buffer so accessor data can be decoded after load (e.g. animation
// sampler times/values).
struct GltfBufferView {
    int buffer = 0;
    int byteOffset = 0;
    int byteLength = 0;
    int byteStride = 0; // 0 = tightly packed
};

struct GltfAccessor {
    int bufferView = -1;
    int byteOffset = 0;
    int componentType = 0;
    int count = 0;
    std::string type; // SCALAR/VEC2/VEC3/VEC4/MAT4
};

struct GltfAsset {
    std::vector<GltfMeshNode> nodes;
    std::vector<GltfNode> nodesAll;
    std::vector<gfx::Skin> skins;
    std::vector<uint8_t> rawBin;
    std::vector<GltfBufferView> bufferViews;
    std::vector<GltfAccessor> accessors;
    // True when a glTF loaded: at least one renderable mesh node OR a full node
    // hierarchy (pure-animation / rig-only assets have no mesh nodes).
    bool Valid() const { return !nodes.empty() || !nodesAll.empty(); }

    // Decodes an accessor into floats (FLOAT plus integer component types),
    // one scalar per component in accessor order. Err on a bad index/layout.
    core::Result<std::vector<float>> ReadAccessorFloats(int accessorIndex) const;
};

// Aggregate statistics for the editor "resource" panel.
struct AssetStats {
    size_t textures = 0;
    size_t meshes = 0;
    size_t fonts = 0;
    size_t textureBytes = 0;
    size_t meshTriangles = 0;
    size_t meshVertices = 0;
};

// Runtime asset cache. Files are loaded once and reused by path.
class AssetManager {
public:
    void Init(gfx::Renderer* renderer) { renderer_ = renderer; }

    gfx::Texture LoadTexture(const std::string& path);
    gfx::Mesh LoadMeshOBJ(const std::string& path);
    // glTF 2.0 importer: POSITION/NORMAL/TEXCOORD_0, PBR metallic-roughness
    // materials (baseColor/metalRoughness/occlusion/emissive), node transforms.
    GltfAsset LoadGLTF(const std::string& path);
    gfx::Font LoadFont(const std::string& path, int pixelHeight);
    // Loads a system CJK font (per-platform path list) and bakes the codepoints
    // that appear in sampleTexts (plus ASCII). Returns an invalid Font if none found.
    gfx::Font LoadSystemCJKFont(int pixelHeight, const std::vector<std::string>& sampleTexts);

    // Editor tooling: current cache contents and aggregate stats.
    AssetStats Stats() const;
    const std::map<std::string, gfx::Texture>& Textures() const { return textures_; }
    const std::map<std::string, gfx::Mesh>& Meshes() const { return meshes_; }
    const std::map<std::pair<std::string, int>, gfx::Font>& Fonts() const { return fonts_; }

private:
    gfx::Renderer* renderer_ = nullptr;
    std::map<std::string, gfx::Texture> textures_;
    std::map<std::string, gfx::Mesh> meshes_;
    std::map<std::pair<std::string, int>, gfx::Font> fonts_;
};

} // namespace neon::assets
