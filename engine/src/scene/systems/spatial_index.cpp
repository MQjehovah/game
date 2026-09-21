#include "neon/scene/systems/spatial_index.hpp"

#include "neon/scene/scene_file.hpp" // SceneHealth / SceneTransform

namespace neon::scene {
namespace {

// Conservative leaf pad (world units): a query may find an entity that has
// moved up to this far since the cache was built. Larger than any per-tick
// displacement for walking units (speed * dt << 1); script teleports bump the
// dirty flag so they rebuild before the next query.
constexpr float kLeafPad = 2.0f;

} // namespace

void CombatSpatialIndex::Sync(ecs::World& world, uint64_t stamp) {
    if (stamp == builtStamp_ && !dirty_) return;
    builtStamp_ = stamp;
    dirty_ = false;
    bvh_.Clear();
    idToEnt_.clear();
    auto view = world.ViewAll<SceneHealth>();
    idToEnt_.reserve(view.Size());
    for (size_t i = 0; i < view.Size(); ++i) {
        const ecs::Entity e = world.EntityAt<SceneHealth>(i);
        const SceneHealth* h = world.Get<SceneHealth>(e);
        const SceneTransform* t = world.Get<SceneTransform>(e);
        if (h == nullptr || t == nullptr || h->hp <= 0.0f) continue;
        const uint32_t id = static_cast<uint32_t>(idToEnt_.size());
        idToEnt_.push_back(e);
        math::AABB box;
        box.min = t->pos - math::Vec3{kLeafPad, kLeafPad, kLeafPad};
        box.max = t->pos + math::Vec3{kLeafPad, kLeafPad, kLeafPad};
        bvh_.Insert(id, box);
    }
}

void CombatSpatialIndex::Clear() {
    bvh_.Clear();
    idToEnt_.clear();
    builtStamp_ = ~0ull;
    dirty_ = true;
}

} // namespace neon::scene
