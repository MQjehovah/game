#pragma once
#include <cstdint>
#include <utility>
#include <vector>
#include "neon/math/math.hpp"
#include "neon/math/vec3.hpp"

namespace neon::physics {

// Lightweight built-in physics: dynamic spheres vs static AABBs + ground,
// with collision events and raycasts. Swappable for Jolt/Bullet later via
// a separate integration layer (see docs/ROADMAP.md).
class World {
public:
    struct BodyId {
        uint32_t id = 0;
        bool Valid() const { return id != 0; }
    };

    BodyId AddSphere(uint64_t owner, const math::Vec3& pos, float radius, bool dynamic);
    BodyId AddBox(uint64_t owner, const math::AABB& box, bool dynamic = false);
    void Remove(BodyId body);
    void Clear();

    void SetPosition(BodyId body, const math::Vec3& pos);
    math::Vec3 GetPosition(BodyId body) const;
    void SetVelocity(BodyId body, const math::Vec3& vel);
    math::Vec3 GetVelocity(BodyId body) const;
    void SetEnabled(BodyId body, bool enabled);
    bool IsOnGround(BodyId body) const;

    void Step(float dt, const math::Vec3& gravity);

    // Number of enabled bodies (editor profiler / debug panels).
    size_t BodyCount() const;

    // Pairs of owners that collided this step (dynamic involved).
    const std::vector<std::pair<uint64_t, uint64_t>>& Collisions() const { return collisions_; }
    void ClearCollisions() { collisions_.clear(); }

    bool Raycast(const math::Ray& ray, float maxDist, float& outT, uint64_t* hitOwner) const;

private:
    struct Body {
        BodyId id;
        uint64_t owner = 0;
        enum class Kind : uint8_t { Sphere, Box } kind = Kind::Sphere;
        math::Vec3 pos{};
        float radius = 1.0f;
        math::AABB box;
        math::Vec3 velocity{};
        bool dynamic = true;
        bool enabled = true;
        bool onGround = false;
    };

    const Body* Find(BodyId id) const;
    Body* Find(BodyId id);

    std::vector<Body> bodies_;
    std::vector<BodyId> freeIds_;
    uint32_t nextId_ = 1;
    std::vector<std::pair<uint64_t, uint64_t>> collisions_;
};

} // namespace neon::physics
