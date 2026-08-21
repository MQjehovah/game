#include <cmath>
#include <string>

#include "neon/neon.hpp"
#include "helpers.hpp"
#include "test_backend.hpp"

using namespace neon;

// ---------------------------------------------------------------------------
// Task 3.6: HDR + bloom post-processing.
// ---------------------------------------------------------------------------

// --- Pure bloom math (headless, no GL) --------------------------------------

TEST(BloomBrightPass) {
    // Below the threshold: nothing.
    CHECK_NEAR(gfx::BrightPass(0.0f, 1.0f), 0.0f, 1e-6);
    CHECK_NEAR(gfx::BrightPass(0.9f, 1.0f), 0.0f, 1e-6);
    CHECK_NEAR(gfx::BrightPass(1.0f, 1.0f), 0.0f, 1e-6);
    // At/above the threshold: the surplus only.
    CHECK_NEAR(gfx::BrightPass(1.5f, 1.0f), 0.5f, 1e-6);
    CHECK_NEAR(gfx::BrightPass(3.0f, 1.0f), 2.0f, 1e-6);
    CHECK_NEAR(gfx::BrightPass(1.25f, 0.5f), 0.75f, 1e-6);
}

TEST(BloomBrightPassVec3) {
    math::Vec3 c = gfx::BrightPass({1.2f, 0.8f, 2.0f}, gfx::kBloomThreshold);
    CHECK_NEAR(c.x, 0.2f, 1e-6);
    CHECK_NEAR(c.y, 0.0f, 1e-6);
    CHECK_NEAR(c.z, 1.0f, 1e-6);
}

TEST(BloomClampLdr) {
    CHECK_NEAR(gfx::ClampLdr(0.5f), 0.5f, 1e-6);
    CHECK_NEAR(gfx::ClampLdr(1.0f), 1.0f, 1e-6);
    CHECK_NEAR(gfx::ClampLdr(1.5f), 1.0f, 1e-6); // HDR > 1 saturates to white
    CHECK_NEAR(gfx::ClampLdr(9.0f), 1.0f, 1e-6);
}

TEST(BloomCombine) {
    // Modest strength: bloom lifts a sub-1.0 pixel but stays in range.
    math::Vec3 c = gfx::BloomCombine({0.5f, 0.5f, 0.5f}, {0.2f, 0.2f, 0.2f}, 0.35f);
    CHECK_NEAR(c.x, 0.5f + 0.2f * 0.35f, 1e-5);
    CHECK_NEAR(c.y, c.x, 1e-5);
    // A pixel already clipped to 1.0 in HDR cannot get any brighter.
    math::Vec3 clipped = gfx::BloomCombine({1.5f, 1.5f, 1.5f}, {0.2f, 0.2f, 0.2f}, 0.35f);
    CHECK_NEAR(clipped.x, 1.0f, 1e-6);
    CHECK_NEAR(clipped.y, 1.0f, 1e-6);
    // No bloom (uBloomEnabled == 0 path): pure clamp of the HDR term.
    math::Vec3 noBloom = gfx::BloomCombine({1.4f, 0.7f, 0.3f}, {0.0f, 0.0f, 0.0f}, 0.35f);
    CHECK_NEAR(noBloom.x, 1.0f, 1e-6);
    CHECK_NEAR(noBloom.y, 0.7f, 1e-6);
    CHECK_NEAR(noBloom.z, 0.3f, 1e-6);
}

TEST(BloomGaussianKernelNormalized) {
    // The 5-tap separable kernel baked into kBlurFragmentShader must sum to 1
    // (a blur must not change overall brightness).
    float sum = 0.0f;
    for (int i = 0; i < gfx::kBloomBlurTaps; ++i) sum += gfx::kBloomKernel[i];
    CHECK_NEAR(sum, 1.0f, 1e-4);
    CHECK_EQ(gfx::kBloomBlurTaps, 5);
    CHECK_NEAR(gfx::kBloomKernel[2], 0.402620f, 1e-6); // center tap
    // Symmetric: tap -k and +k share a weight.
    for (int k = 1; k <= 2; ++k)
        CHECK_NEAR(gfx::kBloomKernel[2 - k], gfx::kBloomKernel[2 + k], 1e-9);
}

// --- Shader source guards (string-token, headless) --------------------------
// The built-in post shaders live in bloom.hpp precisely so tests can pin the
// source without a GL context (same idea as the CSM math headers).

TEST(BloomShaderSourceTokens) {
    const std::string bright(gfx::kBrightPassFragmentShader);
    CHECK(bright.find("#version 330 core") != std::string::npos);
    CHECK(bright.find("uThreshold") != std::string::npos);
    CHECK(bright.find("max(c.rgb - vec3(uThreshold), vec3(0.0))") != std::string::npos);

    const std::string blur(gfx::kBlurFragmentShader);
    CHECK(blur.find("uTexelSize") != std::string::npos);
    CHECK(blur.find("uDirection") != std::string::npos);
    // Kernel taps match the tested constants (center weight present).
    CHECK(blur.find("0.402620") != std::string::npos);

    const std::string down(gfx::kDownsampleFragmentShader);
    CHECK(down.find("uSrcTexelSize") != std::string::npos);

    const std::string up(gfx::kUpsampleAddFragmentShader);
    CHECK(up.find("uHalf") != std::string::npos);
    CHECK(up.find("uQuarter") != std::string::npos);

    const std::string composite(gfx::kCompositeFragmentShader);
    CHECK(composite.find("#version 330 core") != std::string::npos);
    // Every uniform the renderer uploads for the composite is declared.
    CHECK(composite.find("uHdr") != std::string::npos);
    CHECK(composite.find("uBloom") != std::string::npos);
    CHECK(composite.find("uStrength") != std::string::npos);
    CHECK(composite.find("uBloomEnabled") != std::string::npos);
    // Provisional T3.6 clamp; T3.7 replaces it with a tonemapper.
    CHECK(composite.find("min(c, vec3(1.0))") != std::string::npos);

    const std::string vertex(gfx::kPostVertexShader);
    CHECK(vertex.find("layout(location = 0) in vec3 aPos") != std::string::npos);
    CHECK(vertex.find("layout(location = 2) in vec2 aUV") != std::string::npos);
}

// --- Renderer state on the headless backend --------------------------------
// No float-target capability test runs on the NullBackend (no GL), so the HDR
// pipeline must stay inert: BeginFrame/EndFrame draw straight to the "default"
// target exactly like before, and the toggles still track their user state.

TEST(BloomStateHeadless) {
    test::HeadlessAssetFixture fx;
    CHECK(fx.renderer.BloomEnabled()); // default on
    CHECK(!fx.renderer.HdrEnabled());  // no GL backend -> float path off

    // Begin/End with the headless backend must not crash (HDR is inert).
    gfx::Camera cam;
    fx.renderer.BeginFrame({0.02f, 0.03f, 0.08f, 1.0f});
    fx.renderer.DrawSky();
    fx.renderer.SetCamera(cam, 16.0f / 9.0f);
    gfx::Mesh cube = gfx::Mesh::CreateCube(fx.renderer, 1, 1, 1, "bloom_test_cube");
    fx.renderer.DrawMesh(cube, gfx::Material::Lit({}), math::Mat4::Identity());
    fx.renderer.DrawRect({0, 0}, {10, 10}, gfx::Color{1, 1, 1, 1}); // 2D overlay
    fx.renderer.EndFrame();

    // CaptureFrame on the headless path stays a no-op (no composite logic).
    std::vector<uint8_t> pixels;
    fx.renderer.BeginFrame({0, 0, 0, 1});
    fx.renderer.CaptureFrame(pixels);
    fx.renderer.EndFrame();
    CHECK(!pixels.empty());

    // The same-frame bloom comparison needs the HDR pipeline: off on NullBackend.
    std::vector<uint8_t> off, on;
    fx.renderer.BeginFrame({0, 0, 0, 1});
    CHECK(!fx.renderer.CaptureBloomComparison(off, on));
    CHECK(off.empty() && on.empty());
    fx.renderer.EndFrame();

    // User toggle round-trips even without GL.
    fx.renderer.SetBloomEnabled(false);
    CHECK(!fx.renderer.BloomEnabled());
    fx.renderer.SetBloomEnabled(true);
    CHECK(fx.renderer.BloomEnabled());
    fx.renderer.EndFrame(); // no crash after toggling
}

// EndScene on the legacy (non-HDR) path must be a no-op: the NullBackend has no
// float-target capability, so EndScene must not composite or crash, and the
// whole frame (3D + 2D) still reaches the "backbuffer" exactly like before HDR.
TEST(EndSceneLegacyPathNoop) {
    test::HeadlessAssetFixture fx;
    CHECK(!fx.renderer.HdrEnabled()); // no GL backend -> float path off

    fx.renderer.BeginFrame({0.02f, 0.03f, 0.08f, 1.0f});
    gfx::Mesh cube = gfx::Mesh::CreateCube(fx.renderer, 1, 1, 1, "endscene_test_cube");
    fx.renderer.DrawMesh(cube, gfx::Material::Lit({}), math::Mat4::Identity());
    // EndScene is a no-op here; 2D below must still draw fine on the backbuffer.
    fx.renderer.EndScene();
    fx.renderer.DrawRect({0, 0}, {10, 10}, gfx::Color{1, 1, 1, 1});
    fx.renderer.EndFrame();

    // Capture after EndScene must still return the final frame (composite is a
    // no-op on the legacy path; CaptureFrame just flushes 2D + reads).
    std::vector<uint8_t> pixels;
    fx.renderer.BeginFrame({0, 0, 0, 1});
    fx.renderer.EndScene();
    fx.renderer.DrawRect({0, 0}, {10, 10}, gfx::Color{1, 1, 1, 1});
    CHECK(fx.renderer.CaptureFrame(pixels));
    CHECK(!pixels.empty());
    fx.renderer.EndFrame();
}
