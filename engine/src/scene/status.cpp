#include "neon/scene/status.hpp"

#include <algorithm>

namespace neon::scene {

void ApplyStatus(StatusComponent& c, uint32_t id, float duration, float magnitude,
                 float tickInterval) {
    if (id == 0 || duration <= 0.0f) return;
    for (StatusEffect& e : c.effects) {
        if (e.id == id) {
            e.remaining = duration;
            e.magnitude = magnitude;
            e.tickAccum = 0.0f;
            return;
        }
    }
    StatusEffect e;
    e.id = id;
    e.remaining = duration;
    e.magnitude = magnitude;
    e.tickInterval = tickInterval;
    c.effects.push_back(e);
}

void RemoveStatus(StatusComponent& c, uint32_t id) {
    c.effects.erase(std::remove_if(c.effects.begin(), c.effects.end(),
                                   [id](const StatusEffect& e) { return e.id == id; }),
                    c.effects.end());
}

bool HasStatus(const StatusComponent& c, uint32_t id) {
    for (const StatusEffect& e : c.effects)
        if (e.id == id) return true;
    return false;
}

float StatusMagnitude(const StatusComponent& c, uint32_t id) {
    float total = 0.0f;
    for (const StatusEffect& e : c.effects)
        if (e.id == id) total += e.magnitude;
    return total;
}

bool TickStatus(StatusComponent& c, float dt,
                const std::function<void(uint32_t id, float magnitude)>& onTick) {
    bool changed = false;
    for (auto it = c.effects.begin(); it != c.effects.end();) {
        StatusEffect& e = *it;
        e.remaining -= dt;
        e.tickAccum += dt;
        // Epsilon tolerance: float accumulation of dt (e.g. 1/60f summed over
        // 60 ticks) can land just below the nominal interval and would
        // otherwise skip the final tick; the expiry epsilon likewise keeps a
        // just-positive remainder from lingering a frame past its duration.
        constexpr float kEps = 1e-4f;
        if (e.tickInterval > 0.0f && e.tickAccum >= e.tickInterval - kEps) {
            e.tickAccum = 0.0f;
            if (onTick) onTick(e.id, e.magnitude);
            changed = true;
        }
        if (e.remaining <= kEps) {
            it = c.effects.erase(it);
            changed = true;
        } else {
            ++it;
        }
    }
    return changed;
}

} // namespace neon::scene
