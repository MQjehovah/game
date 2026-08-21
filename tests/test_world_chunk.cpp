#include <limits>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "neon/neon.hpp"
#include "neon/scene/world_chunk.hpp"
#include "helpers.hpp"

using namespace neon;

namespace {

std::string Ent(const std::string& name, int x, int z) {
    std::string s = "{\"name\":\"" + name + "\",\"components\":{\"transform\":{\"pos\":[";
    s += std::to_string(x) + ",0," + std::to_string(z) + "]}}}";
    return s;
}

std::string SceneOf(const std::vector<std::string>& ents) {
    std::string s = "{\"entities\":[";
    for (size_t i = 0; i < ents.size(); ++i) {
        if (i) s += ",";
        s += ents[i];
    }
    s += "]}";
    return s;
}

scene::ChunkStreamer::Config MakeConfig(ecs::World& world, scene::ComponentRegistry& reg,
                                        const std::string& dir, int workers = 2) {
    scene::ChunkStreamer::Config cfg;
    cfg.chunkDir = dir;
    cfg.world = &world;
    cfg.registry = &reg;
    cfg.workerCount = workers;
    return cfg;
}

void Drain(scene::ChunkStreamer& s) {
    for (int i = 0; i < 4000 && s.PendingChunkCount() > 0; ++i) {
        s.PumpAsync();
#if defined(_WIN32)
        ::Sleep(1);
#else
        ::usleep(1000);
#endif
    }
}

// Pumps for a fixed duration even when nothing is pending. Used to let worker
// threads finish delivering completions that were submitted before a Clear()
// (which drops the pending_ bookkeeping that Drain() keys off of).
void PumpFor(scene::ChunkStreamer& s, int ms) {
    for (int i = 0; i < ms; ++i) {
        s.PumpAsync();
#if defined(_WIN32)
        ::Sleep(1);
#else
        ::usleep(1000);
#endif
    }
}

// Collects the (cx, cz) pairs of every loaded chunk.
std::set<std::pair<int, int>> LoadedIds(const scene::ChunkStreamer& s) {
    std::set<std::pair<int, int>> ids;
    for (const auto& kv : s.Chunks())
        if (kv.second.loaded) ids.insert({kv.first.first, kv.first.second});
    return ids;
}

} // namespace

// ---------------------------------------------------------------------------
// Chunk coordinate math
// ---------------------------------------------------------------------------

TEST(ChunkCoordMath) {
    CHECK_EQ(scene::ChunkCoord(0.0f, 64.0f), 0);
    CHECK_EQ(scene::ChunkCoord(63.9f, 64.0f), 0);
    CHECK_EQ(scene::ChunkCoord(64.0f, 64.0f), 1);   // edge belongs to the chunk above
    CHECK_EQ(scene::ChunkCoord(128.0f, 64.0f), 2);
    CHECK_EQ(scene::ChunkCoord(-1.0f, 64.0f), -1);  // floor division for negatives
    CHECK_EQ(scene::ChunkCoord(-64.0f, 64.0f), -1);
    CHECK_EQ(scene::ChunkCoord(-64.5f, 64.0f), -2);

    auto c = scene::ChunkCoordFromWorldPos({0.0f, 5.0f, 0.0f}, 64.0f);
    CHECK_EQ(c.first, 0);
    CHECK_EQ(c.second, 0);
    auto c2 = scene::ChunkCoordFromWorldPos({64.0f, 0.0f, 64.0f}, 64.0f);
    CHECK_EQ(c2.first, 1);
    CHECK_EQ(c2.second, 1);
    auto c3 = scene::ChunkCoordFromWorldPos({-1.0f, 0.0f, 127.0f}, 64.0f);
    CHECK_EQ(c3.first, -1);
    CHECK_EQ(c3.second, 1);

    // Non-finite positions and non-positive sizes map to chunk 0 (guarded: no
    // UB from casting NaN/Inf to int).
    CHECK_EQ(scene::ChunkCoord(std::numeric_limits<float>::quiet_NaN(), 64.0f), 0);
    CHECK_EQ(scene::ChunkCoord(std::numeric_limits<float>::infinity(), 64.0f), 0);
    CHECK_EQ(scene::ChunkCoord(10.0f, 0.0f), 0);
    CHECK_EQ(scene::ChunkCoord(10.0f, -64.0f), 0);

    CHECK_EQ(scene::ChunkFileName(3, -2), std::string("chunk_3_-2.json"));
}

// ---------------------------------------------------------------------------
// Async 3x3 window: loads complete only via PumpAsync; moving the focus unloads
// evicted chunks (entities destroyed) and loads the new column.
// ---------------------------------------------------------------------------

TEST(ChunkStreamWindowLoadsUnloads) {
    test::TempDir tmp;
    const std::string dir = tmp.Str();
    test::WriteFileAll(dir + "/" + scene::ChunkFileName(0, 0), SceneOf({Ent("a", 10, 10), Ent("b", 20, 20)}));
    test::WriteFileAll(dir + "/" + scene::ChunkFileName(1, 0), SceneOf({Ent("c", 80, 10)}));
    test::WriteFileAll(dir + "/" + scene::ChunkFileName(2, 0), SceneOf({Ent("d", 140, 10), Ent("e", 150, 20), Ent("f", 160, 30)}));

    scene::ComponentRegistry reg;
    scene::RegisterBuiltinComponents(reg);
    ecs::World world;
    scene::ChunkStreamer streamer(MakeConfig(world, reg, dir));

    int loadedCount = 0;
    int unloadedCount = 0;
    streamer.onChunkLoaded = [&](int, int) { ++loadedCount; };
    streamer.onChunkUnloaded = [&](int, int) { ++unloadedCount; };

    // Focus in chunk (0,0): nothing is loaded until PumpAsync runs.
    streamer.Update({32.0f, 0.0f, 32.0f});
    CHECK_EQ(streamer.PendingChunkCount(), 9);
    CHECK_EQ(streamer.LoadedChunkCount(), 0);
    CHECK_EQ(world.EntityCount(), 0u);
    CHECK_EQ(loadedCount, 0);

    Drain(streamer);
    CHECK_EQ(streamer.LoadedChunkCount(), 9);
    CHECK_EQ(world.EntityCount(), 3u);      // only (0,0)=2 + (1,0)=1 have files
    CHECK_EQ(loadedCount, 9);               // every window chunk fires onChunkLoaded
    const scene::WorldChunk* c00 = streamer.Get(0, 0);
    CHECK(c00 != nullptr && c00->loaded);
    CHECK_EQ(c00->entities.size(), 2u);
    const scene::WorldChunk* c10 = streamer.Get(1, 0);
    CHECK(c10 != nullptr && c10->loaded);
    CHECK_EQ(c10->entities.size(), 1u);

    // Focus moves to chunk (2,0): columns cx=-1 and cx=0 leave (6 chunks), the
    // cx=2 and cx=3 columns enter. (0,0) unloads; (2,0) loads.
    streamer.Update({160.0f, 0.0f, 32.0f});
    CHECK_EQ(unloadedCount, 6);
    CHECK_EQ(streamer.Get(0, 0), nullptr);
    CHECK_EQ(world.EntityCount(), 1u);      // (1,0)'s single entity remains

    Drain(streamer);
    CHECK_EQ(streamer.LoadedChunkCount(), 9);
    CHECK_EQ(world.EntityCount(), 4u);      // (1,0)=1 + (2,0)=3
    CHECK_EQ(loadedCount, 15);              // 9 initial + 6 newly entered
    const scene::WorldChunk* c20 = streamer.Get(2, 0);
    CHECK(c20 != nullptr && c20->loaded);
    CHECK_EQ(c20->entities.size(), 3u);
    CHECK(streamer.Get(3, 0) != nullptr);   // empty chunk (no file)
    CHECK_EQ(streamer.Get(3, 0)->entities.size(), 0u);

    // The loaded identity matches the 3x3 window around chunk (2,0).
    std::set<std::pair<int, int>> expected;
    for (int dx = 1; dx <= 3; ++dx)
        for (int dz = -1; dz <= 1; ++dz) expected.insert({dx, dz});
    CHECK(LoadedIds(streamer) == expected);
}

// ---------------------------------------------------------------------------
// Synchronous fallback (workerCount == 0): loads complete inside Update().
// ---------------------------------------------------------------------------

TEST(ChunkStreamSyncFallback) {
    test::TempDir tmp;
    const std::string dir = tmp.Str();
    test::WriteFileAll(dir + "/" + scene::ChunkFileName(0, 0), SceneOf({Ent("a", 1, 1)}));

    scene::ComponentRegistry reg;
    scene::RegisterBuiltinComponents(reg);
    ecs::World world;
    scene::ChunkStreamer streamer(MakeConfig(world, reg, dir, 0));

    streamer.Update({32.0f, 0.0f, 32.0f});
    CHECK_EQ(streamer.PendingChunkCount(), 0);
    CHECK_EQ(streamer.LoadedChunkCount(), 9);   // no PumpAsync needed
    CHECK_EQ(world.EntityCount(), 1u);
    CHECK(streamer.Get(0, 0) != nullptr && streamer.Get(0, 0)->loaded);
}

// ---------------------------------------------------------------------------
// Missing chunk file -> valid empty chunk; stable across repeated updates.
// ---------------------------------------------------------------------------

TEST(ChunkStreamMissingFileEmpty) {
    test::TempDir tmp;
    const std::string dir = tmp.Str();
    test::WriteFileAll(dir + "/" + scene::ChunkFileName(0, 0), SceneOf({Ent("a", 10, 10), Ent("b", 20, 20)}));

    scene::ComponentRegistry reg;
    scene::RegisterBuiltinComponents(reg);
    ecs::World world;
    scene::ChunkStreamer streamer(MakeConfig(world, reg, dir));

    streamer.Update({32.0f, 0.0f, 32.0f});
    Drain(streamer);
    CHECK_EQ(streamer.LoadedChunkCount(), 9);
    CHECK_EQ(world.EntityCount(), 2u);

    const scene::WorldChunk* farChunk = streamer.Get(1, 1);
    CHECK(farChunk != nullptr && farChunk->loaded && !farChunk->failed);
    CHECK_EQ(farChunk->entities.size(), 0u);
    CHECK_EQ(farChunk->meshKeys.size(), 0u);

    // Repeated updates on the same focus never re-load: entity count is stable
    // (no duplicate instantiation) and nothing stays pending.
    for (int i = 0; i < 3; ++i) {
        streamer.Update({32.0f, 0.0f, 32.0f});
        Drain(streamer);
    }
    CHECK_EQ(streamer.PendingChunkCount(), 0);
    CHECK_EQ(streamer.LoadedChunkCount(), 9);
    CHECK_EQ(world.EntityCount(), 2u);
}

// ---------------------------------------------------------------------------
// Malformed file / failed instantiation -> chunk marked failed, no retry storm.
// ---------------------------------------------------------------------------

TEST(ChunkStreamMalformedNoRetryStorm) {
    test::TempDir tmp;
    const std::string dir = tmp.Str();
    test::WriteFileAll(dir + "/" + scene::ChunkFileName(0, 0), std::string("this is not json"));
    test::WriteFileAll(dir + "/" + scene::ChunkFileName(1, 0),
                       "{\"entities\":[{\"components\":{\"health\":{\"hp\":1,\"maxHp\":1}}}]}");

    scene::ComponentRegistry reg;
    scene::RegisterBuiltinComponents(reg);
    ecs::World world;
    scene::ChunkStreamer streamer(MakeConfig(world, reg, dir));

    int failedCount = 0;
    streamer.onChunkFailed = [&](int, int, const std::string&) { ++failedCount; };

    streamer.Update({32.0f, 0.0f, 32.0f});
    Drain(streamer);
    CHECK_EQ(streamer.PendingChunkCount(), 0);
    CHECK_EQ(failedCount, 2);                       // parse + instantiate failure
    CHECK_EQ(streamer.LoadedChunkCount(), 7);       // failed chunks aren't "loaded"
    CHECK_EQ(world.EntityCount(), 0u);              // nothing leaked into the world

    const scene::WorldChunk* bad = streamer.Get(0, 0);
    CHECK(bad != nullptr && bad->failed && !bad->loaded);
    CHECK(!bad->error.empty());
    const scene::WorldChunk* bad2 = streamer.Get(1, 0);
    CHECK(bad2 != nullptr && bad2->failed && !bad2->loaded);
    CHECK(!bad2->error.empty());

    // Repeated updates stay put: the failed chunks are NOT resubmitted.
    for (int i = 0; i < 3; ++i) {
        streamer.Update({32.0f, 0.0f, 32.0f});
        Drain(streamer);
    }
    CHECK_EQ(failedCount, 2);
    CHECK_EQ(streamer.PendingChunkCount(), 0);
    CHECK_EQ(streamer.LoadedChunkCount(), 7);
    CHECK_EQ(world.EntityCount(), 0u);
}

// ---------------------------------------------------------------------------
// Clear() unloads everything and destroys all chunk entities.
// ---------------------------------------------------------------------------

TEST(ChunkStreamClearUnloadsAll) {
    test::TempDir tmp;
    const std::string dir = tmp.Str();
    test::WriteFileAll(dir + "/" + scene::ChunkFileName(0, 0), SceneOf({Ent("a", 10, 10), Ent("b", 20, 20)}));

    scene::ComponentRegistry reg;
    scene::RegisterBuiltinComponents(reg);
    ecs::World world;
    scene::ChunkStreamer streamer(MakeConfig(world, reg, dir));

    streamer.Update({32.0f, 0.0f, 32.0f});
    Drain(streamer);
    CHECK_EQ(streamer.LoadedChunkCount(), 9);
    CHECK_EQ(world.EntityCount(), 2u);

    int unloadedCount = 0;
    streamer.onChunkUnloaded = [&](int, int) { ++unloadedCount; };

    streamer.Clear();
    CHECK_EQ(streamer.LoadedChunkCount(), 0);
    CHECK_EQ(streamer.PendingChunkCount(), 0);
    CHECK_EQ(streamer.Chunks().size(), 0u);
    CHECK_EQ(world.EntityCount(), 0u);
    CHECK_EQ(unloadedCount, 9);
}

// ---------------------------------------------------------------------------
// Clear() cancels in-flight loads: a completion submitted before Clear() must
// not resurrect a chunk on the next PumpAsync() (epoch guard), and a later
// Update() must reload cleanly without double-instantiating.
// ---------------------------------------------------------------------------

TEST(ChunkStreamClearCancelsInflightLoads) {
    test::TempDir tmp;
    const std::string dir = tmp.Str();
    test::WriteFileAll(dir + "/" + scene::ChunkFileName(0, 0), SceneOf({Ent("a", 10, 10)}));

    scene::ComponentRegistry reg;
    scene::RegisterBuiltinComponents(reg);
    ecs::World world;
    scene::ChunkStreamer streamer(MakeConfig(world, reg, dir));

    int loadedFired = 0;
    streamer.onChunkLoaded = [&](int, int) { ++loadedFired; };

    // Submit 9 loads; all are in flight (async) and nothing is instantiated yet.
    streamer.Update({32.0f, 0.0f, 32.0f});
    CHECK_EQ(streamer.PendingChunkCount(), 9);
    CHECK_EQ(world.EntityCount(), 0u);
    CHECK_EQ(loadedFired, 0);

    // Clear() drops all state AND must cancel the in-flight loads.
    streamer.Clear();
    CHECK_EQ(streamer.PendingChunkCount(), 0);
    CHECK_EQ(streamer.Chunks().size(), 0u);

    // Give every worker time to finish its (tiny) read and deliver; each
    // completion is pumped and must be discarded by the epoch guard.
    PumpFor(streamer, 200);
    CHECK_EQ(streamer.LoadedChunkCount(), 0);
    CHECK_EQ(streamer.Chunks().size(), 0u);
    CHECK_EQ(world.EntityCount(), 0u);
    CHECK_EQ(loadedFired, 0);                       // no stale onChunkLoaded

    // A subsequent Update() reloads normally (fresh epoch) and never
    // double-instantiates the chunk whose pre-Clear load also completed.
    streamer.Update({32.0f, 0.0f, 32.0f});
    Drain(streamer);
    CHECK_EQ(streamer.LoadedChunkCount(), 9);
    CHECK_EQ(world.EntityCount(), 1u);
    const scene::WorldChunk* c00 = streamer.Get(0, 0);
    CHECK(c00 != nullptr && c00->loaded);
    CHECK_EQ(c00->entities.size(), 1u);
}

// ---------------------------------------------------------------------------
// Focus exactly on a chunk edge picks the correct window.
// ---------------------------------------------------------------------------

TEST(ChunkStreamFocusNearBoundary) {
    test::TempDir tmp;
    const std::string dir = tmp.Str();
    test::WriteFileAll(dir + "/" + scene::ChunkFileName(1, 1), SceneOf({Ent("edge", 80, 80)}));

    scene::ComponentRegistry reg;
    scene::RegisterBuiltinComponents(reg);
    ecs::World world;
    scene::ChunkStreamer streamer(MakeConfig(world, reg, dir));

    // x=64, z=64 is exactly on the (1,1) chunk origin.
    CHECK_EQ(scene::ChunkCoordFromWorldPos({64.0f, 0.0f, 64.0f}, 64.0f),
             std::make_pair(1, 1));
    streamer.Update({64.0f, 0.0f, 64.0f});
    Drain(streamer);
    CHECK_EQ(streamer.LoadedChunkCount(), 9);
    const scene::WorldChunk* c11 = streamer.Get(1, 1);
    CHECK(c11 != nullptr && c11->loaded);
    CHECK_EQ(c11->entities.size(), 1u);
    CHECK(streamer.Get(0, 0) != nullptr);   // empty, inside the window
    CHECK(streamer.Get(2, 2) != nullptr);

    // Just below the boundary (63.9) still belongs to chunk (0,0).
    CHECK_EQ(scene::ChunkCoordFromWorldPos({63.9f, 0.0f, 63.9f}, 64.0f),
             std::make_pair(0, 0));
}

// ---------------------------------------------------------------------------
// WorldChunk::meshKeys collects mesh + texture refs from a chunk file.
// ---------------------------------------------------------------------------

TEST(ChunkStreamMeshKeysCollected) {
    test::TempDir tmp;
    const std::string dir = tmp.Str();
    const std::string chunkJson =
        "{\"entities\":[{\"name\":\"cube\",\"components\":{"
        "\"transform\":{\"pos\":[5,0,5]},"
        "\"mesh\":{\"meshKey\":\"cube\",\"material\":{\"albedoTex\":\"tex/rock.png\","
        "\"roughness\":0.5,\"emissiveTex\":\"tex/glow.png\"}}}}]}";
    test::WriteFileAll(dir + "/" + scene::ChunkFileName(0, 0), chunkJson);

    scene::ComponentRegistry reg;
    scene::RegisterBuiltinComponents(reg);
    ecs::World world;
    scene::ChunkStreamer streamer(MakeConfig(world, reg, dir));

    streamer.Update({32.0f, 0.0f, 32.0f});
    Drain(streamer);
    const scene::WorldChunk* c00 = streamer.Get(0, 0);
    CHECK(c00 != nullptr && c00->loaded);
    CHECK_EQ(c00->entities.size(), 1u);
    CHECK_EQ(c00->meshKeys.size(), 3u);
    CHECK(c00->meshKeys[0] == "cube");
    CHECK(c00->meshKeys[1] == "tex/rock.png");
    CHECK(c00->meshKeys[2] == "tex/glow.png");

    ecs::Entity e = c00->entities[0];
    CHECK(world.Alive(e));
    CHECK(world.Has<scene::SceneMesh>(e));
    CHECK(world.Get<scene::SceneMesh>(e)->meshKey == "cube");
    CHECK(world.Get<scene::SceneName>(e)->name == "cube");
}
