#include "neon/anim/anim.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>

#include "neon/core/json.hpp"
#include "neon/math/quat.hpp"

namespace neon::anim {

namespace {

float WrapT(float t, float duration) {
    if (duration <= 0.0f) return 0.0f;
    float r = std::fmod(t, duration);
    if (r < 0.0f) r += duration;
    return r;
}

// Cubic spline value layout: [inTangent, value, outTangent] per keyframe.
// Linear/Step store one value per keyframe.
inline const math::Vec3& Vec3At(const std::vector<math::Vec3>& v, Interp interp, int key) {
    return interp == Interp::CubicSpline ? v[static_cast<size_t>(key) * 3 + 1]
                                         : v[static_cast<size_t>(key)];
}
inline const math::Quat& QuatAt(const std::vector<math::Quat>& v, Interp interp, int key) {
    return interp == Interp::CubicSpline ? v[static_cast<size_t>(key) * 3 + 1]
                                         : v[static_cast<size_t>(key)];
}
inline math::Vec3 CubicVec3Tangent(const std::vector<math::Vec3>& v, int key, bool in) {
    return v[static_cast<size_t>(key) * 3 + (in ? 0u : 2u)];
}
inline math::Quat CubicQuatTangent(const std::vector<math::Quat>& v, int key, bool in) {
    return v[static_cast<size_t>(key) * 3 + (in ? 0u : 2u)];
}

// Standard cubic Hermite basis on local parameter u in [0,1], dt = t_{k+1}-t_k.
float Hermite(float p0, float out0, float p1, float in1, float dt, float u) {
    float u2 = u * u;
    float u3 = u2 * u;
    float h1 = 2.0f * u3 - 3.0f * u2 + 1.0f;
    float h2 = u3 - 2.0f * u2 + u;
    float h3 = -2.0f * u3 + 3.0f * u2;
    float h4 = u3 - u2;
    return p0 * h1 + out0 * (dt * h2) + p1 * h3 + in1 * (dt * h4);
}

math::Vec3 SampleVec3Channel(const Track& tr, const std::vector<math::Vec3>& values,
                             int key, int next, float u) {
    if (tr.interp == Interp::Step || next < 0) return Vec3At(values, tr.interp, key);
    if (tr.interp == Interp::Linear)
        return math::Lerp(Vec3At(values, tr.interp, key), Vec3At(values, tr.interp, next), u);
    float dt = tr.times[static_cast<size_t>(next)] - tr.times[static_cast<size_t>(key)];
    if (dt <= 0.0f) return Vec3At(values, tr.interp, key);
    const math::Vec3& p0 = Vec3At(values, tr.interp, key);
    const math::Vec3& p1 = Vec3At(values, tr.interp, next);
    math::Vec3 out0 = CubicVec3Tangent(values, key, false);
    math::Vec3 in1 = CubicVec3Tangent(values, next, true);
    math::Vec3 r;
    r.x = Hermite(p0.x, out0.x, p1.x, in1.x, dt, u);
    r.y = Hermite(p0.y, out0.y, p1.y, in1.y, dt, u);
    r.z = Hermite(p0.z, out0.z, p1.z, in1.z, dt, u);
    return r;
}

math::Quat SampleQuatChannel(const Track& tr, const std::vector<math::Quat>& values,
                             int key, int next, float u) {
    if (tr.interp == Interp::Step || next < 0) return QuatAt(values, tr.interp, key);
    if (tr.interp == Interp::Linear)
        return math::Slerp(QuatAt(values, tr.interp, key), QuatAt(values, tr.interp, next), u);
    float dt = tr.times[static_cast<size_t>(next)] - tr.times[static_cast<size_t>(key)];
    if (dt <= 0.0f) return QuatAt(values, tr.interp, key);
    const math::Quat& p0 = QuatAt(values, tr.interp, key);
    const math::Quat& p1 = QuatAt(values, tr.interp, next);
    math::Quat out0 = CubicQuatTangent(values, key, false);
    math::Quat in1 = CubicQuatTangent(values, next, true);
    math::Quat r;
    r.x = Hermite(p0.x, out0.x, p1.x, in1.x, dt, u);
    r.y = Hermite(p0.y, out0.y, p1.y, in1.y, dt, u);
    r.z = Hermite(p0.z, out0.z, p1.z, in1.z, dt, u);
    r.w = Hermite(p0.w, out0.w, p1.w, in1.w, dt, u);
    return r.Normalized();
}

} // namespace

void Pose::Resize(size_t boneCount) {
    t.assign(boneCount, math::Vec3{0, 0, 0});
    r.assign(boneCount, math::Quat{0, 0, 0, 1});
    s.assign(boneCount, math::Vec3{1, 1, 1});
}

void Pose::Lerp(const Pose& a, const Pose& b, float alpha) {
    size_t n = std::min({a.t.size(), a.r.size(), a.s.size(),
                         b.t.size(), b.r.size(), b.s.size()});
    t.resize(n);
    r.resize(n);
    s.resize(n);
    for (size_t i = 0; i < n; ++i) {
        t[i] = math::Lerp(a.t[i], b.t[i], alpha);
        r[i] = math::Slerp(a.r[i], b.r[i], alpha);
        s[i] = math::Lerp(a.s[i], b.s[i], alpha);
    }
}

namespace {
// Local TRS of bone i from the pose (or the bind pose when not pose-sized).
math::Mat4 LocalFrom(const Pose& pose, bool poseSized, size_t i,
                     const std::vector<Bone>& bones) {
    math::Vec3 t = bones[i].bindT;
    math::Quat r = bones[i].bindR;
    math::Vec3 s = bones[i].bindS;
    if (poseSized) {
        t = pose.t[i];
        r = pose.r[i];
        s = pose.s[i];
    }
    return math::Mat4::Translation(t) * r.ToMat4() * math::Mat4::Scale(s);
}
} // namespace

int Skeleton::FindBone(const std::string& name) const {
    for (size_t i = 0; i < bones.size(); ++i) {
        if (bones[i].name == name) return static_cast<int>(i);
    }
    return -1;
}

Pose Skeleton::BindPose() const {
    Pose p;
    p.Resize(bones.size());
    for (size_t i = 0; i < bones.size(); ++i) {
        p.t[i] = bones[i].bindT;
        p.r[i] = bones[i].bindR;
        p.s[i] = bones[i].bindS;
    }
    return p;
}

std::vector<math::Mat4> Skeleton::ComputeBoneMatrices(const Pose& pose) const {
    size_t n = bones.size();
    bool poseSized = pose.t.size() == n && pose.r.size() == n && pose.s.size() == n;
    std::vector<math::Mat4> skinning(n);
    std::vector<math::Mat4> world(n);
    // Order-independent parent resolution: glTF does not require parents to
    // precede children in the node table (the Blender wolf rig lists child
    // joints before their parents), so a single forward pass would read a
    // not-yet-computed parent world as identity and break the chain. Iterate
    // until every bone is resolved (same pattern as FixSkinBind).
    std::vector<bool> done(n, false);
    for (size_t i = 0; i < n; ++i) {
        const Bone& b = bones[i];
        if (b.parent < 0 || static_cast<size_t>(b.parent) >= n) {
            world[i] = LocalFrom(pose, poseSized, i, bones);
            done[i] = true;
        }
    }
    bool changed = true;
    while (changed) {
        changed = false;
        for (size_t i = 0; i < n; ++i) {
            if (done[i]) continue;
            const Bone& b = bones[i];
            const size_t p = static_cast<size_t>(b.parent);
            if (!done[p]) continue;
            world[i] = world[p] * LocalFrom(pose, poseSized, i, bones);
            done[i] = true;
            changed = true;
        }
    }
    // Cycle / missing parent fallback: local-only (matches the old behavior).
    for (size_t i = 0; i < n; ++i)
        if (!done[i]) world[i] = LocalFrom(pose, poseSized, i, bones);
    for (size_t i = 0; i < n; ++i) skinning[i] = world[i] * bones[i].inverseBind;
    return skinning;
}

void AnimationClip::Sample(float t, Pose& out) const {
    t = WrapT(t, duration);
    for (const Track& tr : tracks) {
        if (tr.bone < 0 || tr.bone >= static_cast<int>(out.t.size())) continue;
        if (tr.times.empty()) continue;
        int key = -1;
        int next = -1;
        float u = 0.0f;
        if (t <= tr.times.front()) {
            key = 0;
        } else if (t >= tr.times.back()) {
            key = static_cast<int>(tr.times.size()) - 1;
        } else {
            // times is sorted; binary search the first index with times[i] > t
            // so an exact key lands on the segment starting there (STEP jumps
            // to the key's value, matching the previous scan semantics).
            auto it = std::upper_bound(tr.times.begin(), tr.times.end(), t);
            key = static_cast<int>(it - tr.times.begin()) - 1;
            next = key + 1;
            float span = tr.times[static_cast<size_t>(next)] - tr.times[static_cast<size_t>(key)];
            u = span > 0.0f ? (t - tr.times[static_cast<size_t>(key)]) / span : 0.0f;
        }
        if (key < 0) continue;
        if (!tr.translations.empty())
            out.t[tr.bone] = SampleVec3Channel(tr, tr.translations, key, next, u);
        if (!tr.rotations.empty())
            out.r[tr.bone] = SampleQuatChannel(tr, tr.rotations, key, next, u);
        if (!tr.scales.empty())
            out.s[tr.bone] = SampleVec3Channel(tr, tr.scales, key, next, u);
    }
}

int AnimationStateMachine::FindState(const std::string& name) const {
    for (size_t i = 0; i < states_.size(); ++i) {
        if (states_[i].name == name) return static_cast<int>(i);
    }
    return -1;
}

void AnimationStateMachine::AddState(const std::string& name, const AnimationClip* clip) {
    states_.push_back({name, clip});
}

void AnimationStateMachine::AddTransition(const std::string& from, const std::string& to,
                                          const std::string& param, float threshold,
                                          float duration) {
    transitions_.push_back({from, to, param, threshold, duration});
}

void AnimationStateMachine::SetParam(const std::string& name, float value) {
    params_[name] = value;
}

void AnimationStateMachine::SetBoneCount(size_t n) {
    bind_.Resize(n);
    workA_.Resize(n);
    workB_.Resize(n);
    result_.Resize(n);
}

void AnimationStateMachine::SetBindPose(const Pose& bind) {
    bind_ = bind;
    workA_.Resize(bind.t.size());
    workB_.Resize(bind.t.size());
    result_.Resize(bind.t.size());
}

void AnimationStateMachine::Play(const std::string& state) {
    int idx = FindState(state);
    if (idx < 0) return;
    current_ = idx;
    previous_ = -1;
    currentTime_ = 0.0f;
    previousTime_ = 0.0f;
    blendAlpha_ = 1.0f;
}

void AnimationStateMachine::SampleStateInto(Pose& out, int stateIdx, float time) const {
    out = bind_;
    if (stateIdx >= 0 && stateIdx < static_cast<int>(states_.size()) && states_[stateIdx].clip)
        states_[stateIdx].clip->Sample(time, out);
}

void AnimationStateMachine::Update(float dt) {
    if (current_ < 0 || current_ >= static_cast<int>(states_.size())) return;

    // Trigger a transition when the blend is idle and a condition parameter
    // crossed its threshold (>=). Only one transition starts per Update.
    if (blendAlpha_ >= 1.0f) {
        for (const AnimTransition& tr : transitions_) {
            if (tr.from != states_[current_].name) continue;
            auto it = params_.find(tr.param);
            if (it == params_.end() || it->second < tr.threshold) continue;
            int to = FindState(tr.to);
            if (to < 0 || to == current_) continue;
            if (tr.duration <= 0.0f) {
                // Zero-duration transition: hard switch, no cross-fade.
                current_ = to;
                currentTime_ = 0.0f;
                previous_ = -1;
                blendAlpha_ = 1.0f;
            } else {
                previous_ = current_;
                previousTime_ = currentTime_;
                current_ = to;
                currentTime_ = 0.0f;
                blendAlpha_ = 0.0f;
                blendDuration_ = tr.duration;
            }
            break;
        }
    }

    // Advance clip time and the cross-fade blend.
    currentTime_ += dt;
    if (previous_ >= 0) previousTime_ += dt;
    if (blendAlpha_ < 1.0f) {
        blendAlpha_ += dt / blendDuration_;
        if (blendAlpha_ >= 1.0f) {
            blendAlpha_ = 1.0f;
            previous_ = -1;
        }
    }

    if (previous_ >= 0) {
        SampleStateInto(workA_, current_, currentTime_);
        SampleStateInto(workB_, previous_, previousTime_);
        result_.Lerp(workB_, workA_, blendAlpha_);
    } else {
        SampleStateInto(result_, current_, currentTime_);
    }
}

std::string AnimationStateMachine::CurrentState() const {
    if (current_ < 0 || current_ >= static_cast<int>(states_.size())) return {};
    return states_[current_].name;
}

void Animator::Play(const AnimationClip* clip, float startTime) {
    clip_ = clip;
    time_ = startTime;
}

void Animator::Update(float dt) { time_ += dt; }

core::Result<AnimSet> ImportGltf(const std::string& jsonText,
                                 const assets::GltfAsset& asset, int skinIndex) {
    std::string parseError;
    core::Json root = core::Json::Parse(jsonText, &parseError);
    if (root.IsNull())
        return core::Result<AnimSet>::Err("anim: glTF JSON parse error: " + parseError);

    AnimSet out;

    // Skeleton: one bone per glTF node (bone index == node index) with bind TRS
    // from the node hierarchy; joints get their inverseBind from the skin.
    out.skeleton.bones.resize(asset.nodesAll.size());
    for (size_t i = 0; i < asset.nodesAll.size(); ++i) {
        const assets::GltfNode& gn = asset.nodesAll[i];
        Bone& b = out.skeleton.bones[i];
        b.name = gn.name.empty() ? "node" + std::to_string(i) : gn.name;
        b.parent = gn.parent;
        b.bindT = gn.t;
        b.bindR = gn.r;
        b.bindS = gn.s;
        b.inverseBind = math::Mat4::Identity();
    }
    if (skinIndex >= 0 && skinIndex < static_cast<int>(asset.skins.size())) {
        const gfx::Skin& skin = asset.skins[static_cast<size_t>(skinIndex)];
        for (size_t j = 0; j < skin.joints.size() && j < skin.inverseBind.size(); ++j) {
            uint32_t nodeIdx = skin.joints[j];
            if (nodeIdx < out.skeleton.bones.size())
                out.skeleton.bones[nodeIdx].inverseBind = skin.inverseBind[j];
        }
    }

    const core::Json* anims = root.Get("animations");
    if (!anims) return core::Result<AnimSet>::Ok(std::move(out));

    for (size_t ai = 0; ai < anims->Size(); ++ai) {
        const core::Json* a = anims->At(ai);
        if (!a) continue;
        AnimationClip clip;
        clip.name = a->Get("name") ? a->Get("name")->GetString() : std::string();
        if (clip.name.empty()) clip.name = "anim" + std::to_string(ai);

        const core::Json* samplers = a->Get("samplers");
        const core::Json* channels = a->Get("channels");
        if (!samplers || !channels) continue;

        struct SamplerData {
            std::vector<float> times;
            std::vector<float> values;
            Interp interp = Interp::Linear;
        };
        std::vector<SamplerData> samplerData(samplers->Size());
        for (size_t si = 0; si < samplers->Size(); ++si) {
            const core::Json* s = samplers->At(si);
            if (!s) continue;
            SamplerData& sd = samplerData[si];
            if (const core::Json* interpNode = s->Get("interpolation")) {
                std::string is = interpNode->GetString();
                if (is == "STEP") sd.interp = Interp::Step;
                else if (is == "CUBICSPLINE") sd.interp = Interp::CubicSpline;
            }
            if (const core::Json* in = s->Get("input")) {
                core::Result<std::vector<float>> r = asset.ReadAccessorFloats(in->GetInt());
                if (r.Ok()) sd.times = std::move(r.Value());
            }
            if (const core::Json* outNode = s->Get("output")) {
                core::Result<std::vector<float>> r = asset.ReadAccessorFloats(outNode->GetInt());
                if (r.Ok()) sd.values = std::move(r.Value());
            }
            if (!sd.times.empty()) clip.duration = std::max(clip.duration, sd.times.back());
        }

        for (size_t ci = 0; ci < channels->Size(); ++ci) {
            const core::Json* ch = channels->At(ci);
            if (!ch) continue;
            const core::Json* samplerNode = ch->Get("sampler");
            const core::Json* target = ch->Get("target");
            if (!samplerNode || !target) continue;
            const core::Json* nodeNode = target->Get("node");
            const core::Json* pathNode = target->Get("path");
            if (!nodeNode || !pathNode) continue;
            int si = samplerNode->GetInt(-1);
            int nodeIdx = nodeNode->GetInt(-1);
            std::string path = pathNode->GetString();
            if (si < 0 || si >= static_cast<int>(samplerData.size())) continue;
            if (nodeIdx < 0 || nodeIdx >= static_cast<int>(out.skeleton.bones.size())) continue;
            const SamplerData& sd = samplerData[static_cast<size_t>(si)];
            if (sd.times.empty() || sd.values.empty()) continue;

            Track tr;
            tr.bone = nodeIdx;
            tr.interp = sd.interp;
            tr.times = sd.times;
            size_t nKeys = tr.times.size();
            size_t slots = tr.interp == Interp::CubicSpline ? nKeys * 3 : nKeys;
            if (path == "translation" || path == "scale") {
                size_t comps = 3;
                if (sd.values.size() < slots * comps) continue;
                std::vector<math::Vec3>& dst =
                    path == "translation" ? tr.translations : tr.scales;
                dst.resize(slots);
                for (size_t k = 0; k < slots; ++k)
                    dst[k] = {sd.values[k * 3], sd.values[k * 3 + 1], sd.values[k * 3 + 2]};
            } else if (path == "rotation") {
                size_t comps = 4;
                if (sd.values.size() < slots * comps) continue;
                tr.rotations.resize(slots);
                for (size_t k = 0; k < slots; ++k)
                    tr.rotations[k] = {sd.values[k * 4], sd.values[k * 4 + 1],
                                       sd.values[k * 4 + 2], sd.values[k * 4 + 3]};
            } else {
                continue; // "weights" (morph targets) not supported yet
            }
            clip.tracks.push_back(std::move(tr));
        }
        out.clips.push_back(std::move(clip));
    }
    return core::Result<AnimSet>::Ok(std::move(out));
}

// ---------------------------------------------------------------------------
// Blend spaces (P1-3)
// ---------------------------------------------------------------------------

void BlendSpace1D::SetClips(const AnimationClip* a, const AnimationClip* b) {
    a_ = a;
    b_ = b;
}

void BlendSpace1D::SetParam(float v) { param_ = std::fmax(0.0f, std::fmin(v, 1.0f)); }

void BlendSpace1D::SetBoneCount(size_t n) {
    bind_.Resize(n);
    workA_.Resize(n);
    workB_.Resize(n);
    result_.Resize(n);
}

void BlendSpace1D::SetBindPose(const Pose& bind) { bind_ = bind; }

void BlendSpace1D::Update(float dt) {
    if (!a_ || !b_) return;
    const float dur = std::fmax(a_->duration, b_->duration);
    if (dur > 0.0f) {
        time_ += dt;
        time_ = std::fmod(time_, dur);
        if (time_ < 0.0f) time_ += dur;
    }
    workA_ = bind_;
    a_->Sample(time_, workA_);
    workB_ = bind_;
    b_->Sample(time_, workB_);
    result_.Lerp(workA_, workB_, param_);
}

void BlendSpace2D::SetClips(const AnimationClip* ll, const AnimationClip* lr,
                            const AnimationClip* ul, const AnimationClip* ur) {
    clips_[0] = ll;
    clips_[1] = lr;
    clips_[2] = ul;
    clips_[3] = ur;
}

void BlendSpace2D::SetParam(float x, float y) {
    px_ = std::fmax(0.0f, std::fmin(x, 1.0f));
    py_ = std::fmax(0.0f, std::fmin(y, 1.0f));
}

void BlendSpace2D::SetBoneCount(size_t n) {
    bind_.Resize(n);
    for (Pose& w : work_) w.Resize(n);
    result_.Resize(n);
}

void BlendSpace2D::SetBindPose(const Pose& bind) { bind_ = bind; }

void BlendSpace2D::Update(float dt) {
    if (!clips_[0] || !clips_[1] || !clips_[2] || !clips_[3]) return;
    float dur = 0.0f;
    for (const AnimationClip* c : clips_) dur = std::fmax(dur, c->duration);
    if (dur > 0.0f) {
        time_ += dt;
        time_ = std::fmod(time_, dur);
        if (time_ < 0.0f) time_ += dur;
    }
    for (int i = 0; i < 4; ++i) {
        work_[i] = bind_;
        clips_[i]->Sample(time_, work_[i]);
    }
    Pose bottom;
    Pose top;
    bottom.Resize(result_.t.size());
    top.Resize(result_.t.size());
    bottom.Lerp(work_[0], work_[1], px_);
    top.Lerp(work_[2], work_[3], px_);
    result_.Lerp(bottom, top, py_);
}

// ---------------------------------------------------------------------------
// Two-bone IK (P1-3)
// ---------------------------------------------------------------------------

TwoBoneIKResult TwoBoneIK(const math::Vec3& hip, const math::Vec3& knee,
                          const math::Vec3& ankle, const math::Vec3& target,
                          const math::Vec3& pole) {
    TwoBoneIKResult out;
    out.a = hip;
    const float l1 = (knee - hip).Length();
    const float l2 = (ankle - knee).Length();
    if (l1 <= 1e-6f || l2 <= 1e-6f) {
        out.b = knee;
        out.c = ankle;
        out.reachable = false;
        return out;
    }

    const math::Vec3 toTarget = target - hip;
    const float rawD = toTarget.Length();
    out.reachable = rawD <= l1 + l2 + 1e-4f;
    const float d = math::Clamp(rawD, std::fabs(l1 - l2), l1 + l2);
    if (d < 1e-6f) {
        out.b = hip + math::Vec3{0, l1, 0};
        out.c = hip + math::Vec3{0, l1 + l2, 0};
        return out;
    }

    const math::Vec3 abDir = toTarget / std::fmax(rawD, 1e-6f);
    const math::Vec3 poleDir = pole - hip;
    const math::Vec3 poleProj = poleDir - abDir * math::Dot(poleDir, abDir);
    math::Vec3 axis = math::Cross(abDir, poleProj);
    if (axis.LengthSq() < 1e-8f) axis = math::Cross(abDir, math::Vec3{0, 1, 0});
    if (axis.LengthSq() < 1e-8f) axis = math::Cross(abDir, math::Vec3{1, 0, 0});
    axis = axis.Normalized();

    const float cosA = math::Clamp((l1 * l1 + d * d - l2 * l2) / (2.0f * l1 * d), -1.0f, 1.0f);
    const float angleA = std::acos(cosA);
    const math::Quat qA = math::Quat::FromAxisAngle(axis, angleA);
    out.b = hip + qA.Rotate(abDir) * l1;
    // Reachable: the analytic solution closes the triangle at the target.
    // Unreachable: clamp the end effector to the farthest point the limb
    // lengths allow along the target direction.
    out.c = out.reachable ? target : hip + abDir * d;
    return out;
}

// ---------------------------------------------------------------------------
// Clip JSON (P1-1 animation timeline editor)
// ---------------------------------------------------------------------------

namespace {

core::Json JsonStr(const std::string& s) {
    core::Json j;
    j.type_ = core::Json::Type::String;
    j.string_ = s;
    return j;
}

core::Json JsonNum(double v) {
    core::Json j;
    j.type_ = core::Json::Type::Number;
    j.number_ = v;
    return j;
}

core::Json Vec3ToJson(const math::Vec3& v) {
    core::Json arr;
    arr.type_ = core::Json::Type::Array;
    for (float f : {v.x, v.y, v.z}) arr.array_.push_back(JsonNum(f));
    return arr;
}

core::Json QuatToJson(const math::Quat& q) {
    core::Json arr;
    arr.type_ = core::Json::Type::Array;
    for (float f : {q.x, q.y, q.z, q.w}) arr.array_.push_back(JsonNum(f));
    return arr;
}

bool JsonIsArrayOfNumbers(const core::Json* j, size_t n) {
    return j && j->IsArray() && j->Size() == n;
}

math::Vec3 Vec3FromJson(const core::Json* j) {
    if (!JsonIsArrayOfNumbers(j, 3)) return {};
    return {static_cast<float>(j->At(0)->GetNumber()),
            static_cast<float>(j->At(1)->GetNumber()),
            static_cast<float>(j->At(2)->GetNumber())};
}

math::Quat QuatFromJson(const core::Json* j) {
    if (!JsonIsArrayOfNumbers(j, 4)) return {0, 0, 0, 1};
    return {static_cast<float>(j->At(0)->GetNumber()),
            static_cast<float>(j->At(1)->GetNumber()),
            static_cast<float>(j->At(2)->GetNumber()),
            static_cast<float>(j->At(3)->GetNumber())};
}

} // namespace

std::string SaveClipJson(const AnimationClip& clip) {
    core::Json root;
    root.type_ = core::Json::Type::Object;
    root.object_["name"] = JsonStr(clip.name);
    root.object_["duration"] = JsonNum(clip.duration);
    core::Json tracks;
    tracks.type_ = core::Json::Type::Array;
    for (const Track& tr : clip.tracks) {
        core::Json t;
        t.type_ = core::Json::Type::Object;
        t.object_["bone"] = JsonNum(tr.bone);
        t.object_["interp"] = JsonStr(
            tr.interp == Interp::Step ? "step"
                                      : (tr.interp == Interp::CubicSpline ? "cubic" : "linear"));
        core::Json times;
        times.type_ = core::Json::Type::Array;
        for (float f : tr.times) times.array_.push_back(JsonNum(f));
        t.object_["times"] = std::move(times);
        if (!tr.translations.empty()) {
            core::Json arr;
            arr.type_ = core::Json::Type::Array;
            for (const math::Vec3& v : tr.translations) arr.array_.push_back(Vec3ToJson(v));
            t.object_["translations"] = std::move(arr);
        }
        if (!tr.rotations.empty()) {
            core::Json arr;
            arr.type_ = core::Json::Type::Array;
            for (const math::Quat& q : tr.rotations) arr.array_.push_back(QuatToJson(q));
            t.object_["rotations"] = std::move(arr);
        }
        if (!tr.scales.empty()) {
            core::Json arr;
            arr.type_ = core::Json::Type::Array;
            for (const math::Vec3& v : tr.scales) arr.array_.push_back(Vec3ToJson(v));
            t.object_["scales"] = std::move(arr);
        }
        tracks.array_.push_back(std::move(t));
    }
    root.object_["tracks"] = std::move(tracks);
    return core::JsonWriter::Write(root);
}

core::Result<AnimationClip> LoadClipJson(const std::string& jsonText) {
    std::string perr;
    core::Json root = core::Json::Parse(jsonText, &perr);
    if (root.IsNull() && !perr.empty())
        return core::Result<AnimationClip>::Err("clip: JSON parse error: " + perr);
    if (!root.IsObject())
        return core::Result<AnimationClip>::Err("clip: root must be a JSON object");
    AnimationClip clip;
    if (const core::Json* name = root.Get("name")) {
        if (!name->IsString())
            return core::Result<AnimationClip>::Err("clip: 'name' must be a string");
        clip.name = name->GetString();
    }
    if (const core::Json* dur = root.Get("duration")) {
        if (!dur->IsNumber())
            return core::Result<AnimationClip>::Err("clip: 'duration' must be a number");
        clip.duration = static_cast<float>(dur->GetNumber());
    }
    const core::Json* tracks = root.Get("tracks");
    if (tracks && tracks->IsArray()) {
        for (const core::Json& t : tracks->Items()) {
            Track tr;
            if (const core::Json* b = t.Get("bone")) tr.bone = b->GetInt(-1);
            if (const core::Json* ip = t.Get("interp")) {
                const std::string s = ip->GetString();
                tr.interp = s == "step" ? Interp::Step
                                        : (s == "cubic" ? Interp::CubicSpline : Interp::Linear);
            }
            if (const core::Json* times = t.Get("times")) {
                if (times->IsArray()) {
                    for (const core::Json& f : times->Items())
                        tr.times.push_back(static_cast<float>(f.GetNumber()));
                }
            }
            if (const core::Json* arr = t.Get("translations")) {
                if (arr->IsArray())
                    for (const core::Json& v : arr->Items())
                        tr.translations.push_back(Vec3FromJson(&v));
            }
            if (const core::Json* arr = t.Get("rotations")) {
                if (arr->IsArray())
                    for (const core::Json& q : arr->Items())
                        tr.rotations.push_back(QuatFromJson(&q));
            }
            if (const core::Json* arr = t.Get("scales")) {
                if (arr->IsArray())
                    for (const core::Json& v : arr->Items())
                        tr.scales.push_back(Vec3FromJson(&v));
            }
            clip.tracks.push_back(std::move(tr));
        }
    }
    return core::Result<AnimationClip>::Ok(std::move(clip));
}

} // namespace neon::anim
