#include <memory>

#include "helpers.hpp"
#include "neon/gfx/terrain_streamer.hpp"
#include "test_backend.hpp"

TEST(TerrainStreamerResidencyBudgetAndHysteresis) {
    neon::gfx::Renderer renderer;
    renderer.AttachBackendForTesting(std::make_unique<test::NullBackend>());
    neon::world::TerrainWorld world({1337, 1.0f / 420.0f, {}, 300.0f});
    neon::gfx::TerrainStreamer s;
    neon::gfx::TerrainStreamerConfig cfg;
    cfg.maxLevel = 2;
    cfg.viewDistance = 128.0f;
    cfg.maxResident = 40;
    s.Configure(&world, cfg);

    for (int i = 0; i < 60; ++i) s.Update(renderer, {0, 0, 0}, 0.016f);
    CHECK(s.Resident() > 0u);
    CHECK(s.Resident() <= cfg.maxResident);

    const size_t before = s.Resident();
    for (int i = 0; i < 3; ++i) s.Update(renderer, {0, 0, 0}, 0.016f);
    CHECK_EQ(s.Resident(), before);  // focus unchanged -> no rebuild

    for (int i = 0; i < 60; ++i) s.Update(renderer, {4096, 0, 0}, 0.016f);
    CHECK(s.Resident() <= cfg.maxResident);
    CHECK(s.Resident() > 0u);
}

TEST(TerrainStreamerCullsOffscreenChunks) {
    test::HeadlessAssetFixture fx;
    neon::world::TerrainWorld world({1337, 1.0f / 420.0f, {}, 300.0f});
    neon::gfx::TerrainStreamer s;
    neon::gfx::TerrainStreamerConfig cfg;
    cfg.maxLevel = 2;
    cfg.viewDistance = 128.0f;
    cfg.maxResident = 40;
    cfg.cells = 8;
    s.Configure(&world, cfg);
    for (int i = 0; i < 60; ++i) s.Update(fx.renderer, {0, 0, 0}, 0.016f);
    CHECK(s.Resident() > 0u);

    neon::gfx::Material mat;
    neon::gfx::Camera down;
    down.position = {0.0f, 120.0f, 0.0f};
    down.target = {0.0f, 0.0f, 0.0f};
    fx.renderer.SetCamera(down, 1.0f);
    s.Draw(fx.renderer, down.ViewProjection(1.0f), mat);
    const size_t lookingDown = s.DrawnLastFrame();

    neon::gfx::Camera up;
    up.position = {0.0f, 120.0f, 0.0f};
    up.target = {0.0f, 240.0f, 0.0f};
    fx.renderer.SetCamera(up, 1.0f);
    s.Draw(fx.renderer, up.ViewProjection(1.0f), mat);
    const size_t lookingUp = s.DrawnLastFrame();

    CHECK(lookingDown > 0u);
    CHECK_EQ(lookingUp, 0u);
}
