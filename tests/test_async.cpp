#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "neon/assets/async_loader.hpp"
#include "neon/assets/bc1.hpp"
#include "neon/neon.hpp"

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"

#include "helpers.hpp"
#include "test_backend.hpp"

using namespace neon;

// Test-suite 5.2: BC1 texture compression (stb_dxt) + async texture decode.
// ---------------------------------------------------------------------------
// The MinGW 8.1 win32 toolchain has no std::thread (__STDCPP_THREADS__
// undefined), so the AsyncLoader pool uses Win32 CreateThread / POSIX pthread
// and the tests below only ever interact with it through Submit/Deliver/Pump.

namespace {

void SleepMs(int ms) {
#if defined(_WIN32)
    ::Sleep(static_cast<DWORD>(ms));
#else
    ::usleep(static_cast<useconds_t>(ms) * 1000);
#endif
}

// Pumps `assets` until `fired` reaches `target` or ~4s elapse.
void PumpUntil(neon::assets::AssetManager& assets, std::atomic<int>& fired, int target) {
    for (int i = 0; i < 4000 && fired.load() < target; ++i) {
        assets.PumpAsync();
        if (fired.load() < target) SleepMs(1);
    }
}

// Minimal BC1 decoder (test-only sanity for the compressor math). BC1 blocks
// are 8 bytes: two RGB565 endpoints + a 32-bit little-endian field of sixteen
// 2-bit pixel indices (row-major). Decodes the single pixel (px,py) in a block.
void Bc1DecodePixel(const uint8_t* block, int px, int py, uint8_t rgb[3]) {
    const uint16_t c0 = static_cast<uint16_t>(block[0]) | (static_cast<uint16_t>(block[1]) << 8);
    const uint16_t c1 = static_cast<uint16_t>(block[2]) | (static_cast<uint16_t>(block[3]) << 8);
    auto unpack = [](uint16_t c, uint8_t out[3]) {
        out[0] = static_cast<uint8_t>((((c >> 11) & 0x1F) * 255 + 15) / 31);
        out[1] = static_cast<uint8_t>((((c >> 5) & 0x3F) * 255 + 31) / 63);
        out[2] = static_cast<uint8_t>(((c & 0x1F) * 255 + 15) / 31);
    };
    uint8_t col0[3], col1[3];
    unpack(c0, col0);
    unpack(c1, col1);
    const bool fourColor = c0 > c1;
    const uint32_t bits = static_cast<uint32_t>(block[4]) |
                          (static_cast<uint32_t>(block[5]) << 8) |
                          (static_cast<uint32_t>(block[6]) << 16) |
                          (static_cast<uint32_t>(block[7]) << 24);
    const int idx = (bits >> (2 * (py * 4 + px))) & 0x3;
    if (fourColor) {
        for (int i = 0; i < 3; ++i) {
            if (idx == 0) rgb[i] = col0[i];
            else if (idx == 1) rgb[i] = col1[i];
            else if (idx == 2) rgb[i] = static_cast<uint8_t>((2 * col0[i] + col1[i]) / 3);
            else rgb[i] = static_cast<uint8_t>((col0[i] + 2 * col1[i]) / 3);
        }
    } else {
        for (int i = 0; i < 3; ++i) {
            if (idx == 0) rgb[i] = col0[i];
            else if (idx == 1) rgb[i] = col1[i];
            else rgb[i] = static_cast<uint8_t>((col0[i] + col1[i]) / 2);
        }
    }
}

// Decodes an entire BC1 image into `rgba` (RGB only; alpha is opaque).
void Bc1DecodeImage(const uint8_t* bc1, int width, int height, std::vector<uint8_t>& rgba) {
    const int bw = (width + 3) / 4;
    const int bh = (height + 3) / 4;
    rgba.assign(static_cast<size_t>(width) * static_cast<size_t>(height) * 3, 0);
    for (int by = 0; by < bh; ++by) {
        for (int bx = 0; bx < bw; ++bx) {
            const uint8_t* block = bc1 + (static_cast<size_t>(by) * bw + bx) * 8;
            for (int py = 0; py < 4; ++py) {
                for (int px = 0; px < 4; ++px) {
                    const int sx = bx * 4 + px;
                    const int sy = by * 4 + py;
                    if (sx >= width || sy >= height) continue;
                    uint8_t rgb[3];
                    Bc1DecodePixel(block, px, py, rgb);
                    uint8_t* dst = &rgba[(static_cast<size_t>(sy) * width + sx) * 3];
                    dst[0] = rgb[0];
                    dst[1] = rgb[1];
                    dst[2] = rgb[2];
                }
            }
        }
    }
}

// Backend that records compressed-upload attempts and always rejects them,
// standing in for a driver without S3TC support.
class RejectingBackend : public test::NullBackend {
public:
    int compressedUploads = 0;
    neon::gfx::TextureHandle CreateTextureCompressed(int, int, uint32_t, const void*,
                                                     size_t) override {
        ++compressedUploads;
        return {};
    }
};

} // namespace

// --- BC1 math ---------------------------------------------------------------

TEST(Bc1BlockMath) {
    CHECK_EQ(neon::assets::Bc1BlockWidth(4), 1);
    CHECK_EQ(neon::assets::Bc1BlockHeight(4), 1);
    CHECK_EQ(neon::assets::Bc1BlockWidth(8), 2);
    CHECK_EQ(neon::assets::Bc1BlockHeight(8), 2);
    CHECK_EQ(neon::assets::Bc1BlockWidth(6), 2);
    CHECK_EQ(neon::assets::Bc1BlockHeight(5), 2);
    CHECK_EQ(neon::assets::Bc1ByteSize(4, 4), 8u);   // 1x1 block x 8 bytes
    CHECK_EQ(neon::assets::Bc1ByteSize(8, 8), 32u);  // 2x2 blocks
    CHECK_EQ(neon::assets::Bc1ByteSize(6, 5), 32u);  // padded 2x2 blocks
    CHECK_EQ(neon::assets::Bc1ByteSize(1, 1), 8u);   // sub-block pixels pad to one block
    CHECK_EQ(neon::assets::Bc1ByteSize(0, 0), 0u);
}

TEST(Bc1EncodeDeterministic) {
    std::vector<uint8_t> rgba;
    for (int y = 0; y < 8; ++y)
        for (int x = 0; x < 8; ++x) {
            rgba.push_back(static_cast<uint8_t>(x * 31));
            rgba.push_back(static_cast<uint8_t>(y * 29));
            rgba.push_back(static_cast<uint8_t>((x + y) * 13));
            rgba.push_back(255);
        }
    std::vector<uint8_t> a, b;
    CHECK(neon::assets::Bc1EncodeOpaque(rgba.data(), 8, 8, a));
    CHECK(neon::assets::Bc1EncodeOpaque(rgba.data(), 8, 8, b));
    CHECK_EQ(a.size(), 32u);
    CHECK_EQ(a.size(), b.size());
    CHECK(std::memcmp(a.data(), b.data(), a.size()) == 0);
}

TEST(Bc1EncodePaddedSize) {
    // 6x5 is not a multiple of the 4x4 block grid: the padded allocation must
    // cover ceil(6/4) x ceil(5/4) = 2x2 blocks = 32 bytes.
    std::vector<uint8_t> rgba(6 * 5 * 4, 255);
    std::vector<uint8_t> out;
    CHECK(neon::assets::Bc1EncodeOpaque(rgba.data(), 6, 5, out));
    CHECK_EQ(out.size(), 32u);
}

TEST(Bc1SolidColorRoundTrip) {
    // A solid red 8x8 image: RGB565 can represent (255,0,0) exactly, so the
    // decoder must reproduce it exactly.
    std::vector<uint8_t> rgba(8 * 8 * 4, 0);
    for (size_t i = 0; i < 8 * 8; ++i) rgba[i * 4 + 0] = 255;
    std::vector<uint8_t> bc1;
    CHECK(neon::assets::Bc1EncodeOpaque(rgba.data(), 8, 8, bc1));
    std::vector<uint8_t> decoded;
    Bc1DecodeImage(bc1.data(), 8, 8, decoded);
    for (size_t i = 0; i < decoded.size(); i += 3) {
        CHECK_EQ(decoded[i + 0], 255u);
        CHECK_EQ(decoded[i + 1], 0u);
        CHECK_EQ(decoded[i + 2], 0u);
    }
}

TEST(Bc1GradientRoundTrip) {
    // A smooth gradient: BC1 is lossy, but a block-compressed reconstruction
    // must stay close to the source (bounded per-channel error) and its
    // average must track the source average - i.e. this is a real compressor,
    // not garbage.
    const int w = 32, h = 32;
    std::vector<uint8_t> rgba(static_cast<size_t>(w) * h * 4);
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            uint8_t* px = &rgba[(static_cast<size_t>(y) * w + x) * 4];
            px[0] = static_cast<uint8_t>(20 + x * 7 + y * 2);
            px[1] = static_cast<uint8_t>(40 + y * 6);
            px[2] = static_cast<uint8_t>(30 + x * 5);
            px[3] = 255;
        }
    std::vector<uint8_t> bc1;
    CHECK(neon::assets::Bc1EncodeOpaque(rgba.data(), w, h, bc1));
    std::vector<uint8_t> decoded;
    Bc1DecodeImage(bc1.data(), w, h, decoded);

    double avgErr[3] = {0, 0, 0};
    int maxErr[3] = {0, 0, 0};
    const size_t n = static_cast<size_t>(w) * h;
    for (size_t i = 0; i < n; ++i) {
        for (int c = 0; c < 3; ++c) {
            const int src = rgba[i * 4 + c];
            const int dst = decoded[i * 3 + c];
            const int err = dst > src ? dst - src : src - dst;
            avgErr[c] += err;
            if (err > maxErr[c]) maxErr[c] = err;
        }
    }
    for (int c = 0; c < 3; ++c) avgErr[c] /= static_cast<double>(n);
    for (int c = 0; c < 3; ++c) {
        CHECK(avgErr[c] < 16.0);
        CHECK(maxErr[c] < 48);
    }
}

// --- AsyncLoader pool --------------------------------------------------------

TEST(AsyncLoaderBasicPump) {
    neon::assets::AsyncLoader loader(2);
    CHECK(loader.Available());
    std::atomic<bool> workDone{false};
    std::atomic<bool> completionRan{false};
    CHECK(loader.Submit([&]() {
        workDone.store(true);
        loader.Deliver([&]() { completionRan.store(true); });
    }));
    for (int i = 0; i < 4000 && !completionRan.load(); ++i) {
        loader.Pump();
        if (!completionRan.load()) SleepMs(1);
    }
    CHECK(workDone.load());
    CHECK(completionRan.load());
    loader.Shutdown();
}

TEST(AsyncLoaderUnavailable) {
    neon::assets::AsyncLoader loader(0);
    CHECK(!loader.Available());
    CHECK(!loader.Submit([]() {}));
    loader.Shutdown(); // safe no-op
}

// --- AssetManager::LoadTextureAsync (headless, injected decode) -------------

TEST(AsyncLoadTextureCached) {
    test::HeadlessAssetFixture fx;
    std::atomic<int> decodeCalls{0};
    fx.assets.SetDecodeHook(
        [&decodeCalls](const std::string&, const neon::assets::TextureLoadOptions&) {
            ++decodeCalls;
            neon::assets::DecodedImage img;
            img.width = 4;
            img.height = 4;
            img.channels = 4;
            img.rgba.assign(4 * 4 * 4, 255);
            return img;
        });

    std::atomic<int> fired{0};
    std::atomic<bool> cbOk{false};
    fx.assets.LoadTextureAsync("fake://tex", [&](bool ok) {
        cbOk.store(ok);
        fired.fetch_add(1);
    });
    PumpUntil(fx.assets, fired, 1);
    CHECK_EQ(fired.load(), 1);
    CHECK(cbOk.load());
    CHECK_EQ(decodeCalls.load(), 1);
    CHECK_EQ(fx.assets.Textures().count("fake://tex"), 1u);
    const auto& tex = fx.assets.Textures().at("fake://tex");
    CHECK(tex.Valid());
    CHECK_EQ(tex.Width(), 4);
    CHECK_EQ(tex.Height(), 4);
}

TEST(TextureCacheKeySeparatesLoadOptions) {
    // glTF textures load with REPEAT wrapping while editor thumbnails of the
    // same image load clamped; the two must never share a cache entry (and a
    // flip variant must stay distinct too).
    test::HeadlessAssetFixture fx;
    std::atomic<int> decodeCalls{0};
    fx.assets.SetDecodeHook(
        [&decodeCalls](const std::string&, const neon::assets::TextureLoadOptions&) {
            ++decodeCalls;
            neon::assets::DecodedImage img;
            img.width = 2;
            img.height = 2;
            img.channels = 4;
            img.rgba.assign(2 * 2 * 4, 200);
            return img;
        });

    neon::assets::TextureLoadOptions clampOpts;
    neon::assets::TextureLoadOptions repeatOpts;
    repeatOpts.wrap = neon::gfx::Wrap::Repeat;
    neon::assets::TextureLoadOptions flipOpts;
    flipOpts.flipVertically = true;

    CHECK(neon::assets::AssetManager::TextureCacheKey("fake://w", clampOpts) !=
          neon::assets::AssetManager::TextureCacheKey("fake://w", repeatOpts));
    CHECK(neon::assets::AssetManager::TextureCacheKey("fake://w", clampOpts) !=
          neon::assets::AssetManager::TextureCacheKey("fake://w", flipOpts));

    CHECK(fx.assets.LoadTexture("fake://w", clampOpts).Valid());
    CHECK(fx.assets.LoadTexture("fake://w", repeatOpts).Valid());
    CHECK(fx.assets.LoadTexture("fake://w", flipOpts).Valid());
    CHECK_EQ(decodeCalls.load(), 3); // three distinct keys -> three decodes
    CHECK_EQ(fx.assets.Textures().count(
                 neon::assets::AssetManager::TextureCacheKey("fake://w", clampOpts)),
             1u);
    CHECK_EQ(fx.assets.Textures().count(
                 neon::assets::AssetManager::TextureCacheKey("fake://w", repeatOpts)),
             1u);
    CHECK_EQ(fx.assets.Textures().count(
                 neon::assets::AssetManager::TextureCacheKey("fake://w", flipOpts)),
             1u);
}

TEST(AsyncLoadTextureCachedImmediate) {
    // Once a path is in the cache, LoadTextureAsync fires cb(true) inline
    // (main thread) without touching the pool or the decode hook.
    test::HeadlessAssetFixture fx;
    fx.assets.SetDecodeHook([](const std::string&, const neon::assets::TextureLoadOptions&) {
        neon::assets::DecodedImage img;
        img.width = 2;
        img.height = 2;
        img.channels = 4;
        img.rgba.assign(2 * 2 * 4, 128);
        return img;
    });
    gfx::Texture t = fx.assets.LoadTexture("fake://cached");
    CHECK(t.Valid());

    std::atomic<int> fired{0};
    std::atomic<bool> cbOk{false};
    fx.assets.LoadTextureAsync("fake://cached", [&](bool ok) {
        cbOk.store(ok);
        fired.fetch_add(1);
    });
    CHECK_EQ(fired.load(), 1);
    CHECK(cbOk.load());
    CHECK_EQ(fx.assets.Textures().count("fake://cached"), 1u);
}

TEST(AsyncLoadTextureFails) {
    test::HeadlessAssetFixture fx;
    fx.assets.SetDecodeHook([](const std::string&, const neon::assets::TextureLoadOptions&) {
        return neon::assets::DecodedImage{}; // channels == 0 -> decode failure
    });
    std::atomic<int> fired{0};
    std::atomic<bool> cbOk{true};
    fx.assets.LoadTextureAsync("fake://broken", [&](bool ok) {
        cbOk.store(ok);
        fired.fetch_add(1);
    });
    PumpUntil(fx.assets, fired, 1);
    CHECK_EQ(fired.load(), 1);
    CHECK(!cbOk.load());
    CHECK_EQ(fx.assets.Textures().count("fake://broken"), 0u);
}

TEST(AsyncDedupeConcurrentRequests) {
    test::HeadlessAssetFixture fx;
    std::atomic<int> decodeCalls{0};
    fx.assets.SetDecodeHook(
        [&decodeCalls](const std::string&, const neon::assets::TextureLoadOptions&) {
            ++decodeCalls;
            neon::assets::DecodedImage img;
            img.width = 4;
            img.height = 4;
            img.channels = 4;
            img.rgba.assign(4 * 4 * 4, 255);
            return img;
        });

    std::atomic<int> fired{0};
    std::atomic<bool> cbOk{true};
    const auto makeCb = [&]() {
        return std::function<void(bool)>([&](bool ok) {
            if (!ok) cbOk.store(false);
            fired.fetch_add(1);
        });
    };
    fx.assets.LoadTextureAsync("fake://dup", makeCb());
    fx.assets.LoadTextureAsync("fake://dup", makeCb());
    PumpUntil(fx.assets, fired, 2);
    CHECK_EQ(fired.load(), 2);           // both callers notified
    CHECK(cbOk.load());                  // ... with ok=true
    CHECK_EQ(decodeCalls.load(), 1);     // ... but only ONE decode ran
    CHECK_EQ(fx.assets.Textures().count("fake://dup"), 1u);
}

TEST(AsyncSyncFallback) {
    // SetAsyncEnabled(false) models a pool that cannot start: LoadTextureAsync
    // degrades to a synchronous load and the callback fires inline.
    test::HeadlessAssetFixture fx;
    fx.assets.SetAsyncEnabled(false);
    fx.assets.SetDecodeHook([](const std::string&, const neon::assets::TextureLoadOptions&) {
        neon::assets::DecodedImage img;
        img.width = 2;
        img.height = 2;
        img.channels = 4;
        img.rgba.assign(2 * 2 * 4, 200);
        return img;
    });
    std::atomic<int> fired{0};
    std::atomic<bool> cbOk{false};
    fx.assets.LoadTextureAsync("fake://sync", [&](bool ok) {
        cbOk.store(ok);
        fired.fetch_add(1);
    });
    CHECK_EQ(fired.load(), 1); // inline, before LoadTextureAsync returns
    CHECK(cbOk.load());
    CHECK_EQ(fx.assets.Textures().count("fake://sync"), 1u);
}

TEST(AsyncShutdownWithPendingDecode) {
    // Destroying the AssetManager while a worker decode is still in flight must
    // join cleanly (no crash, no GL call on a dead context, no leak).
    {
        test::HeadlessAssetFixture fx;
        fx.assets.SetDecodeHook([](const std::string&, const neon::assets::TextureLoadOptions&) {
            SleepMs(80); // long enough that we tear down mid-decode
            neon::assets::DecodedImage img;
            img.width = 2;
            img.height = 2;
            img.channels = 4;
            img.rgba.assign(2 * 2 * 4, 1);
            return img;
        });
        std::atomic<int> fired{0};
        fx.assets.LoadTextureAsync("fake://pending", [&](bool) { fired.fetch_add(1); });
        fx.assets.PumpAsync(); // may drain nothing; the worker is still asleep
        // Scope exit: AssetManager::~AssetManager joins the worker pool.
    }
    // Also exercise the pool directly: pending work discarded on destruction.
    {
        neon::assets::AsyncLoader loader(1);
        (void)loader.Submit([]() { SleepMs(80); });
        // Scope exit: AsyncLoader::~AsyncLoader joins + discards.
    }
}

// --- Compression pipeline through AssetManager ------------------------------

TEST(GrayAndGrayAlphaDecodeExpandsToRgba) {
    // Real stb_image decode with NATIVE channel counts (the engine switched from
    // req_comp=4 to req_comp=0 so opaque 1/3-channel images can be BC1'd). The
    // expansion to RGBA must match stbi's req_comp=4 byte-for-byte:
    //   * 1ch gray   -> gray replicated into R/G/B, alpha=255
    //   * 2ch g+a    -> gray replicated, alpha preserved
    // Regression guard: a WIP draft read pixelCount*4 bytes from the 2-channel
    // (pixelCount*2 byte) stbi buffer - a heap over-read that also produced
    // wrong RGBA.
    test::TempDir tmp;

    const int w = 3, h = 2;
    // P5 PGM (1 channel, gray).
    {
        const std::string path = tmp.Str() + "/gray.pgm";
        std::string pgm = "P5\n3 2\n255\n";
        unsigned char gray[6] = {10, 20, 30, 40, 50, 60};
        pgm.append(reinterpret_cast<const char*>(gray), sizeof(gray));
        CHECK(test::WriteFileAll(path, pgm));
        neon::assets::DecodedImage img = neon::assets::DecodeImageFile(path, false);
        CHECK_EQ(img.channels, 4);
        CHECK_EQ(img.width, w);
        CHECK_EQ(img.height, h);
        CHECK_EQ(img.rgba.size(), 6u * 4u);
        for (int i = 0; i < 6; ++i) {
            CHECK_EQ(img.rgba[i * 4 + 0], gray[i]);
            CHECK_EQ(img.rgba[i * 4 + 1], gray[i]);
            CHECK_EQ(img.rgba[i * 4 + 2], gray[i]);
            CHECK_EQ(img.rgba[i * 4 + 3], 255u);
        }
    }
    // Gray+alpha PNG (2 channels), written via stb_image_write.
    {
        const std::string path = tmp.Str() + "/gray_alpha.png";
        unsigned char ga[6 * 2] = {10, 200, 20, 180, 30, 160, 40, 140, 50, 120, 60, 100};
        CHECK(stbi_write_png(path.c_str(), w, h, 2, ga, w * 2) != 0);
        neon::assets::DecodedImage img = neon::assets::DecodeImageFile(path, false);
        CHECK_EQ(img.channels, 4);
        CHECK_EQ(img.width, w);
        CHECK_EQ(img.height, h);
        CHECK_EQ(img.rgba.size(), 6u * 4u);
        for (int i = 0; i < 6; ++i) {
            const unsigned char g = ga[i * 2 + 0];
            const unsigned char a = ga[i * 2 + 1];
            CHECK_EQ(img.rgba[i * 4 + 0], g);
            CHECK_EQ(img.rgba[i * 4 + 1], g);
            CHECK_EQ(img.rgba[i * 4 + 2], g);
            CHECK_EQ(img.rgba[i * 4 + 3], a);
        }
        // Alpha-bearing: BC1 must NOT be produced even when requested.
        neon::assets::DecodedImage c = neon::assets::DecodeImageFile(path, true);
        CHECK(c.bc1.empty());
    }
    // 1-channel gray with compression requested DOES compress (opaque).
    {
        const std::string path = tmp.Str() + "/gray.pgm";
        neon::assets::DecodedImage img = neon::assets::DecodeImageFile(path, true);
        CHECK_EQ(img.channels, 4);
        CHECK(!img.bc1.empty());
        CHECK_EQ(img.bc1.size(), 8u); // 3x2 pads to one 4x4 block
    }
}

TEST(SyncCompressedTexturePath) {
    // Injected opaque decode + BC1 bytes -> LoadTexture(compressBc1) uploads
    // through CreateTextureCompressed (NullBackend accepts) and caches it.
    test::HeadlessAssetFixture fx;
    fx.assets.SetDecodeHook([](const std::string&, const neon::assets::TextureLoadOptions&) {
        neon::assets::DecodedImage img;
        img.width = 8;
        img.height = 8;
        img.channels = 4;
        img.rgba.assign(8 * 8 * 4, 255);
        for (size_t i = 0; i < 8 * 8; ++i) img.rgba[i * 4 + 0] = 255;
        neon::assets::Bc1EncodeOpaque(img.rgba.data(), 8, 8, img.bc1);
        return img;
    });
    neon::assets::TextureLoadOptions opts;
    opts.compressBc1 = true;
    gfx::Texture tex = fx.assets.LoadTexture("fake://bc1", opts);
    CHECK(tex.Valid());
    CHECK_EQ(tex.Width(), 8);
    CHECK_EQ(tex.Height(), 8);
    CHECK_EQ(fx.assets.Textures().count("fake://bc1"), 1u);
}

TEST(CompressedUploadRejectedFallsBack) {
    // A driver without S3TC rejects the compressed upload: the asset layer
    // falls back to RGBA8, still caches a valid texture, logs once and stops
    // trying to compress for the rest of the session (the second load never
    // calls CreateTextureCompressed again).
    RejectingBackend* backend = new RejectingBackend();
    neon::gfx::Renderer renderer;
    renderer.AttachBackendForTesting(std::unique_ptr<neon::gfx::IRenderBackend>(backend));
    neon::assets::AssetManager assets;
    assets.Init(&renderer);

    // Real opaque RGB images (PPM, native 3 channels) run through the built-in
    // stbi_load decode path, so DecodeImageFile + the driver-capability latch
    // are what gets exercised - not the test decode hook.
    test::TempDir tmp;
    const std::string pathA = tmp.Str() + "/opaque_a.ppm";
    const std::string pathB = tmp.Str() + "/opaque_b.ppm";
    std::string ppm = "P6\n8 8\n255\n";
    ppm.append(static_cast<size_t>(8 * 8 * 3), static_cast<char>(200));
    CHECK(test::WriteFileAll(pathA, ppm));
    CHECK(test::WriteFileAll(pathB, ppm));

    neon::assets::TextureLoadOptions opts;
    opts.compressBc1 = true;
    gfx::Texture t1 = assets.LoadTexture(pathA, opts); // compressed -> rejected -> RGBA fallback
    CHECK(t1.Valid());
    CHECK_EQ(backend->compressedUploads, 1);

    gfx::Texture t2 = assets.LoadTexture(pathB, opts); // latch flipped: no compressed attempt
    CHECK(t2.Valid());
    CHECK_EQ(backend->compressedUploads, 1);
    CHECK_EQ(assets.Textures().count(pathA), 1u);
    CHECK_EQ(assets.Textures().count(pathB), 1u);
}
