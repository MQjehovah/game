#pragma once

// Status effect container (buffs / debuffs) for the combat core.
//
// Entities carry a StatusComponent (ECS); each effect is identified by a
// stable uint32 id and carries a custom per-effect tick interval. GameRuntime::
// Tick advances every entity's effects: per-interval ticks fire through a
// callback and expired effects are removed. Name -> id resolution and the
// tick rules (damage / heal / slow ...) live in the Lua Gameplay library; the
// engine only owns the numeric-id container primitives (ApplyStatus /
// HasStatus / StatusMagnitude / RemoveStatus / TickStatus).

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace neon::scene {

// A status effect spec applied to a hit target (numeric id resolved by the
// caller; tick rules live in the Lua Gameplay library). Used by projectiles
// and data-driven skills to carry their on-hit effects.
struct SkillStatus {
    uint32_t id = 0;
    float duration = 0.0f;
    float magnitude = 0.0f;
    float tickInterval = 1.0f;
};

// Stable built-in effect ids (name -> id mapping lives in the Lua Gameplay
// library; these constants stay for serialization stability and C++ tests).
enum : uint32_t {
    kStatusBurning = 1, // fire: damage tick every interval
    kStatusPoison = 2,  // poison: damage tick every interval
    kStatusRegen = 3,   // regen: heal tick every interval
    kStatusSlow = 4,    // slow: movement modifier (magnitude = speed factor), no tick
    kStatusCount = 5,
};

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
// when an instance of the same id is already active. `tickInterval` sets the
// per-effect tick cadence (default 1.0s).
void ApplyStatus(StatusComponent& c, uint32_t id, float duration, float magnitude,
                 float tickInterval = 1.0f);
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
