#include <cmath>

#include "helpers.hpp"
#include "neon/world/terrain_world.hpp"

TEST(TerrainWorldCampIsFlatAndNorthRises) {
    neon::world::TerrainWorld w({1337, 1.0f / 420.0f, {}, 300.0f});
    for (int i = 0; i < 32; ++i) {
        float a = static_cast<float>(i) * 0.196f;
        for (float r = 0.0f; r <= 40.0f; r += 10.0f)
            CHECK(std::fabs(w.Height(std::cos(a) * r, std::sin(a) * r)) < 0.6f);
    }
    CHECK(w.Height(0.0f, 800.0f) > 30.0f);
}

TEST(TerrainWorldFlattenWeightMarksRoadsAndPads) {
    neon::world::WorldFeatures features;
    neon::world::RoadSpec road;
    road.points = {{0.0f, -20.0f}, {0.0f, 20.0f}};
    road.halfWidth = 4.0f;
    road.shoulder = 2.0f;
    features.roads.push_back(road);
    neon::world::AddPad(features, {40.0f, 0.0f}, 8.0f, 6.0f);

    neon::world::TerrainWorldConfig cfg;
    cfg.seed = 1337;
    cfg.features = features;
    neon::world::TerrainWorld w(cfg);

    CHECK(w.FlattenWeight(0.0f, 0.0f) > 0.9f);   // road core
    CHECK(w.FlattenWeight(40.0f, 0.0f) > 0.9f);  // pad core
    CHECK(w.FlattenWeight(80.0f, 80.0f) < 0.01f); // open terrain
}

TEST(TerrainWorldDeterministicAndFinite) {    neon::world::TerrainWorld a({7, 1.0f / 420.0f, {}, 300.0f});
    neon::world::TerrainWorld b({7, 1.0f / 420.0f, {}, 300.0f});
    for (int i = 0; i < 200; ++i) {
        float x = static_cast<float>(i) * 37.0f - 3700.0f;
        float z = static_cast<float>(i) * -53.0f + 5300.0f;
        CHECK_NEAR(a.Height(x, z), b.Height(x, z), 1e-6);
    }
    for (float x : {-100000.0f, 0.0f, 100000.0f})
        for (float z : {-100000.0f, 0.0f, 100000.0f}) {
            CHECK(std::isfinite(a.Height(x, z)));
            auto n = a.Normal(x, z);
            CHECK(std::isfinite(n.x) && std::isfinite(n.y) && std::isfinite(n.z));
            CHECK_NEAR(n.y, 1.0f, 0.5f);
        }
}

TEST(TerrainWorldRoadStaysFlatAcross) {
    neon::world::WorldFeatures f;
    f.roads.push_back({{{0, 0}, {0, 400}}, 4.0f, 4.0f});
    neon::world::TerrainWorld w({1337, 1.0f / 420.0f, f, 300.0f});
    for (float z : {60.0f, 70.0f, 80.0f}) {
        const float h0 = w.Height(0.0f, z);
        for (float x : {-3.5f, -1.0f, 1.0f, 3.5f})
            CHECK_NEAR(w.Height(x, z), h0, 0.02f);
    }
}

TEST(TerrainWorldPoiPadFlattensToAuthoredZero) {
    neon::world::WorldFeatures f;
    neon::world::AddPad(f, {160.5f, -51.5f}, 14.0f, 12.0f);
    f.pads.back().height = 0.0f;
    neon::world::TerrainWorld w({1337, 1.0f / 420.0f, f, 300.0f});
    CHECK_NEAR(w.Height(160.5f, -51.5f), 0.0f, 0.01f);
    CHECK_NEAR(w.Height(160.5f + 5.0f, -51.5f), 0.0f, 0.01f);
}

TEST(TerrainWorldSurfaceMatchesRenderedTriangles) {
    neon::world::TerrainWorld w({1337, 1.0f / 420.0f, {}, 300.0f, 4.0f});
    // At a grid corner the triangle surface and the analytic height agree.
    CHECK_NEAR(w.SurfaceHeight(64.0f, 400.0f), w.Height(64.0f, 400.0f), 1e-4);
    // At the centre of the (00,10,01) triangle the surface is the barycentric
    // average of those three corners.
    const float h00 = w.Height(64.0f, 400.0f);
    const float h10 = w.Height(68.0f, 400.0f);
    const float h01 = w.Height(64.0f, 404.0f);
    // (65,401) is fx=fz=0.25 inside the (00,10,01) triangle.
    CHECK_NEAR(w.SurfaceHeight(65.0f, 401.0f),
               0.5f * h00 + 0.25f * h10 + 0.25f * h01, 1e-3);
}
