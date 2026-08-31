// StatusSystem implementation. Migrated from GameRuntime::TickStatuses /
// HasStatus / StatusMagnitude (C1): advances every entity's StatusComponent
// (damage/heal ticks + expiry) and forwards each per-interval tick through the
// script global OnStatusTick(entity, id, magnitude) so the actual damage/heal/
// slow rules live in the Lua Gameplay base library. The world and the script
// host are Tick parameters instead of GameRuntime members; pure code movement,
// no semantic change.
#include "neon/scene/systems/status_system.hpp"

#include "neon/scene/status.hpp"

namespace neon::scene {
namespace {

// Entity handle as a Lua table {id, gen} (matches the T2.3 bindings' shape and
// the EntityFromValue parser in bindings.cpp).
script::Value EntityToValue(const ecs::Entity& e) {
    script::Value t = script::Value::Tbl();
    t.table->fields.emplace_back("id", script::Value::Num(static_cast<double>(e.id)));
    t.table->fields.emplace_back("gen", script::Value::Num(static_cast<double>(e.generation)));
    return t;
}

} // namespace

void StatusSystem::Tick(float dt, ecs::World& world, script::IScriptHost* luaHost) {
    auto view = world.ViewAll<StatusComponent>();
    for (size_t i = 0; i < view.Size(); ++i) {
        ecs::Entity ent = world.EntityAt<StatusComponent>(i);
        StatusComponent* c = world.Get<StatusComponent>(ent);
        if (!c) continue;
        TickStatus(*c, dt, [luaHost, ent](uint32_t id, float magnitude) {
            // 状态 tick 效果下沉 Lua：调用脚本全局 OnStatusTick(entity, id, magnitude)。
            // 具体掉血/回血/减速规则由基础库 Gameplay（或项目脚本覆盖）定义。
            if (luaHost && luaHost->HasFunction("OnStatusTick")) {
                (void)luaHost->Call("OnStatusTick",
                    { EntityToValue(ent), script::Value::Num(static_cast<double>(id)),
                      script::Value::Num(static_cast<double>(magnitude)) });
            }
        });
    }
}

bool StatusSystem::Has(ecs::World& world, ecs::Entity e, uint32_t id) const {
    const StatusComponent* c = world.Get<StatusComponent>(e);
    return c ? scene::HasStatus(*c, id) : false;
}

float StatusSystem::Magnitude(ecs::World& world, ecs::Entity e, uint32_t id) const {
    const StatusComponent* c = world.Get<StatusComponent>(e);
    return c ? scene::StatusMagnitude(*c, id) : 0.0f;
}

} // namespace neon::scene
