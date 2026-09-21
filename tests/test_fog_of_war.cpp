#include "neon/neon.hpp"
#include "neon/gfx/camera.hpp"
#include "neon/nav/nav_grid.hpp"
#include "neon/scene/fog_of_war.hpp"
#include "neon/scene/systems/hud_system.hpp"
#include "helpers.hpp"

using namespace neon;

// --- GroundPick ------------------------------------------------------------

TEST(GroundPickPerspectiveHitsGroundPlane) {
    scene::HudSystem hud;
    gfx::Camera cam;
    cam.position = {0.0f, 10.0f, 10.0f};
    cam.target = {0.0f, 0.0f, 0.0f};
    cam.up = {0.0f, 1.0f, 0.0f};
    cam.ortho = false;
    // fovY default 55deg; the exact value only scales the ray, the centre ray
    // still points straight at the target.
    hud.CaptureView(cam, 16.0f / 9.0f, 1280.0f, 720.0f);
    math::Vec3 out{};
    CHECK(hud.GroundPick({640.0f, 360.0f}, out));
    CHECK_NEAR(out.x, 0.0f, 0.5);
    CHECK_NEAR(out.y, 0.0f, 1e-4); // always on the y = 0 plane
    CHECK_NEAR(out.z, 0.0f, 0.5);
}

TEST(GroundPickOrthoReturnsWorldXY) {
    scene::HudSystem hud;
    gfx::Camera cam;
    cam.ortho = true;
    cam.orthoSize = 10.0f;
    cam.target = {5.0f, 3.0f, 0.0f};
    hud.CaptureView(cam, 2.0f, 1280.0f, 720.0f);
    math::Vec3 out{};
    CHECK(hud.GroundPick({640.0f, 360.0f}, out));
    CHECK_NEAR(out.x, 5.0f, 0.01);
    CHECK_NEAR(out.y, 3.0f, 0.01); // ortho: .y is the world Y (2D convention)
    CHECK_NEAR(out.z, 0.0f, 0.01);
}

TEST(GroundPickFailsWithoutCameraSnapshot) {
    scene::HudSystem hud;
    math::Vec3 out{};
    CHECK(!hud.GroundPick({10.0f, 10.0f}, out));
}

// --- FogOfWar --------------------------------------------------------------

TEST(FogOfWarRevealAndExploredMemory) {
    scene::FogOfWar fog;
    fog.Setup(20, 20, 1.0f, 0.0f, 0.0f, 0.5f, 0.95f, {0.0f, 0.0f, 0.1f});
    CHECK(fog.Configured());
    fog.BeginFrame();
    fog.AddSource(5.5f, 5.5f, 2.0f, nullptr);
    CHECK(fog.VisibleAt(5.5f, 5.5f));
    CHECK(!fog.VisibleAt(0.5f, 0.5f));
    CHECK(fog.VisibleAt(99.0f, 99.0f)); // outside the grid counts as visible

    std::vector<script::Draw2DCmd> cmds;
    const auto project = [](const math::Vec3& w, float& sx, float& sy) {
        sx = w.x * 10.0f;
        sy = w.z * 10.0f;
        return true;
    };
    const int tris = fog.Draw(cmds, project, 400.0f, 400.0f);
    CHECK(tris > 0);
    CHECK(!cmds.empty());
    CHECK(cmds.front().kind == script::Draw2DCmd::Kind::TriangleGradient);

    // Next frame the visible set clears but the explored memory remains, so the
    // mask still covers the previously seen area.
    fog.BeginFrame();
    CHECK(!fog.VisibleAt(5.5f, 5.5f));
    std::vector<script::Draw2DCmd> cmds2;
    CHECK(fog.Draw(cmds2, project, 400.0f, 400.0f) > 0);
}

TEST(FogOfWarLineOfSightOccludedByNavGrid) {
    nav::NavGrid grid = nav::NavGrid::Create(20, 20, 1.0f, {0.0f, 0.0f});
    CHECK(grid.Valid());
    for (int y = 0; y < 20; ++y) grid.SetWalkable(5, y, false); // wall column x=5

    scene::FogOfWar fog;
    fog.Setup(20, 20, 1.0f, 0.0f, 0.0f, 0.5f, 0.95f, {0.0f, 0.0f, 0.1f});
    fog.BeginFrame();
    fog.AddSource(1.5f, 10.5f, 20.0f, &grid);
    CHECK(fog.VisibleAt(1.5f, 10.5f));  // the source cell itself
    CHECK(fog.VisibleAt(4.5f, 10.5f));  // in front of the wall
    CHECK(!fog.VisibleAt(10.5f, 10.5f)); // behind the wall -> occluded
    CHECK(!fog.VisibleAt(15.5f, 10.5f));
}

TEST(FogOfWarClearDisablesFog) {    scene::FogOfWar fog;
    fog.Setup(4, 4, 1.0f, 0.0f, 0.0f, 0.5f, 0.95f, {0.0f, 0.0f, 0.1f});
    fog.Clear();
    CHECK(!fog.Configured());
    CHECK(fog.VisibleAt(0.5f, 0.5f)); // no grid -> fully visible
    std::vector<script::Draw2DCmd> cmds;
    CHECK_EQ(fog.Draw(cmds, [](const math::Vec3&, float&, float&) { return true; }, 100.0f,
                      100.0f),
             0);
}

// GPU mask path: BuildMask packs the grid into RGBA8 (dark RGB, alpha = 0
// visible / seenAlpha explored / unseenAlpha unseen), row-flipped for the ground
// plane's V axis, and reports "unchanged" so the caller can skip the upload.
TEST(FogOfWarBuildMaskPacksVisibility) {
    scene::FogOfWar fog;
    fog.Setup(4, 4, 1.0f, 0.0f, 0.0f, 0.5f, 0.95f, {0.0f, 0.0f, 0.0f});
    fog.BeginFrame();
    fog.AddSource(0.5f, 0.5f, 0.5f, nullptr); // reveals cell (0,0) only
    std::vector<uint8_t> rgba;
    CHECK(fog.BuildMask(rgba));
    CHECK_EQ(rgba.size(), 4u * 4u * 4u);

    // Row flip: grid row cz -> buffer row (rows-1-cz).
    auto alpha = [&](int cx, int cz) {
        const int row = 4 - 1 - cz;
        return rgba[(static_cast<size_t>(row) * 4 + cx) * 4 + 3];
    };
    CHECK_EQ(alpha(0, 0), 0);                    // visible
    CHECK_EQ(alpha(3, 3), static_cast<int>(0.95f * 255.0f)); // unseen

    // No change between calls -> BuildMask returns false (skip GPU upload).
    std::vector<uint8_t> again;
    CHECK(!fog.BuildMask(again));

    // Now explore another cell: the mask changes and is reported.
    fog.BeginFrame();
    fog.AddSource(3.5f, 3.5f, 0.5f, nullptr);
    std::vector<uint8_t> changed;
    CHECK(fog.BuildMask(changed));
    CHECK_EQ(changed.size(), rgba.size());
}
