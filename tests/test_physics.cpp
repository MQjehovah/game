#include <cmath>

#include "neon/neon.hpp"
#include "helpers.hpp"

using namespace neon;

// ---------------------------------------------------------------------------
// physics::World
// ---------------------------------------------------------------------------

TEST(PhysicsBallFallsUnderGravity) {
    physics::World world;
    physics::World::BodyId ball = world.AddSphere(100, {0, 5, 0}, 1.0f, true);
    const math::Vec3 gravity{0, -9.81f, 0};
    const float dt = 1.0f / 60.0f;

    // Discrete Euler: after n steps y = 5 - g*dt^2*n*(n+1)/2 (ground clamp at y=1).
    for (int i = 0; i < 10; ++i) world.Step(dt, gravity);
    CHECK_NEAR(world.GetPosition(ball).y, 4.8501, 0.01);
    CHECK(world.GetVelocity(ball).y < 0.0f);

    // Falls until it rests on the ground plane (y == radius).
    for (int i = 0; i < 200; ++i) world.Step(dt, gravity);
    CHECK_NEAR(world.GetPosition(ball).y, 1.0, 1e-5);
    CHECK_NEAR(world.GetVelocity(ball).y, 0.0, 1e-6);
    CHECK(world.IsOnGround(ball));

    // Stays grounded: does not sink or bounce.
    world.Step(dt, gravity);
    world.Step(dt, gravity);
    CHECK_NEAR(world.GetPosition(ball).y, 1.0, 1e-5);
    CHECK(world.IsOnGround(ball));
}

TEST(PhysicsBallVsStaticBox) {
    physics::World world;
    physics::World::BodyId box = world.AddBox(200, {{-2, 0, -2}, {2, 0.5f, 2}}, false);
    CHECK_NEAR(world.GetPosition(box).y, 0.25, 1e-6); // AddBox centers the body

    physics::World::BodyId ball = world.AddSphere(100, {0, 1.5f, 0}, 0.5f, true);
    world.SetVelocity(ball, {0, -2, 0});
    const math::Vec3 zero{};
    const float dt = 1.0f / 60.0f;

    bool collided = false;
    for (int i = 0; i < 120 && !collided; ++i) {
        world.Step(dt, zero);
        if (!world.Collisions().empty()) collided = true;
    }
    CHECK(collided);
    // Rests on the box top (y = boxTop + radius) with vertical velocity killed.
    CHECK_NEAR(world.GetPosition(ball).y, 1.0, 1e-5);
    CHECK_NEAR(world.GetVelocity(ball).y, 0.0, 1e-6);
    CHECK_NEAR(world.GetPosition(ball).x, 0.0, 1e-6);
    CHECK_NEAR(world.GetPosition(ball).z, 0.0, 1e-6);
    // IsOnGround tracks the y=0 ground plane, not box tops.
    CHECK(!world.IsOnGround(ball));

    // Exactly one collision event, dynamic owner first, static owner second.
    CHECK_EQ(world.Collisions().size(), 1u);
    if (world.Collisions().size() == 1u) {
        CHECK_EQ(world.Collisions()[0].first, 100u);
        CHECK_EQ(world.Collisions()[0].second, 200u);
    }
}

TEST(PhysicsBallBallSeparation) {
    physics::World world;
    physics::World::BodyId a = world.AddSphere(100, {0, 1, 0}, 1.0f, true);
    physics::World::BodyId b = world.AddSphere(200, {1.5f, 1, 0}, 1.0f, true);
    const math::Vec3 zero{};
    const float dt = 1.0f / 60.0f;

    // 0.5 overlap split evenly along the contact normal.
    world.Step(dt, zero);
    CHECK_NEAR(world.GetPosition(a).x, -0.25, 1e-5);
    CHECK_NEAR(world.GetPosition(b).x, 1.75, 1e-5);
    CHECK_NEAR(math::Distance(world.GetPosition(a), world.GetPosition(b)), 2.0, 1e-5);

    CHECK_EQ(world.Collisions().size(), 1u);
    if (world.Collisions().size() == 1u) {
        CHECK_EQ(world.Collisions()[0].first, 100u);
        CHECK_EQ(world.Collisions()[0].second, 200u);
    }
    world.ClearCollisions();
    CHECK_EQ(world.Collisions().size(), 0u);

    // After separation the spheres are just touching; stepping again must not
    // re-report a collision.
    world.Step(dt, zero);
    CHECK_EQ(world.Collisions().size(), 0u);
}

TEST(PhysicsRaycastHits) {
    physics::World world;
    physics::World::BodyId sphere = world.AddSphere(100, {0, 0, 0}, 2.0f, false);
    world.AddBox(200, {{-1, -1, -1}, {1, 1, 1}}, false);

    math::Ray ray;
    ray.origin = {0, 10, 0};
    ray.dir = {0, -1, 0};
    float t = -1.0f;
    uint64_t owner = 0;
    CHECK(world.Raycast(ray, 100.0f, t, &owner));
    CHECK_NEAR(t, 8.0, 1e-4); // sphere surface first (box face at t=9)
    CHECK_EQ(owner, 100u);

    // Static bodies must not move under gravity.
    world.Step(1.0f / 60.0f, {0, -9.81f, 0});
    CHECK_NEAR(world.GetPosition(sphere).y, 0.0, 1e-6);

    // Ray pointing away from all bodies.
    math::Ray away;
    away.origin = {0, 0, 10};
    away.dir = {0, 0, 1};
    CHECK(!world.Raycast(away, 100.0f, t, nullptr));

    // maxDist shorter than the first hit.
    math::Ray down;
    down.origin = {0, 10, 0};
    down.dir = {0, -1, 0};
    CHECK(!world.Raycast(down, 4.0f, t, nullptr));
}

TEST(PhysicsRaycastSkipsDisabledAndRemoved) {
    physics::World world;
    physics::World::BodyId ball = world.AddSphere(100, {0, 0, 0}, 2.0f, false);
    math::Ray ray;
    ray.origin = {0, 10, 0};
    ray.dir = {0, -1, 0};
    float t = -1.0f;
    CHECK(world.Raycast(ray, 100.0f, t, nullptr));

    world.SetEnabled(ball, false);
    CHECK(!world.Raycast(ray, 100.0f, t, nullptr));

    world.SetEnabled(ball, true);
    world.Remove(ball);
    CHECK(!world.Raycast(ray, 100.0f, t, nullptr));
    // Removed bodies return a zero position.
    math::Vec3 p = world.GetPosition(ball);
    CHECK_NEAR(p.x, 0.0, 1e-6);
    CHECK_NEAR(p.y, 0.0, 1e-6);
    CHECK_NEAR(p.z, 0.0, 1e-6);
}

TEST(PhysicsClearResetsWorld) {
    physics::World world;
    physics::World::BodyId a = world.AddSphere(100, {0, 0, 0}, 1.0f, true);
    CHECK(a.Valid());
    world.Clear();
    math::Ray ray;
    ray.origin = {0, 10, 0};
    ray.dir = {0, -1, 0};
    float t = -1.0f;
    CHECK(!world.Raycast(ray, 100.0f, t, nullptr));
    // Ids restart after Clear.
    physics::World::BodyId b = world.AddSphere(200, {0, 0, 0}, 1.0f, true);
    CHECK_EQ(b.id, 1u);
}
