#include <cmath>
#include <fstream>
#include <iterator>
#include <string>

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

TEST(WorldFeaturesJsonLoadsRoadsAndPads) {
    std::ifstream in("projects/rv/assets/data/world_features.json");
    CHECK(in.good());
    std::string text((std::istreambuf_iterator<char>(in)),
                     std::istreambuf_iterator<char>());
    auto f = neon::world::LoadWorldFeatures(text);
    CHECK(f.roads.size() >= 12u);
    CHECK(f.pads.size() >= 9u);
    for (const auto& r : f.roads) {
        CHECK(r.points.size() >= 2u);
        for (const auto& p : r.points) CHECK(std::isfinite(p.x) && std::isfinite(p.y));
    }
    for (const auto& p : f.pads) {
        CHECK(std::isfinite(p.center.x) && std::isfinite(p.center.y));
        CHECK(p.radius > 0.0f);
        CHECK(std::isfinite(p.height));
    }
}

TEST(FlattenOverlapBlendsWithoutStep) {
    using namespace neon::world;
    WorldFeatures f;
    f.roads.push_back({{{0, 0}, {0, 200}}, 12.0f, 0.0f});
    AddPad(f, {0.0f, 50.0f}, 12.0f, 10.0f);
    f.pads.back().height = 5.0f;
    auto flat0 = [](float, float) { return 0.0f; };
    float prev = SampleFlattening(f, 0.0f, 20.0f, flat0).height;
    for (float z = 20.5f; z <= 80.0f; z += 0.5f) {
        const float h = SampleFlattening(f, 0.0f, z, flat0).height;
        CHECK_NEAR(h, prev, 0.6);
        CHECK(h >= -0.001f && h <= 5.001f);
        prev = h;
    }
}
