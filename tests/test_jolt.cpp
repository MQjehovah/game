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

#endif // NEON_ENABLE_JOLT
