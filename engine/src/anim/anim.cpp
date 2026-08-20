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
    size_t n = std::min(a.t.size(), b.t.size());
    t.resize(n);
    r.resize(n);
    s.resize(n);
    for (size_t i = 0; i < n; ++i) {
        t[i] = math::Lerp(a.t[i], b.t[i], alpha);
        r[i] = math::Slerp(a.r[i], b.r[i], alpha);
        s[i] = math::Lerp(a.s[i], b.s[i], alpha);
    }
}

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
    for (size_t i = 0; i < n; ++i) {
        const Bone& b = bones[i];
        math::Vec3 t = b.bindT;
        math::Quat r = b.bindR;
        math::Vec3 s = b.bindS;
        if (poseSized) {
            t = pose.t[i];
            r = pose.r[i];
            s = pose.s[i];
        }
        math::Mat4 local = math::Mat4::Translation(t) * r.ToMat4() * math::Mat4::Scale(s);
        world[i] = (b.parent >= 0 && static_cast<size_t>(b.parent) < n)
                       ? world[static_cast<size_t>(b.parent)] * local
                       : local;
    }
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
            for (size_t k = 0; k + 1 < tr.times.size(); ++k) {
                if (t >= tr.times[k] && t < tr.times[k + 1]) {
                    key = static_cast<int>(k);
                    next = static_cast<int>(k) + 1;
                    float span = tr.times[k + 1] - tr.times[k];
                    u = span > 0.0f ? (t - tr.times[k]) / span : 0.0f;
                    break;
                }
            }
            if (key < 0) key = static_cast<int>(tr.times.size()) - 1;
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
            previous_ = current_;
            previousTime_ = currentTime_;
            current_ = to;
            currentTime_ = 0.0f;
            blendAlpha_ = 0.0f;
            blendDuration_ = tr.duration > 0.0f ? tr.duration : 1.0f;
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

} // namespace neon::anim
