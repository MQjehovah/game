#pragma once

#include <string>
#include <vector>

#include "neon/anim/anim.hpp"
#include "neon/assets/asset_manager.hpp"
#include "neon/core/result.hpp"
#include "neon/gfx/mesh.hpp"
#include "neon/gfx/renderer.hpp"
#include "neon/math/mat4.hpp"

namespace neon::scene {

// An animated skinned glTF model bound to one entity. `parts` holds every
// skinned mesh node of the file (body/fur/eyes/teeth...); unskinned
// decorative nodes (e.g. a Blender ground circle) are skipped. The skeleton
// and clips come from the same glTF; Update() advances the default clip and
// BoneMatrices() feeds Renderer::DrawSkinnedMesh.
struct SkinnedModel {
    struct Part {
        gfx::Mesh mesh;
        gfx::Material material;   // the glTF node's own PBR material
        math::Mat4 localTransform; // node world transform relative to the scene root
    };
    std::vector<Part> parts;
    anim::Skeleton skeleton;
    std::vector<anim::AnimationClip> clips;
    int defaultClip = -1;
    anim::Animator animator;

    bool Valid() const { return !parts.empty() && !skeleton.bones.empty(); }

    // Advances the default clip. Fixed-step callers (GameRuntime::Tick) keep
    // the animation deterministic.
    void Update(float dt);

    // Skinning matrices for the current pose (bind pose when no clip).
    std::vector<math::Mat4> BoneMatrices() const;

    // --- Per-instance animation control (M1: gameplay clips) --------------
    // A SkinnedModel is often SHARED between entities (glTF cache); these
    // control a per-entity pose layer so one wolf can play "death" while
    // another loops "idle" from the same loaded model.
    // Plays `name` (case-insensitive substring match, e.g. "attack" picks
    // "01_attack_Armature"). crossFade > 0 blends from the current pose over
    // crossFade seconds; loop replays the clip at its end. Returns false when
    // no clip matches.
    bool PlayClip(const std::string& name, bool loop = true, float crossFade = 0.2f,
                  float speed = 1.0f);
    // True when the current one-shot finished (looping clips never finish).
    bool ClipFinished() const;
    // Normalized progress [0,1] of the current clip (1 when finished).
    float ClipProgress() const;
    // Per-instance update; call INSTEAD of Update() when an instance pose is
    // active. Falls back to the default-clip animator when no override is
    // set, so plain scene placement keeps working unchanged.
    void UpdateInstance(float dt);
    // Skinning matrices for the instance pose.
    std::vector<math::Mat4> InstanceBoneMatrices() const;

private:
    // Per-instance override clip state (cross-fade from the previous pose).
    const anim::AnimationClip* instClip_ = nullptr;
    bool instLoop_ = true;
    float instTime_ = 0.0f;
    float instSpeed_ = 1.0f;
    float instFade_ = 0.0f;      // remaining fade time (0 = done)
    float instFadeTotal_ = 0.0f;
    anim::Pose instFromPose_;    // pose at the moment of the switch
    // Cached bind pose (sized once) reused as the sample scratch/start pose.
    mutable anim::Pose scratch_;
};

// Loads a glTF file and builds the skinned model: parts (skinned mesh nodes
// only), skeleton and clips. The default clip is the first clip whose name
// contains "idle", else clips[0]. Err when the asset has no skinned mesh
// node or the animation import fails.
core::Result<SkinnedModel> LoadSkinnedModel(assets::AssetManager& assets,
                                            const std::string& path);

// Makes a skeleton's bind pose reproduce the authored mesh under skinning:
// rewrites each skin joint's bind global to inverse(inverseBind), so bind-go
// skinning is identity (some exporters put a non-bind TRS on nodes). Called
// by LoadSkinnedModel; expose a detail AnimSet skeleton to it too (e.g. the
// standalone demo's wolf rig).
void FixSkinBind(anim::Skeleton& skeleton, const std::vector<uint32_t>& jointNodes);

// Detects whether the skin's inverseBind reproduces the authored mesh under
// bind-go skinning (bind skin-matrix ~identity). Non-standard exports (e.g.
// Blender fur rigs where node REST != inverseBind) get rebound via
// FixSkinBind; standard models are left untouched. Call on a skeleton built
// from ImportGltf before animating.
void EnsureValidSkinBind(anim::Skeleton& skeleton, const std::vector<uint32_t>& jointNodes);

} // namespace neon::scene
