#pragma once
#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <vector>
#include "neon/assets/async_loader.hpp"
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

// Per-load texture options. Default-constructed options reproduce the classic
// LoadTexture(path) behavior byte-for-byte.
struct TextureLoadOptions {
    // Compress OPAQUE images (no alpha channel) to BC1/DXT1 (8 bytes per 4x4
    // block = 1/8 the GPU memory of RGBA8) at load. Alpha-bearing images stay
    // RGBA8 (BC1 has no alpha). Compression is opt-in per call site; a driver
    // that rejects compressed uploads falls back to RGBA8 once and disables
    // compression for the rest of the session.
    bool compressBc1 = false;
    // Flip the image vertically on load (glTF texture coordinates have their
    // origin in the top-left; OpenGL sampling expects bottom-left).
    bool flipVertically = false;
};

// Runtime asset cache. Files are loaded once and reused by path.
class AssetManager {
public:
    // Shuts down the async worker pool (joins worker threads, discards
    // pending/undelivered work) before the rest of the cache is torn down.
    ~AssetManager();

    void Init(gfx::Renderer* renderer) { renderer_ = renderer; }

    gfx::Texture LoadTexture(const std::string& path);
    // Compression-aware overload; see TextureLoadOptions.
    gfx::Texture LoadTexture(const std::string& path, const TextureLoadOptions& opts);
    gfx::Mesh LoadMeshOBJ(const std::string& path);
    // glTF 2.0 importer: POSITION/NORMAL/TEXCOORD_0, PBR metallic-roughness
    // materials (baseColor/metalRoughness/occlusion/emissive), node transforms.
    GltfAsset LoadGLTF(const std::string& path);
    gfx::Font LoadFont(const std::string& path, int pixelHeight);
    // Loads a system CJK font with DYNAMIC glyphs (stb_truetype atlas grows on
    // demand), so any text renders without declaring a character list.
    gfx::Font LoadSystemCJKFont(int pixelHeight);

    // Async texture load (T5.2): the image decode (stbi_load, optional BC1
    // compression) runs on a worker thread; the GPU upload, cache insert and
    // `cb` all run on the MAIN thread inside PumpAsync() - call it once per
    // frame. Callback contract (always on the main thread, ok = the texture is
    // cached and usable):
    //   * path already cached  -> cb(true) fires immediately.
    //   * pool unavailable or SetAsyncEnabled(false)
    //                          -> synchronous load, cb fires inline.
    //   * concurrent requests for the SAME path
    //                          -> coalesced: one in-flight decode, every
    //                             caller's cb fires when it completes.
    void LoadTextureAsync(const std::string& path, std::function<void(bool)> cb,
                          const TextureLoadOptions& opts = {});
    // Drains completed async decodes: performs the GPU uploads, populates the
    // cache and fires callbacks on the calling (main) thread. The app calls
    // this once per frame (wired into neon_rush/neon_game/neon_editor).
    void PumpAsync();

    // Editor tooling: current cache contents and aggregate stats.
    AssetStats Stats() const;
    const std::map<std::string, gfx::Texture>& Textures() const { return textures_; }
    const std::map<std::string, gfx::Mesh>& Meshes() const { return meshes_; }
    const std::map<std::pair<std::string, int>, gfx::Font>& Fonts() const { return fonts_; }

    // Hot-reload support. Every successful load records the file's mtime;
    // ChangedOnDisk compares the on-disk mtime against that record (false when
    // the asset was never loaded or the file is missing). Reload* drops the
    // stale cached entry and re-reads when the file changed on disk.
    bool TextureChangedOnDisk(const std::string& path) const;
    bool MeshChangedOnDisk(const std::string& path) const;
    void ReloadTexture(const std::string& path);
    void ReloadMeshOBJ(const std::string& path);

    // Test/tooling hooks ---------------------------------------------------
    // Disables the worker pool so LoadTextureAsync degrades to a synchronous
    // load (callback fires inline). Default: enabled.
    void SetAsyncEnabled(bool enabled) { asyncEnabled_ = enabled; }
    // Replaces the worker-side image decode (default: stbi_load). A decode
    // with channels == 0 counts as a failed load. Pass a default-constructed
    // function to restore the built-in stbi_load path. Must be configured
    // before any load.
    void SetDecodeHook(
        std::function<DecodedImage(const std::string&, const TextureLoadOptions&)> fn) {
        decodeFn_ = std::move(fn);
    }

private:
    // Pure-CPU decode (never touches GL). Runs on a worker for async loads and
    // inline for sync loads. `compressed` is the resolved BC1 decision (already
    // gated on driver capability by the caller, on the main thread).
    DecodedImage DecodeImage(const std::string& path, const TextureLoadOptions& opts,
                             bool compressed);
    // Main-thread GPU upload; handles the compressed->RGBA8 driver fallback.
    gfx::Texture UploadDecoded(const DecodedImage& img);
    // Main-thread completion of an async request: cache + fire callbacks.
    void FinishAsyncTexture(const std::string& path, DecodedImage img);

    gfx::Renderer* renderer_ = nullptr;
    std::map<std::string, gfx::Texture> textures_;
    std::map<std::string, gfx::Mesh> meshes_;
    std::map<std::pair<std::string, int>, gfx::Font> fonts_;
    std::map<std::string, uint64_t> textureMtimes_;
    std::map<std::string, uint64_t> meshMtimes_;

    // Async state. All of these are touched only from the MAIN thread:
    //   inFlight_ marks paths with a decode running; pendingCallbacks_ holds
    //   every caller's callback for the in-flight path (coalescing).
    AsyncLoader asyncLoader_{2};
    bool asyncEnabled_ = true;
    std::function<DecodedImage(const std::string&, const TextureLoadOptions&)> decodeFn_;
    std::map<std::string, bool> inFlight_;
    std::map<std::string, std::vector<std::function<void(bool)>>> pendingCallbacks_;
    // Driver capability learned at runtime: the first rejected compressed
    // upload flips this off and the fallback warning is logged once.
    bool bc1Supported_ = true;
    bool bc1FallbackWarned_ = false;
};

} // namespace neon::assets
