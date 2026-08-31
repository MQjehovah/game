#pragma once
#include <cstdint>
#include "neon/ecs/world.hpp"
#include "neon/script/script.hpp"

namespace neon::scene {

// 状态效果系统：驱动 StatusComponent 的 tick（效果规则已下沉 Lua OnStatusTick）。
class StatusSystem {
public:
    void Tick(float dt, ecs::World& world, script::IScriptHost* luaHost);
    bool Has(ecs::World& world, ecs::Entity e, uint32_t id) const;
    float Magnitude(ecs::World& world, ecs::Entity e, uint32_t id) const;
};

} // namespace neon::scene
