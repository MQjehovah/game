// PhysicsBridge implementation. Migrated from GameRuntime's RegisterSceneBodies /
// RegisterCharacters / SyncSceneBodies plus the fixed-step physics block in Tick
// (Task 15). Pure code movement, no semantic change: the ecs::World stays owned
// by GameRuntime (passed as a parameter), while the physics::World ownership and
// the G5-1 native backend (pluginPhysics_) moved in here.
#include "neon/scene/systems/physics_bridge.hpp"

#include "neon/core/profiler.hpp"
#include "neon/scene/scene_file.hpp"

namespace neon::scene {
namespace {

// Stable 64-bit key for per-entity physics body scoping: id occupies the high
// half so an id reused across generations still keys uniquely.
uint64_t EntityKey(const ecs::Entity& e) {
    return (static_cast<uint64_t>(e.id) << 32) | static_cast<uint64_t>(e.generation);
}

} // namespace

void PhysicsBridge::SetWorld(
    std::unique_ptr<physics::World, std::function<void(physics::World*)>> world,
    std::unique_ptr<plugin::PhysicsBackend> pluginBackend) {
    // 替换顺序与成员声明顺序相反（physics_ 先替换）：旧的 world 先于旧的库销毁。
    // 若先替换 pluginPhysics_，旧库先被卸载，之后旧 world 的 deleter 会穿越已卸载
    // 的 DLL（use-after-free）。新 world 由传入的 backend 创建，随即被插件持有。
    physics_ = std::move(world);
    pluginPhysics_ = std::move(pluginBackend);
    physicsAccum_ = 0.0f;
}

// Registers every scene entity that carries a rigidbody component with the
// physics world. The entity's transform provides the initial position; the
// generated physics body id is stored back on the component so per-step
// SyncBodies can follow it.
void PhysicsBridge::RegisterBodies(ecs::World& world) {
    world.ViewAll<SceneRigidBody, SceneTransform>().ForEach(
        [&](ecs::Entity e, SceneRigidBody& rb, const SceneTransform& t) {
            physics::RigidBodyDesc desc;
            desc.dynamic = rb.dynamic;
            desc.mass = rb.mass;
            desc.restitution = rb.restitution;
            desc.friction = rb.friction;
            desc.linearDamping = rb.linearDamping;
            desc.gravityScale = rb.gravityScale;
            desc.layer = rb.layer;
            desc.mask = rb.mask;
            physics::World::BodyId body;
            if (rb.shape == "box") {
                body = physics_->AddBox(EntityKey(e), t.pos, rb.halfExtents, rb.dynamic, desc);
            } else {
                body = physics_->AddSphere(EntityKey(e), t.pos, rb.radius, rb.dynamic, desc);
            }
            rb.bodyId = body.id;
        });
}

// Registers every entity with a character component as a Jolt virtual
// character (capsule controller). The custom deterministic world returns an
// invalid id, in which case the component is left untouched.
void PhysicsBridge::RegisterCharacters(ecs::World& world) {
    world.ViewAll<SceneCharacter, SceneTransform>().ForEach(
        [&](ecs::Entity e, SceneCharacter& c, const SceneTransform& t) {
            physics::RigidBodyDesc desc;
            desc.layer = c.layer;
            desc.mask = c.mask;
            physics::World::BodyId body =
                physics_->AddCharacter(EntityKey(e), t.pos, c.radius, c.halfHeight, desc);
            c.bodyId = body.id;
        });
}

// Writes the physics bodies' positions back into their entities' transforms
// so rendered meshes follow the simulation. Called after every physics step.
void PhysicsBridge::SyncBodies(ecs::World& world) {
    if (!physics_) return;
    world.ViewAll<SceneRigidBody, SceneTransform>().ForEach(
        [&](ecs::Entity e, const SceneRigidBody& rb, SceneTransform& t) {
            if (rb.bodyId == 0) return;
            // B14: static bodies never move after registration -- skip the
            // per-frame GetPosition (Jolt map lookup) entirely. Dynamic bodies
            // (and characters below) keep syncing.
            if (!rb.dynamic) return;
            t.pos = physics_->GetPosition({rb.bodyId});
            (void)e;
        });
    world.ViewAll<SceneCharacter, SceneTransform>().ForEach(
        [&](ecs::Entity, const SceneCharacter& c, SceneTransform& t) {
            if (c.bodyId == 0) return;
            t.pos = physics_->GetPosition({c.bodyId});
        });
}

// Fixed-step physics: accumulate the frame delta and advance the world at 60 Hz
// so collision resolution and scripts stay deterministic regardless of frame
// rate. Cap the catch-up to avoid a spiral of death after a hitch.
void PhysicsBridge::Step(float dt, const math::Vec3& gravity) {
    if (!physics_) return;
    core::ScopedTimer physicsTimer("runtime.physics");
    physicsAccum_ += dt;
    constexpr float kPhysicsStep = 1.0f / 60.0f;
    int physicsSteps = 0;
    while (physicsAccum_ >= kPhysicsStep && physicsSteps < 4) {
        physics_->Step(kPhysicsStep, gravity);
        physicsAccum_ -= kPhysicsStep;
        ++physicsSteps;
    }
    if (physicsSteps == 4) physicsAccum_ = 0.0f;
}

// A8: a scripted move must also move the physics body, otherwise the next
// SyncBodies snaps the entity back and characters walk through walls that only
// physics knows about.
void PhysicsBridge::SetBodyPosition(ecs::World& world, ecs::Entity e,
                                    const math::Vec3& pos) {
    if (!physics_) return;
    if (const SceneRigidBody* rb = world.Get<SceneRigidBody>(e)) {
        if (rb->bodyId != 0) physics_->SetPosition({rb->bodyId}, pos);
    }
    if (const SceneCharacter* c = world.Get<SceneCharacter>(e)) {
        if (c->bodyId != 0) physics_->SetPosition({c->bodyId}, pos);
    }
}

void PhysicsBridge::Clear() {
    if (physics_) physics_->Clear();
    physicsAccum_ = 0.0f;
}

} // namespace neon::scene
