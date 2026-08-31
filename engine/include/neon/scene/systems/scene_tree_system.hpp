#pragma once
#include <cstdint>
#include <unordered_map>
#include <vector>
#include "neon/ecs/world.hpp"
#include "neon/math/mat4.hpp"
#include "neon/math/vec3.hpp"

namespace neon::scene {

// 场景树：SceneParentLink 层级 + 世界变换缓存（父先子后的重建）。
// 纯机械拆分自 GameRuntime 的 GetChildren/GetDescendants/RebuildWorldTransforms/
// CachedLocalToWorld/LocalToWorld（Task 9）：所有方法接收 ecs::World 引用，不再
// 隐含 runtime 的 world_/running_ 状态；空 world（Stop 后）自然产生空缓存。
class SceneTreeSystem {
public:
    void Rebuild(ecs::World& world); // 重建 worldTransforms_
    math::Mat4 CachedLocalToWorld(ecs::Entity e) const;
    std::vector<ecs::Entity> GetChildren(ecs::World& world, ecs::Entity parent) const;
    std::vector<ecs::Entity> GetDescendants(ecs::World& world, ecs::Entity root) const;

private:
    // LocalToWorld：沿 SceneParentLink 祖先组合局部 TRS（原 GameRuntime::LocalToWorld）
    math::Mat4 LocalToWorld(ecs::World& world, ecs::Entity e) const;
    std::unordered_map<uint64_t, math::Mat4> worldTransforms_;
};

} // namespace neon::scene
