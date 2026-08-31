#pragma once

// C2: per-entity animation subsystem (split out of GameRuntime). Owns the
// M1/G5-4-4 animation state that used to live on DrawItem (animClip / animName /
// animLoop / animSpeed / animTime / animFade / animFromPose / animSM*) in a
// plain entityKey -> AnimState table, decoupled from the render structures.
// GameRuntime registers one state per resolved skinned draw item
// (ResolveDrawItem -> InitState), mirrors the entity's SceneAnimOverride
// component into it each frame (SyncOverride), and advances it every Tick
// (Tick). Draw reads the current pose through PoseFor instead of touching
// DrawItem animation fields.
//
// Headless servers (no renderer) never build draw items, so they never
// register animation state and Tick() iterates an empty table - no idle
// animation work (C2).
//
// Not installed as part of the public API surface beyond neon_scene.

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "neon/anim/anim.hpp"
#include "neon/math/mat4.hpp"
#include "neon/scene/skinned_model.hpp"

namespace neon::scene {

class AnimationSystem {
public:
    // Per-entity animation state (the fields moved out of DrawItem). Unlike a
    // DrawItem this is pure simulation/pose state with no render references.
    struct State {
        const anim::AnimationClip* animClip = nullptr; // resolved clip ptr
        std::string animName;                          // requested name (substring)
        bool animLoop = true;
        float animSpeed = 1.0f;
        float animTime = 0.0f;
        float animFade = 0.0f;       // remaining cross-fade seconds
        float animFadeTotal = 0.0f;
        bool animHasOverride = false;
        anim::Pose animFromPose;     // fade source (captured at switch)
        std::shared_ptr<anim::AnimationStateMachine> animSM;
        std::string animSMState;                 // last played state
        bool animSMBound = false;                // clips resolved
        std::map<std::string, float> animSMParams; // script-set params
    };

    // Skinned-model binding for one entity (registered at ResolveDrawItem).
    // Points at the entity's OWN SkinnedModel copy, whose skeleton + clips
    // drive override resolution, whose default animator provides the fade
    // source pose, and whose Update() advances the default loop while the
    // entity has no override. Raw pointer: the owning DrawItem lives in
    // GameRuntime, so Prune() must drop the entry when the draw item goes.
    struct ModelBinding {
        SkinnedModel* model = nullptr;
    };

    // Override intent mirrored from the entity's SceneAnimOverride component
    // (clip/loop/speed/crossFade/active map 1:1 to that component's fields).
    struct OverrideSpec {
        std::string clip;
        bool loop = true;
        float speed = 1.0f;
        float crossFade = 0.0f;
        bool active = false;
    };

    // Registers a skinned entity's animation state (ResolveDrawItem, render
    // path only). Seeds the override intent from `ov`; the clip pointer stays
    // null and is resolved lazily by Tick (fresh cross-fade from the current
    // pose), matching the historical DrawItem behavior.
    void InitState(uint64_t key, const ModelBinding& binding, const OverrideSpec& ov);
    // Mirrors the SceneAnimOverride component into an existing state each
    // frame (BuildDrawList): a clip-name change re-resolves the clip; a
    // deactivated override drops the override. No-op when the key has no state.
    void SyncOverride(uint64_t key, const OverrideSpec& ov);
    // Unconditionally clears the cached clip+name so the next Tick re-resolves
    // (fresh cross-fade). Used by PlayAnimation after re-issuing a clip, even
    // when the name is unchanged. No-op when the key has no state.
    void InvalidateOverride(uint64_t key);

    // Re-issues a clip override on the entity's state immediately (name/loop/
    // speed/crossFade recorded, clip re-resolved by the next Tick). Returns
    // false when the key has no registered state. NOTE: GameRuntime::PlayAnimation
    // does NOT use this - it writes the SceneAnimOverride component and calls
    // InvalidateOverride, preserving the historical "override engages once the
    // draw-list sync re-mirrors it" semantics. Play is the standalone form for
    // callers that already own the state (AnimationSystem unit tests).
    bool Play(uint64_t key, const std::string& clip, bool loop, float crossFade, float speed);
    // Normalized [0,1] progress of the entity's override clip (0 when none,
    // 1 when a one-shot finished). Mirrors GameRuntime::AnimationProgress.
    float Progress(uint64_t key) const;
    // True while an override clip exists AND has finished (one-shots only).
    bool Finished(uint64_t key) const;
    // Attaches a loaded state machine (JSON already parsed by the caller).
    // Returns false when the key has no resolved skinned binding. The machine
    // binds its states' clip names to the model's clips lazily on the first
    // Tick (animSMBound), matching the old TickAnimations behavior.
    bool AttachStateMachine(uint64_t key,
                            const std::shared_ptr<anim::AnimationStateMachine>& sm);
    // Sets a parameter on the entity's state machine (no-op without one).
    void SetParam(uint64_t key, const std::string& name, float value);

    // Advances every registered entity's animation: the state machine (if
    // attached), the override clip (or the model's default loop otherwise).
    // Iterates only registered states, so headless hosts tick an empty table.
    void Tick(float dt);
    // Skinning matrices for the entity's current override pose (bind pose +
    // sampled clip + active cross-fade). Returns false when the entity has no
    // override pose - the caller then falls back to the model's default
    // BoneMatrices() (same branch Draw() used to take on DrawItem). `skeleton`
    // must be the bound model's skeleton (the caller already holds it).
    bool PoseFor(uint64_t key, const anim::Skeleton& skeleton,
                 std::vector<math::Mat4>& out) const;

    // Drops state+binding for keys whose draw item is gone (BuildDrawList
    // calls this with the alive-entity set after pruning dead draws). Keeps the
    // raw binding pointers from dangling into freed SkinnedModels.
    void Prune(const std::function<bool(uint64_t)>& alive);
    // Drops all state + bindings (Stop lifecycle).
    void Clear();
    size_t Count() const { return states_.size(); }
    bool HasBinding(uint64_t key) const { return bindings_.count(key) != 0; }

private:
    std::unordered_map<uint64_t, State> states_;
    std::unordered_map<uint64_t, ModelBinding> bindings_;
};

} // namespace neon::scene
