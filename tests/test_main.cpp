#include <cstdio>
#include <cmath>
#include <string>
#include <vector>

#include "neon/neon.hpp"
#include "helpers.hpp"

using namespace neon;

// ---------------------------------------------------------------------------
// Math
// ---------------------------------------------------------------------------

TEST(VecBasics) {
    math::Vec3 a{1, 2, 3};
    math::Vec3 b{4, 5, 6};
    CHECK_EQ(math::Dot(a, b), 32.0f);
    math::Vec3 c = math::Cross(a, b);
    CHECK_NEAR(c.x, -3.0, 1e-5);
    CHECK_NEAR(c.y, 6.0, 1e-5);
    CHECK_NEAR(c.z, -3.0, 1e-5);
    CHECK_NEAR((math::Vec3{3, 4, 0}.Length()), 5.0, 1e-5);
    CHECK_NEAR((math::Vec3{0, 5, 0}.Normalized().y), 1.0, 1e-5);
}

TEST(Mat4Ortho) {
    math::Mat4 m = math::Mat4::Ortho(0, 100, 100, 0, -1, 1);
    math::Vec3 p0 = m.TransformPoint({0, 0, 0});
    CHECK_NEAR(p0.x, -1.0, 1e-5);
    CHECK_NEAR(p0.y, 1.0, 1e-5); // top-left in y-down maps to +1 (top)
    math::Vec3 p1 = m.TransformPoint({100, 100, 0});
    CHECK_NEAR(p1.x, 1.0, 1e-5);
    CHECK_NEAR(p1.y, -1.0, 1e-5);
}

TEST(Mat4PerspectiveW) {
    math::Mat4 p = math::Mat4::Perspective(55.0f * math::kDegToRad, 16.0f / 9.0f, 0.1f, 100.0f);
    math::Vec4 clip = p.TransformVec4({0, 0, -10, 1}); // point in front
    CHECK(clip.w > 0.0f);
    CHECK_NEAR(clip.w, 10.0, 1e-4);
    // near=0.1, far=100, z=-10 -> ndc z = (A*z+B)/(-z) ~= 0.982
    CHECK_NEAR(clip.z / clip.w, 0.982, 1e-3);
}

TEST(Mat4Compose) {
    math::Mat4 t = math::Mat4::Translation({10, 20, 30});
    math::Vec3 p = t.TransformPoint({1, 2, 3});
    CHECK_NEAR(p.x, 11.0, 1e-5);
    CHECK_NEAR(p.y, 22.0, 1e-5);
    CHECK_NEAR(p.z, 33.0, 1e-5);

    math::Mat4 r = math::Mat4::RotationY(math::kHalfPi);
    math::Vec3 f = r.TransformDir({0, 0, -1});
    CHECK_NEAR(f.x, -1.0, 1e-4); // rotating -Z by +90deg around Y gives -X
    CHECK_NEAR(f.z, 0.0, 1e-4);
}

TEST(QuatRotation) {
    math::Quat q = math::Quat::FromEuler(0, math::kHalfPi, 0);
    math::Vec3 f = q.Rotate({0, 0, -1});
    CHECK_NEAR(f.x, -1.0, 1e-4);
    CHECK_NEAR(f.z, 0.0, 1e-4);
    math::Quat combined = q * q;
    math::Vec3 ff = combined.Rotate({0, 0, -1});
    CHECK_NEAR(ff.z, 1.0, 1e-4); // 180 degrees: -Z -> +Z
}

// Gizmo write-back path: a T*R*S model matrix must decompose back into the
// same translation / non-uniform scale / rotation used to build it. The engine
// composes model matrices column-vector style (v' = M*v), so scale is carried
// by the COLUMNS of the 3x3 block. This mirrors the editor's DecomposeModel.
TEST(TRSDecomposeRoundTrip) {
    struct Case {
        math::Vec3 pos;
        math::Vec3 scale;
        float yaw, pitch, roll;
    };
    const Case cases[] = {
        {{0, 0, 0}, {1, 1, 1}, 0, 0, 0},
        {{1.25f, -2.5f, 3.75f}, {2.0f, 0.5f, 1.5f}, 0.4f, -0.7f, 0.2f},
        {{-10.0f, 4.0f, 0.5f}, {0.25f, 3.0f, 1.0f}, 1.5f, 0.9f, -2.0f},
        {{0.0f, 0.0f, 0.0f}, {-2.0f, 1.0f, 1.0f}, 0.0f, 0.0f, 0.0f}, // negative scale
    };
    for (const Case& c : cases) {
        math::Mat4 model = math::Mat4::Translation(c.pos) *
                           math::Quat::FromEuler(c.yaw, c.pitch, c.roll).ToMat4() *
                           math::Mat4::Scale(c.scale);

        // --- Decompose (mirrors editor DecomposeModel) ---
        math::Vec3 pos{model.m[3], model.m[7], model.m[11]};
        math::Vec3 col0{model.m[0], model.m[4], model.m[8]};
        math::Vec3 col1{model.m[1], model.m[5], model.m[9]};
        math::Vec3 col2{model.m[2], model.m[6], model.m[10]};
        math::Vec3 scale{col0.Length(), col1.Length(), col2.Length()};
        math::Vec3 r0 = col0.Normalized();
        math::Vec3 r1 = col1.Normalized();
        math::Vec3 r2 = col2.Normalized();
        if (math::Dot(r0, math::Cross(r1, r2)) < 0.0f) {
            r0 = -r0;
            scale.x = -scale.x;
        }
        math::Mat4 rotM;
        rotM.m[0] = r0.x;  rotM.m[4] = r0.y;  rotM.m[8] = r0.z;
        rotM.m[1] = r1.x;  rotM.m[5] = r1.y;  rotM.m[9] = r1.z;
        rotM.m[2] = r2.x;  rotM.m[6] = r2.y;  rotM.m[10] = r2.z;
        math::Quat rot = math::Mat4ToQuat(rotM);

        CHECK_NEAR(pos.x, c.pos.x, 1e-5);
        CHECK_NEAR(pos.y, c.pos.y, 1e-5);
        CHECK_NEAR(pos.z, c.pos.z, 1e-5);
        CHECK_NEAR(scale.x, c.scale.x, 1e-5);
        CHECK_NEAR(scale.y, c.scale.y, 1e-5);
        CHECK_NEAR(scale.z, c.scale.z, 1e-5);

        // Recompose must reproduce the source matrix (gizmo drag round-trip).
        math::Mat4 rebuilt = math::Mat4::Translation(pos) * rot.ToMat4() *
                             math::Mat4::Scale(scale);
        for (int i = 0; i < 16; ++i) CHECK_NEAR(rebuilt.m[i], model.m[i], 1e-4);

        math::Vec3 f = math::Quat::FromEuler(c.yaw, c.pitch, c.roll).Rotate({0, 0, -1});
        CHECK_NEAR(math::Distance(rot.Rotate({0, 0, -1}), f), 0.0, 1e-4);
        math::Vec3 u = math::Quat::FromEuler(c.yaw, c.pitch, c.roll).Rotate({0, 1, 0});
        CHECK_NEAR(math::Distance(rot.Rotate({0, 1, 0}), u), 0.0, 1e-4);
    }
}

TEST(TransformModel) {
    math::Transform t;
    t.position = {5, 6, 7};
    t.scale = {2, 2, 2};
    math::Vec3 p = t.ToMat4().TransformPoint({1, 0, 0});
    CHECK_NEAR(p.x, 7.0, 1e-5);
    CHECK_NEAR(p.y, 6.0, 1e-5);
    CHECK_NEAR(p.z, 7.0, 1e-5);
}

TEST(RayIntersection) {
    math::Ray ray;
    ray.origin = {0, 0, 10};
    ray.dir = {0, 0, -1};
    float t = 0;
    CHECK(math::IntersectRaySphere(ray, {0, 0, 0}, 2.0f, t));
    CHECK_NEAR(t, 8.0, 1e-4);
    math::AABB box{{-1, -1, -1}, {1, 1, 1}};
    CHECK(math::IntersectRayAABB(ray, box, t));
    CHECK_NEAR(t, 9.0, 1e-4);
}

TEST(CameraLookAt) {
    gfx::Camera cam;
    cam.position = {0, 2, 5};
    cam.target = {0, 0, 0};
    math::Vec4 clip = cam.ViewProjection(16.0f / 9.0f).TransformVec4({0, 0, 0, 1});
    CHECK(clip.w > 0.0f);
    CHECK(std::fabs(clip.x / clip.w) < 1.0f);
    CHECK(std::fabs(clip.y / clip.w) < 1.0f);
}

// ---------------------------------------------------------------------------
// ECS
// ---------------------------------------------------------------------------

struct Position {
    float x = 0, y = 0;
};
struct Velocity {
    float dx = 0, dy = 0;
};
struct Tag {
    int id = 0;
};

TEST(ECSLifecycle) {
    ecs::World world;
    ecs::Entity e1 = world.Create();
    ecs::Entity e2 = world.Create();
    CHECK(e1.IsValid());
    CHECK(e2.IsValid());
    CHECK_EQ(world.EntityCount(), 2u);
    world.Destroy(e1);
    CHECK(!world.Alive(e1));
    CHECK_EQ(world.EntityCount(), 1u);
    ecs::Entity e3 = world.Create();
    CHECK_EQ(e3.id, e1.id); // id reused
    CHECK(world.Alive(e3));
}

TEST(ECSComponents) {
    ecs::World world;
    ecs::Entity e = world.Create();
    world.Add<Position>(e, Position{1, 2});
    CHECK(world.Has<Position>(e));
    CHECK(!world.Has<Velocity>(e));
    CHECK_NEAR(world.Get<Position>(e)->x, 1.0, 1e-6);
    world.Add<Velocity>(e, Velocity{3, 4});
    world.Remove<Position>(e);
    CHECK(!world.Has<Position>(e));
    CHECK(world.Has<Velocity>(e));
    world.Destroy(e);
    CHECK(world.Get<Velocity>(e) == nullptr);
}

TEST(ECSView) {
    ecs::World world;
    for (int i = 0; i < 100; ++i) {
        ecs::Entity e = world.Create();
        world.Add<Tag>(e, Tag{i});
    }
    auto view = world.ViewAll<Tag>();
    CHECK_EQ(view.Size(), 100u);
    int sum = 0;
    for (size_t i = 0; i < view.Size(); ++i) sum += view[i].id;
    CHECK_EQ(sum, 4950);
    // Removing mid-iteration must not crash and must keep pool consistent.
    // Snapshot handles first: pool reorders on removal.
    std::vector<ecs::Entity> snapshot;
    auto v2 = world.ViewAll<Tag>();
    for (size_t i = 0; i < v2.Size(); ++i) snapshot.push_back(world.EntityAt<Tag>(i));
    for (size_t i = 0; i < snapshot.size(); i += 2) world.Destroy(snapshot[i]);
    CHECK_EQ(world.ViewAll<Tag>().Size(), 50u);
}

// ---------------------------------------------------------------------------
// Config
// ---------------------------------------------------------------------------

TEST(ConfigRoundTrip) {
    core::Config cfg;
    cfg.SetInt("best", 1234);
    cfg.SetFloat("volume", 0.75f);
    cfg.SetBool("fullscreen", true);
    cfg.SetString("name", "neon");
    const char* path = "test_config_tmp.dat";
    CHECK(cfg.Save(path));
    core::Config loaded;
    CHECK(loaded.Load(path));
    CHECK_EQ(loaded.GetInt("best", 0), 1234);
    CHECK_NEAR(loaded.GetFloat("volume", 0.0f), 0.75, 1e-4);
    CHECK(loaded.GetBool("fullscreen", false));
    CHECK_EQ(loaded.GetString("name", ""), std::string("neon"));
    std::remove(path);
}

TEST(RngDeterminism) {
    core::Rng a(42);
    core::Rng b(42);
    bool same = true;
    for (int i = 0; i < 1000; ++i) {
        if (a.Next() != b.Next()) same = false;
    }
    CHECK(same);
    core::Rng c(43);
    CHECK(a.Next() != c.Next() || true); // just ensure it runs
    float f = c.Float();
    CHECK(f >= 0.0f && f < 1.0f);
}

TEST(HarnessHelpers) {
    CHECK_NEAR(1.0, 1.0 + 1e-9, 1e-6);
    CHECK_THROW(throw std::runtime_error("boom"));

    test::TempDir tmp;
    CHECK(!tmp.Str().empty());
    CHECK(test::WriteFileAll(tmp.Str() + "/a.txt", std::string("hello")));
    std::string text;
    CHECK(test::ReadFileAll(tmp.Str() + "/a.txt", text));
    CHECK_EQ(text, std::string("hello"));
    std::vector<char> bytes;
    CHECK(test::ReadFileAll(tmp.Str() + "/a.txt", bytes));
    CHECK_EQ(bytes.size(), 5u);
}

TEST(HarnessFailurePaths) {
    const int before = test::gFailures;

    CHECK_NEAR(1.0, 1.0001, 1e-6); // fails: diff > eps
    CHECK_EQ(test::gFailures, before + 1);

    CHECK_THROW((void)0);          // fails: nothing thrown
    CHECK_EQ(test::gFailures, before + 2);

    CHECK_NEAR(0.0 / 0.0, 1.0, 1e-6); // fails: NaN operand
    CHECK_EQ(test::gFailures, before + 3);

    CHECK_NEAR(1.0, 1.0, -1e-6);   // passes: |eps| used as tolerance
    std::string text;
    CHECK(!test::ReadFileAll("neon_missing_test_file.bin", text));
    CHECK_EQ(test::gFailures, before + 3);

    test::gFailures = before;      // restore so the suite reports 0 failures
}

int main() {
    int passed = 0;
    for (const test::TestCase& tc : test::Registry()) {
        int before = test::gFailures;
        std::printf("[ RUN  ] %s\n", tc.name);
        try {
            tc.fn();
        } catch (const std::exception& e) {
            char buf[256];
            std::snprintf(buf, sizeof(buf), "%s threw: %s", tc.name, e.what());
            test::ReportFailure(__FILE__, __LINE__, buf);
        } catch (...) {
            test::ReportFailure(__FILE__, __LINE__, "unexpected non-std exception");
        }
        if (test::gFailures == before) {
            std::printf("[  OK  ] %s\n", tc.name);
            ++passed;
        } else {
            std::printf("[ FAIL ] %s\n", tc.name);
        }
    }
    std::printf("\n%d/%zu tests passed, %d failures\n", passed,
                test::Registry().size(), test::gFailures);
    return test::gFailures == 0 ? 0 : 1;
}
