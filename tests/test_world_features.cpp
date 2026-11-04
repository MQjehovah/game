#include "helpers.hpp"
#include "neon/world/world_features.hpp"

TEST(FlattenRoadCoreAndShoulder) {
    using namespace neon::world;
    WorldFeatures f;
    f.roads.push_back({{{0, 0}, {0, 200}}, 4.0f, 3.0f});
    auto flat9 = [](float, float) { return 9.0f; };
    // Road centre: fully flattened to the centreline height.
    auto core = SampleFlattening(f, 0.0f, 50.0f, flat9);
    CHECK_NEAR(core.weight, 1.0f, 1e-4);
    CHECK_NEAR(core.height, 9.0f, 1e-4);
    // Outside the shoulder: untouched.
    auto out = SampleFlattening(f, 9.0f, 50.0f, flat9);
    CHECK_NEAR(out.weight, 0.0f, 1e-4);
}

TEST(FlattenPadFalloffMonotonicAndDeterministic) {
    using namespace neon::world;
    WorldFeatures f;
    AddPad(f, {100.0f, 100.0f}, 12.0f, 10.0f);
    auto flat7 = [](float, float) { return 7.0f; };
    float prev = 2.0f;
    for (int i = 0; i <= 20; ++i) {
        auto s = SampleFlattening(f, 100.0f + static_cast<float>(i), 100.0f, flat7);
        CHECK(s.weight <= prev + 1e-5f);
        CHECK(s.height >= 0.0f);
        prev = s.weight;
    }
    CHECK_NEAR(SampleFlattening(f, 100.0f, 100.0f, flat7).weight, 1.0f, 1e-4);
}
