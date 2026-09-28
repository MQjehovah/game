#include "neon/gfx/shadow_system.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

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
uniform mat4 uBoneMatrices[128];
uniform mat4 uMVP;
void main() {
    mat4 skin = mat4(0.0);
    for (int i = 0; i < 4; ++i) {
        int id = int(aJointIds[i]);
        if (id >= 0 && id < 128) skin += aWeights[i] * uBoneMatrices[id];
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
uniform mat4 uBoneMatrices[128];
uniform mat4 uMVP;
uniform mat4 uModel;
out vec3 vWorldPos;
void main() {
    mat4 skin = mat4(0.0);
    for (int i = 0; i < 4; ++i) {
        int id = int(aJointIds[i]);
        if (id >= 0 && id < 128) skin += aWeights[i] * uBoneMatrices[id];
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
    if (!CreateCascadeTargets()) {
        csmEnabled_ = false;
        return;
    }
    csmEnabled_ = TestDepthTargetCapability();
    NEON_LOG_CAT(neon::core::LogCategory::Gfx, neon::core::LogLevel::Info,
                 "Renderer: CSM shadow maps %dx%d x3 (%s)", shadowSize_, shadowSize_,
                 csmEnabled_ ? "ok" : "FAILED -> CPU projected shadows fallback");

    // Point-light cubemap shadows reuse the same color-encoded-depth FBO path,
    // so they engage only when the CSM capability self-test passed. Six 2D
    // maps per light (layered cubemap FBOs are unreliable on the Intel driver);
    // the lit shader picks the face from the fragment->light direction.
    // Point-light cubemap shadows currently produce visible cube-face light
    // patches on the ground; keep point lights unshadowed until the face
    // projection is fixed. Directional/CSM shadows remain enabled.
    pointShadowsEnabled_ = false;
    NEON_LOG_CAT(neon::core::LogCategory::Gfx, neon::core::LogLevel::Info,
                 "Renderer: point light shadows disabled (ground patch workaround)");
    // Diagnostic override (not public API): isolate the point-light shadow
    // contribution for verification (screenshot diffs) without touching CSM.
    if (pointShadowsEnabled_ && std::getenv("NEON_NO_POINT_SHADOWS")) {
        NEON_LOG_CAT(neon::core::LogCategory::Gfx, neon::core::LogLevel::Warn,
                     "Renderer: point light shadows disabled by NEON_NO_POINT_SHADOWS");
        pointShadowsEnabled_ = false;
    }
}

bool ShadowSystem::CreateCascadeTargets() {
    if (backend_ == nullptr) return false;
    for (int i = 0; i < kShadowCascades; ++i) {
        if (shadowRT_[i].Valid()) backend_->DestroyRenderTarget(shadowRT_[i]);
        shadowRT_[i] = {};
        shadowDepthTex_[i] = {};
        // Prefer a depth-attached target so the pass can resolve occlusion with
        // a real depth TEST instead of painter's order (see the header note on
        // CreateRenderTargetWithDepth). Falls back to the plain colour target -
        // and painter's order - when the backend has no usable depth buffer or
        // the FBO comes back incomplete.
        const bool wantDepth = backend_->DepthAvailable();
        if (i == 0) depthTestedCascades_ = wantDepth;
        shadowRT_[i] = wantDepth
                           ? backend_->CreateRenderTargetWithDepth(shadowSize_, shadowSize_)
                           : backend_->CreateRenderTarget(shadowSize_, shadowSize_);
        if (!shadowRT_[i].Valid() && wantDepth) {
            depthTestedCascades_ = false;
            shadowRT_[i] = backend_->CreateRenderTarget(shadowSize_, shadowSize_);
        }
        shadowDepthTex_[i] = backend_->RenderTargetColorTexture(shadowRT_[i]);
        if (!shadowRT_[i].Valid() || !shadowDepthTex_[i].Valid()) {
            NEON_LOG_CAT(neon::core::LogCategory::Gfx, neon::core::LogLevel::Warn,
                         "Renderer: cascade %d shadow target failed", i);
            return false;
        }
    }
    NEON_LOG_CAT(neon::core::LogCategory::Gfx, neon::core::LogLevel::Info,
                 "Renderer: CSM cascades %s",
                 depthTestedCascades_ ? "depth-tested (nearest surface per texel)"
                                      : "painter's order (no usable depth buffer)");
    return true;
}

void ShadowSystem::SetShadowSize(int size) {
    if (size < 256) size = 256;
    if (size > 4096) size = 4096;
    if (size == shadowSize_) return;
    shadowSize_ = size;
    // Before Init (or with shadows forced off) there is nothing to recreate:
    // Init will allocate at the new size.
    if (backend_ == nullptr || shadowsForcedOff_) return;
    const bool ok = CreateCascadeTargets();
    csmEnabled_ = ok && TestDepthTargetCapability();
    NEON_LOG_CAT(neon::core::LogCategory::Gfx, neon::core::LogLevel::Info,
                 "Renderer: CSM shadow maps resized to %dx%d x3 (%s)", shadowSize_, shadowSize_,
                 csmEnabled_ ? "ok" : "FAILED");
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
    // Practical (PSSM) splits over the capped shadow range: near cascade gets a
    // small slice (high texel density where the player looks) and the far
    // cascade a wide one. The renderer fades shadows out at the last split.
    const float shadowFar = shadowDistance_ > 0.0f
                                ? std::fmin(shadowDistance_, camera.farPlane)
                                : camera.farPlane;
    ComputeCascadeSplitsPSSM(camera.nearPlane, shadowFar, cascadeLambda_, cascadeSplits_);
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
                                                        cascadeSplits_[i + 1], scenePtr,
                                                        shadowSize_, &cascadeTexelWorld_[i]);
    }

    for (int i = 0; i < kShadowCascades; ++i) {
        if (!shadowRT_[i].Valid()) continue;
        backend_->BindRenderTarget(shadowRT_[i]);
        // The map must be rasterized at ITS full size: hosts that render the
        // scene into a sub-viewport (the editor's dock rect) leave a viewport
        // AND a scissor rect active - without resetting both, every caster
        // was clipped to the dock-rect intersection, leaving a small stale
        // blob in the map and garbage view-dependent shadows.
        backend_->SetViewport(0, 0, shadowSize_, shadowSize_);
        backend_->SetScissor(0, 0, 0, 0, false);
        // Encoded far depth by default: anything not drawn is lit.
        backend_->Clear({1.0f, 1.0f, 1.0f, 1.0f}, 1.0f);
        backend_->SetBlendMode(BlendMode::Opaque);
        // Casters are rasterized with BOTH faces. A level exported as one
        // merged mesh can contain both windings (Summoner's Rift's terrain and
        // its props live in a single glTF node), and back-face culling silently
        // dropped the whole ground plane from the maps: nothing was left to
        // receive anything, so the ground read as fully lit no matter what the
        // sun did. Culling costs a little shadow-pass fill and removes an entire
        // class of content-authored bugs; the depth buffer keeps the nearest
        // surface per texel either way.
        backend_->SetCullMode(CullMode::None);
        // Depth-attached cascades resolve occlusion with GL_LESS (nearest
        // surface per texel), which is the only correct answer for geometry that
        // interpenetrates inside one merged mesh. Without a depth buffer (the
        // Intel FBO depth defect) fall back to painter's order, sorted far to
        // near in light space.
        backend_->SetDepthTest(depthTestedCascades_, depthTestedCascades_);
        DrawShadowCastersSorted(lightViewProj_[i]);
        // Diagnostic (NEON_DUMP_SHADOW=1): one-shot full-map dump per cascade
        // (RGBA8 packed depth, GL bottom-up rows) + the math the receivers
        // use, so a "shadow wrong" report can be reproduced offline.
        if (std::getenv("NEON_DUMP_SHADOW") != nullptr) {
            // Steady-state dump, and only of passes that actually consumed a
            // caster list (offscreen tool passes would pollute the counter).
            static int passCount[kShadowCascades] = {0, 0, 0};
            if (i < kShadowCascades && !shadowCasters_.empty() &&
                ++passCount[i] == 60) {
                std::vector<unsigned char> px(static_cast<size_t>(shadowSize_) *
                                               shadowSize_ * 4);
                backend_->ReadTargetPixelsRect(0, 0, shadowSize_, shadowSize_, px.data());
                char name[96];
                std::snprintf(name, sizeof(name), "_shadowdump_c%d_%d.bin", i, shadowSize_);
                if (std::FILE* f = std::fopen(name, "wb")) {
                    std::fwrite(px.data(), 1, px.size(), f);
                    std::fclose(f);
                }
                // Caster manifests (debug): world AABB per recorded caster.
                for (const ShadowDraw& d : shadowCasters_) {
                    math::AABB wb;
                    wb.min = {1e30f, 1e30f, 1e30f};
                    wb.max = {-1e30f, -1e30f, -1e30f};
                    if (!d.models.empty()) {
                        for (const math::Mat4& mm : d.models) {
                            wb.Expand(math::TransformAABB(d.bounds, mm).min);
                            wb.Expand(math::TransformAABB(d.bounds, mm).max);
                        }
                    } else {
                        wb.Expand(math::TransformAABB(d.bounds, d.model).min);
                        wb.Expand(math::TransformAABB(d.bounds, d.model).max);
                    }
                    NEON_LOG_CAT(neon::core::LogCategory::Gfx, neon::core::LogLevel::Info,
                                 "Renderer: shadowdump caster bounds=[%.1f,%.1f,%.1f]"
                                 "..[%.1f,%.1f,%.1f]",
                                 wb.min.x, wb.min.y, wb.min.z, wb.max.x, wb.max.y, wb.max.z);
                }
                const math::Mat4& m = lightViewProj_[i];
                NEON_LOG_CAT(neon::core::LogCategory::Gfx, neon::core::LogLevel::Info,
                             "Renderer: shadowdump c%d size=%d casters=%zu texelWorld=%.4f "
                             "zrow=(%.4f,%.4f,%.4f) ztrans=%.3f zRange=%.1f "
                             "splits=%.2f/%.2f/%.2f/%.2f",
                             i, shadowSize_, shadowCasters_.size(), cascadeTexelWorld_[i],
                             m.m[8], m.m[9], m.m[10], m.m[11],
                             2.0f / std::max(std::sqrt(m.m[8] * m.m[8] + m.m[9] * m.m[9] +
                                                       m.m[10] * m.m[10]), 1e-8f),
                             cascadeSplits_[0], cascadeSplits_[1], cascadeSplits_[2],
                             cascadeSplits_[3]);
            }
        }
        // Diagnostic (NEON_DUMP_SHADOW=1): histogram the cascade that was just
        // rendered so a "no shadows" report can distinguish an empty map (casters
        // missing / culled / mis-viewported) from a broken receiver projection.
        if (i == 0 && std::getenv("NEON_DUMP_SHADOW") != nullptr) {
            const int kGrid = 16;
            int hits = 0;
            float minD = 1e9f, maxD = -1e9f, sumD = 0.0f;
            for (int gy = 0; gy < kGrid; ++gy) {
                for (int gx = 0; gx < kGrid; ++gx) {
                    unsigned char px[4] = {0, 0, 0, 0};
                    backend_->ReadCurrentTargetPixel(gx * shadowSize_ / kGrid,
                                                     gy * shadowSize_ / kGrid, px);
                    const float d = static_cast<float>(px[0]) / 255.0f +
                                    static_cast<float>(px[1]) / 255.0f / 255.0f +
                                    static_cast<float>(px[2]) / 255.0f / 65025.0f;
                    if (d < 0.999f) ++hits;
                    minD = d < minD ? d : minD;
                    maxD = d > maxD ? d : maxD;
                    sumD += d;
                }
            }
            const int total = kGrid * kGrid;
            NEON_LOG_CAT(neon::core::LogCategory::Gfx, neon::core::LogLevel::Info,
                         "Renderer: cascade0 depth sample %d/%d texels < 1.0 "
                         "(min=%.4f max=%.4f mean=%.4f)",
                         hits, total, minD, maxD, sumD / static_cast<float>(total));
        }

    }
    // B1: the cascades re-rendered with new lightViewProj_ - the scene uniform
    // block must re-upload before the next lit draw samples them. SetCamera
    // already bumped on entry, so this only matters for a mid-frame
    // RefreshShadowPass (draws already made with the previous matrices).
    if (sceneUniformStamp_) ++*sceneUniformStamp_;
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
        // World-space size of this caster's AABB. The painter's-order pass keys
        // off it: see the sort below. World units (not light-space clip units)
        // because the cascades have different ortho scales, and the bucket has
        // to mean the same thing in every cascade.
        math::AABB worldBounds;
        worldBounds.min = {1e30f, 1e30f, 1e30f};
        worldBounds.max = {-1e30f, -1e30f, -1e30f};
        auto expandWorldAabb = [&](const math::Mat4& m) {
            worldBounds.Expand(math::TransformAABB(draw.bounds, m).min);
            worldBounds.Expand(math::TransformAABB(draw.bounds, m).max);
        };
        if (!draw.models.empty()) {
            for (const math::Mat4& m : draw.models) {
                center += m.TransformPoint(draw.bounds.Center());
                expandWorldAabb(m);
            }
            center = center * (1.0f / static_cast<float>(draw.models.size()));
        } else {
            center = draw.model.TransformPoint(draw.bounds.Center());
            expandWorldAabb(draw.model);
        }
        const math::Vec3 size = worldBounds.max - worldBounds.min;
        float extent = std::fmax(size.x, std::fmax(size.y, size.z));
        // Degenerate/garbage AABB (no valid bounds, e.g. a merged glTF chunk
        // mesh that never had one computed): class it as the LARGEST so it is
        // drawn first. An unbounded mesh is nearly always a big static receiver
        // (the arena floor); drawn last it would overwrite every caster's depth
        // and erase all shadows in the scene, which is exactly the failure the
        // buckets exist to prevent.
        if (!(extent > 0.0f) || extent > 1e6f) extent = 1e9f;
        shadowSortKeys_.push_back({&draw, lightView.TransformPoint(center).z, extent});
    }
    // NDC z grows as light-space z goes negative (ortho slope is negative), so
    // the farthest caster has the largest value; draw it first (last wins).
    //
    // Primary key is the light-space footprint, coarse-bucketed by the largest
    // power of two below the extent, biggest first. Painter's order has one key
    // per object, so a mesh that *contains* other casters must be drawn before
    // them or its depth overwrites theirs. The Summoner's Rift ground is a
    // single mesh spanning the whole map: sorted purely by distance it landed
    // after every unit and erased every unit shadow (measured: toggling CSM
    // changed 0.3/765 per pixel of the frame). Bucketing makes the big ground
    // and building plates land in an earlier class than the units on them,
    // while players/minions/projectiles (same class) still sort far -> near.
    auto extentBucket = [](float extent) {
        if (extent <= 1.0f) return 0.0f;
        if (extent >= 1e8f) return 1000.0f; // degenerate AABB -> drawn first
        return std::floor(std::log2(extent));
    };
    for (ShadowSortKey& k : shadowSortKeys_) k.extent = extentBucket(k.extent);
    std::sort(shadowSortKeys_.begin(), shadowSortKeys_.end(),
              [](const ShadowSortKey& a, const ShadowSortKey& b) {
                  if (a.extent != b.extent) return a.extent > b.extent;
                  return a.z > b.z;
              });
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
        // Same sub-viewport/scissor hazard as the cascades (see RunPass).
        backend_->SetViewport(0, 0, kPointShadowSize, kPointShadowSize);
        backend_->SetScissor(0, 0, 0, 0, false);
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
