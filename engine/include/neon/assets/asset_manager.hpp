#pragma once
#include <cstdint>
#include <functional>
#include <map>
#include <set>
#include <string>
#include <vector>
#include "neon/assets/async_loader.hpp"
#include "neon/core/result.hpp"
#include "neon/gfx/font.hpp"
#include "neon/gfx/mesh.hpp"
#include "neon/gfx/renderer.hpp"
#include "neon/core/json.hpp"
#include "neon/gfx/texture.hpp"
#include "neon/io/vfs.hpp"
#include "neon/math/quat.hpp"

namespace neon::assets {

// G6-2: CPU-parsed OBJ (vertices/indices + the MTL dependency paths); produced
// on an async worker thread, consumed (uploaded) on the main thread.
struct ParsedObjMesh;
// G6-2: container-level glTF parse (JSON + binary buffer), produced on an async
// worker thread; the main thread builds the GPU/asset data from it.
struct ParsedGltf;

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
    // Resource-lifecycle counters (P0-3): entries whose refcount dropped to
    // zero and are waiting out the deferred-reclaim window, and how many GPU
    // resources have actually been reclaimed.
    size_t retiredTextures = 0;
    size_t retiredMeshes = 0;
    size_t reclaimedTextures = 0;
    size_t reclaimedMeshes = 0;
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
    // Flip the image vertically on load. This engine uploads textures with
    // their first row at v=0, which already matches glTF's top-left UV origin,
    // so glTF assets must NOT flip; the flag remains for callers that need the
    // legacy image-space orientation.
    bool flipVertically = false;
    // Wrap mode. glTF samplers default to REPEAT (DamagedHelmet-style assets
    // rely on UVs outside [0,1]), so the glTF importer sets Repeat here.
    gfx::Wrap wrap = gfx::Wrap::Clamp;
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
    // G7-1: optional read-only file system (VFS mount stack). When set, every
    // asset read (textures, fonts, OBJ/MTL, glTF + embedded buffers) goes
    // through this layer (pack container + Mod overlays); when null (default)
    // reads fall back to the real filesystem exactly as before.
    void SetFileSystem(neon::io::IFileSystem* fs) { fs_ = fs; }
    neon::io::IFileSystem* FileSystem() const { return fs_; }
    // G1-4 asset dependency graph: direct dependencies (textures, buffers,
    // MTL) recorded when a glTF/OBJ asset loads, and the reverse edges.
    const std::vector<std::string>& DependenciesOf(const std::string& path) const;
    std::vector<std::string> DependentsOf(const std::string& path) const;
    // Walks the transitive dependency graph and returns every path that is
    // MISSING (cannot be loaded), so callers can fail with a precise error
    // instead of silently falling back. Empty = the whole graph is ready.
    std::vector<std::string> MissingDependencies(const std::string& path) const;
    // Recursively loads every transitive dependency (textures async), then
    // calls cb(true) or cb(false, firstError). Deduplicated per path.
    void LoadDependenciesAsync(const std::string& path,
                               std::function<void(bool, const std::string&)> cb);
    // Cache key that distinguishes load options (flip / wrap) so a texture
    // loaded with different settings never shares a stale entry.
    static std::string TextureCacheKey(const std::string& path,
                                       const TextureLoadOptions& opts);
    gfx::Mesh LoadMeshOBJ(const std::string& path);
    // G6-2: async OBJ mesh load for "load on demand". The file read + parse run
    // on the async worker pool; the GPU upload, cache fill and callback all
    // happen on the MAIN thread inside PumpAsync(). Fires cb(ok) on the main
    // thread (inline when the mesh is already cached or no pool is available),
    // and concurrent requests for the same path coalesce onto one load. With a
    // NullBackend the mesh is produced but not GPU-resident, so this is
    // testable headless.
    void LoadMeshOBJAsync(const std::string& path, std::function<void(bool)> cb);
    // G6-2: async glTF/GLB load. The file read + container parse (JSON + binary
    // buffer extraction) run on the async worker; the mesh/texture build, cache
    // and callback happen on the MAIN thread inside PumpAsync(). Same contract
    // as LoadMeshOBJAsync (inline when cached / no pool, coalesced per path).
    void LoadGLTFAsync(const std::string& path, std::function<void(bool)> cb);
    // glTF 2.0 importer: POSITION/NORMAL/TEXCOORD_0, PBR metallic-roughness
    // materials (baseColor/metalRoughness/occlusion/emissive), node transforms.
    // Handles both .gltf (JSON + external .bin) and .glb (binary container).
    GltfAsset LoadGLTF(const std::string& path);
    // Shared importer body: `root` is the parsed glTF JSON, `bin` the binary
    // buffer (external .bin contents or the GLB BIN chunk).
    GltfAsset LoadGltfJson(core::Json& root, std::vector<uint8_t> bin,
                           const std::string& path, uint64_t mtime, const std::string& dir);
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

    // Resource lifecycle (P0-3) ------------------------------------------
    // Every successful load bumps the entry's refcount (first load starts at
    // 1). Release* drops it; at zero the GPU resource is retired and destroyed
    // after a short deferral window (see kReclaimDelayFrames) inside PumpAsync,
    // so a frame that still references it can never read freed memory.
    //
    // The existing LoadTexture/LoadMeshOBJ call sites are acquisition points:
    // callers that load once and hold the asset for the app lifetime simply
    // never release (identical to the pre-refcount behavior). Streaming
    // consumers (ChunkStreamer) acquire per load and release on unload so the
    // cache stops growing as the focus moves.
    gfx::Texture AcquireTexture(const std::string& path, const TextureLoadOptions& opts = {});
    void ReleaseTexture(const std::string& path, const TextureLoadOptions& opts = {});
    gfx::Mesh AcquireMeshOBJ(const std::string& path);
    void ReleaseMeshOBJ(const std::string& path);
    // Bumps the refcount of every asset referenced by a SceneFile's MeshKeys()
    // (texture paths + "obj:" meshes; procedural and "gltf:" keys are skipped -
    // GLTF assets are multi-mesh and not cached under a single key). Loads the
    // asset synchronously on first acquisition. Returns the number of refs held.
    size_t AcquireChunkAssets(const std::vector<std::string>& refs);
    // Drops the refcount of every asset referenced by a chunk. Safe to call
    // multiple times / with unknown keys (missing entries are ignored).
    void ReleaseChunkAssets(const std::vector<std::string>& refs);
    // Current refcount of a cached asset (0 = not cached / no refs).
    size_t TextureRefCount(const std::string& path, const TextureLoadOptions& opts = {}) const;
    size_t MeshRefCount(const std::string& path) const;
    // Pending (deferred) GPU reclaim queue size.
    size_t RetiredTextureCount() const { return retiredTextures_.size(); }
    size_t RetiredMeshCount() const { return retiredMeshes_.size(); }

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
    gfx::Texture UploadDecoded(const DecodedImage& img, gfx::Wrap wrap = gfx::Wrap::Clamp);
    // Main-thread completion of an async request: cache + fire callbacks.
    void FinishAsyncTexture(const std::string& path, DecodedImage img,
                            const TextureLoadOptions& opts);
    // G6-2: main-thread completion of an async OBJ request (upload + cache +
    // dependency edges + callbacks).
    void FinishAsyncMesh(const std::string& path, ParsedObjMesh&& parsed);
    // G6-2: main-thread completion of an async glTF request.
    void FinishAsyncGltf(const std::string& path, uint64_t mtime, ParsedGltf&& parsed);
    // G1-4: records a direct edge parent -> dep in the dependency graph.
    void RecordDependency(const std::string& parent, const std::string& dep);
    // Destroys retired GPU resources whose deferral window has elapsed. Called
    // from PumpAsync (main thread, once per frame).
    void ReclaimRetired(uint64_t frame);

    gfx::Renderer* renderer_ = nullptr;
    std::map<std::string, gfx::Texture> textures_;
    std::map<std::string, gfx::Mesh> meshes_;
    // Parsed glTF assets (raw JSON layout + uploaded mesh/material nodes),
    // keyed by path with the file mtime recorded at parse time. LoadGLTF
    // re-parses only when the on-disk mtime changes, so N entities sharing one
    // model (editor resolve, playtest starts, thumbnails) reuse the same GPU
    // meshes instead of re-uploading per entity.
    std::map<std::string, GltfAsset> gltfs_;
    std::map<std::string, uint64_t> gltfMtimes_;
    std::map<std::pair<std::string, int>, gfx::Font> fonts_;
    std::map<std::string, uint64_t> textureMtimes_;
    std::map<std::string, uint64_t> meshMtimes_;
    // Negative texture cache (missing files): a failed load is remembered so
    // repeated requests (model previews, hot-reload polls) do not re-open the
    // file and spam the log every frame. Re-validated on each request via
    // FileMTime, so a file added later is picked up on the next call.
    std::set<std::string> failedTextures_;

    // Refcount + deferred GPU reclaim (P0-3). PumpAsync is called once per
    // frame on the main thread, so its frame counter is the authoritative
    // "frame" for the deferral window.
    struct RetiredTexture {
        std::string key;
        gfx::Texture tex;
        uint32_t frame = 0;
    };
    struct RetiredMesh {
        std::string key;
        gfx::Mesh mesh;
        uint32_t frame = 0;
    };
    std::map<std::string, uint32_t> textureRefs_;
    std::map<std::string, uint32_t> meshRefs_;
    neon::io::IFileSystem* fs_ = nullptr;
    // G1-4 dependency graph (path -> direct dependencies, normalized keys).
    std::unordered_map<std::string, std::vector<std::string>> dependencies_;
    std::unordered_map<std::string, std::vector<std::string>> dependents_;
    std::vector<RetiredTexture> retiredTextures_;
    std::vector<RetiredMesh> retiredMeshes_;
    uint64_t pumpFrame_ = 0;
    uint64_t reclaimedTextures_ = 0;
    uint64_t reclaimedMeshes_ = 0;

    // Async state. All of these are touched only from the MAIN thread:
    //   inFlight_ marks paths with a decode running; pendingCallbacks_ holds
    //   every caller's callback for the in-flight path (coalescing).
    AsyncLoader asyncLoader_{2};
    bool asyncEnabled_ = true;
    std::function<DecodedImage(const std::string&, const TextureLoadOptions&)> decodeFn_;
    std::map<std::string, bool> inFlight_;
    std::map<std::string, std::vector<std::function<void(bool)>>> pendingCallbacks_;
    // G6-2: async OBJ mesh state, same main-thread-only contract as the texture
    // async maps (separate key space from textures; mesh paths end in .obj).
    std::map<std::string, bool> meshInFlight_;
    std::map<std::string, std::vector<std::function<void(bool)>>> meshPendingCallbacks_;
    // G6-2: async glTF state, same contract as the mesh async maps.
    std::map<std::string, bool> gltfInFlight_;
    std::map<std::string, std::vector<std::function<void(bool)>>> gltfPendingCallbacks_;
    // Driver capability learned at runtime: the first rejected compressed
    // upload flips this off and the fallback warning is logged once.
    bool bc1Supported_ = true;
    bool bc1FallbackWarned_ = false;
};

} // namespace neon::assets
