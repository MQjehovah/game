// TweenSystem implementation. Migrated verbatim from GameRuntime's TickTweens
// (P1-3): linear/in/out/inout easing over the entity's position, rotation
// (euler degrees) or scale. Finished tweens are dropped. The world is now a
// Tick parameter instead of a GameRuntime member; pure code movement, no
// semantic change.
#include "neon/scene/systems/tween_system.hpp"

#include "neon/math/math.hpp"
#include "neon/math/quat.hpp"
#include "neon/scene/scene_file.hpp"

namespace neon::scene {

void TweenSystem::Start(ecs::Entity target, int prop, const math::Vec3& from,
                        const math::Vec3& to, float time, int easing) {
    if (time <= 0.0f) return;
    Tween tw;
    tw.target = target;
    tw.prop = prop;
    tw.from = from;
    tw.to = to;
    tw.time = time;
    tw.easing = easing;
    tw.elapsed = 0.0f;
    tweens_.push_back(tw);
}

void TweenSystem::Tick(float dt, ecs::World& world) {
    if (tweens_.empty()) return;
    auto ease = [](float a, int kind) {
        switch (kind) {
            case 1: return a * a;                                  // in
            case 2: return 1.0f - (1.0f - a) * (1.0f - a);        // out
            case 3:                                               // inout
                return a < 0.5f ? 2.0f * a * a
                                : 1.0f - (-2.0f * a + 2.0f) * (-2.0f * a + 2.0f) * 0.5f;
            default: return a;                                    // linear
        }
    };
    for (size_t i = 0; i < tweens_.size();) {
        Tween& tw = tweens_[i];
        tw.elapsed += dt;
        const float a = math::Clamp(tw.elapsed / tw.time, 0.0f, 1.0f);
        const float e = ease(a, tw.easing);
        const math::Vec3 v = math::Lerp(tw.from, tw.to, e);
        if (SceneTransform* t = world.Get<SceneTransform>(tw.target)) {
            if (tw.prop == 0) {
                t->pos = v;
            } else if (tw.prop == 1) {
                t->rot = math::Quat::FromEuler(v.x * 3.14159265f / 180.0f,
                                               v.y * 3.14159265f / 180.0f,
                                               v.z * 3.14159265f / 180.0f);
            } else if (tw.prop == 2) {
                t->scale = v;
            }
        }
        if (tw.elapsed >= tw.time) {
            tweens_[i] = tweens_.back();
            tweens_.pop_back();
        } else {
            ++i;
        }
    }
}

} // namespace neon::scene
