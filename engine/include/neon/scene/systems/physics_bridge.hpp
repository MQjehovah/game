#pragma once
#include <functional>
#include <memory>

#include "neon/ecs/world.hpp"
#include "neon/physics/physics.hpp"
#include "neon/plugin/backend.hpp"

namespace neon::scene {

// 物理桥接子系统（Task 15）：把 ECS 实体（rigidbody/character 组件）同步到
// physics::World，以固定 60 Hz 累加器推进世界，并把物理结果写回实体 transform。
// 纯机械拆分——GameRuntime 的 physics_/physicsAccum_/pluginPhysics_ +
// RegisterSceneBodies/RegisterCharacters/SyncSceneBodies 以及 Tick 里的物理固定步进
// 段迁入此类，无行为变化。
//
// 共享状态边界：ecs::World（GameRuntime 拥有）经参数传入；物理世界本体与 G5-1
// 原生后端所有权移入此处。pluginPhysics_ 必须在 physics_ 之前声明，使 world 先于
// 库（DLL）销毁——与 GameRuntime 原声明顺序一致。
class PhysicsBridge {
public:
    // 接管物理世界（及可选的 G5-1 原生后端）所有权。替换顺序为 physics_ 先、
    // pluginPhysics_ 后：旧的 world 先于旧的库销毁（与 GameRuntime 原语义一致）。
    // 非拥有注入（service registry）时 world 用空 deleter 包装、pluginBackend 传 null。
    void SetWorld(std::unique_ptr<physics::World, std::function<void(physics::World*)>> world,
                  std::unique_ptr<plugin::PhysicsBackend> pluginBackend);

    // 把 ECS 里带 rigidbody 组件的实体注册进物理世界；生成的 bodyId 写回组件
    // （后续 SyncBodies 跟随该 id）。
    void RegisterBodies(ecs::World& world);
    // 注册带 character 组件的实体（Jolt 虚拟角色；自定义确定性世界返回无效 id，
    // 组件保持原样）。
    void RegisterCharacters(ecs::World& world);
    // 把物理 body 的位置写回实体 transform（渲染网格跟随模拟）。每次物理步进后调用。
    void SyncBodies(ecs::World& world);
    // 固定步进（60 Hz 累加器，单帧最多追 4 步防螺旋死亡）。原 GameRuntime::Tick
    // 里的 physicsAccum_ 累加 + physics_->Step 段。
    void Step(float dt, const math::Vec3& gravity);
    // 脚本移动实体时同步物理 body（A8：scripted move 必须同时移动物理 body，
    // 否则下一次 SyncBodies 会把实体拉回物理位置）。
    void SetBodyPosition(ecs::World& world, ecs::Entity e, const math::Vec3& pos);
    // 清空物理世界状态并重置累加器（Stop；世界本体与后端保留，Start 可复用）。
    void Clear();

    physics::World* World() { return physics_.get(); }
    const physics::World* World() const { return physics_.get(); }
    size_t BodyCount() const { return physics_ ? physics_->BodyCount() : 0; }

private:
    // 声明顺序保持：pluginPhysics_ 先于 physics_，world 先于库销毁。
    std::unique_ptr<plugin::PhysicsBackend> pluginPhysics_; // native backend owner (G5-1)
    std::unique_ptr<physics::World, std::function<void(physics::World*)>> physics_;
    float physicsAccum_ = 0.0f; // fixed-step accumulator (60 Hz)
};

} // namespace neon::scene
