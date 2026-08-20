#pragma once
#include <map>
#include <string>
#include <vector>
#include "neon/assets/asset_manager.hpp"
#include "neon/core/result.hpp"
#include "neon/math/mat4.hpp"
#include "neon/math/quat.hpp"
#include "neon/math/vec3.hpp"

namespace neon::anim {

enum class Interp { Linear, Step, CubicSpline };

// Per-bone animated transform sample. Sized to the skeleton bone count;
// bones without animation data keep whatever the caller seeded (bind pose).
struct Pose {
    std::vector<math::Vec3> t;
    std::vector<math::Quat> r;
    std::vector<math::Vec3> s;

    void Resize(size_t boneCount);
    // *this = lerp(a, b, alpha); component-wise Vec3 lerp + quat slerp.
    void Lerp(const Pose& a, const Pose& b, float alpha);
};

struct Bone {
    std::string name;
    int parent = -1; // index into skeleton.bones, -1 = root
    math::Vec3 bindT{0, 0, 0};
    math::Quat bindR{0, 0, 0, 1};
    math::Vec3 bindS{1, 1, 1};
    math::Mat4 inverseBind; // from skin; identity for non-joint nodes
};

struct Skeleton {
    std::vector<Bone> bones;
    int FindBone(const std::string& name) const;
    Pose BindPose() const;
    // For each bone: local = T(pose.t)*R(pose.r)*S(pose.s); world[bone] =
    // world[parent] * local; returns skinning matrix world * inverseBind.
    // Falls back to the bind pose when the pose is not sized to the bones.
    std::vector<math::Mat4> ComputeBoneMatrices(const Pose& pose) const;
};

// One bone, one channel path. CubicSpline stores 3 values per keyframe
// ([inTangent, value, outTangent]); Linear/Step store one value per key.
struct Track {
    int bone = -1;
    Interp interp = Interp::Linear;
    std::vector<float> times;
    std::vector<math::Vec3> translations;
    std::vector<math::Quat> rotations;
    std::vector<math::Vec3> scales;
};

struct AnimationClip {
    std::string name;
    float duration = 0.f;
    std::vector<Track> tracks;
    // Writes TRS for every track bone into out (pre-sized by the caller).
    // t is loop-wrapped into [0, duration). Bones without tracks are untouched.
    void Sample(float t, Pose& out) const;
};

struct AnimState {
    std::string name;
    const AnimationClip* clip = nullptr;
};

struct AnimTransition {
    std::string from, to;
    std::string param;
    float threshold = 0.f;
    float duration = 0.f;
};

// Parameter-driven state machine with cross-fade blending. On Update it checks
// transitions whose parameter crossed the threshold, then blends the previous
// clip into the new one over the transition duration.
class AnimationStateMachine {
public:
    void AddState(const std::string& name, const AnimationClip* clip);
    void AddTransition(const std::string& from, const std::string& to,
                       const std::string& param, float threshold, float duration);
    void SetParam(const std::string& name, float value);
    void SetBoneCount(size_t n);
    void SetBindPose(const Pose& bind);
    void Update(float dt);
    void Play(const std::string& state);
    const Pose& ResultPose() const { return result_; }
    std::string CurrentState() const;

private:
    int FindState(const std::string& name) const;
    void SampleStateInto(Pose& out, int stateIdx, float time) const;
    std::vector<AnimState> states_;
    std::vector<AnimTransition> transitions_;
    std::map<std::string, float> params_;
    int current_ = -1;
    int previous_ = -1;
    float currentTime_ = 0.f;
    float previousTime_ = 0.f;
    float blendAlpha_ = 1.f;
    float blendDuration_ = 1.f;
    Pose bind_;
    Pose workA_, workB_;
    Pose result_;
};

// Simple single-clip player: advances clip time, samples at the current time.
class Animator {
public:
    void Play(const AnimationClip* clip, float startTime = 0.f);
    void Update(float dt);
    const AnimationClip* Clip() const { return clip_; }
    float Time() const { return time_; }
    float Duration() const { return clip_ ? clip_->duration : 0.f; }
    void Sample(Pose& out) const { if (clip_) clip_->Sample(time_, out); }

private:
    const AnimationClip* clip_ = nullptr;
    float time_ = 0.f;
};

struct AnimSet {
    Skeleton skeleton;
    std::vector<AnimationClip> clips;
};

// Imports glTF animations (channels/samplers, LINEAR/STEP/CUBICSPLINE) into a
// skeleton + clips. The skeleton gets one bone per glTF node (bone index ==
// node index) with TRS from the node hierarchy and inverseBind from the skin.
core::Result<AnimSet> ImportGltf(const std::string& jsonText,
                                 const assets::GltfAsset& asset, int skinIndex = 0);

} // namespace neon::anim
