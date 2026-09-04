// C2: AnimationSystem implementation. Migrated from GameRuntime's animation
// subsystem (TickAnimations / PlayAnimation / AttachStateMachine / SetAnimParam /
// AnimationProgress / AnimationFinished). The per-entity animation state moved
// out of DrawItem into the entityKey -> State table; DrawItem now only carries
// the SkinnedModel render reference. GameRuntime registers states at
// ResolveDrawItem (render path only) and forwards the script-facing API.
#include "neon/scene/systems/animation_system.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>

namespace neon::scene {

namespace {

// Case-insensitive substring match, first hit wins (deterministic) - mirrors
// SkinnedModel::PlayClip / BindStateMachineClips lookup semantics.
const anim::AnimationClip* FindClip(const std::vector<anim::AnimationClip>& clips,
                                    const std::string& needle) {
    std::string low = needle;
    for (char& ch : low)
        ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    for (const anim::AnimationClip& c : clips) {
        std::string hay = c.name;
        for (char& ch : hay)
            ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
        if (hay.find(low) != std::string::npos) return &c;
    }
    return nullptr;
}

} // namespace

void AnimationSystem::InitState(uint64_t key, const ModelBinding& binding,
                                const OverrideSpec& ov) {
    State st;
    st.animHasOverride = ov.active;
    if (ov.active) {
        st.animName = ov.clip;
        st.animLoop = ov.loop;
        st.animSpeed = ov.speed;
        st.animFadeTotal = ov.crossFade;
        // animClip stays null; Tick resolves it (fresh fade from the current
        // pose), exactly like the historical BuildDrawList -> TickAnimations path.
    }
    states_[key] = std::move(st);
    bindings_[key] = binding;
}

void AnimationSystem::SyncOverride(uint64_t key, const OverrideSpec& ov) {
    auto it = states_.find(key);
    if (it == states_.end()) return;
    State& st = it->second;
    if (!ov.active) {
        st.animHasOverride = false;
        st.animClip = nullptr;
        st.animName.clear();
        return;
    }
    if (st.animName != ov.clip) {
        st.animName = ov.clip;
        st.animClip = nullptr; // re-resolve in Tick
        st.animLoop = ov.loop;
        st.animSpeed = ov.speed;
        st.animFadeTotal = ov.crossFade;
    }
    st.animHasOverride = true;
}

void AnimationSystem::InvalidateOverride(uint64_t key) {
    auto it = states_.find(key);
    if (it == states_.end()) return;
    it->second.animName.clear();
    it->second.animClip = nullptr;
}

bool AnimationSystem::Play(uint64_t key, const std::string& clip, bool loop,
                           float crossFade, float speed) {
    auto it = states_.find(key);
    if (it == states_.end()) return false;
    State& st = it->second;
    st.animName = clip;
    st.animClip = nullptr; // re-resolve in Tick (fresh cross-fade)
    st.animLoop = loop;
    st.animSpeed = speed;
    st.animFadeTotal = crossFade;
    st.animHasOverride = true;
    return true;
}

float AnimationSystem::Progress(uint64_t key) const {
    auto it = states_.find(key);
    if (it == states_.end()) return 0.0f;
    const State& st = it->second;
    if (!st.animHasOverride || !st.animClip) return 0.0f;
    if (st.animClip->duration <= 0.0f) return 1.0f;
    float p = st.animTime / st.animClip->duration;
    return p < 0.0f ? 0.0f : (p > 1.0f ? 1.0f : p);
}

bool AnimationSystem::Finished(uint64_t key) const {
    auto it = states_.find(key);
    if (it == states_.end()) return false;
    const State& st = it->second;
    if (!st.animHasOverride || !st.animClip) return false;
    return !st.animLoop && st.animTime >= st.animClip->duration;
}

bool AnimationSystem::AttachStateMachine(
    uint64_t key, const std::shared_ptr<anim::AnimationStateMachine>& sm) {
    auto it = states_.find(key);
    if (it == states_.end()) return false;
    auto bit = bindings_.find(key);
    if (bit == bindings_.end() || !bit->second.model || !bit->second.model->Valid())
        return false;
    State& st = it->second;
    st.animSM = sm;
    st.animSMState.clear();
    st.animSMBound = false;
    st.animSMParams.clear();
    return true;
}

void AnimationSystem::SetParam(uint64_t key, const std::string& name, float value) {
    auto it = states_.find(key);
    if (it == states_.end() || !it->second.animSM) return;
    it->second.animSMParams[name] = value;
}

bool AnimationSystem::PlayBlend(uint64_t key, const std::string& clipA,
                                const std::string& clipB, float t) {
    auto it = states_.find(key);
    if (it == states_.end()) return false;
    auto bit = bindings_.find(key);
    if (bit == bindings_.end() || !bit->second.model) return false;
    // Resolve both endpoints against the binding's model clips (empty until the
    // draw item resolves the model; Tick will re-resolve once bound).
    const std::vector<anim::AnimationClip>& clips = bit->second.model->clips;
    const anim::AnimationClip* a = FindClip(clips, clipA);
    const anim::AnimationClip* b = FindClip(clips, clipB);
    if (!a || !b) return false;
    State& st = it->second;
    st.blendA = clipA;
    st.blendB = clipB;
    st.blendClipA = a;
    st.blendClipB = b;
    st.blendParam = t < 0.0f ? 0.0f : (t > 1.0f ? 1.0f : t);
    st.blendActive = true;
    st.animTime = 0.0f;
    st.animHasOverride = true;
    return true;
}

void AnimationSystem::SetBlendParam(uint64_t key, float t) {
    auto it = states_.find(key);
    if (it == states_.end()) return;
    it->second.blendParam = t < 0.0f ? 0.0f : (t > 1.0f ? 1.0f : t);
}

void AnimationSystem::Tick(float dt) {
    for (auto& kv : states_) {
        State& st = kv.second;
        auto bit = bindings_.find(kv.first);
        if (bit == bindings_.end() || !bit->second.model || !bit->second.model->Valid())
            continue;
        SkinnedModel* skinned = bit->second.model;
        // B3 BlendSpace1D: advance shared time (wrap in the shorter clip) and
        // continue — the pose is computed in PoseFor from the two blend clips.
        if (st.blendActive && st.blendClipA && st.blendClipB) {
            float dur = st.blendClipA->duration;
            if (st.blendClipB->duration > 0.0f) dur = std::max(dur, st.blendClipB->duration);
            if (dur > 0.0f) st.animTime = std::fmod(st.animTime + dt, dur);
            continue;
        }
        // G5-4-4(项2): data-driven animation state machine. Advance it (params
        // from the script-set map), then map the current state's clip onto the
        // existing override path below - no pose surgery.
        if (st.animSM) {
            if (!st.animSMBound) {
                anim::BindStateMachineClips(*st.animSM, skinned->clips);
                st.animSMBound = true;
                if (!st.animSM->States().empty()) {
                    // Start on the first state so Update() has a current_.
                    const std::string first = st.animSM->States()[0].name;
                    st.animSM->Play(first);
                    st.animSMState = first;
                    const anim::AnimationClip* c = st.animSM->StateClip(0);
                    if (c) {
                        st.animClip = c;
                        st.animName = st.animSM->States()[0].clipName;
                        st.animLoop = true;
                        st.animSpeed = 1.0f;
                        st.animTime = 0.0f;
                        st.animHasOverride = true;
                    }
                }
            }
            for (const auto& [name, value] : st.animSMParams)
                st.animSM->SetParam(name, value);
            st.animSM->Update(dt);
            const std::string state = st.animSM->CurrentState();
            if (!state.empty() && state != st.animSMState) {
                st.animSMState = state;
                const anim::AnimationClip* clip = nullptr;
                std::string clipName;
                for (const anim::AnimState& s : st.animSM->States())
                    if (s.name == state) {
                        clip = s.clip;
                        clipName = s.clipName;
                        break;
                    }
                if (clip) {
                    st.animClip = clip;
                    st.animName = clipName;
                    st.animLoop = true;
                    st.animSpeed = 1.0f;
                    st.animTime = 0.0f;
                    st.animHasOverride = true;
                }
            }
            // Fall through: the override branch below advances animTime for the
            // current state's clip.
        }
        if (st.animHasOverride) {
            // Resolve the clip pointer on first use (or after a re-resolve).
            if (!st.animClip) {
                st.animClip = FindClip(skinned->clips, st.animName);
                if (st.animClip) {
                    // Capture the fade source pose from the shared default
                    // loop so the cross-fade starts where the model is now.
                    st.animFromPose = skinned->skeleton.BindPose();
                    if (skinned->defaultClip >= 0 && skinned->animator.Clip())
                        skinned->animator.Clip()->Sample(skinned->animator.Time(),
                                                         st.animFromPose);
                    st.animTime = 0.0f;
                    st.animFade = st.animFadeTotal;
                }
            }
            if (st.animClip) {
                const float prev = st.animTime;
                st.animTime += dt * st.animSpeed;
                if (st.animFade > 0.0f) st.animFade = std::fmax(0.0f, st.animFade - dt);
                float dur = st.animClip->duration;
                if (st.animLoop && dur > 0.0f) {
                    st.animTime = std::fmod(st.animTime, dur);
                } else if (st.animTime > dur) {
                    st.animTime = dur; // one-shot clamps at end
                } else if (dur <= 0.0f) {
                    dur = st.animTime;
                }
                // B3 animation events: fire any event whose time was crossed
                // between prev and this tick's (loop-wrapped) time. Only when
                // the clip actually advanced (dt>0) and not mid cross-fade skip.
                if (dur > 0.0f) {
                    for (const anim::AnimEvent& ev : st.animClip->events) {
                        if (st.animClip->EventCrossed(ev, prev, st.animTime))
                            st.animEvents.push_back(ev.name);
                    }
                }
                st.animPrevTime = st.animTime;
            }
        } else {
            skinned->Update(dt);
        }
    }
}

bool AnimationSystem::PoseFor(uint64_t key, const anim::Skeleton& skeleton,
                              std::vector<math::Mat4>& out) const {
    auto it = states_.find(key);
    if (it == states_.end()) return false;
    const State& st = it->second;
    // B3 BlendSpace1D: blend endpoint clips (resolved at PlayBlend time) sampled
    // at the shared state time, lerped by blendParam.
    if (st.blendActive && st.blendClipA && st.blendClipB) {
        anim::Pose pose = skeleton.BindPose();
        anim::Pose wa = skeleton.BindPose();
        anim::Pose wb = skeleton.BindPose();
        st.blendClipA->Sample(st.animTime, wa);
        st.blendClipB->Sample(st.animTime, wb);
        pose.Lerp(wa, wb, st.blendParam);
        out = skeleton.ComputeBoneMatrices(pose);
        return true;
    }
    if (!st.animHasOverride || !st.animClip) return false;
    anim::Pose pose = skeleton.BindPose();
    st.animClip->Sample(st.animTime, pose);
    if (st.animFade > 0.0f && st.animFadeTotal > 0.0f &&
        st.animFromPose.t.size() == pose.t.size())
        pose.Lerp(st.animFromPose, pose, 1.0f - st.animFade / st.animFadeTotal);
    out = skeleton.ComputeBoneMatrices(pose);
    return true;
}

void AnimationSystem::Prune(const std::function<bool(uint64_t)>& alive) {
    for (auto it = states_.begin(); it != states_.end();) {
        if (alive(it->first)) {
            ++it;
        } else {
            bindings_.erase(it->first);
            it = states_.erase(it);
        }
    }
}

void AnimationSystem::Clear() {
    states_.clear();
    bindings_.clear();
}

std::vector<std::string> AnimationSystem::ConsumeEvents(uint64_t key) {
    auto it = states_.find(key);
    if (it == states_.end()) return {};
    std::vector<std::string> out = std::move(it->second.animEvents);
    it->second.animEvents.clear();
    return out;
}

} // namespace neon::scene
