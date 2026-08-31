#pragma once
#include <cstdint>
#include <vector>
#include "neon/ecs/world.hpp"
#include "neon/math/vec3.hpp"

namespace neon::scene {

// 属性补间系统：Lua `Tween(ent, prop, from, to, time, easing)` 的引擎侧驱动。
// prop: 0=pos 1=rot(euler degrees) 2=scale; easing: 0=linear 1=in 2=out 3=inout。
class TweenSystem {
public:
    struct Tween {
        ecs::Entity target;
        int prop = 0;
        math::Vec3 from{}, to{};
        float time = 1.0f, elapsed = 0.0f;
        int easing = 0;
    };
    void Start(ecs::Entity target, int prop, const math::Vec3& from, const math::Vec3& to,
               float time, int easing);
    void Tick(float dt, ecs::World& world); // 推进并写入 SceneTransform
    void Clear() { tweens_.clear(); }       // Stop 生命周期：丢弃全部补间
    size_t Count() const { return tweens_.size(); }

private:
    std::vector<Tween> tweens_;
};

} // namespace neon::scene
