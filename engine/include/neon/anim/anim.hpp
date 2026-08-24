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

// 1D blend space (P1-3): two clips blended by a parameter in [0,1]
// (Godot-style BlendSpace1D). The parameter can be any gameplay value (speed,
// direction); Update advances the shared time and produces a blended pose.
class BlendSpace1D {
public:
    void SetClips(const AnimationClip* a, const AnimationClip* b);
    void SetParam(float v);  // clamped to [0,1]
    void SetBoneCount(size_t n);
    void SetBindPose(const Pose& bind);
    void Update(float dt);
    const Pose& ResultPose() const { return result_; }
    float Param() const { return param_; }

private:
    const AnimationClip* a_ = nullptr;
    const AnimationClip* b_ = nullptr;
    float param_ = 0.f;
    float time_ = 0.f;
    Pose bind_, workA_, workB_, result_;
};

// 2D blend space (P1-3): four corner clips blended bilinearly by (x, y) in
// [0,1]^2 (BlendSpace2D). Useful for locomotion (e.g. idle/run crossed with
// strafe direction).
class BlendSpace2D {
public:
    // Corners: 0=(-,-) 1=(+,-) 2=(-,+) 3=(+,+)
    void SetClips(const AnimationClip* ll, const AnimationClip* lr,
                  const AnimationClip* ul, const AnimationClip* ur);
    void SetParam(float x, float y);  // both clamped to [0,1]
    void SetBoneCount(size_t n);
    void SetBindPose(const Pose& bind);
    void Update(float dt);
    const Pose& ResultPose() const { return result_; }
    float ParamX() const { return px_; }
    float ParamY() const { return py_; }

private:
    const AnimationClip* clips_[4] = {};
    float px_ = 0.f;
    float py_ = 0.f;
    float time_ = 0.f;
    Pose bind_, work_[4], result_;
};

// Two-bone IK (P1-3): analytic solver for a hip->knee->ankle chain reaching a
// target. `pole` biases the elbow direction (the projection of the pole onto
// the plane of the chain picks the bend side); returns the solved joint
// positions with the end effector at (or as close as possible to) `target`.
struct TwoBoneIKResult {
    math::Vec3 a;   // hip (unchanged)
    math::Vec3 b;   // knee (solved)
    math::Vec3 c;   // ankle (== target when reachable)
    bool reachable = true;
};
TwoBoneIKResult TwoBoneIK(const math::Vec3& hip, const math::Vec3& knee,
                          const math::Vec3& ankle, const math::Vec3& target,
                          const math::Vec3& pole);

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
