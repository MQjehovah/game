#include <cmath>

#include "neon/neon.hpp"
#include "neon/nav/nav_grid.hpp"
#include "neon/script/bindings.hpp"
#include "helpers.hpp"

using namespace neon;

namespace {

bool Near(math::Vec2 a, math::Vec2 b, float eps = 0.01f) {
    return std::fabs(a.x - b.x) < eps && std::fabs(a.y - b.y) < eps;
}

} // namespace

// Empty grid: A* walks straight from corner to corner.
TEST(NavGridEmptyGridPath) {
    nav::NavGrid g = nav::NavGrid::Create(10, 10, 1.0f, {0, 0});
    CHECK(g.Valid());
    CHECK_EQ(g.Width(), 10);
    CHECK_EQ(g.Height(), 10);
    auto path = g.FindPath({0.5f, 0.5f}, {9.5f, 9.5f});
    CHECK(!path.empty());
    CHECK(Near(path.back(), {9.5f, 9.5f}));
    // 9 steps of 1 or sqrt(2): path length ~ 12.7.
    float len = 0.0f;
    math::Vec2 prev = {0.5f, 0.5f};
    for (const math::Vec2& p : path) {
        len += (p - prev).Length();
        prev = p;
    }
    CHECK(std::fabs(len - 12.7279f) < 0.1f);
}

// Wall blocks the direct route; A* must route around it.
TEST(NavGridRoutesAroundWall) {
    nav::NavGrid g = nav::NavGrid::Create(9, 5, 1.0f, {0, 0});
    for (int x = 0; x < 9; ++x) g.SetWalkable(x, 2, false); // horizontal wall
    auto path = g.FindPath({0.5f, 0.5f}, {8.5f, 0.5f});
    CHECK(!path.empty());
    CHECK(Near(path.back(), {8.5f, 0.5f}));
    for (const math::Vec2& p : path) {
        int cx = 0, cy = 0;
        CHECK(g.WorldToCell(p, &cx, &cy));
        CHECK(g.Walkable(cx, cy));
    }
    // The path must cross row 0 or 4 (never row 2).
    bool crossed = false;
    for (const math::Vec2& p : path) {
        int cx = 0, cy = 0;
        g.WorldToCell(p, &cx, &cy);
        if (cy == 0 || cy == 4) crossed = true;
    }
    CHECK(crossed);
}

// Unreachable destination returns an empty path.
TEST(NavGridUnreachable) {
    nav::NavGrid g = nav::NavGrid::Create(5, 5, 1.0f, {0, 0});
    for (int y = 0; y < 5; ++y) g.SetWalkable(2, y, false);
    CHECK(g.FindPath({0.5f, 0.5f}, {4.5f, 0.5f}).empty());
    // Start or goal outside / unwalkable also yields no path.
    CHECK(g.FindPath({-5.0f, 0.5f}, {4.5f, 0.5f}).empty());
    CHECK(g.FindPath({0.5f, 0.5f}, {2.5f, 0.5f}).empty()); // goal is the wall
}

// .navgrid.json round trip preserves geometry + walkability.
TEST(NavGridJsonRoundTrip) {
    nav::NavGrid g = nav::NavGrid::Create(6, 4, 2.0f, {-10.0f, 5.0f});
    g.SetWalkable(1, 1, false);
    g.SetWalkable(4, 2, false);
    auto json = g.ToJson();
    CHECK(json.Ok());
    auto back = nav::NavGrid::FromJson(core::JsonWriter::Write(json.Value()));
    CHECK(back.Ok());
    CHECK_EQ(back.Value().Width(), 6);
    CHECK_EQ(back.Value().Height(), 4);
    CHECK(std::fabs(back.Value().CellSize() - 2.0f) < 1e-6f);
    CHECK(Near(back.Value().Origin(), {-10.0f, 5.0f}));
    CHECK(!back.Value().Walkable(1, 1));
    CHECK(!back.Value().Walkable(4, 2));
    CHECK(back.Value().Walkable(0, 0));
    // World mapping survives the round trip.
    // Grid covers x: [-10, 2], y: [5, 13]; pick in-bounds endpoints.
    auto p = back.Value().FindPath({-9.0f, 6.0f}, {1.0f, 12.0f});
    CHECK(!p.empty());
}

// B1: the NavFindPath Lua binding wires ScriptContext::navGrid -> NavGrid and
// returns the waypoint array (or empty when no grid is wired / no path).
TEST(NavFindPathBinding) {
    auto host = script::CreateLuaHost();
    CHECK(host != nullptr);
    CHECK(host->Init());
    script::ScriptContext ctx;
    script::RegisterEngineBindings(*host, ctx);

    // Scene WITHOUT a navgrid: binding returns an empty array (no crash).
    CHECK(host->Load("function path_len() local p = NavFindPath({x=0,y=0,z=0},"
                     " {x=9,y=0,z=9}) return #p end"));
    CHECK(host->Run().Ok());
    auto noGrid = host->Call("path_len", {});
    CHECK(noGrid.Ok());
    CHECK_NEAR(noGrid.Value().number, 0.0, 1e-6);

    // Scene WITH a walled grid binding set: NavFindPath routes around the wall.
    nav::NavGrid g = nav::NavGrid::Create(9, 5, 1.0f, {0, 0});
    for (int x = 0; x < 9; ++x) g.SetWalkable(x, 2, false); // horizontal wall
    ctx.navGrid = &g;
    host->Load("function path2() local p = NavFindPath({x=0.5,y=0,z=0.5},"
               " {x=8.5,y=0,z=0.5}) return #p end");
    CHECK(host->Run().Ok());
    auto withGrid = host->Call("path2", {});
    CHECK(withGrid.Ok());
    CHECK(withGrid.Value().number > 0.0); // found a path around the wall

    // A wall-sealed start is unwalkable -> empty path.
    nav::NavGrid sealed = nav::NavGrid::Create(5, 5, 1.0f, {0, 0});
    for (int y = 0; y < 5; ++y) sealed.SetWalkable(2, y, false);
    ctx.navGrid = &sealed;
    host->Load("function p3() local p = NavFindPath({x=0.5,y=0,z=0.5},"
               " {x=4.5,y=0,z=0.5}) return #p end");
    CHECK(host->Run().Ok());
    auto noPath = host->Call("p3", {});
    CHECK(noPath.Ok());
    CHECK_NEAR(noPath.Value().number, 0.0, 1e-6);
}
