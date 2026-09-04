#ifdef _WIN32
#define NOMINMAX
#include <windows.h>
#endif
#include <chrono>
#include "neon/neon.hpp"
#include "neon/assets/asset_manager.hpp"
#include "neon/gfx/renderer.hpp"
#include "helpers.hpp"
#include "test_backend.hpp"
using namespace neon;

// True-async glTF: image decode runs on the AsyncLoader worker
// (DecodeImageForPath); the main-thread FinishAsyncGltf only uploads to the
// GPU (LoadTexturePredecoded). Asserts the glTF completes and its materials
// carry valid texture handles (proves the predecoded upload path works).
TEST(GltfAsyncPredecodedTextures) {
    test::HeadlessAssetFixture fx;
    // DamagedHelmet is a committed sample with 4 PBR textures (albedo / MR /
    // AO / emissive) — exercises the async predecoded-upload path. The original
    // test used a 58MB forest glTF that was never committed, so it failed on
    // fresh checkouts.
    const std::string path = "projects/default/assets/models/DamagedHelmet/DamagedHelmet.gltf";
    bool done = false;
    bool ok = false;
    fx.assets.LoadGLTFAsync(path, [&](bool success) {
        done = true;
        ok = success;
    });
    // Worker decode (bin parse + texture stbi) takes real wall time: wait on a
    // deadline, sleeping between Pump rounds.
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
    while (!done && std::chrono::steady_clock::now() < deadline) {
        fx.assets.PumpAsync();
        ::Sleep(1);
    }
    CHECK(done);
    if (!done) return;
    CHECK(ok);
    auto g = fx.assets.LoadGLTF(path);  // must hit the cache now
    CHECK(g.Valid());
    if (!g.Valid()) return;
    CHECK(!g.nodes.empty());
    // The materials' albedo textures were uploaded through the predecoded
    // path: this fixture never loaded them before, so the texture cache can
    // only have been filled by the async completion.
    bool anyTexture = false;
    for (const auto& n : g.nodes) {
        if (n.material.albedo.Valid()) anyTexture = true;
    }
    CHECK(anyTexture);
}
