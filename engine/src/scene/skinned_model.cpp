#include "neon/scene/skinned_model.hpp"

#include "neon/core/log.hpp"
#include <algorithm>
#include <cctype>
#include <fstream>
#include <sstream>
#include <utility>

namespace neon::scene {
namespace {

bool NameHasIdle(const std::string& name) {
    std::string lower = name;
    std::transform(lower.begin(), lower.end(), lower.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return lower.find("idle") != std::string::npos;
}

std::string ReadFileText(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return {};
    std::stringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

} // namespace

void SkinnedModel::Update(float dt) {
    if (defaultClip >= 0) animator.Update(dt);
}

std::vector<math::Mat4> SkinnedModel::BoneMatrices() const {
    anim::Pose pose = skeleton.BindPose();
    if (defaultClip >= 0) {
        if (const anim::AnimationClip* c = animator.Clip()) c->Sample(animator.Time(), pose);
    }
    return skeleton.ComputeBoneMatrices(pose);
}

core::Result<SkinnedModel> LoadSkinnedModel(assets::AssetManager& assets,
                                            const std::string& path) {
    assets::GltfAsset gltf = assets.LoadGLTF(path);
    if (!gltf.Valid())
        return core::Result<SkinnedModel>::Err("skinned: glTF failed to load: " + path);

    int skinIndex = -1;
    for (const assets::GltfMeshNode& n : gltf.nodes) {
        if (n.mesh.Skinned()) {
            skinIndex = n.mesh.SkinIndex();
            break;
        }
    }
    if (skinIndex < 0)
        return core::Result<SkinnedModel>::Err("skinned: no skinned mesh node in " + path);

    const std::string jsonText = ReadFileText(path);
    if (jsonText.empty())
        return core::Result<SkinnedModel>::Err("skinned: cannot read " + path);

    core::Result<anim::AnimSet> animSet = anim::ImportGltf(jsonText, gltf, skinIndex);
    if (!animSet.Ok())
        return core::Result<SkinnedModel>::Err("skinned: animation import failed: " +
                                               animSet.Error());

    SkinnedModel out;
    out.skeleton = std::move(animSet.Value().skeleton);
    out.clips = std::move(animSet.Value().clips);
    for (const assets::GltfMeshNode& n : gltf.nodes) {
        if (!n.mesh.Skinned()) continue; // skip unskinned decorative nodes
        // Skip transparent layers with no base texture (e.g. Blender fur-card
        // shells whose alpha channel was lost on export). Rendered opaque they
        // paint a messy white film over the skin; as pure-alpha blend with no
        // alpha source they show as a solid white blob. The skin underneath
        // (mat0 Wolf_1) carries the real fur colour.
        if (n.material.transparent && !n.material.albedo.Valid()) continue;
        SkinnedModel::Part p;
        p.mesh = n.mesh;
        p.material = n.material;
        p.localTransform = n.transform;
        out.parts.push_back(std::move(p));
    }
    if (!out.clips.empty()) {
        out.defaultClip = 0;
        for (size_t i = 0; i < out.clips.size(); ++i) {
            // Prefer a locomotion clip (walk/run) so skinned entities visibly
            // animate in the editor/playtest; fall back to idle. Most glTF
            // packs ship a small idle (breathing) that reads as static.
            std::string lower = out.clips[i].name;
            std::transform(lower.begin(), lower.end(), lower.begin(),
                           [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            if (NameHasIdle(out.clips[i].name) ||
                lower.find("walk") != std::string::npos ||
                lower.find("run") != std::string::npos) {
                out.defaultClip = static_cast<int>(i);
                break;
            }
        }
        out.animator.Play(&out.clips[static_cast<size_t>(out.defaultClip)]);
    }
    if (!out.Valid())
        return core::Result<SkinnedModel>::Err("skinned: model invalid for " + path);
    return core::Result<SkinnedModel>::Ok(std::move(out));
}

} // namespace neon::scene
