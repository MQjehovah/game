#pragma once

// Data-driven status effects (buffs / debuffs) for the combat core.
//
// Entities carry a StatusComponent (ECS); effects are identified by stable
// uint32 ids resolved from names through the built-in table (burning/poison
// deal tick damage, regen heals). GameRuntime::Tick advances every entity's
// effects: per-interval ticks fire through a callback and expired effects are
// removed. Scripts apply/query effects through the ApplyStatus/HasStatus/
// StatusMagnitude/RemoveStatus bindings; skills apply effects on hit.

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace neon::scene {

// Stable built-in effect ids (resolved by name; extend the table for more).
enum : uint32_t {
    kStatusBurning = 1, // fire: damage tick every interval
    kStatusPoison = 2,  // poison: damage tick every interval
    kStatusRegen = 3,   // regen: heal tick every interval
    kStatusCount = 4,
};

struct StatusDef {
    uint32_t id = 0;
    const char* name = "";
    float tickInterval = 1.0f;
};

// The built-in definition table (stable order, ids ascending).
const StatusDef* StatusDefs();
// Returns the definition for `id` or nullptr when unknown.
const StatusDef* FindStatusDef(uint32_t id);
// Resolves an effect name to its id (0 = unknown).
uint32_t StatusIdByName(const std::string& name);
// Resolves an id back to its name ("" = unknown).
const char* StatusNameById(uint32_t id);

// One active effect instance on an entity.
struct StatusEffect {
    uint32_t id = 0;
    float remaining = 0.0f;  // seconds left
    float magnitude = 0.0f;  // per-tick amount (damage > 0, heal < 0 for regen)
    float tickInterval = 1.0f;
    float tickAccum = 0.0f;  // time since the last tick
};

// ECS component: the effects currently active on one entity.
struct StatusComponent {
    std::vector<StatusEffect> effects;
};

// Applies `duration` seconds of the effect, refreshing remaining + magnitude
// when an instance of the same id is already active.
void ApplyStatus(StatusComponent& c, uint32_t id, float duration, float magnitude);
// Removes every instance of the effect.
void RemoveStatus(StatusComponent& c, uint32_t id);
// True while at least one instance of the effect is active.
bool HasStatus(const StatusComponent& c, uint32_t id);
// Sum of magnitudes of every active instance (0 when absent).
float StatusMagnitude(const StatusComponent& c, uint32_t id);
// Advances effects by dt: fires onTick(id, magnitude) at each interval
// boundary (once per interval, deterministic order) and erases expired
// effects. Returns true when any effect changed (ticked or expired).
bool TickStatus(StatusComponent& c, float dt,
                const std::function<void(uint32_t id, float magnitude)>& onTick);

} // namespace neon::scene
