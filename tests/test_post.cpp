#include <cmath>
#include <limits>
#include <string>

#include "neon/neon.hpp"
#include "helpers.hpp"
#include "test_backend.hpp"

using namespace neon;

// ---------------------------------------------------------------------------
// Task 3.6: HDR + bloom post-processing. Task 3.7: ACES tonemap + MSAA.
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
    // The T3.6 clamp reference (composite's uTonemapEnabled == 0 branch).
    CHECK_NEAR(gfx::ClampLdr(0.5f), 0.5f, 1e-6);
    CHECK_NEAR(gfx::ClampLdr(1.0f), 1.0f, 1e-6);
    CHECK_NEAR(gfx::ClampLdr(1.5f), 1.0f, 1e-6); // HDR > 1 saturates to white
    CHECK_NEAR(gfx::ClampLdr(9.0f), 1.0f, 1e-6);
}

TEST(AcesFilmValues) {
    // ACES fitted curve (Narkowicz): endpoints, mid-tones, and highlights.
    CHECK_NEAR(gfx::AcesFilm(0.0f), 0.0f, 1e-6);        // black stays black
    CHECK_NEAR(gfx::AcesFilm(0.5f), 0.616307f, 1e-4);   // mid-tone knee
    CHECK_NEAR(gfx::AcesFilm(1.0f), 0.803797f, 1e-4);   // diffuse white
    CHECK_NEAR(gfx::AcesFilm(2.0f), 0.914855f, 1e-4);   // highlights compress
    CHECK_NEAR(gfx::AcesFilm(10.0f), 1.0f, 1e-6);       // saturates (curve peaks > 1)
    CHECK_NEAR(gfx::AcesFilm(100.0f), 1.0f, 1e-6);
    // Inputs above 1.0 never come back as NaN or inf, and the output is always
    // in displayable [0,1].
    CHECK(std::isfinite(gfx::AcesFilm(1e4f)));
    CHECK(gfx::AcesFilm(1e4f) >= 0.0f && gfx::AcesFilm(1e4f) <= 1.0f);
}

TEST(AcesFilmNaNInfGuard) {
    // Inf HDR (half-float overflow) is clamped to the half-float max before the
    // curve -> finite, saturated output (never NaN). NaN maps to 0.
    CHECK(std::isfinite(gfx::AcesFilm(std::numeric_limits<float>::infinity())));
    CHECK_NEAR(gfx::AcesFilm(std::numeric_limits<float>::infinity()), 1.0f, 1e-6);
    CHECK(std::isfinite(gfx::AcesFilm(-std::numeric_limits<float>::infinity())));
    CHECK_NEAR(gfx::AcesFilm(std::numeric_limits<float>::quiet_NaN()), 0.0f, 1e-6);
    CHECK(std::isfinite(gfx::AcesFilm(std::numeric_limits<float>::quiet_NaN())));
    // The guard is a no-op for finite in-range values: boundary of the clamp.
    CHECK_NEAR(gfx::AcesFilm(65504.0f), gfx::AcesFilm(1e6f), 1e-6); // both clamp to max
    // Exposure path: NaN/Inf inputs through ToneMap stay finite too.
    math::Vec3 bad = gfx::ToneMap(1.0f, {std::numeric_limits<float>::infinity(), 0.5f,
                                         std::numeric_limits<float>::quiet_NaN()});
    CHECK(std::isfinite(bad.x) && std::isfinite(bad.z));
    CHECK_NEAR(bad.x, 1.0f, 1e-6);
    CHECK_NEAR(bad.z, 0.0f, 1e-6);
    CHECK_NEAR(bad.y, gfx::AcesFilm(0.5f), 1e-6); // untouched channel
    // A huge-but-finite input stays within [0,1] without NaN (overflow check).
    CHECK(std::isfinite(gfx::AcesFilm(3.4e38f)));
    CHECK(gfx::AcesFilm(3.4e38f) >= 0.0f && gfx::AcesFilm(3.4e38f) <= 1.0f);
}

TEST(AcesFilmMonotonic) {
    // Monotonic non-decreasing over the whole range (no banding / no ringing).
    const float xs[] = {0.0f, 0.25f, 0.5f, 0.75f, 1.0f, 1.5f, 2.0f, 4.0f, 8.0f, 100.0f};
    for (int i = 1; i < 10; ++i) {
        CHECK(gfx::AcesFilm(xs[i - 1]) <= gfx::AcesFilm(xs[i]));
    }
    // Neutral input stays neutral (no colour shift): ACES per-component on a
    // grey pixel keeps all three channels equal.
    math::Vec3 grey = gfx::AcesFilm({0.6f, 0.6f, 0.6f});
    CHECK_NEAR(grey.x, grey.y, 1e-6);
    CHECK_NEAR(grey.y, grey.z, 1e-6);
}

TEST(AcesFilmVec3) {
    math::Vec3 c = gfx::AcesFilm({1.0f, 0.5f, 0.25f});
    CHECK_NEAR(c.x, gfx::AcesFilm(1.0f), 1e-6);
    CHECK_NEAR(c.y, gfx::AcesFilm(0.5f), 1e-6);
    CHECK_NEAR(c.z, gfx::AcesFilm(0.25f), 1e-6);
}

TEST(ToneMapExposure) {
    // Exposure multiplies the HDR input BEFORE the curve; 1.0 is identity.
    math::Vec3 a = gfx::ToneMap(1.0f, {1.0f, 1.0f, 1.0f});
    CHECK_NEAR(a.x, gfx::AcesFilm(1.0f), 1e-6);
    // halving exposure moves the same pixel down the curve.
    math::Vec3 b = gfx::ToneMap(0.5f, {2.0f, 2.0f, 2.0f});
    CHECK_NEAR(b.x, gfx::AcesFilm(1.0f), 1e-6);
    // exposure == 0 collapses to black.
    math::Vec3 z = gfx::ToneMap(0.0f, {9.0f, 9.0f, 9.0f});
    CHECK_NEAR(z.x, 0.0f, 1e-6);
}

TEST(BloomCombine) {
    // T3.7 composite: (hdr + bloom*strength) ACES tonemapped at exposure 1.
    math::Vec3 c = gfx::BloomCombine({0.5f, 0.5f, 0.5f}, {0.2f, 0.2f, 0.2f}, 0.35f);
    CHECK_NEAR(c.x, gfx::AcesFilm(0.5f + 0.2f * 0.35f), 1e-5); // 0.657761
    CHECK_NEAR(c.y, c.x, 1e-5); // neutral stays neutral
    // HDR above 1.0 compresses instead of clipping (no flat white wash).
    math::Vec3 clipped = gfx::BloomCombine({1.5f, 1.5f, 1.5f}, {0.2f, 0.2f, 0.2f}, 0.35f);
    CHECK_NEAR(clipped.x, gfx::AcesFilm(1.57f), 1e-5); // 0.883502 < 1
    CHECK_NEAR(clipped.y, clipped.x, 1e-5);
    // No bloom (uBloomEnabled == 0 path): pure tonemap of the HDR term.
    math::Vec3 noBloom = gfx::BloomCombine({1.4f, 0.7f, 0.3f}, {0.0f, 0.0f, 0.0f}, 0.35f);
    CHECK_NEAR(noBloom.x, gfx::AcesFilm(1.4f), 1e-5); // 0.866080
    CHECK_NEAR(noBloom.y, gfx::AcesFilm(0.7f), 1e-5); // 0.717383
    CHECK_NEAR(noBloom.z, gfx::AcesFilm(0.3f), 1e-5); // 0.438492
    // Exposure feeds through the combine (default 1.0 stays unchanged).
    math::Vec3 e = gfx::BloomCombine({1.0f, 1.0f, 1.0f}, {0.0f, 0.0f, 0.0f}, 0.0f, 0.5f);
    CHECK_NEAR(e.x, gfx::AcesFilm(0.5f), 1e-6);
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
    // T3.7 ACES tonemap: the curve + exposure live in the composite shader, and
    // the legacy clamp is only the uTonemapEnabled == 0 reference branch.
    CHECK(composite.find("uExposure") != std::string::npos);
    CHECK(composite.find("uTonemapEnabled") != std::string::npos);
    CHECK(composite.find("ACESFilm") != std::string::npos);
    CHECK(composite.find("2.51") != std::string::npos);
    CHECK(composite.find("2.43") != std::string::npos);
    CHECK(composite.find("min(c, vec3(1.0))") != std::string::npos); // clamp reference kept

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

// --- T3.7 tonemap / MSAA state on the headless backend ----------------------
// The ACES math is pure (tested above); the GPU side (composite uniforms, MSAA
// FBO + resolve) is capability-gated like CSM: on the NullBackend the HDR
// pipeline never engages, so requesting/setting these must be inert no-crash.

TEST(TonemapAndMsaaStateHeadless) {
    test::HeadlessAssetFixture fx;
    CHECK(!fx.renderer.HdrEnabled());
    CHECK(!fx.renderer.MsaaEnabled()); // no GL -> MSAA capability never tested
    CHECK(fx.renderer.TonemapEnabled()); // default on
    CHECK_NEAR(fx.renderer.Exposure(), 1.0f, 1e-6); // identity default

    // User toggles round-trip without GL.
    fx.renderer.SetExposure(1.5f);
    CHECK_NEAR(fx.renderer.Exposure(), 1.5f, 1e-6);
    fx.renderer.SetTonemapEnabled(false);
    CHECK(!fx.renderer.TonemapEnabled());
    fx.renderer.SetTonemapEnabled(true);
    fx.renderer.SetMsaaEnabled(false);
    fx.renderer.SetMsaaEnabled(true);

    // A full frame with the toggles set must not crash (HDR stays inert).
    fx.renderer.BeginFrame({0.02f, 0.03f, 0.08f, 1.0f});
    gfx::Mesh cube = gfx::Mesh::CreateCube(fx.renderer, 1, 1, 1, "tonemap_msaa_test_cube");
    fx.renderer.DrawMesh(cube, gfx::Material::Lit({}), math::Mat4::Identity());
    fx.renderer.DrawSky();
    fx.renderer.EndFrame();

    // Same-frame tonemap comparison needs the HDR pipeline: off on NullBackend.
    std::vector<uint8_t> clamped, tonemapped;
    fx.renderer.BeginFrame({0, 0, 0, 1});
    CHECK(!fx.renderer.CaptureTonemapComparison(clamped, tonemapped));
    CHECK(clamped.empty() && tonemapped.empty());
    fx.renderer.EndFrame();
    CHECK(!fx.renderer.MsaaEnabled());
}

// 2D canvas camera: Set2DViewport maps the 1280x720 design space into a
// screen rect with fit+center, zoom (around the design center) and pan (the
// design point at the rect center). ToScreen/ScreenToUI must round-trip so
// editor clicks and playtest InputMousePos stay aligned with the canvas.
TEST(Set2DViewportZoomPanMapping) {
    test::HeadlessAssetFixture fx;
    const float vpx = 100.0f, vpy = 50.0f, vpw = 1000.0f, vph = 600.0f;

    // Fit: design center maps to the viewport rect center.
    fx.renderer.Set2DViewport(vpx, vpy, vpw, vph);
    math::Vec2 s = fx.renderer.ToScreen({640.0f, 360.0f});
    CHECK_NEAR(s.x, vpx + vpw * 0.5f, 1e-3f);
    CHECK_NEAR(s.y, vpy + vph * 0.5f, 1e-3f);

    // Zoom around the design center: the center stays put, corners move out.
    fx.renderer.Set2DViewport(vpx, vpy, vpw, vph, 2.0f);
    s = fx.renderer.ToScreen({640.0f, 360.0f});
    CHECK_NEAR(s.x, vpx + vpw * 0.5f, 1e-3f);
    CHECK_NEAR(s.y, vpy + vph * 0.5f, 1e-3f);
    const math::Vec2 corner = fx.renderer.ToScreen({0.0f, 0.0f});
    CHECK_NEAR(corner.x, vpx + vpw * 0.5f - 640.0f * fx.renderer.UIScale(), 1e-3f);
    CHECK_NEAR(corner.y, vpy + vph * 0.5f - 360.0f * fx.renderer.UIScale(), 1e-3f);

    // Pan: the design point (640 + pan) sits at the rect center.
    fx.renderer.Set2DViewport(vpx, vpy, vpw, vph, 1.0f, {200.0f, 100.0f});
    s = fx.renderer.ToScreen({840.0f, 460.0f});
    CHECK_NEAR(s.x, vpx + vpw * 0.5f, 1e-3f);
    CHECK_NEAR(s.y, vpy + vph * 0.5f, 1e-3f);

    // Round-trip with zoom+pan: ScreenToUI(ToScreen(p)) == p.
    fx.renderer.Set2DViewport(vpx, vpy, vpw, vph, 1.7f, {-85.0f, 320.0f});
    const math::Vec2 p{123.5f, 456.25f};
    const math::Vec2 r = fx.renderer.ScreenToUI(fx.renderer.ToScreen(p));
    CHECK_NEAR(r.x, p.x, 1e-3f);
    CHECK_NEAR(r.y, p.y, 1e-3f);

    // 1:1 pixel mapping (3D overlay): design pixel == screen pixel, origin at
    // the given point; round-trip stays exact.
    fx.renderer.Set2DViewportPixels(350.0f, 60.0f);
    s = fx.renderer.ToScreen({14.0f, 20.0f});
    CHECK_NEAR(s.x, 364.0f, 1e-3f);
    CHECK_NEAR(s.y, 80.0f, 1e-3f);
    const math::Vec2 r2 = fx.renderer.ScreenToUI(fx.renderer.ToScreen({777.0f, 123.0f}));
    CHECK_NEAR(r2.x, 777.0f, 1e-3f);
    CHECK_NEAR(r2.y, 123.0f, 1e-3f);
}
