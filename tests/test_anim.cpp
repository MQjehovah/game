#include <cstdint>
#include <string>
#include <vector>

#include "neon/neon.hpp"
#include "helpers.hpp"
#include "test_backend.hpp"

using namespace neon;

// ---------------------------------------------------------------------------
// neon::anim: skeleton LocalToGlobal, clip sampling (LINEAR/STEP/CUBICSPLINE),
// state machine cross-fade, animator, and glTF animation import (Task 3.2)
// ---------------------------------------------------------------------------

namespace {

void AppendBytes(std::vector<uint8_t>& out, const void* data, size_t n) {
    const uint8_t* p = static_cast<const uint8_t*>(data);
    out.insert(out.end(), p, p + n);
}

void AppendF32(std::vector<uint8_t>& out, float v) { AppendBytes(out, &v, sizeof(v)); }
void AppendU16(std::vector<uint8_t>& out, uint16_t v) { AppendBytes(out, &v, sizeof(v)); }
void AppendMat(std::vector<uint8_t>& out, float m[16]) { AppendBytes(out, m, sizeof(float) * 16); }

// glTF fixture: 4 nodes (mesh node 0 + joint nodes 1,2 + transform node 3),
// a skin over joints [1,2], and one "walk" animation with two samplers:
//   sampler 0 (LINEAR)     -> node 1 translation, keys 0 -> (10,0,0)
//   sampler 1 (CUBICSPLINE)-> node 2 translation, values [0,10], out=5/in=-5
// Layout (282 bytes):
//   0    positions (3 x VEC3)
//   36   indices (3 x u16)
//   42   inverseBindMatrices (2 x MAT4): scale(2), scale(3)
//   170  input times (2 x SCALAR) [0,1]
//   178  LINEAR output (2 x VEC3) (0,0,0),(10,0,0)
//   202  input times (2 x SCALAR) [0,1]
//   210  CUBICSPLINE output (6 x VEC3)
std::vector<uint8_t> AnimBin() {
    std::vector<uint8_t> out;
    const float kPos[3][3] = {{0, 0, 0}, {1, 0, 0}, {0, 1, 0}};
    for (int i = 0; i < 3; ++i)
        for (int c = 0; c < 3; ++c) AppendF32(out, kPos[i][c]);
    AppendU16(out, 0);
    AppendU16(out, 1);
    AppendU16(out, 2);
    float kMat0[16] = {2, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
    float kMat1[16] = {3, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
    AppendMat(out, kMat0);
    AppendMat(out, kMat1);
    AppendF32(out, 0.0f); // sampler 0 input
    AppendF32(out, 1.0f);
    const float kLin[6] = {0, 0, 0, 10, 0, 0};
    for (int i = 0; i < 6; ++i) AppendF32(out, kLin[i]);
    AppendF32(out, 0.0f); // sampler 1 input
    AppendF32(out, 1.0f);
    const float kCubic[18] = {
        0, 0, 0, 0, 0, 0, 5, 0, 0, // key 0: in, value, out
        -5, 0, 0, 10, 0, 0, 0, 0, 0 // key 1: in, value, out
    };
    for (int i = 0; i < 18; ++i) AppendF32(out, kCubic[i]);
    return out;
}

const char* kGltfAnim = R"({
  "asset": {"version": "2.0"},
  "scene": 0,
  "scenes": [{"nodes": [0]}],
  "nodes": [
    {"skin": 0, "mesh": 0, "translation": [5, 0, 0], "children": [1, 2, 3]},
    {"translation": [1, 0, 0], "name": "jointA"},
    {"translation": [0, 10, 0], "name": "jointB"},
    {}
  ],
  "skins": [{"joints": [1, 2], "inverseBindMatrices": 2}],
  "meshes": [
    {"primitives": [{"attributes": {"POSITION": 0}, "indices": 1}]}
  ],
  "animations": [
    {
      "name": "walk",
      "samplers": [
        {"input": 3, "output": 4, "interpolation": "LINEAR"},
        {"input": 5, "output": 6, "interpolation": "CUBICSPLINE"}
      ],
      "channels": [
        {"sampler": 0, "target": {"node": 1, "path": "translation"}},
        {"sampler": 1, "target": {"node": 2, "path": "translation"}}
      ]
    }
  ],
  "accessors": [
    {"bufferView": 0, "componentType": 5126, "count": 3, "type": "VEC3"},
    {"bufferView": 1, "componentType": 5123, "count": 3, "type": "SCALAR"},
    {"bufferView": 2, "componentType": 5126, "count": 2, "type": "MAT4"},
    {"bufferView": 3, "componentType": 5126, "count": 2, "type": "SCALAR"},
    {"bufferView": 4, "componentType": 5126, "count": 2, "type": "VEC3"},
    {"bufferView": 5, "componentType": 5126, "count": 2, "type": "SCALAR"},
    {"bufferView": 6, "componentType": 5126, "count": 6, "type": "VEC3"}
  ],
  "bufferViews": [
    {"buffer": 0, "byteOffset": 0, "byteLength": 36},
    {"buffer": 0, "byteOffset": 36, "byteLength": 6},
    {"buffer": 0, "byteOffset": 42, "byteLength": 128},
    {"buffer": 0, "byteOffset": 170, "byteLength": 8},
    {"buffer": 0, "byteOffset": 178, "byteLength": 24},
    {"buffer": 0, "byteOffset": 202, "byteLength": 8},
    {"buffer": 0, "byteOffset": 210, "byteLength": 72}
  ],
  "buffers": [{"byteLength": 282, "uri": "scene.bin"}]
})";

assets::GltfAsset LoadAnimFixture(test::TempDir& tmp) {
    std::vector<uint8_t> bin = AnimBin();
    CHECK(test::WriteFileAll(tmp.Str() + "/scene.bin", bin.data(), bin.size()));
    CHECK(test::WriteFileAll(tmp.Str() + "/scene.gltf", std::string(kGltfAnim)));
    test::HeadlessAssetFixture fix;
    return fix.assets.LoadGLTF(tmp.Str() + "/scene.gltf");
}

anim::AnimationClip MakeConstantClip(const std::string& name, float x) {
    anim::AnimationClip c;
    c.name = name;
    c.duration = 1.f;
    anim::Track tr;
    tr.bone = 0;
    tr.times = {0.f};
    tr.translations = {{x, 0, 0}};
    c.tracks.push_back(std::move(tr));
    return c;
}

} // namespace

// 2-bone chain (root at origin, child at +1 X, both identity rotation):
// world child translation must be (1,0,0) and the skinning matrix must be
// world * inverseBind (hand-computed T(2+1)*T(-1) = T(2)).
TEST(AnimSkeletonLocalToGlobal) {
    anim::Skeleton sk;
    sk.bones.resize(2);
    sk.bones[0] = anim::Bone{"root", -1, {0, 0, 0}, math::Quat::Identity(), {1, 1, 1}, math::Mat4::Identity()};
    sk.bones[1] = anim::Bone{"child", 0, {1, 0, 0}, math::Quat::Identity(), {1, 1, 1}, math::Mat4::Identity()};

    CHECK_EQ(sk.FindBone("root"), 0);
    CHECK_EQ(sk.FindBone("child"), 1);
    CHECK_EQ(sk.FindBone("nope"), -1);

    anim::Pose pose = sk.BindPose();
    std::vector<math::Mat4> m = sk.ComputeBoneMatrices(pose);
    CHECK_EQ(m.size(), 2u);
    if (m.size() != 2u) return;
    CHECK_NEAR(m[0].m[3], 0.0, 1e-6);
    math::Vec3 child0 = m[1].TransformPoint({0, 0, 0});
    CHECK_NEAR(child0.x, 1.0, 1e-5);
    CHECK_NEAR(child0.y, 0.0, 1e-5);
    CHECK_NEAR(child0.z, 0.0, 1e-5);

    // Pose moves the root +2 X; child global becomes (3,0,0).
    pose.t[0] = {2, 0, 0};
    m = sk.ComputeBoneMatrices(pose);
    CHECK_NEAR(m[0].m[3], 2.0, 1e-5);
    CHECK_NEAR(m[1].TransformPoint({0, 0, 0}).x, 3.0, 1e-5);

    // inverseBind child = T(-1): skinning = world * inverseBind = T(3)*T(-1) = T(2).
    sk.bones[1].inverseBind = math::Mat4::Translation({-1, 0, 0});
    m = sk.ComputeBoneMatrices(pose);
    CHECK_NEAR(m[1].TransformPoint({0, 0, 0}).x, 2.0, 1e-5);
}

// Linear keyframes: t=0.5 -> (5,0,0), t=0.25 -> (2.5,0,0); rotation slerps
// identity -> 90deg Y, so at t=0.5 it is a 45deg Y rotation.
TEST(AnimClipLinearInterp) {
    anim::AnimationClip clip;
    clip.name = "move";
    clip.duration = 1.f;
    anim::Track tr;
    tr.bone = 0;
    tr.times = {0.f, 1.f};
    tr.translations = {{0, 0, 0}, {10, 0, 0}};
    tr.rotations = {math::Quat::Identity(), math::Quat::FromAxisAngle({0, 1, 0}, math::kHalfPi)};
    clip.tracks.push_back(std::move(tr));

    anim::Pose pose;
    pose.Resize(1);
    clip.Sample(0.5f, pose);
    CHECK_NEAR(pose.t[0].x, 5.0, 1e-5);
    CHECK_NEAR(pose.t[0].y, 0.0, 1e-6);
    CHECK_NEAR(pose.t[0].z, 0.0, 1e-6);
    math::Vec3 f = pose.r[0].Rotate({0, 0, 1}); // 45 deg about Y
    CHECK_NEAR(f.x, 0.70710678, 1e-5);
    CHECK_NEAR(f.y, 0.0, 1e-5);
    CHECK_NEAR(f.z, 0.70710678, 1e-5);

    pose.Resize(1);
    clip.Sample(0.25f, pose);
    CHECK_NEAR(pose.t[0].x, 2.5, 1e-5);

    pose.Resize(1);
    clip.Sample(0.0f, pose);
    CHECK_NEAR(pose.t[0].x, 0.0, 1e-6);
}

// Loop wrap: duration 1; Sample(1.5) equals Sample(0.5), Sample(2.0) equals
// Sample(0.0), and negative times wrap forward.
TEST(AnimClipWrap) {
    anim::AnimationClip clip;
    clip.duration = 1.f;
    anim::Track tr;
    tr.bone = 0;
    tr.times = {0.f, 1.f};
    tr.translations = {{0, 0, 0}, {10, 0, 0}};
    clip.tracks.push_back(std::move(tr));

    anim::Pose a, b;
    a.Resize(1);
    b.Resize(1);
    clip.Sample(0.5f, a);
    clip.Sample(1.5f, b);
    CHECK_NEAR(b.t[0].x, a.t[0].x, 1e-6);

    a.Resize(1);
    b.Resize(1);
    clip.Sample(0.0f, a);
    clip.Sample(2.0f, b);
    CHECK_NEAR(b.t[0].x, 0.0, 1e-6);
    CHECK_NEAR(a.t[0].x, b.t[0].x, 1e-6);

    a.Resize(1);
    clip.Sample(-0.5f, a);
    CHECK_NEAR(a.t[0].x, 5.0, 1e-5);
}

// STEP interpolation holds the previous key until the next key is reached.
TEST(AnimClipStep) {
    anim::AnimationClip clip;
    clip.duration = 1.f;
    anim::Track tr;
    tr.interp = anim::Interp::Step;
    tr.bone = 0;
    tr.times = {0.f, 0.5f, 1.f};
    tr.translations = {{0, 0, 0}, {10, 0, 0}, {20, 0, 0}};
    clip.tracks.push_back(std::move(tr));

    anim::Pose p;
    p.Resize(1);
    clip.Sample(0.25f, p);
    CHECK_NEAR(p.t[0].x, 0.0, 1e-6);
    clip.Sample(0.6f, p);
    CHECK_NEAR(p.t[0].x, 10.0, 1e-6);
    clip.Sample(0.99f, p);
    CHECK_NEAR(p.t[0].x, 10.0, 1e-6);
    clip.Sample(0.5f, p);
    CHECK_NEAR(p.t[0].x, 10.0, 1e-6);
}

// CUBICSPLINE hermite: values [0,10] over [0,1] with out tangent 5 / in
// tangent -5. p(u) = h1*v0 + h2*dt*out0 + h3*v1 + h4*dt*in1.
// Midpoint (u=0.5): 0 + 0.125*5 + 0.5*10 + (-0.125)*(-5) = 6.25.
// u=0.25: 0 + 0.140625*5 + 0.15625*10 + (-0.046875)*(-5) = 2.5.
TEST(AnimClipCubicSpline) {
    anim::AnimationClip clip;
    clip.duration = 1.f;
    anim::Track tr;
    tr.interp = anim::Interp::CubicSpline;
    tr.bone = 0;
    tr.times = {0.f, 1.f};
    tr.translations = {
        {0, 0, 0}, {0, 0, 0}, {5, 0, 0}, // key 0: in, value, out
        {-5, 0, 0}, {10, 0, 0}, {0, 0, 0} // key 1: in, value, out
    };
    clip.tracks.push_back(std::move(tr));

    anim::Pose p;
    p.Resize(1);
    clip.Sample(0.5f, p);
    CHECK_NEAR(p.t[0].x, 6.25, 1e-4);
    clip.Sample(0.25f, p);
    CHECK_NEAR(p.t[0].x, 2.5, 1e-4);
    clip.Sample(0.0f, p);
    CHECK_NEAR(p.t[0].x, 0.0, 1e-5);
}

// State machine: idle -> run when param "speed" >= 5, cross-fade over 0.5s.
// At alpha 0.25 the pose is 75% idle / 25% run; after the duration it is run.
// A param below the threshold must not trigger the transition.
TEST(AnimStateMachineTransition) {
    anim::AnimationClip idle = MakeConstantClip("idle", 0.0f);
    anim::AnimationClip run = MakeConstantClip("run", 10.0f);

    anim::AnimationStateMachine sm;
    sm.AddState("idle", &idle);
    sm.AddState("run", &run);
    sm.AddTransition("idle", "run", "speed", 5.0f, 0.5f);
    sm.SetBoneCount(1);
    sm.Play("idle");
    sm.Update(0.f);
    CHECK_EQ(sm.CurrentState(), std::string("idle"));

    sm.SetParam("speed", 1.0f);
    sm.Update(0.5f);
    CHECK_EQ(sm.CurrentState(), std::string("idle"));
    CHECK_NEAR(sm.ResultPose().t[0].x, 0.0, 1e-5);

    sm.SetParam("speed", 10.0f);
    sm.Update(0.f);       // condition met -> begin blend
    CHECK_EQ(sm.CurrentState(), std::string("run"));
    sm.Update(0.125f);    // alpha = 0.25
    CHECK_NEAR(sm.ResultPose().t[0].x, 2.5, 1e-4);
    sm.Update(0.375f);    // alpha = 1.0
    CHECK_NEAR(sm.ResultPose().t[0].x, 10.0, 1e-4);
    CHECK_EQ(sm.CurrentState(), std::string("run"));

    // Hard switch resets the clip time (and does not re-trigger a transition
    // once the param is back below the threshold).
    sm.SetParam("speed", 0.0f);
    sm.Play("idle");
    sm.Update(0.1f);
    CHECK_EQ(sm.CurrentState(), std::string("idle"));
    CHECK_NEAR(sm.ResultPose().t[0].x, 0.0, 1e-5);
}

// Animator advances clip time so later samples differ.
TEST(AnimatorAdvancesTime) {
    anim::AnimationClip clip;
    clip.duration = 1.f;
    anim::Track tr;
    tr.bone = 0;
    tr.times = {0.f, 1.f};
    tr.translations = {{0, 0, 0}, {10, 0, 0}};
    clip.tracks.push_back(std::move(tr));

    anim::Animator animator;
    animator.Play(&clip);
    CHECK_EQ(animator.Duration(), 1.0f);

    anim::Pose p;
    p.Resize(1);
    animator.Sample(p);
    CHECK_NEAR(p.t[0].x, 0.0, 1e-5);
    animator.Update(0.5f);
    animator.Sample(p);
    CHECK_NEAR(p.t[0].x, 5.0, 1e-5);
    animator.Update(0.25f);
    animator.Sample(p);
    CHECK_NEAR(p.t[0].x, 7.5, 1e-5);
    animator.Update(0.25f); // t = 1.0 wraps to 0
    animator.Sample(p);
    CHECK_NEAR(p.t[0].x, 0.0, 1e-5);
}

// GltfAsset must retain ALL glTF nodes (mesh, joints, transform-only) with
// parent pointers and local TRS, indexed by glTF node index, while the
// mesh-bearing nodes list keeps its existing behavior.
TEST(GltfNodesAllRetained) {
    test::TempDir tmp;
    assets::GltfAsset asset = LoadAnimFixture(tmp);
    CHECK(asset.Valid());
    CHECK_EQ(asset.nodes.size(), 1u); // mesh nodes unchanged
    if (asset.nodes.size() != 1u) return;

    CHECK_EQ(asset.nodesAll.size(), 4u);
    if (asset.nodesAll.size() != 4u) return;
    const assets::GltfNode& root = asset.nodesAll[0];
    CHECK_EQ(root.parent, -1);
    CHECK_NEAR(root.t.x, 5.0, 1e-5);
    CHECK_NEAR(root.r.w, 1.0, 1e-6);
    CHECK_NEAR(root.s.x, 1.0, 1e-6);

    const assets::GltfNode& jointA = asset.nodesAll[1];
    CHECK_EQ(jointA.parent, 0);
    CHECK_EQ(jointA.name, std::string("jointA"));
    CHECK_NEAR(jointA.t.x, 1.0, 1e-5);

    const assets::GltfNode& jointB = asset.nodesAll[2];
    CHECK_EQ(jointB.parent, 0);
    CHECK_EQ(jointB.name, std::string("jointB"));
    CHECK_NEAR(jointB.t.y, 10.0, 1e-5);

    // Transform-only node (no mesh, not a joint) is retained too.
    const assets::GltfNode& leaf = asset.nodesAll[3];
    CHECK_EQ(leaf.parent, 0);
    CHECK_NEAR(leaf.t.x, 0.0, 1e-6);
    CHECK_NEAR(leaf.s.x, 1.0, 1e-6);

    // The mesh-bearing node still accumulates its world transform.
    CHECK_NEAR(asset.nodes[0].transform.TransformPoint({0, 0, 0}).x, 5.0, 1e-5);
}

// ReadAccessorFloats decodes raw accessor data (times and sampler values).
TEST(GltfReadAccessorFloats) {
    test::TempDir tmp;
    assets::GltfAsset asset = LoadAnimFixture(tmp);
    CHECK(asset.Valid());

    core::Result<std::vector<float>> times = asset.ReadAccessorFloats(3);
    CHECK(times.Ok());
    if (times.Ok()) {
        CHECK_EQ(times.Value().size(), 2u);
        CHECK_NEAR(times.Value()[0], 0.0, 1e-6);
        CHECK_NEAR(times.Value()[1], 1.0, 1e-6);
    }

    core::Result<std::vector<float>> linear = asset.ReadAccessorFloats(4);
    CHECK(linear.Ok());
    if (linear.Ok()) {
        CHECK_EQ(linear.Value().size(), 6u);
        CHECK_NEAR(linear.Value()[0], 0.0, 1e-6);
        CHECK_NEAR(linear.Value()[3], 10.0, 1e-6);
    }

    core::Result<std::vector<float>> cubic = asset.ReadAccessorFloats(6);
    CHECK(cubic.Ok());
    if (cubic.Ok()) {
        CHECK_EQ(cubic.Value().size(), 18u);
        CHECK_NEAR(cubic.Value()[6], 5.0, 1e-6);  // key0 out tangent x
        CHECK_NEAR(cubic.Value()[9], -5.0, 1e-6); // key1 in tangent x
        CHECK_NEAR(cubic.Value()[12], 10.0, 1e-6);
    }

    CHECK(!asset.ReadAccessorFloats(99).Ok());
}

// ImportGltf builds a skeleton (bone per glTF node, parent links, inverseBind
// mapped from skin joints) plus clips with LINEAR and CUBICSPLINE tracks.
TEST(ImportGltfSkeletonAndClips) {
    test::TempDir tmp;
    assets::GltfAsset asset = LoadAnimFixture(tmp);
    CHECK(asset.Valid());

    std::string jsonText;
    CHECK(test::ReadFileAll(tmp.Str() + "/scene.gltf", jsonText));
    core::Result<anim::AnimSet> r = anim::ImportGltf(jsonText, asset, 0);
    CHECK(r.Ok());
    if (!r.Ok()) return;
    const anim::AnimSet& set = r.Value();

    CHECK_EQ(set.skeleton.bones.size(), 4u);
    if (set.skeleton.bones.size() != 4u) return;
    CHECK_EQ(set.skeleton.bones[0].name, std::string("node0"));
    CHECK_EQ(set.skeleton.bones[0].parent, -1);
    CHECK_EQ(set.skeleton.bones[1].name, std::string("jointA"));
    CHECK_EQ(set.skeleton.bones[1].parent, 0);
    CHECK_EQ(set.skeleton.bones[1].bindT.x, 1.0f);
    CHECK_EQ(set.skeleton.bones[2].name, std::string("jointB"));
    CHECK_EQ(set.skeleton.bones[2].parent, 0);
    CHECK_EQ(set.skeleton.bones[3].name, std::string("node3"));
    CHECK_EQ(set.skeleton.FindBone("jointA"), 1);
    CHECK_EQ(set.skeleton.FindBone("jointB"), 2);
    CHECK_EQ(set.skeleton.FindBone("missing"), -1);

    // inverseBind mapped from the skin: joint 1 <- mat0 (m[0]=2), joint 2 <- mat1 (m[0]=3).
    CHECK_NEAR(set.skeleton.bones[1].inverseBind.m[0], 2.0, 1e-6);
    CHECK_NEAR(set.skeleton.bones[2].inverseBind.m[0], 3.0, 1e-6);
    CHECK_NEAR(set.skeleton.bones[0].inverseBind.m[0], 1.0, 1e-6); // non-joint: identity

    CHECK_EQ(set.clips.size(), 1u);
    if (set.clips.size() != 1u) return;
    const anim::AnimationClip& clip = set.clips[0];
    CHECK_EQ(clip.name, std::string("walk"));
    CHECK_NEAR(clip.duration, 1.0, 1e-6);
    CHECK_EQ(clip.tracks.size(), 2u);
    if (clip.tracks.size() != 2u) return;

    const anim::Track& lin = clip.tracks[0];
    CHECK_EQ(lin.bone, 1);
    CHECK(lin.interp == anim::Interp::Linear);
    CHECK_EQ(lin.times.size(), 2u);
    CHECK_EQ(lin.translations.size(), 2u);
    CHECK_NEAR(lin.translations[0].x, 0.0, 1e-6);
    CHECK_NEAR(lin.translations[1].x, 10.0, 1e-6);

    const anim::Track& cubic = clip.tracks[1];
    CHECK_EQ(cubic.bone, 2);
    CHECK(cubic.interp == anim::Interp::CubicSpline);
    CHECK_EQ(cubic.translations.size(), 6u);
    CHECK_NEAR(cubic.translations[2].x, 5.0, 1e-6);  // key0 out tangent
    CHECK_NEAR(cubic.translations[3].x, -5.0, 1e-6); // key1 in tangent

    // Sampling: LINEAR midpoint (5,0,0); CUBICSPLINE hermite midpoint (6.25,0,0).
    anim::Pose pose = set.skeleton.BindPose();
    clip.Sample(0.5f, pose);
    CHECK_NEAR(pose.t[1].x, 5.0, 1e-4);
    CHECK_NEAR(pose.t[2].x, 6.25, 1e-4);
    CHECK_NEAR(pose.t[3].x, 0.0, 1e-6); // uncovered bone keeps bind TRS
}

// Edge cases: a clip with no tracks is a no-op; an empty skeleton yields an
// empty matrix list; sampling beyond the end clamps to the last key.
TEST(AnimEdgeCases) {
    anim::AnimationClip empty;
    empty.duration = 1.f;
    anim::Pose p;
    p.Resize(2);
    anim::Pose before = p;
    empty.Sample(0.5f, p);
    CHECK_EQ(p.t.size(), 2u);
    CHECK_NEAR(p.t[0].x, before.t[0].x, 1e-6);
    CHECK_NEAR(p.r[0].w, before.r[0].w, 1e-6);

    anim::Skeleton sk;
    std::vector<math::Mat4> m = sk.ComputeBoneMatrices(anim::Pose{});
    CHECK(m.empty());

    // Sample beyond the last key clamps to it (wrapped into the loop).
    anim::AnimationClip clip;
    clip.duration = 1.f;
    anim::Track tr;
    tr.bone = 0;
    tr.times = {0.f, 1.f};
    tr.translations = {{0, 0, 0}, {10, 0, 0}};
    clip.tracks.push_back(std::move(tr));
    p.Resize(1);
    clip.Sample(1.0f, p);
    CHECK_NEAR(p.t[0].x, 0.0, 1e-6);
}
