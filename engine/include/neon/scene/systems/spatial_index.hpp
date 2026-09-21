#pragma once
#include <cstdint>
#include <vector>

#include "neon/ecs/world.hpp"
#include "neon/math/bvh.hpp"
#include "neon/math/vec3.hpp"

namespace neon::scene {

// Broadphase for gameplay overlap queries (OverlapSphere / OverlapBox). Both
// used to brute-force scan every SceneHealth entity per call; this maintains a
// dynamic BVH instead so a query only visits the entities near its volume.
//
// The index is rebuilt lazily and cached per simulation tick (Sync() is a
// no-op while the stamp is unchanged), so N queries in one frame share one
// build. MarkDirty() invalidates the cache immediately (transform/health
// writes) so a query later in the same tick still sees current positions.
//
// Leaves are padded by a conservative margin so an entity that moved a little
// since the build is still visited; callers apply the exact containment test to
// every candidate, so the result is identical to the brute-force scan.
class CombatSpatialIndex {
public:
    // (Re)builds the index if `stamp` changed or MarkDirty() was called.
    void Sync(ecs::World& world, uint64_t stamp);
    void MarkDirty() { dirty_ = true; }
    void Clear();
    bool Empty() const { return idToEnt_.empty(); }
    size_t Count() const { return idToEnt_.size(); }

    // Visits candidate entities whose padded leaf AABB overlaps the query.
    template <class Fn>
    void QuerySphere(const math::Vec3& center, float radius, Fn&& fn) const {
        if (bvh_.Empty() || radius <= 0.0f) return;
        math::AABB q;
        q.min = center - math::Vec3{radius, radius, radius};
        q.max = center + math::Vec3{radius, radius, radius};
        QueryAABB(q, fn);
    }
    template <class Fn>
    void QueryAABB(const math::AABB& box, Fn&& fn) const {
        bvh_.QueryAABB(box, [&](math::Bvh::Id id) {
            if (id < idToEnt_.size()) fn(idToEnt_[id]);
        });
    }

private:
    math::Bvh bvh_;
    std::vector<ecs::Entity> idToEnt_;
    uint64_t builtStamp_ = ~0ull;
    bool dirty_ = true;
};

} // namespace neon::scene
