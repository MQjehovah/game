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

} // namespace neon::scene
