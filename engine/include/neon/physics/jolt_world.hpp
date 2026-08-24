#pragma once

#include <memory>

#include "neon/physics/physics.hpp"

// Jolt is optional (NEON_ENABLE_JOLT). When disabled this header degrades to a
// compile-time stub so callers can keep the same include unconditionally.
#ifdef NEON_ENABLE_JOLT

namespace neon::physics {

// Jolt-backed rigid-body world implementing the same public interface as the
// deterministic custom World. Bodies get full rigid-body dynamics (rotation,
// convex shapes, stable stacking, contacts, raycasts) and collision-layer /
// mask filtering mapped to Jolt's ObjectLayerPairFilterMask (Bullet-style:
// two bodies collide when group_a & mask_b and group_b & mask_a are both
// non-zero).
//
// Determinism: Jolt is deterministic for a fixed build + platform (same body
// order, fixed dt, single-threaded job system). The custom World remains the
// cross-platform deterministic fallback for authoritative servers.
class JoltWorld : public World {
public:
    JoltWorld();
    ~JoltWorld() override;

    JoltWorld(const JoltWorld&) = delete;
    JoltWorld& operator=(const JoltWorld&) = delete;

    BodyId AddSphere(uint64_t owner, const math::Vec3& pos, float radius, bool dynamic,
                     const RigidBodyDesc& desc = {}) override;
    BodyId AddBox(uint64_t owner, const math::Vec3& center, const math::Vec3& halfExtents,
                  bool dynamic, const RigidBodyDesc& desc = {}) override;
    BodyId AddBox(uint64_t owner, const math::AABB& box, bool dynamic = false,
                  const RigidBodyDesc& desc = {}) override;
    BodyId AddCharacter(uint64_t owner, const math::Vec3& pos, float radius, float halfHeight,
                        const RigidBodyDesc& desc = {}) override;
    void SetCharacterMove(BodyId body, const math::Vec3& move) override;
    math::Vec3 GetCharacterMove(BodyId body) const override;
    void Remove(BodyId body) override;
    void Clear() override;

    void SetPosition(BodyId body, const math::Vec3& pos) override;
    math::Vec3 GetPosition(BodyId body) const override;
    void SetVelocity(BodyId body, const math::Vec3& vel) override;
    math::Vec3 GetVelocity(BodyId body) const override;
    void SetMass(BodyId body, float mass) override;
    void SetRestitution(BodyId body, float restitution) override;
    void SetFriction(BodyId body, float friction) override;
    void SetLinearDamping(BodyId body, float damping) override;
    void SetGravityScale(BodyId body, float scale) override;
    void SetEnabled(BodyId body, bool enabled) override;
    bool IsOnGround(BodyId body) const override;

    void Step(float dt, const math::Vec3& gravity) override;

    size_t BodyCount() const override;

    const std::vector<std::pair<uint64_t, uint64_t>>& Collisions() const override {
        return collisions_;
    }
    void ClearCollisions() override { collisions_.clear(); }

    bool Raycast(const math::Ray& ray, float maxDist, float& outT,
                 uint64_t* hitOwner) const override;

    std::vector<DebugBody> DebugBodies() const override;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace neon::physics

#else

namespace neon::physics {
// Compile-time stub: Jolt disabled. The engine always ships the custom World;
// this type exists so headers that reference JoltWorld compile without the
// backend, but it should never be instantiated.
class JoltWorld {
public:
    JoltWorld() = delete;
};
} // namespace neon::physics

#endif
