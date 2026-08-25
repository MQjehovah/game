#include "neon/neon.hpp"
#include "neon/gfx/fog.hpp"
#include "helpers.hpp"

using namespace neon;

TEST(FogCurveMonotonicAndBounded) {
    // Fog factor is 0 at zero distance, increases toward 1, monotonic.
    CHECK_NEAR(gfx::FogFactor(0.0f, 0.02f), 0.0f, 1e-4);
    float prev = 0.0f;
    for (float d : {10.0f, 50.0f, 100.0f, 300.0f, 800.0f}) {
        const float f = gfx::FogFactor(d, 0.02f);
        CHECK(f >= prev - 1e-5f);
        CHECK(f >= 0.0f && f <= 1.0f);
        prev = f;
    }
    CHECK(gfx::FogFactor(800.0f, 0.02f) > 0.99f);
}

TEST(FogDensityScales) {
    // Higher density fogs a given distance more.
    CHECK(gfx::FogFactor(100.0f, 0.05f) > gfx::FogFactor(100.0f, 0.01f));
}
