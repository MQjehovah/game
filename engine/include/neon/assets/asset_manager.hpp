#pragma once
#include <map>
#include <string>
#include "neon/gfx/font.hpp"
#include "neon/gfx/mesh.hpp"
#include "neon/gfx/renderer.hpp"
#include "neon/gfx/texture.hpp"

namespace neon::assets {

struct GltfNode {
    math::Mat4 transform;
    gfx::Mesh mesh;
    gfx::Material material;
};

struct GltfAsset {
    std::vector<GltfNode> nodes;
    bool Valid() const { return !nodes.empty(); }
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
