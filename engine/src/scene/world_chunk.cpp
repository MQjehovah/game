#include "neon/scene/world_chunk.hpp"

#include <algorithm>
#include <cmath>
#include <fstream>
#include <iterator>

#include "neon/assets/asset_manager.hpp"
#include "neon/core/log.hpp"

namespace neon::scene {
namespace {

bool ReadFileText(const std::string& path, std::string& out) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return false;
    out.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    return !in.bad();
}

} // namespace

int ChunkCoord(float worldPos, float size) {
    // static_cast of a non-finite float to int is UB; guard against a corrupted
    // focus (NaN/Inf) or a non-positive size and map to chunk 0 (documented).
    if (!std::isfinite(worldPos) || !std::isfinite(size) || size <= 0.0f) return 0;
    return static_cast<int>(std::floor(worldPos / size));
}

std::pair<int, int> ChunkCoordFromWorldPos(const math::Vec3& pos, float size) {
    return {ChunkCoord(pos.x, size), ChunkCoord(pos.z, size)};
}

std::string ChunkFileName(int cx, int cz) {
    return "chunk_" + std::to_string(cx) + "_" + std::to_string(cz) + ".json";
}

ChunkStreamer::ChunkStreamer(const Config& cfg)
    : cfg_(cfg), loader_(std::max(cfg.workerCount, 0)) {
    if (cfg_.size <= 0.0f) cfg_.size = 64.0f;
    if (cfg_.radius < 0) cfg_.radius = 0;
}

ChunkStreamer::~ChunkStreamer() = default;

void ChunkStreamer::Update(const math::Vec3& focus) {
    focusChunk_ = ChunkCoordFromWorldPos(focus, cfg_.size);

    // Unload chunks that left the window (before loading, so an evicted chunk
    // never coexists with its freshly-loaded replacement).
    for (auto it = chunks_.begin(); it != chunks_.end();) {
        const WorldChunk& w = it->second;
        if (!InWindow(w.cx, w.cz)) {
            Unload(w);
            if (w.loaded && onChunkUnloaded) onChunkUnloaded(w.cx, w.cz);
            it = chunks_.erase(it);
        } else {
            ++it;
        }
    }

    // Load chunks that entered the window (chunks already present or in flight
    // are skipped, so a chunk is never submitted twice).
    for (int dz = -cfg_.radius; dz <= cfg_.radius; ++dz) {
        for (int dx = -cfg_.radius; dx <= cfg_.radius; ++dx) {
            const int cx = focusChunk_.first + dx;
            const int cz = focusChunk_.second + dz;
            const auto key = std::make_pair(cx, cz);
            if (chunks_.count(key) || pending_.count(key)) continue;
            StartLoad(cx, cz);
        }
    }
}

void ChunkStreamer::PumpAsync() { loader_.Pump(); }

void ChunkStreamer::Clear() {
    // Bump the epoch FIRST: every completion already delivered (or still to be
    // delivered) for a load submitted under the previous epoch is discarded by
    // CompleteLoad, so in-flight loads cannot resurrect a chunk after Clear().
    ++epoch_;
    for (auto& kv : chunks_) {
        Unload(kv.second);
        if (kv.second.loaded && onChunkUnloaded) onChunkUnloaded(kv.first.first, kv.first.second);
    }
    chunks_.clear();
    pending_.clear();
}

int ChunkStreamer::LoadedChunkCount() const {
    int n = 0;
    for (const auto& kv : chunks_)
        if (kv.second.loaded) ++n;
    return n;
}

int ChunkStreamer::PendingChunkCount() const { return static_cast<int>(pending_.size()); }

const WorldChunk* ChunkStreamer::Get(int cx, int cz) const {
    auto it = chunks_.find(std::make_pair(cx, cz));
    return it == chunks_.end() ? nullptr : &it->second;
}

bool ChunkStreamer::InWindow(int cx, int cz) const {
    return std::abs(cx - focusChunk_.first) <= cfg_.radius &&
           std::abs(cz - focusChunk_.second) <= cfg_.radius;
}

void ChunkStreamer::StartLoad(int cx, int cz) {
    pending_.insert(std::make_pair(cx, cz));
    const std::string path = cfg_.chunkDir + "/" + ChunkFileName(cx, cz);
    const uint32_t epoch = epoch_;

    if (loader_.Available()) {
        // Worker: read the (small) chunk file. Completion is delivered and runs
        // on the main thread inside PumpAsync() - parsing/instantiation never
        // touch the world or renderer off-thread. The submission epoch is
        // captured so Clear() (which bumps epoch_) cancels this load even after
        // the file was already read.
        loader_.Submit([this, cx, cz, epoch, path]() {
            std::string text;
            const bool ok = ReadFileText(path, text);
            loader_.Deliver(
                [this, cx, cz, epoch, text, ok]() { CompleteLoad(cx, cz, text, ok, epoch); });
        });
    } else {
        // Synchronous fallback: complete inline (workerCount == 0).
        std::string text;
        const bool ok = ReadFileText(path, text);
        CompleteLoad(cx, cz, text, ok, epoch);
    }
}

void ChunkStreamer::CompleteLoad(int cx, int cz, const std::string& text, bool readOk,
                                 uint32_t epoch) {
    pending_.erase(std::make_pair(cx, cz));

    // A Clear() happened since this load was submitted: discard the completion.
    // Covers both the chunk-leaves-window case (no focus change involved) and
    // Clear() + re-entry, where the fresh load carries the new epoch.
    if (epoch != epoch_) return;

    // The focus moved while the file was being read: drop the chunk. If the
    // focus comes back it is re-submitted (fresh file read), so a stale chunk
    // is never instantiated.
    if (!InWindow(cx, cz)) return;

    const auto key = std::make_pair(cx, cz);
    WorldChunk w;
    w.cx = cx;
    w.cz = cz;

    if (!cfg_.world || !cfg_.registry) {
        w.failed = true;
        w.error = "chunk streamer: config requires a world and a component registry";
        chunks_[key] = std::move(w);
        NEON_LOG_CAT(neon::core::LogCategory::Scene, neon::core::LogLevel::Error, "chunk %d,%d: %s", cx, cz,
                     w.error.c_str());
        if (onChunkFailed) onChunkFailed(cx, cz, w.error);
        return;
    }

    if (!readOk) {
        // No chunk file: a valid empty chunk (not a failure).
        w.loaded = true;
        chunks_[key] = std::move(w);
        NEON_LOG_CAT(neon::core::LogCategory::Scene, neon::core::LogLevel::Debug, "chunk %d,%d: no file, empty", cx, cz);
        if (onChunkLoaded) onChunkLoaded(cx, cz);
        return;
    }

    auto parsed = SceneFile::Parse(text);
    if (!parsed.Ok()) {
        w.failed = true;
        w.error = parsed.Error();
        chunks_[key] = std::move(w);
        NEON_LOG_CAT(neon::core::LogCategory::Scene, neon::core::LogLevel::Warn, "chunk %d,%d: %s", cx, cz,
                     w.error.c_str());
        if (onChunkFailed) onChunkFailed(cx, cz, w.error);
        return;
    }

    SceneFile sf = std::move(parsed.Value());
    const PrefabLibrary& prefs = cfg_.prefs ? *cfg_.prefs : emptyPrefs_;
    std::vector<ecs::Entity> ents;
    auto inst = Instantiate(*cfg_.world, sf, prefs, *cfg_.registry, &ents);
    if (!inst.Ok()) {
        w.failed = true;
        w.error = inst.Error();
        chunks_[key] = std::move(w);
        NEON_LOG_CAT(neon::core::LogCategory::Scene, neon::core::LogLevel::Warn, "chunk %d,%d: %s", cx, cz,
                     w.error.c_str());
        if (onChunkFailed) onChunkFailed(cx, cz, w.error);
        return;
    }

    const std::vector<std::string> refs = sf.MeshKeys();
    w.entities = std::move(ents);
    w.meshKeys = refs;
    w.loaded = true;
    chunks_[key] = std::move(w);

    // Best-effort texture preload when an AssetManager is wired in (mesh keys
    // are skipped: there is no async mesh load yet).
    if (cfg_.assets) {
        // Hold a reference for every asset the chunk references so unload can
        // release them (the runtime's entity spawn also loads these - cache
        // hits just bump the refcount).
        cfg_.assets->AcquireChunkAssets(refs);
        for (const std::string& r : refs) {
            if (r.find("obj:") == 0 || r.find("gltf:") == 0) continue;
            cfg_.assets->LoadTextureAsync(r, [](bool) {});
        }
    }

    if (onChunkLoaded) onChunkLoaded(cx, cz);
}

void ChunkStreamer::Unload(const WorldChunk& chunk) {
    if (!cfg_.world) return;
    // Release the assets this chunk held so the cache can reclaim GPU memory
    // once the deferred-reclaim window elapses (P0-3).
    if (cfg_.assets) cfg_.assets->ReleaseChunkAssets(chunk.meshKeys);
    for (ecs::Entity e : chunk.entities) cfg_.world->Destroy(e);
}

} // namespace neon::scene
