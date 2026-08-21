#pragma once

#include <cstdint>
#include <functional>
#include <map>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "neon/assets/async_loader.hpp"
#include "neon/ecs/world.hpp"
#include "neon/math/vec3.hpp"
#include "neon/scene/scene_file.hpp"

namespace neon::assets {
class AssetManager;
}

namespace neon::scene {

// World position -> chunk index. Chunk edges sit on integer multiples of
// `size`, so pos/size is floor division (a position exactly on an edge belongs
// to the chunk ABOVE it, and negative positions land in the correctly-signed
// chunk: x=-1 with size=64 -> chunk -1). Non-finite input (NaN/Inf) or a
// non-positive `size` maps to chunk 0 - a documented fallback that avoids the
// UB of casting a non-finite float to int.
int ChunkCoord(float worldPos, float size);

// Chunk coordinates (cx, cz) of a world position. The horizontal (x, z) plane
// is the streaming plane; the y (height) axis is ignored.
std::pair<int, int> ChunkCoordFromWorldPos(const math::Vec3& pos, float size);

// "chunk_<cx>_<cz>.json" - the per-chunk scene file name inside a chunkDir.
std::string ChunkFileName(int cx, int cz);

// One coordinate block: a `size` x `size` (default 64) square of world space
// spanning [cx*size, (cx+1)*size) x [cz*size, (cz+1)*size). The world owns the
// entities; the chunk only tracks which ones it created so unload can destroy
// them. `loaded` means the chunk's data has been processed (entities present,
// or the chunk is legitimately empty because no file exists); `failed` marks a
// file that could not be parsed or instantiated (kept so it is NOT retried on
// every frame while the focus stays on it).
struct WorldChunk {
    int cx = 0, cz = 0;
    std::vector<ecs::Entity> entities;
    std::vector<std::string> meshKeys; // mesh + PBR texture refs (SceneFile::MeshKeys)
    bool loaded = false;
    bool failed = false;
    std::string error; // failure detail; empty otherwise
};

// Maintains a (2*radius+1) x (2*radius+1) window of active chunks around a
// focus position (radius 1 = the default 3x3 window). As the focus moves,
// chunks that leave the window are unloaded (their entities destroyed) and
// chunks that enter are loaded from per-chunk scene files.
//
// Chunk data format: each chunk is a standard componentized SceneFile JSON
// document ({"entities": [...]}, the SAME schema as any scene) stored at
// `<chunkDir>/chunk_<cx>_<cz>.json`, in WORLD space. The streamer parses it
// with SceneFile::Parse and instantiates via scene::Instantiate (which expands
// prefabs through the PrefabLibrary). A chunk without a file is a valid empty
// chunk (logged at debug). Chunk coords map to the file name via ChunkFileName.
//
// Async model: the chunk FILE READ (pure I/O) runs on the engine worker pool
// (AsyncLoader). Parsing + Instantiate + the onChunkLoaded/onChunkFailed
// callbacks all run on the MAIN thread inside PumpAsync() - call it once per
// frame (after assets.PumpAsync()). A chunk whose load completes after the
// focus moved out of the window is discarded without instantiation (re-entering
// the window later reloads it fresh). Config::workerCount == 0 disables the
// pool so loads complete synchronously inside Update() (used by tests and a
// documented fallback).
//
// Clear() cancels in-flight loads: it bumps an epoch counter, and a completion
// delivered for a load submitted under an older epoch is discarded (like the
// focus-based stale-load discard, but epoch-based so it also covers the case
// where Clear() + a later Update() re-enters the same window without a focus
// change). A chunk therefore never resurrects after Clear().
//
// NOTE: the destructor does NOT unload. It joins the worker pool (discarding
// undelivered completions) and drops the streamer's own state, but the chunk
// entities remain alive in the world - the caller owns the world and unloads
// via Clear() or world teardown.
class ChunkStreamer {
public:
    struct Config {
        int radius = 1;             // half-window size in chunks (1 -> 3x3)
        float size = 64.0f;         // chunk edge length in world units
        std::string chunkDir;       // directory containing chunk_<cx>_<cz>.json
        ecs::World* world = nullptr;
        scene::ComponentRegistry* registry = nullptr;
        scene::PrefabLibrary* prefs = nullptr; // optional; empty when null
        // Optional: when set, successfully loaded chunks also kick best-effort
        // async preloads for the PBR textures they reference. Not required for
        // streaming itself.
        assets::AssetManager* assets = nullptr;
        int workerCount = 2;        // 0 = synchronous loads
    };

    explicit ChunkStreamer(const Config& cfg);
    // Joins the worker pool and drops the streamer's own state. Does NOT
    // destroy the chunk entities (see the class comment).
    ~ChunkStreamer();

    ChunkStreamer(const ChunkStreamer&) = delete;
    ChunkStreamer& operator=(const ChunkStreamer&) = delete;

    // Recompute the window around `focus` (world space): unload chunks that
    // left it, start (async) loads for chunks that entered it.
    void Update(const math::Vec3& focus);
    // Complete pending chunk loads on the calling (main) thread. Call once per
    // frame; no-op in the synchronous configuration.
    void PumpAsync();
    // Unload every loaded chunk, destroy its entities and drop all state.
    // Also cancels in-flight loads: completions delivered for loads submitted
    // before the Clear() are discarded on the next PumpAsync() (epoch guard),
    // so a chunk never resurrects. Used on scene change / shutdown. Fires
    // onChunkUnloaded per unloaded chunk.
    void Clear();

    const std::map<std::pair<int, int>, WorldChunk>& Chunks() const { return chunks_; }
    // Number of chunks with loaded == true (empty chunks count; failed don't).
    int LoadedChunkCount() const;
    // Number of loads submitted to the pool but not yet completed.
    int PendingChunkCount() const;
    const WorldChunk* Get(int cx, int cz) const;

    // Callbacks, all fired on the main thread.
    std::function<void(int cx, int cz)> onChunkLoaded;
    std::function<void(int cx, int cz)> onChunkUnloaded;          // loaded chunks only
    std::function<void(int cx, int cz, const std::string& err)> onChunkFailed;

private:
    bool InWindow(int cx, int cz) const;
    void StartLoad(int cx, int cz);
    void CompleteLoad(int cx, int cz, const std::string& text, bool readOk, uint32_t epoch);
    void Unload(const WorldChunk& chunk);

    Config cfg_;
    assets::AsyncLoader loader_;
    scene::PrefabLibrary emptyPrefs_;
    std::map<std::pair<int, int>, WorldChunk> chunks_;
    std::set<std::pair<int, int>> pending_; // loads in flight (not yet completed)
    std::pair<int, int> focusChunk_{0, 0};
    // Bumped by Clear(); a load submitted under an older epoch is discarded
    // when its completion runs, cancelling in-flight loads across Clear().
    uint32_t epoch_ = 0;
};

} // namespace neon::scene
