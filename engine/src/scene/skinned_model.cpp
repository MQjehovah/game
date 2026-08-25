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

bool IsIdentity(const math::Mat4& m) {
    for (int r = 0; r < 4; ++r)
        for (int c = 0; c < 4; ++c) {
            const float expect = (r == c) ? 1.0f : 0.0f;
            if (std::fabs(m.m[r * 4 + c] - expect) > 1e-6f) return false;
        }
    return true;
}

math::Mat4 LocalMat(const anim::Bone& b) {
    return math::Mat4::Translation(b.bindT) * b.bindR.ToMat4() *
           math::Mat4::Scale(b.bindS);
}

void Decompose(const math::Mat4& m, math::Vec3& t, math::Quat& r, math::Vec3& s) {
    t = {m.m[3], m.m[7], m.m[11]};
    const float sx = std::sqrt(m.m[0] * m.m[0] + m.m[1] * m.m[1] + m.m[2] * m.m[2]);
    const float sy = std::sqrt(m.m[4] * m.m[4] + m.m[5] * m.m[5] + m.m[6] * m.m[6]);
    const float sz = std::sqrt(m.m[8] * m.m[8] + m.m[9] * m.m[9] + m.m[10] * m.m[10]);
    s = {sx > 1e-8f ? sx : 1.0f, sy > 1e-8f ? sy : 1.0f, sz > 1e-8f ? sz : 1.0f};
    const float r00 = m.m[0] / s.x, r01 = m.m[1] / s.x, r02 = m.m[2] / s.x;
    const float r10 = m.m[4] / s.y, r11 = m.m[5] / s.y, r12 = m.m[6] / s.y;
    const float r20 = m.m[8] / s.z, r21 = m.m[9] / s.z, r22 = m.m[10] / s.z;
    const float trace = r00 + r11 + r22;
    if (trace > 0.0f) {
        const float sq = std::sqrt(trace + 1.0f) * 2.0f;
        r = {0.25f * sq, (r21 - r12) / sq, (r02 - r20) / sq, (r10 - r01) / sq};
    } else if (r00 > r11 && r00 > r22) {
        const float sq = std::sqrt(1.0f + r00 - r11 - r22) * 2.0f;
        r = {(r21 - r12) / sq, 0.25f * sq, (r01 + r10) / sq, (r02 + r20) / sq};
    } else if (r11 > r22) {
        const float sq = std::sqrt(1.0f + r11 - r00 - r22) * 2.0f;
        r = {(r02 - r20) / sq, (r01 + r10) / sq, 0.25f * sq, (r12 + r21) / sq};
    } else {
        const float sq = std::sqrt(1.0f + r22 - r00 - r11) * 2.0f;
        r = {(r10 - r01) / sq, (r02 + r20) / sq, (r12 + r21) / sq, 0.25f * sq};
    }
    r = r.Normalized();
}

// Some exporters store a node REST pose that differs from the skin's own
// inverseBind, so bind-go skinning (node-global * inverseBind) is not the
// identity and the authored mesh comes out distorted (and animation, which is
// authored relative to the node REST pose, gets distorted too). Keep the
// skeleton bind as the node REST pose and instead rewrite each skin joint's
// inverseBind to inverse(node-REST global): then bind-go skinning is identity
// AND animation (relative to the node REST pose) is correct.
void FixSkinBindImpl(anim::Skeleton& sk, const std::vector<uint32_t>& jointNodes) {
    const size_t n = sk.bones.size();
    if (n == 0) return;
    std::vector<math::Mat4> g(n);
    std::vector<bool> done(n, false);
    // Compute node-REST world transforms from the bind local chain (this is
    // the tree the animation clips are authored against).
    for (size_t i = 0; i < n; ++i)
        if (sk.bones[i].parent < 0) {
            g[i] = LocalMat(sk.bones[i]);
            done[i] = true;
        }
    bool changed = true;
    while (changed) {
        changed = false;
        for (size_t i = 0; i < n; ++i) {
            if (done[i]) continue;
            const int p = sk.bones[i].parent;
            if (p < 0 || p >= static_cast<int>(n) || !done[static_cast<size_t>(p)]) continue;
            g[i] = g[static_cast<size_t>(p)] * LocalMat(sk.bones[i]);
            done[i] = true;
            changed = true;
        }
    }
    // Rewrite the joints' inverseBind to inverse(node-REST global).
    for (uint32_t j : jointNodes) {
        if (j < n && done[j]) sk.bones[j].inverseBind = g[j].Inverted();
    }
}

std::string ReadFileText(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return {};
    std::stringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

} // namespace

void FixSkinBind(anim::Skeleton& sk, const std::vector<uint32_t>& jointNodes) {
    FixSkinBindImpl(sk, jointNodes);
}

void EnsureValidSkinBind(anim::Skeleton& sk, const std::vector<uint32_t>& jointNodes) {
    const anim::Pose bp = sk.BindPose();
    const std::vector<math::Mat4> bm = sk.ComputeBoneMatrices(bp);
    float worst = 0.0f;
    for (const math::Mat4& m : bm)
        for (int r = 0; r < 4; ++r)
            for (int c = 0; c < 4; ++c) {
                const float e = (r == c) ? 1.0f : 0.0f;
                worst = std::max(worst, std::fabs(m.m[r * 4 + c] - e));
            }
    if (worst > 1e-3f) {
        FixSkinBind(sk, jointNodes);
        NEON_LOG_INFO("Skin: non-standard bind (offset %.3f); rebound from node-REST", worst);
    }
}

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
    {
        std::vector<uint32_t> jointNodes;
        if (skinIndex >= 0 && skinIndex < static_cast<int>(gltf.skins.size()))
            jointNodes = gltf.skins[static_cast<size_t>(skinIndex)].joints;
        EnsureValidSkinBind(out.skeleton, jointNodes);
    }
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
    // Painter's order: opaque parts first, transparent last. Alpha-blended
    // geometry does not write depth (Renderer::ApplyMaterial), so a
    // transparent part drawn BEFORE an opaque one gets overwritten by it.
    std::stable_sort(out.parts.begin(), out.parts.end(),
                     [](const SkinnedModel::Part& a, const SkinnedModel::Part& b) {
                         return !a.material.transparent && b.material.transparent;
                     });
    if (!out.clips.empty()) {
        out.defaultClip = 0;
        for (size_t i = 0; i < out.clips.size(); ++i) {
            // Idle first: a locomotion clip (walk/run) often carries root
            // translation which, played in place, drags the rig off its bind
            // pose. Idle keeps the model at its authored bind stance.
            if (NameHasIdle(out.clips[i].name)) {
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
