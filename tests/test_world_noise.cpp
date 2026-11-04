#include "helpers.hpp"
#include "neon/world/noise.hpp"

TEST(WorldNoiseDeterministic) {
    CHECK_EQ(neon::world::Hash2D(3, -7, 42), neon::world::Hash2D(3, -7, 42));
    CHECK(neon::world::Hash2D(3, -7, 42) != neon::world::Hash2D(3, -7, 43));
    CHECK_NEAR(neon::world::ValueNoise2D(1.25f, -3.5f, 7),
               neon::world::ValueNoise2D(1.25f, -3.5f, 7), 1e-9);
}

TEST(WorldNoiseRangeAndContinuity) {
    float prev = neon::world::ValueNoise2D(0.0f, 0.0f, 1);
    for (int i = 1; i <= 200; ++i) {
        float x = static_cast<float>(i) * 0.05f;
        float v = neon::world::ValueNoise2D(x, 0.0f, 1);
        CHECK(v >= -1.0001f && v <= 1.0001f);
        CHECK_NEAR(v, prev, 0.08);   // continuous
        prev = v;
    }
    float r = neon::world::Ridged2D(11.3f, -4.2f, 9, 4);
    CHECK(r >= -0.0001f && r <= 1.0001f);
    float f = neon::world::Fbm2D(11.3f, -4.2f, 9, 5, 2.0f, 0.5f);
    CHECK(f >= -1.0001f && f <= 1.0001f);
}
