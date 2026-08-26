#include <algorithm>
#include <atomic>
#include <string>
#include <vector>

#include "neon/neon.hpp"
#include "helpers.hpp"
#include "test_backend.hpp"

using namespace neon;

namespace {

void SleepMs(int ms) {
#if defined(_WIN32)
    ::Sleep(static_cast<DWORD>(ms));
#else
    usleep(static_cast<useconds_t>(ms) * 1000);
#endif
}

// Drives the async worker pool until `fired` reaches `target` (or a timeout).
bool PumpUntil(neon::assets::AssetManager& assets, const std::atomic<int>& fired, int target,
               int maxIters = 4000) {
    for (int i = 0; i < maxIters && fired.load() < target; ++i) {
        assets.PumpAsync();
        SleepMs(1);
    }
    return fired.load() >= target;
}

// A minimal glTF: one material referencing image 0 (missing texture) + a tiny
// binary buffer, so LoadGLTF runs the full dependency path.
const char* kMinimalGltf =
    R"({"asset":{"version":"2.0"},"scene":0,"scenes":[{"nodes":[]}],"nodes":[],)"
    R"("materials":[{"name":"m","pbrMetallicRoughness":{"baseColorTexture":{"index":0}}}],)"
    R"("textures":[{"source":0}],"images":[{"uri":"missing_tex.png"}],)"
    R"("buffers":[{"uri":"tiny.bin","byteLength":8}],"bufferViews":[],"accessors":[],"meshes":[]})";

} // namespace

// Loading a real glTF records its texture/buffer dependencies and the reverse
// edges; every repo-referenced dependency resolves.
TEST(AssetDepsDamagedHelmetRecorded) {
    test::HeadlessAssetFixture fx;
    const std::string path = "assets/models/DamagedHelmet/DamagedHelmet.gltf";
    fx.assets.LoadGLTF(path);

    const std::vector<std::string> deps = fx.assets.DependenciesOf(path);
    CHECK(!deps.empty());
    bool hasAlbedo = false;
    for (const std::string& d : deps)
        if (d.find("Default_albedo") != std::string::npos) hasAlbedo = true;
    CHECK(hasAlbedo);
    CHECK(fx.assets.MissingDependencies(path).empty());

    bool reverseFound = false;
    for (const std::string& d : deps) {
        const std::vector<std::string> rev = fx.assets.DependentsOf(d);
        if (std::find(rev.begin(), rev.end(), path) != rev.end()) reverseFound = true;
    }
    CHECK(reverseFound);
}

// A glTF referencing a missing texture reports exactly that dependency as
// missing (precise error propagation instead of a silent white fallback).
TEST(AssetDepsMissingTextureReported) {
    test::HeadlessAssetFixture fx;
    test::TempDir tmp;
    const std::string base = tmp.Str();
    CHECK(test::WriteFileAll(base + "/tiny.bin", std::string(8, '\0')));
    const std::string gltfPath = base + "/scene.gltf";
    CHECK(test::WriteFileAll(gltfPath, kMinimalGltf));

    fx.assets.LoadGLTF(gltfPath);
    const std::vector<std::string> deps = fx.assets.DependenciesOf(gltfPath);
    CHECK_EQ(deps.size(), 2u); // tiny.bin + missing_tex.png
    const std::vector<std::string> missing = fx.assets.MissingDependencies(gltfPath);
    CHECK_EQ(missing.size(), 1u);
    if (missing.size() == 1u)
        CHECK(missing[0].find("missing_tex.png") != std::string::npos);
}

// LoadDependenciesAsync reports success when every dependency is loadable and
// a precise error (naming the missing path) otherwise.
TEST(AssetDepsAsyncLoads) {
    test::HeadlessAssetFixture fx;

    // Happy path: the DamagedHelmet's textures were loaded by LoadGLTF, so the
    // async dependency load completes inline with ok=true.
    const std::string good = "assets/models/DamagedHelmet/DamagedHelmet.gltf";
    fx.assets.LoadGLTF(good);
    std::atomic<int> goodFired{0};
    std::string goodErr;
    fx.assets.LoadDependenciesAsync(good, [&](bool ok, const std::string& e) {
        goodFired.store(1);
        goodErr = e;
    });
    CHECK(PumpUntil(fx.assets, goodFired, 1));
    CHECK_EQ(goodFired.load(), 1);
    CHECK(goodErr.empty());

    // Missing dependency: the callback reports the failing path.
    test::TempDir tmp;
    const std::string base = tmp.Str();
    CHECK(test::WriteFileAll(base + "/tiny.bin", std::string(8, '\0')));
    const std::string gltfPath = base + "/scene.gltf";
    CHECK(test::WriteFileAll(gltfPath, kMinimalGltf));
    fx.assets.LoadGLTF(gltfPath);
    std::atomic<int> badFired{0};
    std::string badErr;
    fx.assets.LoadDependenciesAsync(gltfPath, [&](bool ok, const std::string& e) {
        badFired.store(1);
        if (!ok) badErr = e;
    });
    CHECK(PumpUntil(fx.assets, badFired, 1));
    CHECK_EQ(badFired.load(), 1);
    CHECK(badErr.find("missing_tex.png") != std::string::npos);
}

// G6-2: async OBJ mesh load — the file read + parse run on the worker pool, the
// upload + cache happen on the main thread inside PumpAsync, and the callback
// fires with the result. Concurrent requests for the same path coalesce.
TEST(AssetDepsAsyncMeshLoad) {
    test::HeadlessAssetFixture fx;
    const std::string obj = "assets/kenney_nature/Models/OBJ format/flower_redA.obj";
    std::atomic<int> fired{0};
    std::atomic<int> fired2{0};
    bool ok1 = false, ok2 = false;
    fx.assets.LoadMeshOBJAsync(obj, [&](bool ok) {
        ok1 = ok;
        fired.store(1);
    });
    // Second concurrent request coalesces onto the in-flight load.
    fx.assets.LoadMeshOBJAsync(obj, [&](bool ok) {
        ok2 = ok;
        fired2.store(1);
    });
    CHECK(PumpUntil(fx.assets, fired, 1));
    CHECK(PumpUntil(fx.assets, fired2, 1));
    CHECK(ok1);
    CHECK(ok2);
    // The mesh is now cached: a sync load hits the cache with a valid mesh.
    CHECK(fx.assets.LoadMeshOBJ(obj).Valid());

    // Missing file: the async callback reports failure.
    std::atomic<int> badFired{0};
    bool badOk = true;
    fx.assets.LoadMeshOBJAsync("assets/missing_never_there.obj", [&](bool ok) {
        badOk = ok;
        badFired.store(1);
    });
    CHECK(PumpUntil(fx.assets, badFired, 1));
    CHECK(!badOk);
}
