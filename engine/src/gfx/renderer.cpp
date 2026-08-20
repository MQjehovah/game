#include "neon/gfx/renderer.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>

#include "neon/core/log.hpp"

namespace neon::gfx {
namespace {

constexpr uint32_t kMaxQuads = 4096;
constexpr uint32_t kMaxUIVertices = kMaxQuads * 4;

const char* kLitVertexShader = R"(
#version 330 core
layout(location = 0) in vec3 aPos;
layout(location = 1) in vec3 aNormal;
layout(location = 2) in vec2 aUV;
layout(location = 3) in vec4 aColor;
uniform mat4 uMVP;
uniform mat4 uModel;
uniform mat4 uNormalMat;
uniform vec3 uCamPos;
out vec3 vWorldPos;
out vec3 vNormal;
out vec2 vUV;
out vec4 vColor;
void main() {
    vWorldPos = (uModel * vec4(aPos, 1.0)).xyz;
    vNormal = (uNormalMat * vec4(aNormal, 0.0)).xyz;
    vUV = aUV;
    vColor = aColor;
    gl_Position = uMVP * vec4(aPos, 1.0);
}
)";

const char* kLitFragmentShader = R"(
#version 330 core
in vec3 vWorldPos;
in vec3 vNormal;
in vec2 vUV;
in vec4 vColor;
out vec4 FragColor;
uniform sampler2D uAlbedo;
uniform sampler2D uMR;
uniform sampler2D uOcclusion;
uniform sampler2D uEmissive;
uniform vec4 uTint;
uniform bool uHasTexture;
uniform bool uHasMR;
uniform bool uHasAO;
uniform bool uHasEmissive;
uniform float uShininess;
uniform float uMetallic;
uniform float uRoughness;
uniform vec3 uSunDir;
uniform vec3 uSunColor;
uniform float uAmbient;
uniform vec3 uPointPos[8];
uniform vec3 uPointColor[8];
uniform float uPointRadius[8];
uniform int uPointCount;
uniform vec3 uPlayerLightPos;
uniform vec3 uPlayerLightColor;
uniform float uPlayerLightRadius;
uniform bool uPlayerLightEnabled;
uniform vec3 uFogColor;
uniform float uFogStart;
uniform float uFogEnd;
uniform vec3 uCamPos;
uniform sampler2D uShadowMap;
uniform mat4 uShadowVP;
uniform vec2 uShadowTexel;
uniform bool uShadowEnabled;
float DecodeDepth(vec4 v) {
    return dot(v, vec4(1.0, 1.0 / 255.0, 1.0 / 65025.0, 1.0 / 16581375.0));
}
float D_GGX(float ndh, float a) {
    float a2 = a * a;
    float d = ndh * ndh * (a2 - 1.0) + 1.0;
    return a2 / (3.14159265 * d * d);
}
float G_Schlick(float ndl, float ndv, float a) {
    float k = a * a * 0.5;
    return (ndl / (ndl * (1.0 - k) + k)) * (ndv / (ndv * (1.0 - k) + k));
}
vec3 F_Schlick(float vdh, vec3 f0) {
    return f0 + (1.0 - f0) * pow(1.0 - vdh, 5.0);
}
void main() {
    vec4 albedo = uHasTexture ? texture(uAlbedo, vUV) : vec4(1.0);
    albedo *= uTint * vColor;
    vec3 N = normalize(vNormal);
    vec3 V = normalize(uCamPos - vWorldPos);
    vec3 L = normalize(-uSunDir);
    float ndl = max(dot(N, L), 0.0);
    vec3 H = normalize(L + V);
    float metallic = uHasMR ? texture(uMR, vUV).b : uMetallic;
    float roughness = uHasMR ? texture(uMR, vUV).g : uRoughness;
    roughness = clamp(roughness, 0.045, 1.0);
    float a = roughness * roughness;
    float ndv = max(dot(N, V), 1e-4);
    float ndh = max(dot(N, H), 0.0);
    float vdh = max(dot(V, H), 0.0);
    vec3 f0 = mix(vec3(0.04), albedo.rgb, metallic);
    float D = D_GGX(ndh, a);
    float G = G_Schlick(ndl, ndv, a);
    vec3 F = F_Schlick(vdh, f0);
    vec3 spec = D * G * F / (4.0 * ndl * ndv + 1e-3);
    vec3 kd = (1.0 - F) * (1.0 - metallic);
    vec3 ambientLight = albedo.rgb * uAmbient;
    if (uHasAO) ambientLight *= texture(uOcclusion, vUV).r;
    vec3 color = (kd * albedo.rgb + spec) * uSunColor * ndl + ambientLight;
    if (uHasEmissive) color += texture(uEmissive, vUV).rgb;
    for (int i = 0; i < 8; ++i) {
        if (i >= uPointCount) break;
        vec3 toL = uPointPos[i] - vWorldPos;
        float d = length(toL);
        float atten = clamp(1.0 - d / uPointRadius[i], 0.0, 1.0);
        atten *= atten;
        vec3 pl = toL / max(d, 1e-4);
        float pndl = max(dot(N, pl), 0.0);
        vec3 ph = normalize(pl + V);
        float pndh = max(dot(N, ph), 0.0);
        float pvdh = max(dot(V, ph), 0.0);
        float pD = D_GGX(pndh, a);
        float pG = G_Schlick(pndl, ndv, a);
        vec3 pF = F_Schlick(pvdh, f0);
        vec3 pSpec = pD * pG * pF / (4.0 * pndl * ndv + 1e-3);
        vec3 pKd = (1.0 - pF) * (1.0 - metallic);
        color += (pKd * albedo.rgb + pSpec) * uPointColor[i] * pndl * atten;
    }
    if (uPlayerLightEnabled) {
        vec3 toL = uPlayerLightPos - vWorldPos;
        float d = length(toL);
        float atten = clamp(1.0 - d / uPlayerLightRadius, 0.0, 1.0);
        atten *= atten;
        vec3 pl = toL / max(d, 1e-4);
        float pndl = max(dot(N, pl), 0.0);
        color += albedo.rgb * uPlayerLightColor * pndl * atten;
    }
    float dist = length(vWorldPos - uCamPos);
    float fog = smoothstep(uFogStart, uFogEnd, dist);
    color = mix(color, uFogColor, fog);

    float shadow = 1.0;
    if (uShadowEnabled) {
        vec4 sp = uShadowVP * vec4(vWorldPos, 1.0);
        vec3 ndc = sp.xyz / sp.w;
        if (ndc.z > 0.0 && ndc.z < 1.0) {
            vec3 sc = ndc * 0.5 + 0.5;
            float bias = 0.003;
            float s = 0.0;
            for (int x = -1; x <= 1; ++x) {
                for (int y = -1; y <= 1; ++y) {
                    float lit =
                        DecodeDepth(texture(uShadowMap, sc.xy + vec2(float(x), float(y)) * uShadowTexel)) >
                                sc.z - bias
                            ? 1.0
                            : 0.0;
                    s += lit;
                }
            }
            shadow = s / 9.0;
        }
    }
    color *= shadow;
    FragColor = vec4(color, albedo.a);
}
)";

const char* kUnlitVertexShader = R"(
#version 330 core
layout(location = 0) in vec3 aPos;
layout(location = 2) in vec2 aUV;
layout(location = 3) in vec4 aColor;
uniform mat4 uMVP;
out vec2 vUV;
out vec4 vColor;
void main() {
    vUV = aUV;
    vColor = aColor;
    gl_Position = uMVP * vec4(aPos, 1.0);
}
)";

const char* kUnlitFragmentShader = R"(
#version 330 core
in vec2 vUV;
in vec4 vColor;
out vec4 FragColor;
uniform sampler2D uAlbedo;
uniform vec4 uTint;
uniform bool uHasTexture;
void main() {
    vec4 albedo = uHasTexture ? texture(uAlbedo, vUV) : vec4(1.0);
    FragColor = albedo * uTint * vColor;
}
)";

const char* kLitInstancedVertexShader = R"(
#version 330 core
layout(location = 0) in vec3 aPos;
layout(location = 1) in vec3 aNormal;
layout(location = 2) in vec2 aUV;
layout(location = 3) in vec4 aColor;
layout(location = 4) in mat4 aInstance;
uniform mat4 uMVP;
uniform vec3 uCamPos;
out vec3 vWorldPos;
out vec3 vNormal;
out vec2 vUV;
out vec4 vColor;
void main() {
    vWorldPos = (aInstance * vec4(aPos, 1.0)).xyz;
    vNormal = mat3(aInstance) * aNormal;
    vUV = aUV;
    vColor = aColor;
    gl_Position = uMVP * aInstance * vec4(aPos, 1.0);
}
)";

const char* kUnlitInstancedVertexShader = R"(
#version 330 core
layout(location = 0) in vec3 aPos;
layout(location = 2) in vec2 aUV;
layout(location = 3) in vec4 aColor;
layout(location = 4) in mat4 aInstance;
uniform mat4 uMVP;
out vec2 vUV;
out vec4 vColor;
void main() {
    vUV = aUV;
    vColor = aColor;
    gl_Position = uMVP * aInstance * vec4(aPos, 1.0);
}
)";

const char* kShadowVertexShader = R"(
#version 330 core
layout(location = 0) in vec3 aPos;
uniform mat4 uMVP;
void main() {
    gl_Position = uMVP * vec4(aPos, 1.0);
}
)";

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

const char* kUIVertexShader = R"(
#version 330 core
layout(location = 0) in vec2 aPos;
layout(location = 1) in vec2 aUV;
layout(location = 2) in vec4 aColor;
uniform mat4 uMVP;
out vec2 vUV;
out vec4 vColor;
void main() {
    vUV = aUV;
    vColor = aColor;
    gl_Position = uMVP * vec4(aPos, 0.0, 1.0);
}
)";

const char* kUIFragmentShader = R"(
#version 330 core
in vec2 vUV;
in vec4 vColor;
out vec4 FragColor;
uniform sampler2D uTex;
void main() {
    FragColor = vColor * texture(uTex, vUV);
}
)";

const char* kLineVertexShader = R"(
#version 330 core
layout(location = 0) in vec3 aPos;
layout(location = 1) in vec4 aColor;
uniform mat4 uMVP;
out vec4 vColor;
void main() {
    vColor = aColor;
    gl_Position = uMVP * vec4(aPos, 1.0);
}
)";

const char* kLineFragmentShader = R"(
#version 330 core
in vec4 vColor;
out vec4 FragColor;
void main() {
    FragColor = vColor;
}
)";

// Inverse-transpose of the upper 3x3 of a model matrix (normal matrix).
math::Mat4 NormalMatrix(const math::Mat4& m) {
    float a00 = m.m[0], a01 = m.m[1], a02 = m.m[2];
    float a10 = m.m[4], a11 = m.m[5], a12 = m.m[6];
    float a20 = m.m[8], a21 = m.m[9], a22 = m.m[10];
    float det = a00 * (a11 * a22 - a12 * a21) - a01 * (a10 * a22 - a12 * a20) +
                a02 * (a10 * a21 - a11 * a20);
    math::Mat4 r;
    if (std::fabs(det) < 1e-8f) return r;
    float invDet = 1.0f / det;
    r.m[0] = (a11 * a22 - a12 * a21) * invDet;
    r.m[1] = (a02 * a21 - a01 * a22) * invDet;
    r.m[2] = (a01 * a12 - a02 * a11) * invDet;
    r.m[4] = (a12 * a20 - a10 * a22) * invDet;
    r.m[5] = (a00 * a22 - a02 * a20) * invDet;
    r.m[6] = (a02 * a10 - a00 * a12) * invDet;
    r.m[8] = (a10 * a21 - a11 * a20) * invDet;
    r.m[9] = (a01 * a20 - a00 * a21) * invDet;
    r.m[10] = (a00 * a11 - a01 * a10) * invDet;
    return r;
}

} // namespace

Renderer::~Renderer() { Shutdown(); }

bool Renderer::Init(platform::IWindow* window) {
    window_ = window;
    backend_ = CreateOpenGLBackend();
    if (!backend_ || !backend_->Init(window)) {
        NEON_LOG_ERROR("Renderer: OpenGL backend initialization failed");
        return false;
    }
    InitBuiltinResources();

    uiIndices_.reserve(kMaxQuads * 6);
    uiVerts_.reserve(kMaxUIVertices);
    screenW_ = window_->Width();
    screenH_ = window_->Height();
    depthAvailable_ = backend_->DepthAvailable();
    return true;
}

void Renderer::AttachBackendForTesting(std::unique_ptr<IRenderBackend> backend) {
    backend_ = std::move(backend);
}

void Renderer::Shutdown() {
    if (!backend_) return;
    if (litShader_.Valid()) backend_->DestroyShader(litShader_);
    if (unlitShader_.Valid()) backend_->DestroyShader(unlitShader_);
    if (uiShader_.Valid()) backend_->DestroyShader(uiShader_);
    if (linesShader_.Valid()) backend_->DestroyShader(linesShader_);
    if (litInstancedShader_.Valid()) backend_->DestroyShader(litInstancedShader_);
    if (unlitInstancedShader_.Valid()) backend_->DestroyShader(unlitInstancedShader_);
    if (depthShader_.Valid()) backend_->DestroyShader(depthShader_);
    if (shadowRT_.Valid()) backend_->DestroyRenderTarget(shadowRT_);
    if (white_.Valid()) backend_->DestroyTexture(white_);
    backend_->Shutdown();
    backend_.reset();
}

void Renderer::InitBuiltinResources() {
    unsigned char whitePx[4] = {255, 255, 255, 255};
    TextureDesc whiteDesc;
    whiteDesc.width = 1;
    whiteDesc.height = 1;
    whiteDesc.rgba = whitePx;
    white_ = backend_->CreateTexture(whiteDesc);

    litShader_ = backend_->CreateShader(kLitVertexShader, kLitFragmentShader, "lit");
    unlitShader_ = backend_->CreateShader(kUnlitVertexShader, kUnlitFragmentShader, "unlit");
    uiShader_ = backend_->CreateShader(kUIVertexShader, kUIFragmentShader, "ui");
    linesShader_ = backend_->CreateShader(kLineVertexShader, kLineFragmentShader, "lines");
    litInstancedShader_ =
        backend_->CreateShader(kLitInstancedVertexShader, kLitFragmentShader, "lit_instanced");
    unlitInstancedShader_ =
        backend_->CreateShader(kUnlitInstancedVertexShader, kUnlitFragmentShader, "unlit_instanced");
    depthShader_ = backend_->CreateShader(kShadowVertexShader, kShadowFragmentShader, "shadow");
    shadowRT_ = backend_->CreateRenderTarget(shadowSize_, shadowSize_);
    shadowColorTex_ = backend_->RenderTargetColorTexture(shadowRT_);
    NEON_LOG_INFO("Renderer: shadow map %dx%d (%s)", shadowSize_, shadowSize_,
                  shadowRT_.Valid() ? "ok" : "FAILED");
}

void Renderer::BeginFrame(const Color& clearColor, float clearDepth) {
    stats_ = RenderStats{};
    shadowEnabled_ = false;
    screenW_ = window_ ? window_->Width() : screenW_;
    screenH_ = window_ ? window_->Height() : screenH_;
    uiScale_ = static_cast<float>(screenH_) / static_cast<float>(kDesignHeight);
    uiOffsetX_ = (static_cast<float>(screenW_) - static_cast<float>(kDesignWidth) * uiScale_) * 0.5f;
    backend_->SetViewport(screenW_, screenH_);
    backend_->Clear(clearColor, clearDepth);
}

void Renderer::EndFrame() {
    Flush2D();
    backend_->EndFrame();
}

void Renderer::SetCamera(const Camera& camera, float aspect) {
    camera_ = camera;
    viewProj_ = camera.ViewProjection(aspect);
    camPos_ = camera.position;
    frustum_ = math::Frustum::FromViewProjection(viewProj_);
    frustumValid_ = true;
}

void Renderer::SetSky(const Color& top, const Color& horizon) {
    skyTop_ = top;
    skyHorizon_ = horizon;
}

void Renderer::SetFog(const Color& color, float start, float end) {
    fogColor_ = color;
    fogStart_ = start;
    fogEnd_ = end;
}

void Renderer::SetDirectionalLight(const math::Vec3& direction, const Color& color, float ambientStrength) {
    sunDir_ = direction.Normalized();
    sunColor_ = color;
    ambient_ = ambientStrength;
}

void Renderer::BeginShadowPass(const math::Vec3& lightDir, const math::Vec3& center,
                               float orthoSize) {
    if (!shadowRT_.Valid()) return;
    math::Vec3 lightPos = center + lightDir * orthoSize;
    gfx::Camera lightCam;
    lightCam.position = lightPos;
    lightCam.target = center;
    lightCam.up = {0, 1, 0};
    math::Mat4 view = lightCam.View();
    math::Mat4 proj = math::Mat4::Ortho(-orthoSize, orthoSize, -orthoSize, orthoSize, 0.1f,
                                        orthoSize * 2.5f);
    shadowVP_ = proj * view;

    Flush2D();
    backend_->BindRenderTarget(shadowRT_);
    // Encode(1.0) = far depth: everything is lit by default.
    backend_->Clear({1.0f, 1.0f, 1.0f, 1.0f}, 1.0f);
    backend_->UseShader(depthShader_);
    backend_->SetCullMode(CullMode::Back);
    backend_->SetDepthTest(false, false); // depth buffer is broken on some drivers
    backend_->SetBlendMode(BlendMode::Opaque);
}

void Renderer::DrawShadow(const Mesh& mesh, const math::Mat4& model) {
    if (!shadowRT_.Valid() || !mesh.Valid()) return;
    backend_->SetUniformMat4("uMVP", shadowVP_ * model);
    backend_->DrawMesh(mesh.Handle());
}

void Renderer::EndShadowPass() {
    if (!shadowRT_.Valid()) return;
    backend_->BindDefaultTarget();
    shadowEnabled_ = true;
}

void Renderer::SetPointLight(int index, const math::Vec3& position, const Color& color, float radius) {
    if (index < 0 || index >= kMaxPointLights) return;
    pointPos_[index] = position;
    pointColor_[index] = color;
    pointRadius_[index] = radius;
    pointCount_ = std::max(pointCount_, index + 1);
}

void Renderer::SetPlayerLight(const math::Vec3& position, const Color& color, float radius) {
    playerLightPos_ = position;
    playerLightColor_ = color;
    playerLightRadius_ = radius;
    playerLightEnabled_ = true;
}

void Renderer::DrawSky() {
    // Full-screen gradient in screen pixels (depth already cleared).
    if (uiVerts_.size() >= kMaxUIVertices) Flush2D();
    auto push = [&](float x, float y, const Color& c) {
        UIVertex v;
        v.x = x;
        v.y = y;
        v.u = 0.0f;
        v.v = 0.0f;
        v.r = c.r;
        v.g = c.g;
        v.b = c.b;
        v.a = 1.0f;
        uiVerts_.push_back(v);
    };
    float w = static_cast<float>(screenW_);
    float h = static_cast<float>(screenH_);
    push(0, 0, skyTop_);
    push(w, 0, skyTop_);
    push(w, h, skyHorizon_);
    push(0, h, skyHorizon_);
    uint16_t base = static_cast<uint16_t>(uiVerts_.size() - 4);
    uiIndices_.push_back(base + 0);
    uiIndices_.push_back(base + 1);
    uiIndices_.push_back(base + 2);
    uiIndices_.push_back(base + 2);
    uiIndices_.push_back(base + 3);
    uiIndices_.push_back(base + 0);
    Flush2D();
}

void Renderer::DrawMesh(const Mesh& mesh, const Material& material, const math::Mat4& model) {
    if (!mesh.Valid()) return;
    Flush2D();

    if (frustumValid_ && !frustum_.Intersects(math::TransformAABB(mesh.Bounds(), model))) return;

    ShaderHandle shader = material.shader.Valid() ? material.shader
                                                  : (material.lit ? litShader_ : unlitShader_);
    ApplyMaterial(material, viewProj_ * model, model, NormalMatrix(model), shader);
    backend_->DrawMesh(mesh.Handle());
    ++stats_.drawCalls;
    stats_.triangles += mesh.TriangleCount();
}

void Renderer::DrawMeshInstanced(const Mesh& mesh, const Material& material,
                                 const math::Mat4* models, uint32_t count, bool frustumCull) {
    if (!mesh.Valid() || !models || count == 0) return;
    Flush2D();

    std::vector<math::Mat4> visible;
    visible.reserve(count);
    const math::AABB& bounds = mesh.Bounds();
    for (uint32_t i = 0; i < count; ++i) {
        if (frustumCull && frustumValid_ &&
            !frustum_.Intersects(math::TransformAABB(bounds, models[i]))) {
            continue;
        }
        visible.push_back(models[i]);
    }
    if (visible.empty()) return;

    ShaderHandle shader = material.shader.Valid()
                              ? material.shader
                              : (material.lit ? litInstancedShader_ : unlitInstancedShader_);
    ApplyMaterial(material, viewProj_, math::Mat4::Identity(), math::Mat4::Identity(), shader);
    backend_->DrawMeshInstanced(mesh.Handle(), visible.data(),
                                static_cast<uint32_t>(visible.size()));
    ++stats_.drawCalls;
    stats_.instances += static_cast<uint32_t>(visible.size());
    stats_.triangles += mesh.TriangleCount() * static_cast<uint32_t>(visible.size());
}

void Renderer::DrawProjectedShadow(const Mesh& mesh, const math::Mat4& model,
                                   const math::Vec3& lightDir, const Color& color) {
    if (!mesh.Valid()) return;
    const std::vector<Vertex3D>& verts = mesh.CpuVerts();
    const std::vector<uint16_t>& indices = mesh.CpuIndices();
    if (verts.empty() || indices.size() < 3 || std::fabs(lightDir.y) < 1e-4f) return;

    std::vector<LineVertex> projected;
    projected.reserve(indices.size());
    for (size_t i = 0; i + 2 < indices.size(); i += 3) {
        math::Vec3 w0 = model.TransformPoint(verts[indices[i]].pos);
        math::Vec3 w1 = model.TransformPoint(verts[indices[i + 1]].pos);
        math::Vec3 w2 = model.TransformPoint(verts[indices[i + 2]].pos);
        if (w0.y < 0.02f && w1.y < 0.02f && w2.y < 0.02f) continue; // below ground
        auto projectToGround = [&](const math::Vec3& p) {
            float t = -p.y / lightDir.y;
            return p + lightDir * t;
        };
        math::Vec3 p0 = projectToGround(w0);
        math::Vec3 p1 = projectToGround(w1);
        math::Vec3 p2 = projectToGround(w2);
        projected.push_back({p0, color});
        projected.push_back({p1, color});
        projected.push_back({p2, color});
    }
    if (projected.empty()) return;

    Flush2D();
    backend_->SetBlendMode(BlendMode::Alpha);
    backend_->SetDepthTest(false, false);
    backend_->SetCullMode(CullMode::None);
    backend_->UseShader(linesShader_);
    backend_->SetUniformMat4("uMVP", viewProj_);
    backend_->DrawPrimitives(projected.data(), static_cast<uint32_t>(projected.size()), 28, nullptr,
                             0, PrimitiveTopology::Triangles);
}

void Renderer::ApplyMaterial(const Material& material, const math::Mat4& mvp,
                             const math::Mat4& model, const math::Mat4& normalMat,
                             ShaderHandle shader) {
    backend_->UseShader(shader);
    backend_->SetCullMode(CullMode::Back);
    backend_->SetDepthTest(depthAvailable_ ? !material.transparent : false,
                           !material.transparent);
    backend_->SetBlendMode(material.transparent ? BlendMode::Alpha : BlendMode::Opaque);

    backend_->SetUniformMat4("uMVP", mvp);
    backend_->BindTexture(0, material.albedo.Valid() ? material.albedo : white_);
    backend_->SetUniformInt("uAlbedo", 0);
        backend_->SetUniformInt("uHasTexture", material.albedo.Valid() ? 1 : 0);
        backend_->SetUniformInt("uHasMR", material.metallicRoughness.Valid() ? 1 : 0);
        backend_->SetUniformInt("uHasAO", material.occlusion.Valid() ? 1 : 0);
        backend_->SetUniformInt("uHasEmissive", material.emissive.Valid() ? 1 : 0);
        backend_->SetUniformVec4("uTint", {material.tint.r, material.tint.g, material.tint.b, material.tint.a});
        backend_->SetUniformFloat("uMetallic", material.metallic);
        backend_->SetUniformFloat("uRoughness", material.roughness);
        backend_->BindTexture(2, material.metallicRoughness);
        backend_->SetUniformInt("uMR", 2);
        backend_->BindTexture(3, material.occlusion);
        backend_->SetUniformInt("uOcclusion", 3);
        backend_->BindTexture(4, material.emissive);
        backend_->SetUniformInt("uEmissive", 4);

    if (material.lit) {
        backend_->SetUniformMat4("uModel", model);
        backend_->SetUniformMat4("uNormalMat", normalMat);
        backend_->SetUniformVec3("uCamPos", camPos_);
        backend_->SetUniformFloat("uShininess", material.shininess);
        backend_->SetUniformVec3("uSunDir", sunDir_);
        backend_->SetUniformVec3("uSunColor", {sunColor_.r, sunColor_.g, sunColor_.b});
        backend_->SetUniformFloat("uAmbient", ambient_);
        backend_->SetUniformInt("uPointCount", pointCount_);
        for (int i = 0; i < pointCount_; ++i) {
            std::string suffix = "[" + std::to_string(i) + "]";
            backend_->SetUniformVec3(("uPointPos" + suffix).c_str(), pointPos_[i]);
            backend_->SetUniformVec3(("uPointColor" + suffix).c_str(),
                                     {pointColor_[i].r, pointColor_[i].g, pointColor_[i].b});
            backend_->SetUniformFloat(("uPointRadius" + suffix).c_str(), pointRadius_[i]);
        }
        backend_->SetUniformVec3("uPlayerLightPos", playerLightPos_);
        backend_->SetUniformVec3("uPlayerLightColor",
                                 {playerLightColor_.r, playerLightColor_.g, playerLightColor_.b});
        backend_->SetUniformFloat("uPlayerLightRadius", playerLightRadius_);
        backend_->SetUniformInt("uPlayerLightEnabled", playerLightEnabled_ ? 1 : 0);
        backend_->SetUniformVec3("uFogColor", {fogColor_.r, fogColor_.g, fogColor_.b});
        backend_->SetUniformFloat("uFogStart", fogStart_);
        backend_->SetUniformFloat("uFogEnd", fogEnd_);
        backend_->SetUniformMat4("uShadowVP", shadowVP_);
        backend_->SetUniformInt("uShadowMap", 1);
        backend_->SetUniformVec2("uShadowTexel",
                                 {1.0f / static_cast<float>(shadowSize_),
                                  1.0f / static_cast<float>(shadowSize_)});
        backend_->SetUniformInt("uShadowEnabled", shadowEnabled_ ? 1 : 0);
        backend_->BindTexture(1, shadowColorTex_);
        static int debugOnce = 0;
        if (debugOnce < 3) {
            NEON_LOG_INFO("SHADOW-UNIFORM: enabled=%d tex=%u vp.m0=%.2f", shadowEnabled_ ? 1 : 0,
                          shadowColorTex_.id, shadowVP_.m[0]);
            ++debugOnce;
        }
    }
}

void Renderer::DrawLines(const LineVertex* vertices, uint32_t count, const math::Mat4& model) {
    if (!vertices || count == 0) return;
    Flush2D();
    backend_->SetBlendMode(BlendMode::Alpha);
    backend_->SetDepthTest(depthAvailable_, false);
    backend_->SetCullMode(CullMode::None);
    backend_->UseShader(linesShader_);
    backend_->SetUniformMat4("uMVP", viewProj_ * model);
    backend_->DrawPrimitives(vertices, count, 28, nullptr, 0, PrimitiveTopology::Lines);
}

void Renderer::DrawBox(const math::AABB& box, const Color& color) {
    math::Vec3 c[8] = {
        {box.min.x, box.min.y, box.min.z}, {box.max.x, box.min.y, box.min.z},
        {box.max.x, box.max.y, box.min.z}, {box.min.x, box.max.y, box.min.z},
        {box.min.x, box.min.y, box.max.z}, {box.max.x, box.min.y, box.max.z},
        {box.max.x, box.max.y, box.max.z}, {box.min.x, box.max.y, box.max.z}};
    const uint8_t edges[12][2] = {
        {0, 1}, {1, 2}, {2, 3}, {3, 0},
        {4, 5}, {5, 6}, {6, 7}, {7, 4},
        {0, 4}, {1, 5}, {2, 6}, {3, 7}};
    LineVertex verts[24];
    for (int i = 0; i < 12; ++i) {
        verts[i * 2] = {c[edges[i][0]], color};
        verts[i * 2 + 1] = {c[edges[i][1]], color};
    }
    DrawLines(verts, 24, math::Mat4::Identity());
}

void Renderer::DrawSphere(const math::Vec3& center, float radius, const Color& color, int segments) {
    std::vector<LineVertex> verts;
    auto ring = [&](const math::Vec3& axisA, const math::Vec3& axisB) {
        for (int i = 0; i < segments; ++i) {
            float a0 = static_cast<float>(i) / segments * math::kTwoPi;
            float a1 = static_cast<float>(i + 1) / segments * math::kTwoPi;
            math::Vec3 p0 = center + (axisA * std::cos(a0) + axisB * std::sin(a0)) * radius;
            math::Vec3 p1 = center + (axisA * std::cos(a1) + axisB * std::sin(a1)) * radius;
            verts.push_back({p0, color});
            verts.push_back({p1, color});
        }
    };
    ring({1, 0, 0}, {0, 1, 0});
    ring({1, 0, 0}, {0, 0, 1});
    ring({0, 1, 0}, {0, 0, 1});
    DrawLines(verts.data(), static_cast<uint32_t>(verts.size()), math::Mat4::Identity());
}

Texture Renderer::CreateTexture(const TextureDesc& desc) {
    TextureHandle handle = backend_->CreateTexture(desc);
    return Texture(handle, desc.width, desc.height);
}

Shader Renderer::CreateShader(const char* vertexSource, const char* fragmentSource, const char* name) {
    return Shader(backend_->CreateShader(vertexSource, fragmentSource, name), name);
}

void Renderer::DrawQuad(const math::Vec2& pos, const math::Vec2& size, const Color& color,
                        TextureHandle texture, const math::Vec2& uv0, const math::Vec2& uv1,
                        BlendMode blend) {
    PushQuad(pos, {pos.x + size.x, pos.y}, pos + size, {pos.x, pos.y + size.y},
             color, uv0, uv1, texture, blend);
}

void Renderer::DrawRect(const math::Vec2& pos, const math::Vec2& size, const Color& color) {
    DrawQuad(pos, size, color, {}, {0, 0}, {1, 1}, BlendMode::Alpha);
}

void Renderer::DrawRectOutline(const math::Rect2& rect, float thickness, const Color& color) {
    DrawRect({rect.x, rect.y}, {rect.w, thickness}, color);
    DrawRect({rect.x, rect.y + rect.h - thickness}, {rect.w, thickness}, color);
    DrawRect({rect.x, rect.y}, {thickness, rect.h}, color);
    DrawRect({rect.x + rect.w - thickness, rect.y}, {thickness, rect.h}, color);
}

void Renderer::DrawText(const Font& font, const std::string& text, const math::Vec2& pos, float size,
                        const Color& color, bool centerX, bool centerY) {
    if (!font.Valid() || text.empty()) return;
    math::Vec2 p = pos;
    if (centerX || centerY) {
        math::Vec2 m = font.Measure(text, size);
        if (centerX) p.x -= m.x * 0.5f;
        if (centerY) p.y -= m.y * 0.5f;
    }
    float scale = size / static_cast<float>(font.bakedSize_);
    float cursorX = 0.0f;
    float cursorY = 0.0f;
    const char* it = text.data();
    const char* end = it + text.size();
    while (it < end) {
        int32_t cp = DecodeUTF8Next(it, end);
        if (cp == 0) continue;
        if (cp == '\n') {
            cursorX = 0.0f;
            cursorY += font.LineHeight(size);
            continue;
        }
        const Font::Glyph* g = font.FindGlyph(cp);
        if (!g) continue;
        math::Vec2 a{p.x + cursorX + g->xoff * scale, p.y + cursorY + g->yoff * scale};
        math::Vec2 b{p.x + cursorX + g->xoff2 * scale, p.y + cursorY + g->yoff2 * scale};
        PushQuad(a, {b.x, a.y}, b, {a.x, b.y}, color, {g->u0, g->v0}, {g->u1, g->v1},
                 font.Atlas(), BlendMode::Alpha);
        cursorX += g->advance * scale;
    }
}

void Renderer::DrawBillboard(const math::Vec3& worldPos, float size, const Color& color,
                             TextureHandle texture, BlendMode blend) {
    math::Vec4 clip = viewProj_.TransformVec4(math::Vec4(worldPos.x, worldPos.y, worldPos.z, 1.0f));
    if (clip.w <= 0.1f) return;
    float ndcX = clip.x / clip.w;
    float ndcY = clip.y / clip.w;
    float px = (ndcX * 0.5f + 0.5f) * static_cast<float>(screenW_);
    float py = (0.5f - ndcY * 0.5f) * static_cast<float>(screenH_);
    float pixelSize = size * static_cast<float>(screenH_) * 0.5f /
                      (std::tan(camera_.fovY * 0.5f) * clip.w);
    math::Vec2 design = ScreenToUI({px, py});
    float designSize = pixelSize / uiScale_;
    DrawQuad(design - math::Vec2{designSize * 0.5f, designSize * 0.5f},
             {designSize, designSize}, color, texture, {0, 1}, {1, 0}, blend);
}

math::Vec2 Renderer::ScreenToUI(const math::Vec2& screenPixels) const {
    return {(screenPixels.x - uiOffsetX_) / uiScale_, screenPixels.y / uiScale_};
}

bool Renderer::CaptureFrame(std::vector<uint8_t>& out) {
    if (!backend_) return false;
    out.resize(static_cast<size_t>(screenW_) * screenH_ * 4);
    backend_->CaptureFrame(screenW_, screenH_, out.data());
    return true;
}

math::Vec2 Renderer::ToScreen(const math::Vec2& design) const {
    return {design.x * uiScale_ + uiOffsetX_, design.y * uiScale_};
}

void Renderer::PushQuad(const math::Vec2& a, const math::Vec2& b, const math::Vec2& c,
                        const math::Vec2& d, const Color& color, const math::Vec2& uv0,
                        const math::Vec2& uv1, TextureHandle texture, BlendMode blend) {
    PushQuadColored(a, b, c, d, color, color, color, color, uv0, uv1, texture, blend);
}

void Renderer::PushQuadColored(const math::Vec2& a, const math::Vec2& b, const math::Vec2& c,
                               const math::Vec2& d, const Color& ca, const Color& cb, const Color& cc,
                               const Color& cd, const math::Vec2& uv0, const math::Vec2& uv1,
                               TextureHandle texture, BlendMode blend) {
    if (texture.id != currentUITexture_.id || blend != currentUIBlend_) Flush2D();
    if (uiVerts_.size() + 4 > kMaxUIVertices) Flush2D();
    currentUITexture_ = texture;
    currentUIBlend_ = blend;

    math::Vec2 s[4] = {ToScreen(a), ToScreen(b), ToScreen(c), ToScreen(d)};
    const Color cols[4] = {ca, cb, cc, cd};
    // a -> uv0, b -> (u1, v0), c -> uv1, d -> (u0, v1)
    const math::Vec2 uvs[4] = {uv0, {uv1.x, uv0.y}, uv1, {uv0.x, uv1.y}};
    uint16_t base = static_cast<uint16_t>(uiVerts_.size());
    for (int i = 0; i < 4; ++i) {
        UIVertex v;
        v.x = s[i].x;
        v.y = s[i].y;
        v.u = uvs[i].x;
        v.v = uvs[i].y;
        v.r = cols[i].r;
        v.g = cols[i].g;
        v.b = cols[i].b;
        v.a = cols[i].a;
        uiVerts_.push_back(v);
    }
    uiIndices_.push_back(base + 0);
    uiIndices_.push_back(base + 1);
    uiIndices_.push_back(base + 2);
    uiIndices_.push_back(base + 2);
    uiIndices_.push_back(base + 3);
    uiIndices_.push_back(base + 0);
}

void Renderer::Flush2D() {
    if (uiVerts_.empty()) return;
    backend_->SetBlendMode(currentUIBlend_);
    backend_->SetDepthTest(false, false);
    backend_->SetCullMode(CullMode::None);
    backend_->UseShader(uiShader_);
    backend_->SetUniformMat4("uMVP",
                             math::Mat4::Ortho(0, static_cast<float>(screenW_),
                                               static_cast<float>(screenH_), 0, -1, 1));
    backend_->BindTexture(0, currentUITexture_.Valid() ? currentUITexture_ : white_);
    backend_->SetUniformInt("uTex", 0);
    backend_->DrawPrimitives(uiVerts_.data(), static_cast<uint32_t>(uiVerts_.size()), 32,
                             uiIndices_.data(), static_cast<uint32_t>(uiIndices_.size()),
                             PrimitiveTopology::Triangles);
    uiVerts_.clear();
    uiIndices_.clear();
    currentUITexture_ = {};
    currentUIBlend_ = BlendMode::Alpha;
}

} // namespace neon::gfx
