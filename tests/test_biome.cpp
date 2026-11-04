#include "helpers.hpp"
#include "neon/world/biome.hpp"

TEST(BiomeWeightsNormalizedAndDeterministic) {
    for (int i = 0; i < 500; ++i) {
        float x = static_cast<float>(i % 25) * 80.0f - 1000.0f;
        float z = static_cast<float>(i / 25) * 80.0f - 800.0f;
        auto w = neon::world::SampleBiomeWeights(x, z, 1337);
        CHECK_NEAR(w.Sum(), 1.0f, 1e-4);
        for (int b = 0; b < 4; ++b) CHECK(w.w[b] >= 0.0f && w.w[b] <= 1.0f);
        auto w2 = neon::world::SampleBiomeWeights(x, z, 1337);
        for (int b = 0; b < 4; ++b) CHECK_NEAR(w.w[b], w2.w[b], 1e-9);
    }
}

TEST(BiomeCampIsGrasslandAndAllBiomesReachable) {
    using namespace neon::world;
    CHECK(DominantBiome(SampleBiomeWeights(0.0f, 0.0f, 1337)) == Biome::Grassland);
    bool seen[4] = {false, false, false, false};
    for (int z = -2000; z <= 2000; z += 100)
        for (int x = -2000; x <= 2000; x += 100)
            seen[static_cast<int>(DominantBiome(SampleBiomeWeights(
                static_cast<float>(x), static_cast<float>(z), 1337)))] = true;
    for (bool b : seen) CHECK(b);
}
