#pragma once
#include <cstdint>
#include <utility>
#include <vector>
#include "neon/math/math.hpp"
#include "neon/math/vec3.hpp"

namespace neon::physics {

// Rigid body material / spawn parameters. `mass <= 0` for a dynamic body
// derives the mass from the shape volume (density 1), which keeps defaults
// intuitive for spheres/boxes of any size. Static bodies always have infinite
// mass (inverse mass 0) regardless of this field.
struct RigidBodyDesc {
    bool dynamic = true;
    float mass = 0.0f;          // <=0 -> auto from volume (dynamic only)
    float restitution = 0.0f;   // 0 = inelastic, 1 = perfectly elastic
    float friction = 0.4f;      // tangential impulse coefficient
    float linearDamping = 0.0f; // exponential velocity decay per second
    float gravityScale = 1.0f;  // 0 disables gravity for this body
    uint32_t layer = 1;         // collision group id
    uint32_t mask = 0xFFFFFFFFu; // collision mask (which groups this body hits)
};

// Lightweight deterministic rigid-body engine: dynamic spheres + AABBs against
// static/dynamic spheres + AABBs and the y=0 ground plane. Impulse-based
// collision resolution (restitution + friction + mass weighting), per-step
// collision events and raycasts. Linear only (no rotation yet); swappable for
// Jolt/Bullet later via a separate integration layer.
class World {
public:
    virtual ~World() = default;

    struct BodyId {
        uint32_t id = 0;
        bool Valid() const { return id != 0; }
    };

    // Shape spawn helpers. `owner` is an opaque gameplay id (entity key etc.)
    // that comes back in Collisions() so the game layer can react.
    virtual BodyId AddSphere(uint64_t owner, const math::Vec3& pos, float radius, bool dynamic,
                             const RigidBodyDesc& desc = {});
    virtual BodyId AddBox(uint64_t owner, const math::Vec3& center, const math::Vec3& halfExtents,
                          bool dynamic, const RigidBodyDesc& desc = {});
    // Convenience: absolute AABB box (min/max), static by default.
    virtual BodyId AddBox(uint64_t owner, const math::AABB& box, bool dynamic = false,
                          const RigidBodyDesc& desc = {});
    // Character controller (Jolt backend): a capsule-shaped kinematic body that
    // follows the desired velocity each step and lands on geometry. The custom
    // deterministic world returns an invalid id (characters are a Jolt feature).
    virtual BodyId AddCharacter(uint64_t owner, const math::Vec3& pos, float radius,
                                float halfHeight, const RigidBodyDesc& desc = {});
    // Desired horizontal/vertical velocity for a character body (clamped by the
    // controller's collision response).
    virtual void SetCharacterMove(BodyId body, const math::Vec3& move);
    virtual math::Vec3 GetCharacterMove(BodyId body) const;
    virtual void Remove(BodyId body);
    virtual void Clear();

    virtual void SetPosition(BodyId body, const math::Vec3& pos);
    virtual math::Vec3 GetPosition(BodyId body) const;
    virtual void SetVelocity(BodyId body, const math::Vec3& vel);
    virtual math::Vec3 GetVelocity(BodyId body) const;
    virtual void SetMass(BodyId body, float mass);
    virtual void SetRestitution(BodyId body, float restitution);
    virtual void SetFriction(BodyId body, float friction);
    virtual void SetLinearDamping(BodyId body, float damping);
    virtual void SetGravityScale(BodyId body, float scale);
    virtual void SetEnabled(BodyId body, bool enabled);
    virtual bool IsOnGround(BodyId body) const;

    // Advances the world by `dt` seconds under `gravity` (e.g. {0,-9.81,0}).
    // Deterministic for a fixed dt: same body order + inputs => same result.
    virtual void Step(float dt, const math::Vec3& gravity);

    // Number of enabled bodies (editor profiler / debug panels).
    virtual size_t BodyCount() const;

    // Pairs of owners that collided this step (a dynamic body is always the
    // first element; both dynamic keeps insertion order).
    virtual const std::vector<std::pair<uint64_t, uint64_t>>& Collisions() const {
        return collisions_;
    }
    virtual void ClearCollisions() { collisions_.clear(); }

    virtual bool Raycast(const math::Ray& ray, float maxDist, float& outT,
                         uint64_t* hitOwner) const;

    // Read-only snapshot for debug rendering (editor collider wireframes).
    enum class ShapeKind : uint8_t { Sphere, Box };
    struct DebugBody {
        ShapeKind kind = ShapeKind::Sphere;
        math::Vec3 pos{};
        float radius = 1.0f;
        math::Vec3 halfExtents{1, 1, 1};
        bool dynamic = true;
    };
    virtual std::vector<DebugBody> DebugBodies() const;

protected:
    // Collision pairs for the current step (dynamic-first ordering). Backends
    // append here; ClearCollisions() resets it.
    std::vector<std::pair<uint64_t, uint64_t>> collisions_;

private:
    struct Body {
        BodyId id;
        uint64_t owner = 0;
        enum class Kind : uint8_t { Sphere, Box } kind = Kind::Sphere;
        math::Vec3 pos{};
        float radius = 1.0f;
        math::Vec3 halfExtents{1, 1, 1};
        math::Vec3 velocity{};
        float mass = 1.0f;
        float invMass = 1.0f;
        float restitution = 0.0f;
        float friction = 0.4f;
        float linearDamping = 0.0f;
        float gravityScale = 1.0f;
        bool dynamic = true;
        bool enabled = true;
        bool onGround = false;
        uint32_t layer = 1;
        uint32_t mask = 0xFFFFFFFFu;

        math::AABB Box() const { return {pos - halfExtents, pos + halfExtents}; }
        float Bottom() const {
            return kind == Kind::Sphere ? pos.y - radius : pos.y - halfExtents.y;
        }
    };

    const Body* Find(BodyId id) const;
    Body* Find(BodyId id);
    void InitBody(Body& b, uint64_t owner, bool dynamic, const RigidBodyDesc& desc);
    void SolvePair(Body& a, Body& b);

    std::vector<Body> bodies_;
    std::vector<BodyId> freeIds_;
    uint32_t nextId_ = 1;
};

} // namespace neon::physics
