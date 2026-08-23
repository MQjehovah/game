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

// ---------------------------------------------------------------------------
// Upgraded rigid-body features: dynamic boxes, restitution, friction, mass,
// damping and deterministic fixed-step simulation.
// ---------------------------------------------------------------------------

TEST(PhysicsDynamicBoxRestsOnStaticBox) {
    physics::World world;
    // Static platform: top at y=1, spans x/z [-3,3].
    world.AddBox(10, {{-3, 0, -3}, {3, 1, 3}}, false);
    // Dynamic box above it, half extents 0.5.
    physics::World::BodyId box = world.AddBox(20, {0, 2.5f, 0}, {0.5f, 0.5f, 0.5f}, true);
    world.SetVelocity(box, {0, -3, 0});

    bool collided = false;
    const float dt = 1.0f / 60.0f;
    for (int i = 0; i < 180 && !collided; ++i) {
        world.Step(dt, {0, -9.81f, 0});
        for (const auto& c : world.Collisions()) {
            if (c.first == 20u && c.second == 10u) collided = true;
        }
    }
    CHECK(collided);
    // Rests exactly on the platform top (y = 1 + halfExtent.y).
    CHECK_NEAR(world.GetPosition(box).y, 1.5, 1e-4);
    CHECK_NEAR(world.GetVelocity(box).y, 0.0, 1e-4);
}

TEST(PhysicsRestitutionBounces) {
    physics::World world;
    physics::RigidBodyDesc desc;
    desc.restitution = 0.8f;
    physics::World::BodyId ball =
        world.AddSphere(100, {0, 5, 0}, 0.5f, true, desc);
    const float dt = 1.0f / 60.0f;
    // Fall until first ground contact, then the ball must rebound upward and
    // rise well above the ground before the next impact.
    bool sawUpward = false;
    float peak = -1.0f;
    for (int i = 0; i < 240; ++i) {
        world.Step(dt, {0, -9.81f, 0});
        if (world.GetPosition(ball).y > peak) {
            peak = world.GetPosition(ball).y;
        }
        if (world.GetVelocity(ball).y > 2.0f) sawUpward = true;
    }
    CHECK(sawUpward);          // bounced back upward
    CHECK(peak > 2.5f);        // rose well above the ground after the bounce
    CHECK(peak < 5.0f);        // but lower than the drop (energy lost)
}

TEST(PhysicsSphereSphereMomentumExchange) {
    physics::World world;
    physics::RigidBodyDesc aDesc;
    aDesc.restitution = 1.0f;
    aDesc.mass = 2.0f;
    physics::World::BodyId a = world.AddSphere(100, {-1.1f, 1, 0}, 1.0f, true, aDesc);
    physics::World::BodyId b = world.AddSphere(200, {1.1f, 1, 0}, 1.0f, true, aDesc);
    world.SetVelocity(a, {2.0f, 0, 0});

    const float p0 = 2.0f * 2.0f; // a's linear momentum along x
    const float dt = 1.0f / 60.0f;
    for (int i = 0; i < 10; ++i) {
        world.Step(dt, {0, 0, 0});
        if (!world.Collisions().empty()) break;
    }
    CHECK(!world.Collisions().empty());
    const float p1 = 2.0f * world.GetVelocity(a).x + 2.0f * world.GetVelocity(b).x;
    CHECK_NEAR(p1, p0, 1e-3); // momentum conserved
    // Equal masses + full restitution => a stops, b takes the velocity.
    CHECK_NEAR(world.GetVelocity(a).x, 0.0, 1e-3);
    CHECK_NEAR(world.GetVelocity(b).x, 2.0, 1e-3);
}

TEST(PhysicsMassWeightedSeparation) {
    physics::World world;
    physics::RigidBodyDesc light;
    light.mass = 1.0f;
    physics::RigidBodyDesc heavy;
    heavy.mass = 3.0f;
    // Overlapping spheres: the light one should move 3x as far as the heavy one.
    physics::World::BodyId a = world.AddSphere(100, {-0.5f, 1, 0}, 1.0f, true, light);
    physics::World::BodyId b = world.AddSphere(200, {0.5f, 1, 0}, 1.0f, true, heavy);
    world.Step(1.0f / 60.0f, {0, 0, 0});
    const float moveA = std::fabs(world.GetPosition(a).x + 0.5f);
    const float moveB = std::fabs(world.GetPosition(b).x - 0.5f);
    CHECK_NEAR(moveA / moveB, 3.0, 1e-3);
}

TEST(PhysicsFrictionSlowsTangentVelocity) {
    physics::World world;
    physics::RigidBodyDesc desc;
    desc.friction = 0.9f;
    desc.restitution = 0.0f;
    physics::World::BodyId ball = world.AddSphere(100, {0, 1.5f, 0}, 0.5f, true, desc);
    world.SetVelocity(ball, {3.0f, -4.0f, 0});
    const float dt = 1.0f / 60.0f;
    bool hitBox = false;
    world.AddBox(200, {{-5, 0, -5}, {5, 1, 5}}, false); // static floor box at y 0..1
    for (int i = 0; i < 120; ++i) {
        world.Step(dt, {0, -9.81f, 0});
        if (!world.Collisions().empty()) {
            hitBox = true;
            break;
        }
    }
    CHECK(hitBox);
    CHECK(world.GetVelocity(ball).x < 3.0f); // tangential speed reduced
    CHECK_NEAR(world.GetVelocity(ball).y, 0.0, 1e-3);
}

TEST(PhysicsDampingAndGravityScale) {
    physics::World world;
    physics::RigidBodyDesc damped;
    damped.linearDamping = 2.0f;
    physics::World::BodyId a = world.AddSphere(100, {0, 1, 0}, 0.5f, true, damped);
    world.SetVelocity(a, {10.0f, 0, 0});
    physics::RigidBodyDesc noGravity;
    noGravity.gravityScale = 0.0f;
    physics::World::BodyId b = world.AddSphere(200, {0, 2, 0}, 0.5f, true, noGravity);
    const float dt = 1.0f / 60.0f;
    for (int i = 0; i < 60; ++i) world.Step(dt, {0, -9.81f, 0});
    CHECK(world.GetVelocity(a).x < 10.0f);        // damping decayed
    CHECK_NEAR(world.GetPosition(b).y, 2.0, 1e-4); // no gravity -> floats
}

TEST(PhysicsDynamicBoxVsStaticSphere) {
    physics::World world;
    world.AddSphere(10, {0, 1.0f, 0}, 1.0f, false); // static ball center y=1
    physics::World::BodyId box =
        world.AddBox(20, {0, 3.0f, 0}, {0.5f, 0.5f, 0.5f}, true);
    world.SetVelocity(box, {0, -4, 0});
    bool collided = false;
    const float dt = 1.0f / 60.0f;
    for (int i = 0; i < 120 && !collided; ++i) {
        world.Step(dt, {0, -9.81f, 0});
        for (const auto& c : world.Collisions()) {
            if (c.first == 20u && c.second == 10u) collided = true;
        }
    }
    CHECK(collided);
    // Box rests on top of the sphere (y = sphereTop + boxHalf).
    CHECK_NEAR(world.GetPosition(box).y, 2.5, 1e-4);
}

TEST(PhysicsDeterministicFixedStep) {
    auto run = []() {
        physics::World w;
        physics::RigidBodyDesc bouncy;
        bouncy.restitution = 0.6f;
        w.AddSphere(100, {0, 4, 0}, 0.5f, true, bouncy);
        w.AddBox(200, {{-4, 0, -4}, {4, 1, 4}}, false);
        physics::World::BodyId box = w.AddBox(300, {0, 3, 0}, {0.5f, 0.5f, 0.5f}, true, bouncy);
        const float dt = 1.0f / 60.0f;
        for (int i = 0; i < 120; ++i) w.Step(dt, {0, -9.81f, 0});
        return w.GetPosition(box);
    };
    const math::Vec3 p1 = run();
    const math::Vec3 p2 = run();
    CHECK_NEAR(p1.x, p2.x, 1e-6);
    CHECK_NEAR(p1.y, p2.y, 1e-6);
    CHECK_NEAR(p1.z, p2.z, 1e-6);
}

TEST(PhysicsDebugBodiesSnapshot) {
    physics::World world;
    world.AddSphere(100, {0, 1, 0}, 0.5f, true);
    world.AddBox(200, {{-2, 0, -2}, {2, 1, 2}}, false);
    const auto bodies = world.DebugBodies();
    CHECK_EQ(bodies.size(), 2u);
    if (bodies.size() != 2u) return;
    // First body: dynamic sphere with radius 0.5 at (0,1,0).
    CHECK(bodies[0].kind == physics::World::ShapeKind::Sphere);
    CHECK(bodies[0].dynamic);
    CHECK_NEAR(bodies[0].radius, 0.5, 1e-6);
    CHECK_NEAR(bodies[0].pos.y, 1.0, 1e-6);
    // Second body: static box with half extents from the AABB.
    CHECK(bodies[1].kind == physics::World::ShapeKind::Box);
    CHECK(!bodies[1].dynamic);
    CHECK_NEAR(bodies[1].halfExtents.x, 2.0, 1e-6);
    CHECK_NEAR(bodies[1].halfExtents.y, 0.5, 1e-6);

    // Disabled bodies are excluded from the snapshot.
    world.Remove({1});
    CHECK_EQ(world.DebugBodies().size(), 1u);
}
