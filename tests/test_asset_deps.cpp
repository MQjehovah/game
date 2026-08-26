#include <algorithm>
#include <atomic>
#include <string>
#include <vector>

#include "neon/neon.hpp"
#include "neon/assets/asset_path.hpp"
#include "neon/assets/asset_variants.hpp"
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

// G6-2: async glTF/GLB load — the container parse runs off the main thread, the
// asset build + cache happen inside PumpAsync, and the callback fires with the
// result. A GLB (embedded buffer) is parsed the same way as a .gltf.
TEST(AssetDepsAsyncGltfLoad) {
    test::HeadlessAssetFixture fx;
    // A real repo glTF with an external .bin + textures.
    const std::string gltf = "assets/models/DamagedHelmet/DamagedHelmet.gltf";
    std::atomic<int> fired{0};
    bool ok = false;
    fx.assets.LoadGLTFAsync(gltf, [&](bool o) {
        ok = o;
        fired.store(1);
    });
    CHECK(PumpUntil(fx.assets, fired, 1));
    CHECK(ok);
    // Cached by the async path: a sync LoadGLTF hits the mtime cache.
    CHECK(fx.assets.LoadGLTF(gltf).Valid());

    // Concurrent request coalesces onto the in-flight load.
    std::atomic<int> fired2{0};
    fx.assets.LoadGLTFAsync(gltf, [&](bool) { fired2.store(1); });
    CHECK(PumpUntil(fx.assets, fired2, 1));

    // Missing file: the async callback reports failure.
    std::atomic<int> badFired{0};
    bool badOk = true;
    fx.assets.LoadGLTFAsync("assets/missing_never_there.gltf", [&](bool o) {
        badOk = o;
        badFired.store(1);
    });
    CHECK(PumpUntil(fx.assets, badFired, 1));
    CHECK(!badOk);
}

// G6-1: platform/LOD asset variant table — logical paths resolve to concrete
// files, unlisted paths fall back to themselves, and the JSON (de)serialization
// plus named-variant selection round-trip.
TEST(AssetVariantTableResolvesAndFallsBack) {
    assets::AssetVariantTable t;
    CHECK(t.Empty());
    CHECK(t.Set("models/wolf.obj", "models/wolf_low.obj"));
    CHECK(!t.Empty());
    CHECK_EQ(t.Resolve("models/wolf.obj"), "models/wolf_low.obj");
    CHECK_EQ(t.Resolve("models/tree.obj"), "models/tree.obj"); // unlisted -> fallback
    CHECK_EQ(t.Size(), 1u);

    const std::string json = t.ToJson();
    assets::AssetVariantTable t2;
    std::string err;
    CHECK(t2.LoadJson(json, &err));
    CHECK(err.empty());
    CHECK_EQ(t2.Resolve("models/wolf.obj"), "models/wolf_low.obj");
    CHECK_EQ(t2.Resolve("models/tree.obj"), "models/tree.obj");

    // A named variant selected from a variants.json document.
    const char* doc = R"({"mobile":{"models/wolf.obj":"models/wolf_mobile.obj"},
                          "pc":{"models/wolf.obj":"models/wolf_hi.obj"}})";
    assets::AssetVariantTable mob;
    CHECK(assets::AssetVariantTable::LoadVariant(doc, "mobile", mob, &err));
    CHECK_EQ(mob.Resolve("models/wolf.obj"), "models/wolf_mobile.obj");
    CHECK_EQ(mob.Resolve("models/tree.obj"), "models/tree.obj");
    assets::AssetVariantTable pc;
    CHECK(assets::AssetVariantTable::LoadVariant(doc, "pc", pc, &err));
    CHECK_EQ(pc.Resolve("models/wolf.obj"), "models/wolf_hi.obj");

    // Missing variant / bad shape are rejected with a message.
    assets::AssetVariantTable missing;
    CHECK(!assets::AssetVariantTable::LoadVariant(doc, "console", missing, &err));
    CHECK(!err.empty());
    assets::AssetVariantTable bad;
    std::string badErr;
    CHECK(!bad.LoadJson(R"({"a": 7})", &badErr));
    CHECK(!badErr.empty());
}

// G7-1: the "assets:/" scheme normalizes to a plain relative path.
TEST(AssetPathSchemeNormalization) {
    CHECK_EQ(assets::NormalizeAssetPath("assets:/models/x.obj"), "models/x.obj");
    CHECK_EQ(assets::NormalizeAssetPath("asset:/models/x.obj"), "models/x.obj");
    CHECK_EQ(assets::NormalizeAssetPath("models/x.obj"), "models/x.obj"); // no scheme
    CHECK_EQ(assets::NormalizeAssetPath("assets/textures/t.png"), "assets/textures/t.png");
    CHECK_EQ(assets::NormalizeAssetPath("C:/abs/x.obj"), "C:/abs/x.obj"); // absolute untouched
}
