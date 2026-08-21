#include <cmath>
#include <vector>

#include "neon/neon.hpp"
#include "helpers.hpp"
#include "test_backend.hpp"

using namespace neon;

namespace {

const gfx::Color kTop{0.8f, 0.5f, 0.3f, 1.0f};
const gfx::Color kHorizon{0.2f, 0.1f, 0.05f, 1.0f};
constexpr float kGp = 0.65f;

gfx::Color DecodeRgb(const std::vector<uint8_t>& buf, size_t i) {
    return {buf[i] / 255.0f, buf[i + 1] / 255.0f, buf[i + 2] / 255.0f, 1.0f};
}

} // namespace

// ---------------------------------------------------------------------------
// Sky gradient environment (pure, headless).
// ---------------------------------------------------------------------------

TEST(IblSkyGradientProperties) {
    // Zenith is exactly the top color, nadir exactly the horizon color.
    const gfx::Color up = gfx::ibl::SkyColor(kTop, kHorizon, {0, 1, 0}, kGp);
    CHECK_NEAR(up.r, kTop.r, 1e-4);
    CHECK_NEAR(up.g, kTop.g, 1e-4);
    CHECK_NEAR(up.b, kTop.b, 1e-4);
    const gfx::Color down = gfx::ibl::SkyColor(kTop, kHorizon, {0, -1, 0}, kGp);
    CHECK_NEAR(down.r, kHorizon.r, 1e-4);
    CHECK_NEAR(down.g, kHorizon.g, 1e-4);
    CHECK_NEAR(down.b, kHorizon.b, 1e-4);

    // A level direction sits strictly between the horizon and top on every
    // channel (the horizon band blends smoothly, not a hard cutoff).
    const gfx::Color level = gfx::ibl::SkyColor(kTop, kHorizon, {0, 0, 1}, kGp);
    const float topC[3] = {kTop.r, kTop.g, kTop.b};
    const float horC[3] = {kHorizon.r, kHorizon.g, kHorizon.b};
    const float lvlC[3] = {level.r, level.g, level.b};
    for (int c = 0; c < 3; ++c) {
        const float lo = std::min(topC[c], horC[c]);
        const float hi = std::max(topC[c], horC[c]);
        CHECK(lvlC[c] > lo + 1e-4f && lvlC[c] < hi - 1e-4f);
    }

    // Deterministic: same input always gives the same colour.
    const gfx::Color again = gfx::ibl::SkyColor(kTop, kHorizon, {0, 0, 1}, kGp);
    CHECK_NEAR(again.r, level.r, 1e-6);
    CHECK_NEAR(again.g, level.g, 1e-6);
    CHECK_NEAR(again.b, level.b, 1e-6);

    // Gradient power 1 = pure smoothstep; a steeper power pushes the top colour
    // toward the zenith (the value at a fixed elevated direction drops).
    const gfx::Color steep =
        gfx::ibl::SkyColor(kTop, kHorizon, {0.0f, 0.2f, 0.98f}, 2.0f);
    const gfx::Color shallow =
        gfx::ibl::SkyColor(kTop, kHorizon, {0.0f, 0.2f, 0.98f}, 0.3f);
    CHECK(steep.r < shallow.r); // kTop.r < kHorizon.r, so more top colour = lower r
}

// ---------------------------------------------------------------------------
// Irradiance (diffuse convolution).
// ---------------------------------------------------------------------------

TEST(IblIrradianceUniformWhite) {
    // A uniform white environment must integrate to irradiance = 1 (the 1/pi
    // is baked into the cosine-weighted estimator), at any normal.
    const gfx::Color white{1, 1, 1, 1};
    const math::Vec3 normals[3] = {
        {0, 1, 0},
        {0.6f, 0.3f, 0.4f},
        {0, -1, 0},
    };
    for (const math::Vec3& n : normals) {
        const math::Vec3 irr = gfx::ibl::IrradianceForNormal(white, white, n, kGp);
        CHECK_NEAR(irr.x, 1.0, 1e-4);
        CHECK_NEAR(irr.y, 1.0, 1e-4);
        CHECK_NEAR(irr.z, 1.0, 1e-4);
    }
}

TEST(IblIrradianceTracksSkyBrightness) {
    // With a brighter sky above, the diffuse irradiance must grow as the
    // normal tilts from down to up (this is what shades the ground brighter
    // than a ceiling under an open sky).
    const math::Vec3 up = gfx::ibl::IrradianceForNormal(kTop, kHorizon, {0, 1, 0}, kGp);
    const math::Vec3 level = gfx::ibl::IrradianceForNormal(kTop, kHorizon, {0, 0, 1}, kGp);
    const math::Vec3 down = gfx::ibl::IrradianceForNormal(kTop, kHorizon, {0, -1, 0}, kGp);
    const float upC[3] = {up.x, up.y, up.z};
    const float levelC[3] = {level.x, level.y, level.z};
    const float downC[3] = {down.x, down.y, down.z};
    for (int c = 0; c < 3; ++c) {
        CHECK(upC[c] > levelC[c] + 0.05f);
        CHECK(levelC[c] > downC[c] + 0.05f);
    }
}

TEST(IblIrradianceMapLayoutAndDeterminism) {
    const std::vector<uint8_t> a = gfx::ibl::BuildIrradianceMap(kTop, kHorizon, kGp);
    const std::vector<uint8_t> b = gfx::ibl::BuildIrradianceMap(kTop, kHorizon, kGp);
    CHECK_EQ(a.size(), static_cast<size_t>(gfx::ibl::kEnvRows) * 4);
    CHECK(a == b); // deterministic
    // Row 0 is the nadir (horizon tint), the last row the zenith (top colour).
    const gfx::Color nadir = DecodeRgb(a, 0);
    CHECK_NEAR(nadir.r, kHorizon.r, 0.03);
    CHECK_NEAR(nadir.g, kHorizon.g, 0.03);
    CHECK_NEAR(nadir.b, kHorizon.b, 0.03);
    const gfx::Color zenith = DecodeRgb(a, a.size() - 4);
    CHECK_NEAR(zenith.r, kTop.r, 0.05);
    CHECK_NEAR(zenith.g, kTop.g, 0.05);
    CHECK_NEAR(zenith.b, kTop.b, 0.05);
    // The map is monotonic in y (brighter sky above): each channel increases
    // as the row moves from the nadir toward the zenith.
    float prevR = -1.0f, prevG = -1.0f, prevB = -1.0f;
    for (size_t i = 0; i < a.size(); i += 4) {
        const float r = a[i] / 255.0f, g = a[i + 1] / 255.0f, bl = a[i + 2] / 255.0f;
        CHECK(r >= prevR - 1.0f / 255.0f);
        CHECK(g >= prevG - 1.0f / 255.0f);
        CHECK(bl >= prevB - 1.0f / 255.0f);
        prevR = r;
        prevG = g;
        prevB = bl;
    }
    // The rendered irradiance is a pure function of n.y (the environment is a
    // vertical gradient): the map is identical for any azimuth at a given y.
    const math::Vec3 offAxis =
        gfx::ibl::IrradianceForNormal(kTop, kHorizon, {0.7f, 0.0f, 0.7f}, kGp);
    const math::Vec3 level = gfx::ibl::IrradianceForNormal(kTop, kHorizon, {0, 0, 1}, kGp);
    // MC noise is azimuth-dependent at fixed sample count; the y-only claim is
    // enforced on the map (which only ever evaluates N = {0, ny, 0}), so here
    // we only check the two estimates agree within the MC variance.
    CHECK_NEAR(offAxis.x, level.x, 0.06);
    CHECK_NEAR(offAxis.y, level.y, 0.06);
    CHECK_NEAR(offAxis.z, level.z, 0.06);
}

// ---------------------------------------------------------------------------
// Prefiltered specular (GGX convolution).
// ---------------------------------------------------------------------------

TEST(IblPrefilteredUniformWhite) {
    const gfx::Color white{1, 1, 1, 1};
    for (const float r : {0.0f, 0.5f, 1.0f}) {
        const math::Vec3 v =
            gfx::ibl::PrefilteredForReflection(white, white, {0.3f, 0.5f, 0.8f}, r, kGp);
        CHECK_NEAR(v.x, 1.0, 1e-4);
        CHECK_NEAR(v.y, 1.0, 1e-4);
        CHECK_NEAR(v.z, 1.0, 1e-4);
    }
}

TEST(IblPrefilteredDeterministicAndSmoothLimit) {
    // A perfectly smooth surface (roughness 0) reflects exactly the sky at the
    // reflection direction: up -> top, down -> horizon.
    const math::Vec3 up0 = gfx::ibl::PrefilteredForReflection(kTop, kHorizon, {0, 1, 0}, 0.0f, kGp);
    CHECK_NEAR(up0.x, kTop.r, 1e-4);
    CHECK_NEAR(up0.y, kTop.g, 1e-4);
    CHECK_NEAR(up0.z, kTop.b, 1e-4);
    const math::Vec3 down1 =
        gfx::ibl::PrefilteredForReflection(kTop, kHorizon, {0, -1, 0}, 1.0f, kGp);
    CHECK_NEAR(down1.x, kHorizon.r, 0.02);
    CHECK_NEAR(down1.y, kHorizon.g, 0.02);
    CHECK_NEAR(down1.z, kHorizon.b, 0.02);

    // Deterministic: identical inputs -> identical output.
    const math::Vec3 a =
        gfx::ibl::PrefilteredForReflection(kTop, kHorizon, {0.4f, 0.3f, 0.8f}, 0.7f, kGp);
    const math::Vec3 b =
        gfx::ibl::PrefilteredForReflection(kTop, kHorizon, {0.4f, 0.3f, 0.8f}, 0.7f, kGp);
    CHECK_NEAR(a.x, b.x, 1e-6);
    CHECK_NEAR(a.y, b.y, 1e-6);
    CHECK_NEAR(a.z, b.z, 1e-6);
}

TEST(IblPrefilteredMapLayoutAndDeterminism) {
    const std::vector<uint8_t> a = gfx::ibl::BuildPrefilteredMap(kTop, kHorizon, kGp);
    const std::vector<uint8_t> b = gfx::ibl::BuildPrefilteredMap(kTop, kHorizon, kGp);
    CHECK_EQ(a.size(), static_cast<size_t>(gfx::ibl::kRoughnessCols) *
                           static_cast<size_t>(gfx::ibl::kEnvRows) * 4);
    CHECK(a == b);
    // Row 0 is v = 0 (R.y ~ -1, nadir -> horizon tint) and the last row is
    // v = 1 (R.y ~ +1, zenith -> sky top), matching the shader's
    // v = R.y * 0.5 + 0.5 lookup. Column 0 is the lowest roughness.
    const gfx::Color nadir = DecodeRgb(a, 0);
    CHECK_NEAR(nadir.r, kHorizon.r, 0.05);
    CHECK_NEAR(nadir.g, kHorizon.g, 0.05);
    CHECK_NEAR(nadir.b, kHorizon.b, 0.05);
    const size_t lastRow =
        (static_cast<size_t>(gfx::ibl::kEnvRows) - 1) * gfx::ibl::kRoughnessCols * 4;
    const gfx::Color zenith = DecodeRgb(a, lastRow);
    CHECK_NEAR(zenith.r, kTop.r, 0.05);
    CHECK_NEAR(zenith.g, kTop.g, 0.05);
    CHECK_NEAR(zenith.b, kTop.b, 0.05);
}

// ---------------------------------------------------------------------------
// BRDF LUT (split-sum integration).
// ---------------------------------------------------------------------------

TEST(IblBrdfLutSanity) {
    // Head-on, perfectly smooth: the LUT collapses to Schlick's F = F0, i.e.
    // scale ~= 1 and bias ~= 0.
    const math::Vec2 headOn = gfx::ibl::BrdfLutValue(1.0f, 0.0f);
    CHECK_NEAR(headOn.x, 1.0, 0.05);
    CHECK_NEAR(headOn.y, 0.0, 0.05);

    // Values live in [0,1] across the domain.
    for (int i = 0; i <= 4; ++i) {
        for (int j = 0; j <= 4; ++j) {
            const math::Vec2 v = gfx::ibl::BrdfLutValue(i / 4.0f, j / 4.0f);
            CHECK(v.x >= -1e-4f && v.x <= 1.0f + 1e-4f);
            CHECK(v.y >= -1e-4f && v.y <= 1.0f + 1e-4f);
        }
    }

    // Fixed roughness: at more grazing angles (lower NoV) the F0 scale shrinks
    // and the constant Fresnel bias grows (rough surfaces lose the grazing
    // specular boost that smooth surfaces keep).
    const math::Vec2 head = gfx::ibl::BrdfLutValue(1.0f, 0.5f);
    const math::Vec2 grazing = gfx::ibl::BrdfLutValue(0.1f, 0.5f);
    CHECK(grazing.x < head.x);
    CHECK(grazing.y > head.y);

    // Determinism.
    const math::Vec2 d1 = gfx::ibl::BrdfLutValue(0.37f, 0.61f);
    const math::Vec2 d2 = gfx::ibl::BrdfLutValue(0.37f, 0.61f);
    CHECK_NEAR(d1.x, d2.x, 1e-6);
    CHECK_NEAR(d1.y, d2.y, 1e-6);
}

TEST(IblBrdfLutMapDeterministic) {
    const std::vector<uint8_t> a = gfx::ibl::BuildBrdfLut();
    const std::vector<uint8_t> b = gfx::ibl::BuildBrdfLut();
    CHECK_EQ(a.size(), static_cast<size_t>(gfx::ibl::kBrdfLutSize) *
                           static_cast<size_t>(gfx::ibl::kBrdfLutSize) * 4);
    CHECK(a == b);
}

// ---------------------------------------------------------------------------
// Renderer integration (NullBackend, headless).
// ---------------------------------------------------------------------------

TEST(IblRendererHeadlessLifecycle) {
    test::HeadlessAssetFixture fx;
    // IBL off by default setting? No - default is on; setting strength to 1 is
    // the same. Recompute must run against the NullBackend without a crash.
    fx.renderer.SetIblStrength(1.0f);
    CHECK_NEAR(fx.renderer.IblStrength(), 1.0f, 1e-6);
    CHECK(!fx.renderer.IblValid());

    fx.renderer.SetSky(kTop, kHorizon);
    CHECK(fx.renderer.IblValid()); // NullBackend CreateTexture returns valid handles

    // Same sky again: no recompute needed (accumulated delta below epsilon).
    fx.renderer.SetSky(kTop, kHorizon);
    CHECK(fx.renderer.IblValid());

    // A visibly different sky triggers a recompute.
    fx.renderer.SetSky({0.1f, 0.2f, 0.3f, 1.0f}, {0.4f, 0.5f, 0.6f, 1.0f});
    CHECK(fx.renderer.IblValid());

    // Disabling IBL skips further recomputes and keeps the flat ambient path.
    fx.renderer.SetIblStrength(0.0f);
    CHECK_NEAR(fx.renderer.IblStrength(), 0.0f, 1e-6);
    fx.renderer.SetSky({0.9f, 0.3f, 0.2f, 1.0f}, {0.1f, 0.2f, 0.9f, 1.0f});
    CHECK(fx.renderer.IblValid());
}
