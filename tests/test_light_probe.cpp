#include <vector>

#include "neon/neon.hpp"
#include "neon/gfx/light_probe.hpp"
#include "helpers.hpp"

using namespace neon;

TEST(LightProbeFieldCount) {
    math::AABB bounds{{-10, 0, -10}, {10, 20, 10}};
    std::vector<gfx::IrradianceProbe> probes;
    gfx::ProbeLightInput input;
    const size_t n = gfx::BuildProbeField(bounds, 4, input, probes);
    CHECK_EQ(n, 4u * 4 * 4);
    CHECK_EQ(probes.size(), n);
    // First probe sits in the lower corner of the field.
    CHECK(probes[0].pos.x >= -10.0f && probes[0].pos.x <= 10.0f);
    CHECK(probes[0].pos.y >= 0.0f && probes[0].pos.y <= 20.0f);
}

TEST(LightProbePointLightFalloff) {
    math::AABB bounds{{-10, 0, -10}, {10, 10, 10}};
    std::vector<gfx::IrradianceProbe> probes;
    gfx::ProbeLightInput input;
    input.pointLights.push_back({{8.0f, 1.0f, 0.0f}, {1, 1, 1, 1}, 4.0f, 10.0f});

    // Build a probe field with res=1: exactly one probe at the field centre.
    gfx::BuildProbeField(bounds, 1, input, probes);
    CHECK_EQ(probes.size(), 1u);
    const float centreIrr = probes[0].irradiance.x;

    // A probe far from the light should be dimmer than one right next to it.
    gfx::ProbeLightInput dark;
    dark.pointLights.push_back({{-100.0f, -100.0f, -100.0f}, {1, 1, 1, 1}, 4.0f, 10.0f});
    std::vector<gfx::IrradianceProbe> darkProbes;
    gfx::BuildProbeField(bounds, 1, dark, darkProbes);
    CHECK(darkProbes[0].irradiance.x < centreIrr);
}

TEST(LightProbeUniformFieldSampling) {
    // A field with no lights and no sun: only flat sky irradiance everywhere.
    math::AABB bounds{{0, 0, 0}, {8, 8, 8}};
    std::vector<gfx::IrradianceProbe> probes;
    gfx::ProbeLightInput input;
    input.sunIntensity = 0.0f; // kill the directional term
    input.skyIrradiance = {0.2f, 0.3f, 0.4f, 1.0f};
    gfx::BuildProbeField(bounds, 2, input, probes);

    math::Vec3 irr = gfx::SampleProbeField(probes, 2, bounds, {3.9f, 3.9f, 3.9f});
    CHECK_NEAR(irr.x, 0.2f, 1e-5);
    CHECK_NEAR(irr.y, 0.3f, 1e-5);
    CHECK_NEAR(irr.z, 0.4f, 1e-5);

    // Sampling outside the field clamps to the nearest probe (still the flat sky).
    math::Vec3 out = gfx::SampleProbeField(probes, 2, bounds, {-50.0f, -50.0f, -50.0f});
    CHECK_NEAR(out.x, 0.2f, 1e-5);

    // Empty / malformed field returns black.
    std::vector<gfx::IrradianceProbe> empty;
    math::Vec3 black = gfx::SampleProbeField(empty, 2, bounds, {1, 1, 1});
    CHECK_NEAR(black.x, 0.0f, 1e-6);
}
