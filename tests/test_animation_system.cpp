// Task 12: AnimationSystem standalone unit tests (the subsystem split out of
// GameRuntime). Covers InitState registration, Play / Tick advancement, the
// PoseFor pose query Draw() uses, cross-fade blending, looping, the
// SceneAnimOverride mirror (SyncOverride / InvalidateOverride), state-machine
// attachment and the alive-entity prune.
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "neon/neon.hpp"
#include "neon/scene/systems/animation_system.hpp"
#include "helpers.hpp"

using namespace neon;

namespace {

// One-bone model with two clips. "run" (1s) and "idle" (0.5s) both translate
// the root bone along +Z; the pose is sampled from the clips and the skinning
// matrix's translation z (m[11]) reflects the sample value.
scene::SkinnedModel MakeModel() {
    scene::SkinnedModel m;
    m.skeleton.bones.resize(1);
    m.skeleton.bones[0].name = "root";
    m.skeleton.bones[0].parent = -1;
    m.skeleton.bones[0].inverseBind = math::Mat4::Identity();
    m.parts.push_back({}); // Valid() requires at least one part

    anim::AnimationClip run;
    run.name = "run";
    run.duration = 1.0f;
    anim::Track tr;
    tr.bone = 0;
    tr.interp = anim::Interp::Linear;
    tr.times = {0.0f, 1.0f};
    tr.translations = {{0, 0, 0}, {0, 0, 1.0f}};
    run.tracks.push_back(tr);
    m.clips.push_back(run);

    anim::AnimationClip idle;
    idle.name = "idle";
    idle.duration = 0.5f;
    anim::Track itr;
    itr.bone = 0;
    itr.interp = anim::Interp::Linear;
    itr.times = {0.0f, 0.5f};
    itr.translations = {{0, 0, 0}, {0, 0, 2.0f}};
    idle.tracks.push_back(itr);
    m.clips.push_back(idle);

    m.defaultClip = -1; // no default loop: Update(dt) is a no-op without override
    return m;
}

uint64_t Key(uint32_t id, uint32_t gen) {
    return (static_cast<uint64_t>(id) << 32) | static_cast<uint64_t>(gen);
}

} // namespace

TEST(AnimationSystemInitPlayTickPose) {
    scene::SkinnedModel model = MakeModel();
    scene::AnimationSystem anims;
    const uint64_t key = Key(7, 1);
    anims.InitState(key, {&model}, {});
    CHECK_EQ(anims.Count(), 1u);
    CHECK(anims.HasBinding(key));

    // No override: progress 0, not finished, no override pose (Draw() falls
    // back to the model's own BoneMatrices()).
    CHECK_NEAR(anims.Progress(key), 0.0f, 1e-6f);
    CHECK(!anims.Finished(key));
    std::vector<math::Mat4> bones;
    CHECK(!anims.PoseFor(key, model.skeleton, bones));

    // Play a one-shot; the clip resolves lazily and advances on Tick.
    CHECK(anims.Play(key, "run", false, 0.0f, 1.0f));
    CHECK_NEAR(anims.Progress(key), 0.0f, 1e-6f); // unresolved clip -> 0
    anims.Tick(0.5f);
    CHECK_NEAR(anims.Progress(key), 0.5f, 1e-6f);
    CHECK(!anims.Finished(key));

    // Pose at t=0.5: the run clip translated the root +0.5 z (m[11]).
    CHECK(anims.PoseFor(key, model.skeleton, bones));
    CHECK_EQ(bones.size(), 1u);
    CHECK_NEAR(bones[0].m[11], 0.5f, 1e-5f);

    // One-shot clamps at the end.
    anims.Tick(0.6f); // total 1.1s > 1.0s duration
    CHECK_NEAR(anims.Progress(key), 1.0f, 1e-6f);
    CHECK(anims.Finished(key));

    // Prune drops the state once its entity/draw item is gone.
    anims.Prune([key](uint64_t k) { return k != key; });
    CHECK_EQ(anims.Count(), 0u);
    CHECK(!anims.HasBinding(key));
    CHECK_NEAR(anims.Progress(key), 0.0f, 1e-6f);
}

TEST(AnimationSystemFadeLoopAndInvalidate) {
    scene::SkinnedModel model = MakeModel();
    scene::AnimationSystem anims;
    const uint64_t key = Key(3, 0);

    // Looping play: time wraps, Finished never reports.
    anims.InitState(key, {&model}, {});
    CHECK(anims.Play(key, "run", true, 0.0f, 1.0f));
    anims.Tick(0.75f);
    anims.Tick(0.75f); // total 1.5s -> wraps to 0.5
    CHECK_NEAR(anims.Progress(key), 0.5f, 1e-6f);
    CHECK(!anims.Finished(key));

    // Cross-fade: the fade source is the bind pose (no default clip), so a
    // blend at alpha=0.5 sits halfway between bind (0) and the sampled pose.
    scene::AnimationSystem fade;
    const uint64_t fk = Key(4, 0);
    fade.InitState(fk, {&model}, {});
    CHECK(fade.Play(fk, "run", false, 0.4f, 1.0f));
    fade.Tick(0.2f); // animFade = 0.2 -> blend alpha = 1 - 0.2/0.4 = 0.5
    std::vector<math::Mat4> bones;
    CHECK(fade.PoseFor(fk, model.skeleton, bones));
    CHECK_NEAR(bones[0].m[11], 0.1f, 1e-5f); // lerp(bind 0, sample 0.2, 0.5)

    // InvalidateOverride forces a fresh resolve: the clip restarts at t=0.
    fade.InvalidateOverride(fk);
    fade.Tick(0.1f);
    CHECK_NEAR(fade.Progress(fk), 0.1f, 1e-6f);
}

TEST(AnimationSystemSyncAndStateMachine) {
    scene::SkinnedModel model = MakeModel();
    scene::AnimationSystem anims;
    const uint64_t key = Key(5, 2);

    // Mirror a SceneAnimOverride-style component into the state (the same
    // spec BuildDrawList's per-frame sync forwards).
    anims.InitState(key, {&model}, {});
    anims.SyncOverride(key, {"run", false, 1.0f, 0.0f, true});
    anims.Tick(0.25f);
    CHECK_NEAR(anims.Progress(key), 0.25f, 1e-6f);

    // Deactivating the override drops it.
    anims.SyncOverride(key, {});
    CHECK_NEAR(anims.Progress(key), 0.0f, 1e-6f);
    CHECK(!anims.Finished(key));

    // Attach a state machine; states bind their clip names to the model's
    // clips and the first state plays on the next Tick.
    auto sm = std::make_shared<anim::AnimationStateMachine>();
    sm->AddState("idle", nullptr);
    sm->AddState("run", nullptr);
    sm->SetStateClipName("idle", "idle");
    sm->SetStateClipName("run", "run");
    sm->AddTransition("idle", "run", "speed", 0.5f, 0.0f);
    CHECK(anims.AttachStateMachine(key, sm));
    anims.SetParam(key, "speed", 1.0f);
    anims.Tick(0.0f); // bind + fire the idle->run transition (zero-duration)
    anims.Tick(0.25f);
    // The run clip (duration 1.0) restarted and advanced 0.25s.
    CHECK_NEAR(anims.Progress(key), 0.25f, 1e-6f);

    // Unknown key: attach refuses (no resolved skinned binding).
    CHECK(!anims.AttachStateMachine(Key(999, 0), sm));
    anims.SetParam(Key(999, 0), "speed", 2.0f); // no-op, must not crash
}

TEST(AnimationSystemBlendSpace) {
    scene::SkinnedModel model = MakeModel(); // clips: "idle"(z:+2 over 0.5s), "run"(z:+1 over 1.0s)
    scene::AnimationSystem anims;
    const uint64_t key = Key(11, 2);
    anims.InitState(key, {&model}, {});

    // Blend both clips by param: at t=0 both samples are z=0, so set param and
    // advance to a shared time where the two endpoints differ, then compare the
    // root-z against the lerp of the two endpoint clips.
    CHECK(anims.PlayBlend(key, "idle", "run", 0.0f));
    anims.Tick(0.5f); // shared time 0.5 (idle wraps to 0, run at 0.5)

    // At param 0 the pose leans toward run (endpoint A="idle" is A, B="run").
    // PoseFor must produce a valid pose.
    std::vector<math::Mat4> bones;
    CHECK(anims.PoseFor(key, model.skeleton, bones));
    CHECK_EQ(bones.size(), 1u);

    // Blend param t: pose.z should be between the two endpoint samples at 0.5s.
    // idle sampled at 0.5 -> wraps to 0 -> z=0; run at 0.5 -> z=0.5.
    anims.SetBlendParam(key, 1.0f); // fully toward B (run)
    anims.Tick(0.0f);
    CHECK(anims.PoseFor(key, model.skeleton, bones));
    CHECK_NEAR(bones[0].m[11], 0.5f, 1e-4f); // run z at t=0.5 = 0.5

    // Unknown clip: PlayBlend refuses.
    CHECK(!anims.PlayBlend(key, "nope", "run", 0.5f));
}
