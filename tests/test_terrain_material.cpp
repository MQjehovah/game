#include <memory>

#include "helpers.hpp"
#include "neon/gfx/renderer.hpp"
#include "test_backend.hpp"

TEST(TerrainMaterialDefaultsAndArrayTexture) {
    neon::gfx::Renderer renderer;
    renderer.AttachBackendForTesting(std::make_unique<test::NullBackend>());
    neon::gfx::TextureDesc desc;
    desc.width = 4;
    desc.height = 4;
    desc.layers = 8;
    unsigned char px[4 * 4 * 8 * 4] = {};
    desc.rgba = px;
    auto tex = renderer.CreateTexture(desc);
    CHECK(tex.Valid());

    neon::gfx::Material m;
    CHECK_EQ(m.terrainLayers[0], 0);
    CHECK_EQ(m.terrainLayers[3], 3);
    CHECK_NEAR(m.terrainUvScale[0], 0.25, 1e-6);
    m.terrainAlbedoArray = tex.Handle();
    CHECK(m.terrainAlbedoArray.Valid());
}
