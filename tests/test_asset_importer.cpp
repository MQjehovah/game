#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

#include "neon/neon.hpp"
#include "neon/assets/asset_importer.hpp"
#include "helpers.hpp"
#include "test_backend.hpp"

#include "stb_image_write.h"

using namespace neon;

namespace {

// Writes an NxN opaque PNG (solid RGB) — BC1-eligible.
void WriteOpaquePng(const std::string& path, int n, uint8_t r, uint8_t g, uint8_t b) {
    std::vector<uint8_t> pixels(static_cast<size_t>(n) * n * 3);
    for (size_t i = 0; i < pixels.size(); i += 3) {
        pixels[i] = r;
        pixels[i + 1] = g;
        pixels[i + 2] = b;
    }
    stbi_write_png(path.c_str(), n, n, 3, pixels.data(), n * 3);
}

// Writes an NxN RGBA PNG (alpha < 255) — NOT BC1-eligible (stays source).
void WriteAlphaPng(const std::string& path, int n) {
    std::vector<uint8_t> pixels(static_cast<size_t>(n) * n * 4);
    for (size_t i = 0; i < pixels.size(); i += 4) {
        pixels[i] = 200;
        pixels[i + 1] = 100;
        pixels[i + 2] = 50;
        pixels[i + 3] = 128;
    }
    stbi_write_png(path.c_str(), n, n, 4, pixels.data(), n * 4);
}

} // namespace

// G5-4-3: the offline importer bakes opaque textures to BC1 (cache .nbc1),
// skips alpha-bearing ones, and the AssetManager uploads the bake directly.
TEST(AssetImporterBakesTextures) {
    test::TempDir tmp;
    const std::string project = tmp.Str();
    CHECK(test::WriteFileAll(project + "/game.json", "{}"));
    // Create assets/opaque.png (bakes) and assets/alpha.png (skipped).
    const std::string assets = project + "/assets";
    std::filesystem::create_directories(assets);
    WriteOpaquePng(assets + "/opaque.png", 8, 10, 200, 30);
    WriteAlphaPng(assets + "/alpha.png", 8);

    const assets::ImportReport report = assets::ImportProjectTextures(project);
    CHECK_EQ(report.bakedCount, 1u);
    CHECK_EQ(report.skippedCount, 1u);
    CHECK(report.errors.empty());

    const std::string bakedPath = project + "/.neon/imported/assets/opaque.png.nbc1";
    int w = 0, h = 0;
    std::vector<uint8_t> bc1;
    CHECK(assets::ReadBakedTexture(bakedPath, w, h, bc1));
    CHECK_EQ(w, 8);
    CHECK_EQ(h, 8);
    CHECK(!bc1.empty()); // BC1 blocks present

    // No bake for the alpha image.
    CHECK(!assets::ReadBakedTexture(project + "/.neon/imported/assets/alpha.png.nbc1",
                                    w, h, bc1));
}

// The AssetManager uploads a pre-baked texture (bakeDir set) without needing
// the runtime decode path.
TEST(AssetManagerLoadsBakedTexture) {
    test::TempDir tmp;
    const std::string project = tmp.Str();
    const std::string assets = project + "/assets";
    std::filesystem::create_directories(assets);
    WriteOpaquePng(assets + "/tex.png", 16, 0, 0, 255);
    const assets::ImportReport report = assets::ImportProjectTextures(project);
    CHECK_EQ(report.bakedCount, 1u);

    test::HeadlessAssetFixture fx;
    fx.assets.SetTextureBakeDir(project + "/.neon/imported");
    const gfx::Texture tex = fx.assets.LoadTexture("assets/tex.png");
    CHECK(tex.Valid());
    CHECK_EQ(tex.Width(), 16);
    CHECK_EQ(tex.Height(), 16);

    // Without a bake dir the source must decode (still works).
    test::HeadlessAssetFixture fx2;
    const gfx::Texture tex2 = fx2.assets.LoadTexture(assets + "/tex.png");
    CHECK(tex2.Valid());
}

// The HUD sprite PNGs (authored via System.Drawing in tools/gif_to_sheet and
// asset-pack copies) must decode through the standard texture path — the HUD
// renders them as UI images and silently falls back to a flat color on failure.
TEST(UiCardTexturesDecode) {
    test::HeadlessAssetFixture fx;
    const gfx::Texture tray = fx.assets.LoadTexture("projects/pvz/assets/sprites/tray_bg.png");
    CHECK(tray.Valid());
    CHECK_EQ(tray.Width(), 522);
    CHECK_EQ(tray.Height(), 87);
    const gfx::Texture panel =
        fx.assets.LoadTexture("projects/pvz/assets/sprites/panel_bg.png");
    CHECK(panel.Valid());
    const gfx::Texture shovel =
        fx.assets.LoadTexture("projects/pvz/assets/sprites/shovel.png");
    CHECK(shovel.Valid());
}
