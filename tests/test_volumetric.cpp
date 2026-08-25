#include "neon/neon.hpp"
#include "neon/gfx/volumetric.hpp"
#include "helpers.hpp"

using namespace neon;

TEST(VolumetricStepBrightAccumulates) {
    float transmittance = 1.0f;
    const float contribution =
        gfx::VolumetricStep(1.5f, gfx::kVolumetricWeight, gfx::kVolumetricDecay, transmittance);
    CHECK(contribution > 0.0f);
    CHECK(transmittance < 1.0f); // decayed
}

TEST(VolumetricStepDarkContributesNothing) {
    float transmittance = 1.0f;
    const float contribution =
        gfx::VolumetricStep(0.2f, gfx::kVolumetricWeight, gfx::kVolumetricDecay, transmittance);
    CHECK_NEAR(contribution, 0.0f, 1e-6); // below threshold -> nothing scatters
    CHECK(transmittance < 1.0f);          // decay still applies
}

TEST(VolumetricTransmittanceDecays) {
    // 20 steps of a bright ray should monotonically reduce transmittance.
    float t = 1.0f;
    float prev = t;
    for (int i = 0; i < gfx::kVolumetricSteps; ++i) {
        (void)gfx::VolumetricStep(1.2f, gfx::kVolumetricWeight, gfx::kVolumetricDecay, t);
        CHECK(t <= prev);
        prev = t;
    }
    CHECK(t > 0.0f && t < 1.0f);
}
