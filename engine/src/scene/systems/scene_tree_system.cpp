// SceneTreeSystem implementation. The scene-tree subsystem extracted from
// GameRuntime (Task 9): SceneParentLink hierarchy traversal (GetChildren /
// GetDescendants), the world-transform cache (Rebuild / CachedLocalToWorld) and
// the private LocalToWorld ancestor walk. Pure code movement -- every method
// takes the ecs::World instead of reading GameRuntime's implicit world_, and
// the old `running_` early-out is dropped (an empty world yields an empty
// cache, which is exactly what the pre-Start / post-Stop runtime produced).
#include "neon/scene/systems/scene_tree_system.hpp"

#include <set>

#include "neon/scene/scene_file.hpp"

namespace neon::scene {

namespace {

// Stable 64-bit key for per-entity scoping: id occupies the high half so an id
// reused across generations still keys uniquely (same shape as the runtime's
// internal EntityKey).
uint64_t EntityKey(const ecs::Entity& e) {
    return (static_cast<uint64_t>(e.id) << 32) | static_cast<uint64_t>(e.generation);
}

} // namespace

math::Mat4 SceneTreeSystem::LocalToWorld(ecs::World& world, ecs::Entity e) const {
    math::Mat4 m = math::Mat4::Identity();
    for (int depth = 0; depth < 8 && e.IsValid(); ++depth) {
        const SceneTransform* t = world.Get<SceneTransform>(e);
        if (!t) break;
        m = math::Mat4::Translation(t->pos) * t->rot.ToMat4() * math::Mat4::Scale(t->scale) * m;
        const SceneParentLink* link = world.Get<SceneParentLink>(e);
        e = link ? link->parent : ecs::Entity{};
    }
    return m;
}

std::vector<ecs::Entity> SceneTreeSystem::GetChildren(ecs::World& world,
                                                      ecs::Entity parent) const {
    std::vector<ecs::Entity> out;
    if (!parent.IsValid()) return out;
    auto view = world.ViewAll<SceneParentLink, SceneTransform>();
    for (size_t i = 0; i < view.Size(); ++i) {
        const ecs::Entity child = world.EntityAt<SceneParentLink>(i);
        const SceneParentLink* link = world.Get<SceneParentLink>(child);
        if (link && link->parent == parent) out.push_back(child);
    }
    return out;
}

std::vector<ecs::Entity> SceneTreeSystem::GetDescendants(ecs::World& world,
                                                         ecs::Entity root) const {
    std::vector<ecs::Entity> out;
    std::vector<ecs::Entity> stack = GetChildren(world, root);
    std::set<uint64_t> visited;
    while (!stack.empty()) {
        const ecs::Entity e = stack.back();
        stack.pop_back();
        if (!visited.insert(EntityKey(e)).second) continue; // cycle guard
        out.push_back(e);
        const std::vector<ecs::Entity> kids = GetChildren(world, e);
        stack.insert(stack.end(), kids.begin(), kids.end());
    }
    return out;
}

void SceneTreeSystem::Rebuild(ecs::World& world) {
    worldTransforms_.clear();

    // parent EntityKey -> children (entities with a SceneTransform whose
    // SceneParentLink points at it). Roots = entities with a SceneTransform
    // and no live parent link.
    std::unordered_map<uint64_t, std::vector<ecs::Entity>> children;
    std::vector<ecs::Entity> roots;
    auto linkView = world.ViewAll<SceneParentLink, SceneTransform>();
    for (size_t i = 0; i < linkView.Size(); ++i) {
        const ecs::Entity child = world.EntityAt<SceneParentLink>(i);
        const SceneParentLink* link = world.Get<SceneParentLink>(child);
        if (!link) continue;
        if (world.Alive(link->parent))
            children[EntityKey(link->parent)].push_back(child);
        else
            roots.push_back(child); // dangling parent: treat as a root
    }
    auto transformView = world.ViewAll<SceneTransform>();
    for (size_t i = 0; i < transformView.Size(); ++i) {
        const ecs::Entity e = world.EntityAt<SceneTransform>(i);
        const SceneParentLink* link = world.Get<SceneParentLink>(e);
        if (!link || !world.Alive(link->parent)) roots.push_back(e);
    }

    // Iterative DFS from roots: a parent's world is always computed before its
    // children are visited, so arbitrary tree depth is handled without the old
    // 8-level walk cap.
    std::vector<ecs::Entity> stack;
    for (const ecs::Entity& r : roots) {
        if (worldTransforms_.count(EntityKey(r)) != 0) continue;
        stack.push_back(r);
        while (!stack.empty()) {
            const ecs::Entity e = stack.back();
            stack.pop_back();
            if (worldTransforms_.count(EntityKey(e)) != 0) continue; // cycle guard
            const SceneTransform* t = world.Get<SceneTransform>(e);
            if (!t) continue;
            math::Mat4 worldMat = math::Mat4::Translation(t->pos) * t->rot.ToMat4() *
                                  math::Mat4::Scale(t->scale);
            const SceneParentLink* link = world.Get<SceneParentLink>(e);
            if (link && world.Alive(link->parent)) {
                const auto pit = worldTransforms_.find(EntityKey(link->parent));
                if (pit != worldTransforms_.end()) worldMat = pit->second * worldMat;
            }
            worldTransforms_[EntityKey(e)] = worldMat;
            const auto cit = children.find(EntityKey(e));
            if (cit != children.end())
                for (const ecs::Entity& c : cit->second) stack.push_back(c);
        }
    }
}

math::Mat4 SceneTreeSystem::CachedLocalToWorld(ecs::Entity e) const {
    const auto it = worldTransforms_.find(EntityKey(e));
    return it == worldTransforms_.end() ? math::Mat4::Identity() : it->second;
}

} // namespace neon::scene
