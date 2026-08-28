#include <atomic>
#include <string>

#include "neon/core/object_pool.hpp"
#include "neon/neon.hpp"

#include "helpers.hpp"
#include "test_backend.hpp"

using namespace neon;

// Test-suite P0-3: resource lifecycle (refcounted asset cache + deferred GPU
// reclaim) and the fixed-capacity object pool.
// ---------------------------------------------------------------------------

TEST(ObjectPoolBasic) {
    core::ObjectPool<int, 4> pool;
    CHECK_EQ(pool.Capacity(), 4u);
    CHECK_EQ(pool.Count(), 0u);

    int* a = nullptr;
    int* b = nullptr;
    int* c = nullptr;
    int* d = nullptr;
    int* e = nullptr;
    core::ObjectPool<int, 4>::Index ia = 0, ib = 0, ic = 0, id = 0;
    CHECK(pool.Acquire(ia, a));
    CHECK(pool.Acquire(ib, b));
    CHECK(pool.Acquire(ic, c));
    CHECK(pool.Acquire(id, d));
    CHECK(!pool.Acquire(e));  // exhausted
    CHECK_EQ(pool.Count(), 4u);

    *a = 10;
    *b = 20;
    *c = 30;
    *d = 40;

    pool.Release(ib);  // slot holding 20
    CHECK_EQ(pool.Count(), 3u);
    int* f = nullptr;
    core::ObjectPool<int, 4>::Index iff = 0;
    CHECK(pool.Acquire(iff, f));  // reuses the freed slot
    *f = 99;
    CHECK_EQ(*f, 99);
    CHECK_EQ(iff, ib);
    CHECK_EQ(pool.Count(), 4u);

    // Used slot contents stay intact across release/reacquire; verify the
    // remaining live slots are untouched.
    CHECK_EQ(*a, 10);
    CHECK_EQ(*c, 30);
    CHECK_EQ(*d, 40);

    pool.Clear();
    CHECK_EQ(pool.Count(), 0u);
    CHECK(pool.Acquire(a));
}

TEST(ObjectPoolReleaseOnlyLive) {
    core::ObjectPool<int, 3> pool;
    int* a = nullptr;
    int* b = nullptr;
    int* c = nullptr;
    pool.Acquire(a);
    pool.Acquire(b);
    pool.Acquire(c);
    pool.Release(2);
    pool.Release(2);  // double release is a safe no-op
    CHECK_EQ(pool.Count(), 2u);
    pool.Release(5);  // out of range is a safe no-op
    CHECK_EQ(pool.Count(), 2u);
}

// --- AssetManager refcount + deferred reclaim (headless) --------------------

namespace {

void SetWhiteDecodeHook(neon::assets::AssetManager& assets, std::atomic<int>& decodes) {
    assets.SetDecodeHook([&decodes](const std::string&,
                                    const neon::assets::TextureLoadOptions&) {
        ++decodes;
        neon::assets::DecodedImage img;
        img.width = 2;
        img.height = 2;
        img.channels = 4;
        img.rgba.assign(2 * 2 * 4, 255);
        return img;
    });
}

} // namespace

TEST(AssetRefCountTexture) {
    test::HeadlessAssetFixture fx;
    std::atomic<int> decodes{0};
    SetWhiteDecodeHook(fx.assets, decodes);

    gfx::Texture t1 = fx.assets.LoadTexture("fake://tex");
    CHECK(t1.Valid());
    CHECK_EQ(fx.assets.TextureRefCount("fake://tex"), 1u);
    CHECK_EQ(decodes.load(), 1);

    gfx::Texture t2 = fx.assets.LoadTexture("fake://tex");
    CHECK_EQ(fx.assets.TextureRefCount("fake://tex"), 2u);
    CHECK_EQ(decodes.load(), 1);  // cache hit, no re-decode

    fx.assets.ReleaseTexture("fake://tex");
    CHECK_EQ(fx.assets.TextureRefCount("fake://tex"), 1u);
    fx.assets.ReleaseTexture("fake://tex");
    CHECK_EQ(fx.assets.TextureRefCount("fake://tex"), 0u);
    CHECK_EQ(fx.assets.RetiredTextureCount(), 1u);

    // Deferred reclaim: still pending after one frame, destroyed after the
    // 2-frame deferral window elapses.
    fx.assets.PumpAsync();
    CHECK_EQ(fx.assets.RetiredTextureCount(), 1u);
    CHECK_EQ(fx.assets.Stats().reclaimedTextures, 0u);
    fx.assets.PumpAsync();
    CHECK_EQ(fx.assets.RetiredTextureCount(), 0u);
    CHECK_EQ(fx.assets.Stats().reclaimedTextures, 1u);
    CHECK_EQ(fx.assets.Textures().count("fake://tex"), 0u);
}

TEST(AssetAcquireRevivesRetired) {
    test::HeadlessAssetFixture fx;
    std::atomic<int> decodes{0};
    SetWhiteDecodeHook(fx.assets, decodes);

    fx.assets.AcquireTexture("fake://tex");
    CHECK_EQ(fx.assets.TextureRefCount("fake://tex"), 1u);
    fx.assets.ReleaseTexture("fake://tex");
    CHECK_EQ(fx.assets.RetiredTextureCount(), 1u);

    // Re-acquire before the reclaim window: the GPU data is revived, no re-decode.
    gfx::Texture tex = fx.assets.AcquireTexture("fake://tex");
    CHECK(tex.Valid());
    CHECK_EQ(fx.assets.RetiredTextureCount(), 0u);
    CHECK_EQ(fx.assets.TextureRefCount("fake://tex"), 1u);
    CHECK_EQ(decodes.load(), 1);

    fx.assets.PumpAsync();
    fx.assets.PumpAsync();
    fx.assets.PumpAsync();
    CHECK_EQ(fx.assets.Stats().reclaimedTextures, 0u);  // never retired again
}

TEST(AssetChunkRefsAcquireRelease) {
    test::HeadlessAssetFixture fx;
    std::atomic<int> decodes{0};
    SetWhiteDecodeHook(fx.assets, decodes);

    std::vector<std::string> refs = {"assets/tex/a.png", "obj:assets/mesh/b.obj", "terrain",
                                     "gltf:assets/mesh/c.gltf"};
    // "obj:" acquisition hits a real file path - use a path that fails to load
    // (still exercises the parse path without a GPU mesh); procedural keys and
    // glTF keys are skipped.
    refs[1] = "obj:missing/file.obj";

    const size_t held = fx.assets.AcquireChunkAssets(refs);
    CHECK_EQ(held, 1u);  // only the texture acquires successfully
    CHECK_EQ(fx.assets.TextureRefCount("assets/tex/a.png"), 1u);
    CHECK_EQ(fx.assets.MeshRefCount("missing/file.obj"), 0u);

    fx.assets.ReleaseChunkAssets(refs);
    CHECK_EQ(fx.assets.TextureRefCount("assets/tex/a.png"), 0u);
    CHECK_EQ(fx.assets.RetiredTextureCount(), 1u);
}

// A9 (2026-08-28): chunk release must free EVERY cached variant of a path.
// glTF loads textures with Repeat wrap (suffixed cache key); releasing only
// the default-opts key used to leave the variant referenced forever.
TEST(AssetChunkReleaseCoversVariants) {
    test::HeadlessAssetFixture fx;
    std::atomic<int> decodes{0};
    SetWhiteDecodeHook(fx.assets, decodes);

    // Load the same path under two option variants (plain + Repeat wrap).
    neon::assets::TextureLoadOptions repeat;
    repeat.wrap = gfx::Wrap::Repeat;
    CHECK(fx.assets.AcquireTexture("assets/tex/v.png").Valid());
    CHECK(fx.assets.AcquireTexture("assets/tex/v.png", repeat).Valid());
    CHECK_EQ(decodes.load(), 2); // two cache entries, two decodes

    const std::vector<std::string> refs = {"assets/tex/v.png"};
    fx.assets.ReleaseChunkAssets(refs);
    CHECK_EQ(fx.assets.TextureRefCount("assets/tex/v.png"), 0u);            // plain key
    CHECK_EQ(fx.assets.TextureRefCount("assets/tex/v.png", repeat), 0u);    // variant key
    CHECK_EQ(fx.assets.RetiredTextureCount(), 2u);                          // both retired

    // Re-acquire revives both variants from the retire queue (no re-decode).
    const size_t held = fx.assets.AcquireChunkAssets(refs);
    CHECK_EQ(held, 2u);
    CHECK_EQ(decodes.load(), 2);
}
