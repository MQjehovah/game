#include <cmath>

#include "neon/neon.hpp"
#include "neon/physics/jolt_world.hpp"

#include "helpers.hpp"

using namespace neon;

// Test-suite P0-1: Jolt rigid-body backend (compiled with NEON_ENABLE_JOLT).
// The custom deterministic world keeps its own suite (test_physics.cpp); these
// assert the same qualitative contracts on Jolt: gravity, ground contact,
// collision events, layer/mask filtering, raycasts and the virtual character.

#ifdef NEON_ENABLE_JOLT

namespace {

void StepN(physics::World& w, int n, float dt = 1.0f / 60.0f,
           const math::Vec3& gravity = {0, -9.81f, 0}) {
    for (int i = 0; i < n; ++i) w.Step(dt, gravity);
}

} // namespace

TEST(JoltSphereLandsOnGround) {
    physics::JoltWorld world;
    physics::World::BodyId ball = world.AddSphere(100, {0, 5, 0}, 1.0f, true);
    CHECK(ball.Valid());
    StepN(world, 300);
    const math::Vec3 p = world.GetPosition(ball);
    CHECK(std::fabs(p.y - 1.0f) < 0.15f);  // rests at radius above y=0
    CHECK(world.IsOnGround(ball));
}

TEST(JoltBoxStopsBallAndReportsCollision) {
    physics::JoltWorld world;
    physics::World::BodyId box = world.AddBox(200, {{-4, 0, -4}, {4, 1, 4}}, false);
    physics::World::BodyId ball = world.AddSphere(100, {-6, 0.5f, 0}, 0.5f, true);
    world.SetVelocity(ball, {10, 0, 0});
    world.ClearCollisions();
    // Collisions() is a per-step snapshot (A7): settled bodies stop reporting,
    // so poll each step instead of inspecting after the whole run.
    bool found = false;
    for (int i = 0; i < 120 && !found; ++i) {
        world.Step(1.0f / 60.0f, {0, -9.81f, 0});
        for (const auto& c : world.Collisions()) {
            if ((c.first == 100 && c.second == 200) || (c.first == 200 && c.second == 100))
                found = true;
        }
    }
    // The ball must be stopped by the static box (x stays left of the box edge).
    const math::Vec3 p = world.GetPosition(ball);
    CHECK(p.x < -3.5f);
    CHECK(p.x > -6.0f);
    CHECK(found);
}

TEST(JoltCollisionsBoundedPerStep) {
    // A7: Collisions() reports the pairs of the LAST step only (matching the
    // custom world's clear-per-step contract). It used to append without bound.
    physics::JoltWorld world;
    physics::World::BodyId box = world.AddBox(200, {{-4, 0, -4}, {4, 1, 4}}, false);
    physics::World::BodyId ball = world.AddSphere(100, {-6, 0.5f, 0}, 0.5f, true);
    world.SetVelocity(ball, {10, 0, 0});
    StepN(world, 60);
    const size_t after60 = world.Collisions().size();
    CHECK(after60 <= 16u); // a resting/impacting pair per contact per step, never 60x accumulated
    StepN(world, 60);
    const size_t after120 = world.Collisions().size();
    CHECK(after120 <= after60 + 16u); // bounded: did not grow by another 60 steps of pairs
    (void)box;
}

TEST(JoltLayerMaskFiltering) {
    physics::JoltWorld world;
    // Ground on layer 1, mask hits layer 1 only.
    physics::RigidBodyDesc groundDesc;
    groundDesc.layer = 1;
    groundDesc.mask = 1u << 0;
    world.AddBox(200, {{-10, 0, -10}, {10, 1, 10}}, false, groundDesc);

    // Ball on layer 0, mask = layer 1 -> collides with the ground.
    physics::RigidBodyDesc ballDesc;
    ballDesc.layer = 0;
    ballDesc.mask = 1u << 1;
    physics::World::BodyId ball = world.AddSphere(100, {0, 5, 0}, 1.0f, true, ballDesc);
    StepN(world, 240);
    CHECK(world.GetPosition(ball).y < 3.0f);  // fell and rests on the ground

    // Ghost ball on layer 2, mask = layer 3 -> never collides with the ground,
    // so it keeps falling.
    physics::RigidBodyDesc ghostDesc;
    ghostDesc.layer = 2;
    ghostDesc.mask = 1u << 3;
    physics::World::BodyId ghost = world.AddSphere(300, {2, 5, 0}, 1.0f, true, ghostDesc);
    StepN(world, 240);
    CHECK(world.GetPosition(ghost).y < -10.0f);
}

TEST(JoltRaycast) {
    physics::JoltWorld world;
    world.AddBox(200, {{-2, 0, -2}, {2, 1, 2}}, false);
    float t = 0.0f;
    uint64_t owner = 0;
    CHECK(world.Raycast({math::Vec3{0, 5, 0}, math::Vec3{0, -1, 0}}, 10.0f, t, &owner));
    CHECK(std::fabs(t - 4.0f) < 0.2f);  // from y=5 to box top y=1
    CHECK_EQ(owner, 200u);
}

TEST(JoltCharacterWalksAndLands) {
    physics::JoltWorld world;
    world.AddBox(200, {{-10, 0, -10}, {10, 1, 10}}, false);
    physics::RigidBodyDesc desc;
    desc.layer = 1;
    desc.mask = 0xFFFFFFFFu;
    physics::World::BodyId hero = world.AddCharacter(100, {0, 2, 0}, 0.4f, 0.9f, desc);
    CHECK(hero.Valid());
    // No movement: the character should fall and land on the ground.
    StepN(world, 240);
    CHECK(world.IsOnGround(hero));
    const math::Vec3 p = world.GetPosition(hero);
    // Box top at y=1; Jolt capsule bottom sits halfHeight + radius below the
    // character center, so the resting center is 1 + 0.9 + 0.4.
    CHECK(std::fabs(p.y - 2.3f) < 0.2f);

    // Walk forward: the character moves along X and stays grounded.
    world.SetCharacterMove(hero, {3, 0, 0});
    const math::Vec3 start = world.GetPosition(hero);
    StepN(world, 120);
    const math::Vec3 end = world.GetPosition(hero);
    CHECK(end.x > start.x + 4.0f);
    CHECK(world.IsOnGround(hero));
}

TEST(JoltSamePlatformDeterministic) {
    // Same build + same inputs => identical trajectory (Jolt is deterministic
    // per platform; the custom world remains the cross-platform fallback).
    physics::JoltWorld a;
    physics::JoltWorld b;
    physics::RigidBodyDesc desc;
    desc.restitution = 0.5f;
    desc.friction = 0.3f;
    a.AddSphere(1, {0, 4, 0}, 0.5f, true, desc);
    b.AddSphere(1, {0, 4, 0}, 0.5f, true, desc);
    for (int i = 0; i < 120; ++i) {
        a.Step(1.0f / 60.0f, {0, -9.81f, 0});
        b.Step(1.0f / 60.0f, {0, -9.81f, 0});
    }
    const math::Vec3 pa = a.GetPosition({1});
    const math::Vec3 pb = b.GetPosition({1});
    CHECK(std::fabs(pa.x - pb.x) < 1e-4f);
    CHECK(std::fabs(pa.y - pb.y) < 1e-4f);
    CHECK(std::fabs(pa.z - pb.z) < 1e-4f);
}

TEST(JoltTriggerReportsOverlapWithoutBlocking) {
    physics::JoltWorld world;
    physics::World::BodyId zone = world.AddTriggerSphere(900, {0, 1, 0}, 2.0f);
    CHECK(zone.Valid());
    physics::World::BodyId ball = world.AddSphere(100, {0, 5, 0}, 0.5f, true);
    bool reported = false;
    for (int i = 0; i < 180 && !reported; ++i) {
        world.Step(1.0f / 60.0f, {0, -9.81f, 0});
        for (const auto& t : world.Triggers()) {
            if (t.first == 900 && t.second == 100) reported = true;
        }
        // Sensors must never surface as physical collisions.
        for (const auto& c : world.Collisions()) {
            CHECK(c.first != 900);
            CHECK(c.second != 900);
        }
    }
    CHECK(reported);
    // Let it settle, then confirm the sensor did not block it (ground at r=0.5).
    for (int i = 0; i < 300; ++i) world.Step(1.0f / 60.0f, {0, -9.81f, 0});
    CHECK(std::fabs(world.GetPosition(ball).y - 0.5f) < 0.2f);
}

TEST(JoltTriggerBoxDetectsRestingBody) {
    physics::JoltWorld world;
    physics::World::BodyId zone = world.AddTriggerBox(900, {0, 1, 0}, {3, 2, 3});
    CHECK(zone.Valid());
    // Dynamic sphere inside the zone (Jolt sensors report dynamic overlaps).
    physics::World::BodyId ball = world.AddSphere(100, {0, 1, 0}, 0.5f, true);
    CHECK(ball.Valid());
    bool reported = false;
    for (int i = 0; i < 180 && !reported; ++i) {
        world.Step(1.0f / 60.0f, {0, -9.81f, 0});
        for (const auto& t : world.Triggers()) {
            if (t.first == 900 && t.second == 100) reported = true;
        }
    }
    CHECK(reported);
}

TEST(JoltTriggerDetectsVirtualCharacter) {
    // Characters are CharacterVirtual (not Jolt bodies), so sensors rely on the
    // capsule-vs-sensor pass rather than the contact listener.
    physics::JoltWorld world;
    physics::World::BodyId zone = world.AddTriggerSphere(900, {0, 1, 0}, 2.0f);
    physics::World::BodyId ch = world.AddCharacter(100, {0, 1, 0}, 0.5f, 0.9f);
    CHECK(zone.Valid());
    CHECK(ch.Valid());
    bool reported = false;
    for (int i = 0; i < 30 && !reported; ++i) {
        world.Step(1.0f / 60.0f, {0, -9.81f, 0});
        for (const auto& t : world.Triggers()) {
            if (t.first == 900 && t.second == 100) reported = true;
        }
    }
    CHECK(reported);

    // A character outside the zone (and away from the sensor) is not reported.
    physics::JoltWorld world2;
    world2.AddTriggerSphere(900, {0, 1, 0}, 1.0f);
    world2.AddCharacter(100, {50, 1, 50}, 0.5f, 0.9f);
    world2.Step(1.0f / 60.0f, {0, -9.81f, 0});
    for (const auto& t : world2.Triggers()) {
        CHECK(t.first != 900); // no nearby overlap
    }
}

TEST(JoltOverlapQueriesAndSphereCast) {
    physics::JoltWorld world;
    world.AddBox(200, {{-2, 0, -2}, {2, 1, 2}}, false); // static floor box (top y=1)
    world.AddSphere(300, {5, 5, 0}, 0.5f, false);       // static ball, far away

    const std::vector<uint64_t> nearby = world.OverlapSphere({0, 0.5f, 0}, 1.0f);
    bool hasBox = false, hasBall = false;
    for (uint64_t o : nearby) {
        if (o == 200) hasBox = true;
        if (o == 300) hasBall = true;
    }
    CHECK(hasBox);
    CHECK(!hasBall);

    const std::vector<uint64_t> qbox = world.OverlapBox({5, 5, 0}, {0.6f, 0.6f, 0.6f});
    bool boxHasBall = false;
    for (uint64_t o : qbox) {
        if (o == 300) boxHasBall = true;
    }
    CHECK(boxHasBall);

    physics::World::ShapeCastHit hit;
    CHECK(world.SphereCast({0, 5, 0}, 0.5f, {0, -1, 0}, 10.0f, hit));
    CHECK(hit.owner == 200);
    CHECK(std::fabs(hit.distance - 3.5f) < 0.1f);
    CHECK(std::fabs(hit.normal.y) > 0.5f);
}

TEST(JoltRotationRoundTrip) {
    physics::JoltWorld world;
    physics::World::BodyId b = world.AddBox(1, {0, 5, 0}, {1, 1, 1}, true);
    const math::Quat q = math::Quat::FromAxisAngle({0, 1, 0}, 0.7f);
    world.SetRotation(b, q);
    const math::Quat got = world.GetRotation(b); // read before any Step
    CHECK(std::fabs(got.y - q.y) < 1e-4);
    CHECK(std::fabs(got.w - q.w) < 1e-4);
}

TEST(JoltContinuousCollisionStopsFastBody) {
    // A fast, small dynamic sphere vs a thin static wall: discrete stepping
    // tunnels through; LinearCast motion quality must stop it at the wall.
    physics::JoltWorld world;
    world.AddBox(200, {0, 2, 0}, {0.05f, 5, 5}, false); // thin wall at x=0
    physics::RigidBodyDesc ball;
    ball.continuous = true;
    physics::World::BodyId b = world.AddSphere(100, {-5, 2, 0}, 0.2f, true, ball);
    world.SetVelocity(b, {200, 0, 0});
    for (int i = 0; i < 60; ++i) world.Step(1.0f / 60.0f, {0, 0, 0});
    CHECK(world.GetPosition(b).x < 0.0f); // did not pass through the wall
}

TEST(JoltFixedJointKeepsBodiesTogether) {
    physics::JoltWorld world;
    physics::World::BodyId a = world.AddSphere(1, {0, 5, 0}, 0.5f, true);
    physics::World::BodyId b = world.AddSphere(2, {2, 5, 0}, 0.5f, true);
    physics::World::JointId j = world.AddFixedJoint(a, b, {1, 5, 0});
    CHECK(j.Valid());
    CHECK(world.JointCount() == 1u);
    // Try to tear them apart; the weld must hold the 2-unit separation.
    world.SetVelocity(a, {-20, 0, 0});
    world.SetVelocity(b, {20, 0, 0});
    for (int i = 0; i < 120; ++i) world.Step(1.0f / 60.0f, {0, 0, 0});
    const float dist = (world.GetPosition(a) - world.GetPosition(b)).Length();
    CHECK(std::fabs(dist - 2.0f) < 0.3f);
    world.RemoveJoint(j);
    CHECK(world.JointCount() == 0u);
}

TEST(JoltDistanceJointClampsSeparation) {
    physics::JoltWorld world;
    physics::World::BodyId anchor = world.AddSphere(1, {0, 5, 0}, 0.5f, false); // static
    physics::World::BodyId bob = world.AddSphere(2, {0, 3, 0}, 0.5f, true);
    physics::World::JointId j = world.AddDistanceJoint(anchor, bob, {0, 5, 0}, {0, 3, 0}, 2.0f, 2.0f);
    CHECK(j.Valid());
    world.SetVelocity(bob, {0, -10, 0});
    for (int i = 0; i < 180; ++i) world.Step(1.0f / 60.0f, {0, -9.81f, 0});
    const float dist = (world.GetPosition(anchor) - world.GetPosition(bob)).Length();
    CHECK(std::fabs(dist - 2.0f) < 0.3f); // rope length preserved (pendulum)
}

TEST(JoltHingeJointAndBodyRemovalCleansJoints) {
    physics::JoltWorld world;
    physics::World::BodyId a = world.AddBox(1, {0, 5, 0}, {0.5f, 0.5f, 0.5f}, false);
    physics::World::BodyId b = world.AddBox(2, {1.5f, 5, 0}, {0.5f, 0.5f, 0.5f}, true);
    physics::World::JointId j = world.AddHingeJoint(a, b, {0.5f, 5, 0}, {0, 0, 1});
    CHECK(j.Valid());
    CHECK(world.JointCount() == 1u);
    for (int i = 0; i < 120; ++i) world.Step(1.0f / 60.0f, {0, -9.81f, 0});
    // The bob swings about the hinge (stays 1 unit from the anchor point).
    const math::Vec3 p = world.GetPosition(b);
    CHECK(std::fabs(p.y - 5.0f) < 1.6f);
    // Removing the body drops its joint automatically.
    world.Remove(b);
    CHECK(world.JointCount() == 0u);
}

TEST(JoltConfigurableLimitsAndWorkerThreads) {
    // Small custom pools (implicit ground uses one slot).
    physics::JoltWorld small(16, 0, 64, 32);
    CHECK(small.AddSphere(1, {0, 5, 0}, 0.5f, true).Valid());
    CHECK(small.AddBox(2, {{-1, 0, -1}, {1, 1, 1}}, false).Valid());
    for (int i = 0; i < 60; ++i) small.Step(1.0f / 60.0f, {0, -9.81f, 0});
    CHECK(small.BodyCount() >= 2u);

    // Opt-in multithreaded job system still simulates correctly.
    physics::JoltWorld mt(256, 2);
    physics::World::BodyId b = mt.AddSphere(1, {0, 5, 0}, 0.5f, true);
    for (int i = 0; i < 300; ++i) mt.Step(1.0f / 60.0f, {0, -9.81f, 0});
    CHECK(std::fabs(mt.GetPosition(b).y - 0.5f) < 0.2f);
}

#endif // NEON_ENABLE_JOLT
