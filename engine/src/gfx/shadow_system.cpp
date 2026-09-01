#include "neon/gfx/shadow_system.hpp"

#include <algorithm>
#include <cstdlib>
#include <cstring>

#include "neon/core/log.hpp"
#include "neon/gfx/csm.hpp"
#include "neon/gfx/point_shadow.hpp"

namespace neon::gfx {
namespace {

const char* kShadowVertexShader = R"(
#version 330 core
layout(location = 0) in vec3 aPos;
uniform mat4 uMVP;
void main() {
    gl_Position = uMVP * vec4(aPos, 1.0);
}
)";

const char* kShadowInstancedVertexShader = R"(
#version 330 core
layout(location = 0) in vec3 aPos;
layout(location = 4) in mat4 aInstance;
uniform mat4 uMVP;
void main() {
    gl_Position = uMVP * aInstance * vec4(aPos, 1.0);
}
)";

const char* kShadowSkinnedVertexShader = R"(
#version 330 core
layout(location = 0) in vec3 aPos;
layout(location = 4) in vec4 aJointIds;
layout(location = 5) in vec4 aWeights;
uniform mat4 uBoneMatrices[64];
uniform mat4 uMVP;
void main() {
    mat4 skin = mat4(0.0);
    for (int i = 0; i < 4; ++i) {
        int id = int(aJointIds[i]);
        if (id >= 0 && id < 64) skin += aWeights[i] * uBoneMatrices[id];
    }
    gl_Position = uMVP * skin * vec4(aPos, 1.0);
}
)";

// Depth is packed into an RGBA8 color target (EncodeDepth) because the
// window depth buffer AND FBO depth textures are broken on the tested Intel
// driver while color FBO rendering works. 24 bits of precision is ample.
const char* kShadowFragmentShader = R"(
#version 330 core
out vec4 FragColor;
vec4 EncodeDepth(float d) {
    vec4 bits = vec4(1.0, 255.0, 65025.0, 16581375.0) * d;
    bits = fract(bits);
    bits -= bits.yzww * vec4(1.0 / 255.0, 1.0 / 255.0, 1.0 / 255.0, 0.0);
    return bits;
}
void main() {
    FragColor = EncodeDepth(gl_FragCoord.z);
}
)";

// Point-light shadow variants. The depth is NOT gl_FragCoord.z: for a point
// light the per-face map must store a single linear distance (dist from the
// light) so the lit shader can compare it against the per-fragment distance in
// every direction of that face. The vertex shaders therefore output the world
// position and the fragment shader encodes length(worldPos - uLightPos)/range.
const char* kPointShadowVertexShader = R"(
#version 330 core
layout(location = 0) in vec3 aPos;
uniform mat4 uMVP;
uniform mat4 uModel;
out vec3 vWorldPos;
void main() {
    vWorldPos = (uModel * vec4(aPos, 1.0)).xyz;
    gl_Position = uMVP * vec4(aPos, 1.0);
}
)";

const char* kPointShadowInstancedVertexShader = R"(
#version 330 core
layout(location = 0) in vec3 aPos;
layout(location = 4) in mat4 aInstance;
uniform mat4 uMVP;
out vec3 vWorldPos;
void main() {
    vWorldPos = (aInstance * vec4(aPos, 1.0)).xyz;
    gl_Position = uMVP * aInstance * vec4(aPos, 1.0);
}
)";

const char* kPointShadowSkinnedVertexShader = R"(
#version 330 core
layout(location = 0) in vec3 aPos;
layout(location = 4) in vec4 aJointIds;
layout(location = 5) in vec4 aWeights;
uniform mat4 uBoneMatrices[64];
uniform mat4 uMVP;
uniform mat4 uModel;
out vec3 vWorldPos;
void main() {
    mat4 skin = mat4(0.0);
    for (int i = 0; i < 4; ++i) {
        int id = int(aJointIds[i]);
        if (id >= 0 && id < 64) skin += aWeights[i] * uBoneMatrices[id];
    }
    vWorldPos = (uModel * skin * vec4(aPos, 1.0)).xyz;
    gl_Position = uMVP * skin * vec4(aPos, 1.0);
}
)";

const char* kPointShadowFragmentShader = R"(
#version 330 core
in vec3 vWorldPos;
out vec4 FragColor;
uniform vec3 uLightPos;
uniform float uLightRange;
vec4 EncodeDepth(float d) {
    vec4 bits = vec4(1.0, 255.0, 65025.0, 16581375.0) * d;
    bits = fract(bits);
    bits -= bits.yzww * vec4(1.0 / 255.0, 1.0 / 255.0, 1.0 / 255.0, 0.0);
    return bits;
}
void main() {
    FragColor = EncodeDepth(clamp(length(vWorldPos - uLightPos) / uLightRange, 0.0, 1.0));
}
)";

} // namespace

void ShadowSystem::Init(IRenderBackend& backend, MeshHandle probeQuad, ShaderHandle unlitShader) {
    probeQuad_ = probeQuad;
    unlitShader_ = unlitShader;
    depthShader_ = backend.CreateShader(kShadowVertexShader, kShadowFragmentShader, "shadow");
    depthInstancedShader_ =
        backend.CreateShader(kShadowInstancedVertexShader, kShadowFragmentShader, "shadow_inst");
    depthSkinnedShader_ =
        backend.CreateShader(kShadowSkinnedVertexShader, kShadowFragmentShader, "shadow_skin");
    pointDepthShader_ =
        backend.CreateShader(kPointShadowVertexShader, kPointShadowFragmentShader, "point_shadow");
    pointDepthInstancedShader_ = backend.CreateShader(kPointShadowInstancedVertexShader,
                                                      kPointShadowFragmentShader,
                                                      "point_shadow_inst");
    pointDepthSkinnedShader_ = backend.CreateShader(kPointShadowSkinnedVertexShader,
                                                    kPointShadowFragmentShader,
                                                    "point_shadow_skin");
    NEON_LOG_CAT(neon::core::LogCategory::Gfx, neon::core::LogLevel::Info,
                 "Renderer: point shadow shaders %s",
                 (pointDepthShader_.Valid() && pointDepthInstancedShader_.Valid() &&
                  pointDepthSkinnedShader_.Valid())
                     ? "ok"
                     : "FAILED");

    if (shadowsForcedOff_) {
        NEON_LOG_CAT(neon::core::LogCategory::Gfx, neon::core::LogLevel::Warn,
                     "Renderer: CSM disabled by flag (--disable-fbo/--no-shadows)");
        return;
    }
    for (int i = 0; i < kShadowCascades; ++i) {
        shadowRT_[i] = backend.CreateRenderTarget(shadowSize_, shadowSize_);
        shadowDepthTex_[i] = backend.RenderTargetColorTexture(shadowRT_[i]);
        if (!shadowRT_[i].Valid() || !shadowDepthTex_[i].Valid()) {
            NEON_LOG_CAT(neon::core::LogCategory::Gfx, neon::core::LogLevel::Warn,
                         "Renderer: cascade %d shadow target failed", i);
            csmEnabled_ = false;
            return;
        }
    }
    csmEnabled_ = TestDepthTargetCapability();
    NEON_LOG_CAT(neon::core::LogCategory::Gfx, neon::core::LogLevel::Info,
                 "Renderer: CSM shadow maps %dx%d x3 (%s)", shadowSize_, shadowSize_,
                 csmEnabled_ ? "ok" : "FAILED -> CPU projected shadows fallback");

    // Point-light cubemap shadows reuse the same color-encoded-depth FBO path,
    // so they engage only when the CSM capability self-test passed. Six 2D
    // maps per light (layered cubemap FBOs are unreliable on the Intel driver);
    // the lit shader picks the face from the fragment->light direction.
    if (csmEnabled_) {
        pointShadowsEnabled_ = true;
        for (int li = 0; li < kShadowPointLights && pointShadowsEnabled_; ++li) {
            for (int face = 0; face < 6; ++face) {
                pointShadowRT_[li][face] =
                    backend.CreateRenderTarget(kPointShadowSize, kPointShadowSize);
                pointShadowDepthTex_[li][face] =
                    backend.RenderTargetColorTexture(pointShadowRT_[li][face]);
                if (!pointShadowRT_[li][face].Valid() || !pointShadowDepthTex_[li][face].Valid()) {
                    NEON_LOG_CAT(neon::core::LogCategory::Gfx, neon::core::LogLevel::Warn,
                                 "Renderer: point light %d face %d shadow target failed", li, face);
                    pointShadowsEnabled_ = false;
                    break;
                }
            }
        }
        NEON_LOG_CAT(neon::core::LogCategory::Gfx, neon::core::LogLevel::Info,
                     "Renderer: point light shadow maps %dx%d x%d lights (%s)",
                     kPointShadowSize, kPointShadowSize, kShadowPointLights,
                     pointShadowsEnabled_ ? "ok" : "FAILED");
    } else {
        pointShadowsEnabled_ = false;
    }
    // Diagnostic override (not public API): isolate the point-light shadow
    // contribution for verification (screenshot diffs) without touching CSM.
    if (pointShadowsEnabled_ && std::getenv("NEON_NO_POINT_SHADOWS")) {
        NEON_LOG_CAT(neon::core::LogCategory::Gfx, neon::core::LogLevel::Warn,
                     "Renderer: point light shadows disabled by NEON_NO_POINT_SHADOWS");
        pointShadowsEnabled_ = false;
    }
}

void ShadowSystem::Shutdown(IRenderBackend& backend) {
    if (depthShader_.Valid()) backend.DestroyShader(depthShader_);
    if (depthInstancedShader_.Valid()) backend.DestroyShader(depthInstancedShader_);
    if (depthSkinnedShader_.Valid()) backend.DestroyShader(depthSkinnedShader_);
    if (pointDepthShader_.Valid()) backend.DestroyShader(pointDepthShader_);
    if (pointDepthInstancedShader_.Valid()) backend.DestroyShader(pointDepthInstancedShader_);
    if (pointDepthSkinnedShader_.Valid()) backend.DestroyShader(pointDepthSkinnedShader_);
    depthShader_ = {};
    depthInstancedShader_ = {};
    depthSkinnedShader_ = {};
    pointDepthShader_ = {};
    pointDepthInstancedShader_ = {};
    pointDepthSkinnedShader_ = {};
    for (int i = 0; i < kShadowCascades; ++i) {
        if (shadowRT_[i].Valid()) backend.DestroyRenderTarget(shadowRT_[i]);
        shadowRT_[i] = {};
        shadowDepthTex_[i] = {};
    }
    for (int li = 0; li < kShadowPointLights; ++li) {
        for (int face = 0; face < 6; ++face) {
            if (pointShadowRT_[li][face].Valid())
                backend.DestroyRenderTarget(pointShadowRT_[li][face]);
            pointShadowRT_[li][face] = {};
            pointShadowDepthTex_[li][face] = {};
        }
    }
    shadowCasters_.clear();
    shadowSortKeys_.clear();
    boneUniformFlat_.clear();
}

void ShadowSystem::BeginFrame() {
    csmActive_ = false;
    pointShadowsActive_ = false;
    shadowPassRanThisFrame_ = false;
}

void ShadowSystem::SetShadowsEnabled(bool enabled) {
    shadowsForcedOff_ = !enabled;
    if (!enabled) csmEnabled_ = false;
}

void ShadowSystem::RunPass(const Camera& camera, float aspect, const math::Vec3& sunDir,
                           const math::Vec3* pointPos, const float* pointRadius,
                           int pointCount) {
    if (!csmEnabled_) return;
    shadowPassRanThisFrame_ = true;
    ComputeCascadeSplits(camera.nearPlane, camera.farPlane, cascadeSplits_);
    // Cascade frusta must match the camera projection (which may use the
    // viewport rect's aspect when the editor renders into a sub-viewport).
    const float a = aspect;

    // Union of all shadow-caster world AABBs: the cascade light frusta are
    // tightened to it so a small scene fills the shadow maps instead of being
    // squished into a corner.
    math::AABB sceneBounds;
    sceneBounds.min = {1e30f, 1e30f, 1e30f};
    sceneBounds.max = {-1e30f, -1e30f, -1e30f};
    bool hasScene = false;
    for (const ShadowDraw& draw : shadowCasters_) {
        if (!draw.mesh.Valid()) continue;
        if (!draw.models.empty()) {
            for (const math::Mat4& m : draw.models) {
                sceneBounds.Expand(math::TransformAABB(draw.bounds, m).min);
                sceneBounds.Expand(math::TransformAABB(draw.bounds, m).max);
                hasScene = true;
            }
        } else {
            math::AABB w = math::TransformAABB(draw.bounds, draw.model);
            sceneBounds.Expand(w.min);
            sceneBounds.Expand(w.max);
            hasScene = true;
        }
    }
    const math::AABB* scenePtr = hasScene ? &sceneBounds : nullptr;

    for (int i = 0; i < kShadowCascades; ++i) {
        lightViewProj_[i] = ComputeCascadeLightViewProj(sunDir, camera, a, cascadeSplits_[i],
                                                        cascadeSplits_[i + 1], scenePtr);
    }

    for (int i = 0; i < kShadowCascades; ++i) {
        if (!shadowRT_[i].Valid()) continue;
        backend_->BindRenderTarget(shadowRT_[i]);
        // Encoded far depth by default: anything not drawn is lit.
        backend_->Clear({1.0f, 1.0f, 1.0f, 1.0f}, 1.0f);
        backend_->SetBlendMode(BlendMode::Opaque);
        backend_->SetCullMode(CullMode::Back);
        // No depth buffer in the color-encoded map (the window/FBO depth path
        // is broken on the tested Intel driver); painter's-order (far to near
        // in light space) gives the correct nearest-surface per texel.
        backend_->SetDepthTest(false, false);
        DrawShadowCastersSorted(lightViewProj_[i]);
    }
    // Point-light cubemap faces reuse the same caster list (cleared below).
    RunPointShadowPass(pointPos, pointRadius, pointCount);
    backend_->BindDefaultTarget();
    shadowCasters_.clear();
    csmActive_ = true;
}

void ShadowSystem::DrawShadowCastersSorted(const math::Mat4& lightVP) {
    if (shadowCasters_.empty()) return;
    // Extract the light view (projection is ortho, translation-only per axis)
    // to sort casters by their distance along the light direction.
    const math::Mat4 lightView = lightVP;
    shadowSortKeys_.clear();
    shadowSortKeys_.reserve(shadowCasters_.size());
    for (const ShadowDraw& draw : shadowCasters_) {
        math::Vec3 center;
        if (!draw.models.empty()) {
            for (const math::Mat4& m : draw.models) {
                center += m.TransformPoint(draw.bounds.Center());
            }
            center = center * (1.0f / static_cast<float>(draw.models.size()));
        } else {
            center = draw.model.TransformPoint(draw.bounds.Center());
        }
        shadowSortKeys_.push_back({&draw, lightView.TransformPoint(center).z});
    }
    // NDC z grows as light-space z goes negative (ortho slope is negative), so
    // the farthest caster has the largest value; draw it first (last wins).
    std::sort(shadowSortKeys_.begin(), shadowSortKeys_.end(),
              [](const ShadowSortKey& a, const ShadowSortKey& b) { return a.z > b.z; });
    for (const ShadowSortKey& k : shadowSortKeys_) DrawShadowCaster(*k.draw, lightVP);
}

void ShadowSystem::DrawShadowCaster(const ShadowDraw& draw, const math::Mat4& lightVP) {
    if (!draw.mesh.Valid()) return;
    if (!draw.models.empty()) {
        backend_->UseShader(depthInstancedShader_);
        backend_->SetUniformMat4("uMVP", lightVP);
        backend_->DrawMeshInstanced(draw.mesh, draw.models.data(),
                                    static_cast<uint32_t>(draw.models.size()));
    } else if (!draw.bones.empty()) {
        backend_->UseShader(depthSkinnedShader_);
        boneUniformFlat_.resize(static_cast<size_t>(draw.boneCount) * 16);
        for (int i = 0; i < draw.boneCount; ++i)
            std::memcpy(boneUniformFlat_.data() + static_cast<size_t>(i) * 16,
                        draw.bones[static_cast<size_t>(i)].Data(), 16 * sizeof(float));
        backend_->SetUniformMat4Array("uBoneMatrices", boneUniformFlat_.data(), draw.boneCount);
        backend_->SetUniformMat4("uMVP", lightVP * draw.model);
        backend_->DrawMesh(draw.mesh);
    } else {
        backend_->UseShader(depthShader_);
        backend_->SetUniformMat4("uMVP", lightVP * draw.model);
        backend_->DrawMesh(draw.mesh);
    }
}

void ShadowSystem::RunPointShadowPass(const math::Vec3* pointPos, const float* pointRadius,
                                      int pointCount) {
    if (!pointShadowsEnabled_) return;
    pointShadowsActive_ = false;
    const int lightCount = std::min(pointCount, kShadowPointLights);
    for (int li = 0; li < lightCount; ++li) {
        if (pointRadius[li] <= 0.0f) continue;
        const float range = pointRadius[li];
        const math::Vec3 lightPos = pointPos[li];
        bool allFaces = true;
        for (int face = 0; face < 6; ++face) {
            if (!pointShadowRT_[li][face].Valid()) {
                allFaces = false;
                break;
            }
            pointLightViewProj_[li][face] =
                ComputePointLightFaceViewProj(lightPos, face, kPointShadowNear, range);
        }
        if (!allFaces) continue;
        DrawPointShadowCastersSorted(li, lightPos, range);
        pointShadowsActive_ = true;
        if (sceneUniformStamp_) ++*sceneUniformStamp_; // B1: shadow uniform set changed
    }
}

void ShadowSystem::DrawPointShadowCastersSorted(int lightIndex, const math::Vec3& lightPos,
                                                float range) {
    if (shadowCasters_.empty()) return;

    // Color-encoded maps have no depth buffer, so draw casters far -> near from
    // the light (last wins = nearest surface). Casters fully outside the
    // light's sphere of influence cannot shadow anything the light reaches.
    shadowSortKeys_.clear();
    shadowSortKeys_.reserve(shadowCasters_.size());
    for (const ShadowDraw& draw : shadowCasters_) {
        if (!draw.mesh.Valid()) continue;
        math::Vec3 center;
        if (!draw.models.empty()) {
            for (const math::Mat4& m : draw.models) center += m.TransformPoint(draw.bounds.Center());
            center = center * (1.0f / static_cast<float>(draw.models.size()));
        } else {
            center = draw.model.TransformPoint(draw.bounds.Center());
        }
        const math::Vec3 ext = draw.bounds.Extents();
        const float boxRadius = ext.Length();
        const float dist = (center - lightPos).Length();
        if (dist - boxRadius > range) continue;
        shadowSortKeys_.push_back({&draw, dist});
    }
    std::sort(shadowSortKeys_.begin(), shadowSortKeys_.end(),
              [](const ShadowSortKey& a, const ShadowSortKey& b) { return a.z > b.z; });

    backend_->SetBlendMode(BlendMode::Opaque);
    backend_->SetCullMode(CullMode::Back);
    backend_->SetDepthTest(false, false);
    for (int face = 0; face < 6; ++face) {
        backend_->BindRenderTarget(pointShadowRT_[lightIndex][face]);
        backend_->Clear({1.0f, 1.0f, 1.0f, 1.0f}, 1.0f);
        for (const ShadowSortKey& k : shadowSortKeys_)
            DrawPointShadowCaster(*k.draw, pointLightViewProj_[lightIndex][face], lightPos, range);
    }
    backend_->BindDefaultTarget();
}

void ShadowSystem::DrawPointShadowCaster(const ShadowDraw& draw, const math::Mat4& lightVP,
                                         const math::Vec3& lightPos, float range) {
    if (!draw.mesh.Valid()) return;
    if (!draw.models.empty()) {
        backend_->UseShader(pointDepthInstancedShader_);
        backend_->SetUniformMat4("uMVP", lightVP);
        backend_->SetUniformVec3("uLightPos", lightPos);
        backend_->SetUniformFloat("uLightRange", range);
        backend_->DrawMeshInstanced(draw.mesh, draw.models.data(),
                                    static_cast<uint32_t>(draw.models.size()));
    } else if (!draw.bones.empty()) {
        backend_->UseShader(pointDepthSkinnedShader_);
        boneUniformFlat_.resize(static_cast<size_t>(draw.boneCount) * 16);
        for (int i = 0; i < draw.boneCount; ++i)
            std::memcpy(boneUniformFlat_.data() + static_cast<size_t>(i) * 16,
                        draw.bones[static_cast<size_t>(i)].Data(), 16 * sizeof(float));
        backend_->SetUniformMat4Array("uBoneMatrices", boneUniformFlat_.data(), draw.boneCount);
        backend_->SetUniformMat4("uMVP", lightVP * draw.model);
        backend_->SetUniformMat4("uModel", draw.model);
        backend_->SetUniformVec3("uLightPos", lightPos);
        backend_->SetUniformFloat("uLightRange", range);
        backend_->DrawMesh(draw.mesh);
    } else {
        backend_->UseShader(pointDepthShader_);
        backend_->SetUniformMat4("uMVP", lightVP * draw.model);
        backend_->SetUniformMat4("uModel", draw.model);
        backend_->SetUniformVec3("uLightPos", lightPos);
        backend_->SetUniformFloat("uLightRange", range);
        backend_->DrawMesh(draw.mesh);
    }
}

bool ShadowSystem::TestDepthTargetCapability() {
    if (!backend_ || !depthShader_.Valid() || !probeQuad_.Valid()) return false;
    constexpr int kSize = 64;

    // --- Part A: DrawElements writes into a color FBO (encoded depth reaches
    // the render target). Uses the color readback path, which is reliable even
    // on the Intel driver whose GL_DEPTH readback returns garbage.
    bool fboWrites = false;
    {
        RenderTargetHandle rt = backend_->CreateRenderTarget(kSize, kSize);
        if (rt.Valid()) {
            backend_->BindRenderTarget(rt);
            backend_->Clear({1.0f, 1.0f, 1.0f, 1.0f}, 1.0f);
            backend_->UseShader(depthShader_);
            backend_->SetUniformMat4("uMVP", math::Mat4::Identity());
            backend_->SetCullMode(CullMode::None);
            backend_->SetDepthTest(false, false);
            backend_->SetBlendMode(BlendMode::Opaque);
            backend_->DrawMesh(probeQuad_);
            unsigned char px[4] = {0, 0, 0, 0};
            backend_->ReadCurrentTargetPixel(kSize / 2, kSize / 2, px);
            backend_->DestroyRenderTarget(rt);
            const float decoded = static_cast<float>(px[0]) / 255.0f +
                                  static_cast<float>(px[1]) / 255.0f / 255.0f +
                                  static_cast<float>(px[2]) / 255.0f / 65025.0f +
                                  static_cast<float>(px[3]) / 255.0f / 16581375.0f;
            fboWrites = decoded > 0.1f && decoded < 0.99f;
            NEON_LOG_CAT(neon::core::LogCategory::Gfx, neon::core::LogLevel::Info,
                         "Renderer: CSM FBO write self-test: px=%d,%d,%d,%d decoded=%.3f -> %s",
                         px[0], px[1], px[2], px[3], decoded,
                         fboWrites ? "PASS" : "FAIL");
        }
    }
    if (!fboWrites) {
        NEON_LOG_CAT(
            neon::core::LogCategory::Gfx, neon::core::LogLevel::Warn,
            "Renderer: FBO DrawElements does not write -> CSM disabled, CPU projected shadows");
        return false;
    }

    // --- Part B: using an FBO must not corrupt later backbuffer VAO rendering
    // (the documented Intel FBO/VAO defect). Draw a reference red quad into the
    // backbuffer, exercise a 3-cascade pass, then redraw and confirm unchanged.
    auto drawRedQuad = [&]() {
        backend_->UseShader(unlitShader_);
        backend_->SetUniformMat4("uMVP", math::Mat4::Identity());
        backend_->SetUniformInt("uHasTexture", 0);
        backend_->SetUniformVec4("uTint", {1.0f, 0.0f, 0.0f, 1.0f});
        backend_->SetCullMode(CullMode::None);
        backend_->SetDepthTest(false, false);
        backend_->SetBlendMode(BlendMode::Opaque);
        backend_->DrawMesh(probeQuad_);
    };
    unsigned char refPx[4] = {0, 0, 0, 0};
    unsigned char postPx[4] = {0, 0, 0, 0};
    backend_->BindDefaultTarget();
    drawRedQuad();
    backend_->ReadCurrentTargetPixel(kSize, kSize, refPx);
    {
        RenderTargetHandle rt = backend_->CreateRenderTarget(kSize, kSize);
        if (rt.Valid()) {
            for (int c = 0; c < kShadowCascades; ++c) { // mimic the 3-cascade pass
                backend_->BindRenderTarget(rt);
                backend_->Clear({1.0f, 1.0f, 1.0f, 1.0f}, 1.0f);
                backend_->UseShader(depthShader_);
                backend_->SetUniformMat4("uMVP", math::Mat4::Identity());
                backend_->SetCullMode(CullMode::None);
                backend_->SetDepthTest(false, false);
                backend_->SetBlendMode(BlendMode::Opaque);
                backend_->DrawMesh(probeQuad_);
            }
            backend_->DestroyRenderTarget(rt);
        }
        backend_->BindDefaultTarget();
        drawRedQuad();
        backend_->ReadCurrentTargetPixel(kSize, kSize, postPx);
    }
    const bool backbufferIntact = refPx[0] > 200 && postPx[0] > 200 && postPx[0] >= refPx[0] - 32;
    NEON_LOG_CAT(neon::core::LogCategory::Gfx, neon::core::LogLevel::Info,
                 "Renderer: CSM backbuffer integrity after FBO: ref=%d,%d,%d post=%d,%d,%d -> %s",
                 refPx[0], refPx[1], refPx[2], postPx[0], postPx[1], postPx[2],
                 backbufferIntact ? "PASS" : "FAIL");
    if (!backbufferIntact) {
        NEON_LOG_CAT(
            neon::core::LogCategory::Gfx, neon::core::LogLevel::Warn,
            "Renderer: FBO usage corrupts backbuffer rendering -> CSM disabled, CPU projected shadows");
        return false;
    }
    NEON_LOG_CAT(neon::core::LogCategory::Gfx, neon::core::LogLevel::Info,
                 "Renderer: CSM shadow-map self-test PASS");
    return true;
}

} // namespace neon::gfx
