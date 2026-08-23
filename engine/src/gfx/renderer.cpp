#include "neon/gfx/renderer.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <cstdlib>

#include "neon/core/log.hpp"
#include "neon/gfx/bloom.hpp"
#include "neon/gfx/csm.hpp"
#include "neon/gfx/point_shadow.hpp"

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
#ifdef SKINNED
layout(location = 4) in vec4 aJointIds;
layout(location = 5) in vec4 aWeights;
uniform mat4 uBoneMatrices[64];
#endif
uniform mat4 uMVP;
uniform mat4 uModel;
uniform mat4 uNormalMat;
uniform mat4 uViewMatrix;
uniform vec3 uCamPos;
out vec3 vWorldPos;
out vec3 vNormal;
out vec2 vUV;
out vec4 vColor;
out float vViewZ;
void main() {
#ifdef SKINNED
    mat4 skin = mat4(0.0);
    for (int i = 0; i < 4; ++i) {
        int id = int(aJointIds[i]);
        if (id >= 0 && id < 64) skin += aWeights[i] * uBoneMatrices[id];
    }
    vec4 p = skin * vec4(aPos, 1.0);
    vec4 n = skin * vec4(aNormal, 0.0);
    vWorldPos = (uModel * p).xyz;
    vNormal = (uNormalMat * n).xyz;
    vUV = aUV;
    vColor = aColor;
    vViewZ = (uViewMatrix * vec4(vWorldPos, 1.0)).z;
    gl_Position = uMVP * p;
#else
    vWorldPos = (uModel * vec4(aPos, 1.0)).xyz;
    vNormal = (uNormalMat * vec4(aNormal, 0.0)).xyz;
    vUV = aUV;
    vColor = aColor;
    vViewZ = (uViewMatrix * vec4(vWorldPos, 1.0)).z;
    gl_Position = uMVP * vec4(aPos, 1.0);
#endif
}
)";

const char* kLitFragmentShader = R"(
#version 330 core
in vec3 vWorldPos;
in vec3 vNormal;
in vec2 vUV;
in vec4 vColor;
in float vViewZ;
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
uniform float uAOStrength;
uniform float uEmissiveIntensity;
uniform float uShininess;
uniform float uMetallic;
uniform float uRoughness;
uniform vec3 uSunDir;
uniform vec3 uSunColor;
uniform float uAmbient;
uniform sampler2D uIrradianceMap;
uniform sampler2D uPrefilteredMap;
uniform sampler2D uBrdfLUT;
uniform float uIblStrength;
uniform float uRoughnessMin;
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
uniform sampler2D uShadowMap0;
uniform sampler2D uShadowMap1;
uniform sampler2D uShadowMap2;
uniform mat4 uLightVP[3];
uniform vec4 uCascadeSplits;
uniform vec2 uShadowTexel;
uniform bool uShadowEnabled;
uniform sampler2D uPointShadowMap0;
uniform sampler2D uPointShadowMap1;
uniform sampler2D uPointShadowMap2;
uniform sampler2D uPointShadowMap3;
uniform sampler2D uPointShadowMap4;
uniform sampler2D uPointShadowMap5;
uniform sampler2D uPointShadowMap6;
uniform sampler2D uPointShadowMap7;
uniform sampler2D uPointShadowMap8;
uniform sampler2D uPointShadowMap9;
uniform sampler2D uPointShadowMap10;
uniform sampler2D uPointShadowMap11;
uniform vec2 uPointShadowTexel;
uniform bool uPointShadowEnabled;
uniform int uPointShadowLightCount;
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
float ShadowFactor(sampler2D sm, vec2 uv, float lightDepth) {
    // Bias scaled by the shadow map's own per-texel depth gradient. The map is
    // stored at texel centers while the compared fragment depth is continuous,
    // so on a sloped receiver the two differ by up to half a texel of depth;
    // a bias of one gradient step (measured from the same map, so it tracks
    // any surface orientation) keeps coplanar receivers lit without a large
    // constant bias that would peter-pan shadows on the thin cascade boxes.
    float d0 = DecodeDepth(texture(sm, uv));
    float dx = DecodeDepth(texture(sm, uv + vec2(uShadowTexel.x, 0.0)));
    float dy = DecodeDepth(texture(sm, uv + vec2(0.0, uShadowTexel.y)));
    float slope = max(abs(dx - d0), abs(dy - d0));
    float bias = clamp(0.002 + slope, 0.002, 0.02);

    float lit = 0.0;
    for (int x = 0; x < 2; ++x) {
        for (int y = 0; y < 2; ++y) {
            vec2 off = (vec2(float(x), float(y)) - vec2(0.5)) * uShadowTexel;
            lit += DecodeDepth(texture(sm, uv + off)) > lightDepth - bias ? 1.0 : 0.0;
        }
    }
    return lit / 4.0;
}
// Maps the light->fragment direction to a cube face + uv. Same convention as
// the CPU-side CubemapFaceAndUV (GL cube-map spec table 8.19), which is how
// the 6 per-face shadow maps were rendered.
vec2 PointCubemapFaceUV(vec3 dir, out int face) {
    vec3 ad = abs(dir);
    float ma = max(max(ad.x, ad.y), ad.z);
    vec2 uv;
    if (ad.x >= ad.y && ad.x >= ad.z) {
        if (dir.x >= 0.0) { face = 0; uv = vec2(-dir.z, -dir.y); }
        else              { face = 1; uv = vec2( dir.z, -dir.y); }
    } else if (ad.y >= ad.x && ad.y >= ad.z) {
        if (dir.y >= 0.0) { face = 2; uv = vec2( dir.x,  dir.z); }
        else              { face = 3; uv = vec2( dir.x, -dir.z); }
    } else {
        if (dir.z >= 0.0) { face = 4; uv = vec2( dir.x, -dir.y); }
        else              { face = 5; uv = vec2(-dir.x, -dir.y); }
    }
    return uv / ma * 0.5 + 0.5;
}
// Point-light shadow factor for one face map: manual depth compare + PCF. The
// map stores linear dist/range, so the compared depth is current = dist/range.
// Bias is the map's own per-texel depth gradient (like the CSM path) so sloped
// receivers stay lit without peter-panning. taps=1 keeps secondary lights
// cheap (4 fetches); taps=4 (2x2 PCF) is used for the primary point light.
float PointShadowFactor(sampler2D sm, vec2 uv, float current, int taps) {
    float d0 = DecodeDepth(texture(sm, uv));
    float dx = DecodeDepth(texture(sm, uv + vec2(uPointShadowTexel.x, 0.0)));
    float dy = DecodeDepth(texture(sm, uv + vec2(0.0, uPointShadowTexel.y)));
    float slope = max(abs(dx - d0), abs(dy - d0));
    float bias = clamp(0.003 + slope, 0.003, 0.03);
    float lit = 0.0;
    if (taps == 1) {
        lit = d0 > current - bias ? 1.0 : 0.0;
    } else {
        for (int x = 0; x < 2; ++x) {
            for (int y = 0; y < 2; ++y) {
                vec2 off = (vec2(float(x), float(y)) - vec2(0.5)) * uPointShadowTexel;
                lit += DecodeDepth(texture(sm, uv + off)) > current - bias ? 1.0 : 0.0;
            }
        }
        lit /= 4.0;
    }
    return lit;
}
// Selects the 2D shadow map for (light, face) and applies PCF. The face index
// is static per branch, so every sampler reference is constant-resolved.
float PointShadowForLight(int light, vec3 worldPos, vec3 lightPos, float range) {
    vec3 dir = worldPos - lightPos;
    float dist = length(dir);
    if (dist < 1e-4) return 1.0;
    int face;
    vec2 uv = PointCubemapFaceUV(dir / dist, face);
    float current = dist / max(range, 1e-4);
    int taps = light == 0 ? 4 : 1;
    if (light == 0) {
        if (face == 0) return PointShadowFactor(uPointShadowMap0, uv, current, taps);
        if (face == 1) return PointShadowFactor(uPointShadowMap1, uv, current, taps);
        if (face == 2) return PointShadowFactor(uPointShadowMap2, uv, current, taps);
        if (face == 3) return PointShadowFactor(uPointShadowMap3, uv, current, taps);
        if (face == 4) return PointShadowFactor(uPointShadowMap4, uv, current, taps);
        return PointShadowFactor(uPointShadowMap5, uv, current, taps);
    }
    if (face == 0) return PointShadowFactor(uPointShadowMap6, uv, current, taps);
    if (face == 1) return PointShadowFactor(uPointShadowMap7, uv, current, taps);
    if (face == 2) return PointShadowFactor(uPointShadowMap8, uv, current, taps);
    if (face == 3) return PointShadowFactor(uPointShadowMap9, uv, current, taps);
    if (face == 4) return PointShadowFactor(uPointShadowMap10, uv, current, taps);
    return PointShadowFactor(uPointShadowMap11, uv, current, taps);
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
    // IBL environment ambient (Task 3.8): diffuse irradiance + prefiltered
    // specular via the split-sum BRDF LUT. Both maps are baked from the sky
    // gradient and, because that environment is a vertical gradient, sampled
    // by direction.y only (see ibl.hpp). The legacy flat `uAmbient` fades out
    // as uIblStrength -> 1, so `--ibl 0` reproduces the pre-IBL look exactly
    // while the shadows/AO interplay below is unchanged (only the sun term is
    // shadowed; ambient stays unshadowed so shadows read as dim, not black).
    vec3 iblIrradiance = texture(uIrradianceMap, vec2(0.5, N.y * 0.5 + 0.5)).rgb;
    vec3 iblDiffuse = kd * iblIrradiance * albedo.rgb * uIblStrength;
    vec3 R = reflect(-V, N);
    float roughU = clamp((roughness - uRoughnessMin) / (1.0 - uRoughnessMin), 0.0, 1.0);
    vec3 prefiltered = texture(uPrefilteredMap, vec2(roughU, R.y * 0.5 + 0.5)).rgb;
    vec2 brdf = texture(uBrdfLUT, vec2(ndv, roughness)).rg;
    vec3 iblSpecular = prefiltered * (f0 * brdf.x + brdf.y) * uIblStrength;
    vec3 ambientLight = iblDiffuse + iblSpecular + albedo.rgb * uAmbient * (1.0 - uIblStrength);
    if (uHasAO) ambientLight *= mix(1.0, texture(uOcclusion, vUV).r, uAOStrength);
    vec3 color = (kd * albedo.rgb + spec) * uSunColor * ndl + ambientLight;
    if (uHasEmissive) color += texture(uEmissive, vUV).rgb * uEmissiveIntensity;
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
        vec3 pContrib = (pKd * albedo.rgb + pSpec) * uPointColor[i] * pndl * atten;
        if (uPointShadowEnabled && i < uPointShadowLightCount) {
            pContrib *= PointShadowForLight(i, vWorldPos, uPointPos[i], uPointRadius[i]);
        }
        color += pContrib;
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
        // Cascade selection by positive view-space depth (distance along the
        // camera forward axis), matching the CPU-side split computation.
        float viewDepth = -vViewZ;
        int cascade = viewDepth < uCascadeSplits.x ? 0 : (viewDepth < uCascadeSplits.y ? 1 : 2);
        vec4 sp;
        if (cascade == 0) sp = uLightVP[0] * vec4(vWorldPos, 1.0);
        else if (cascade == 1) sp = uLightVP[1] * vec4(vWorldPos, 1.0);
        else sp = uLightVP[2] * vec4(vWorldPos, 1.0);
        vec3 ndc = sp.xyz / sp.w;
        if (ndc.x > -1.0 && ndc.x < 1.0 && ndc.y > -1.0 && ndc.y < 1.0 && ndc.z > -1.0 &&
            ndc.z < 1.0) {
            vec3 sc = ndc * 0.5 + 0.5;
            if (cascade == 0) shadow = ShadowFactor(uShadowMap0, sc.xy, sc.z);
            else if (cascade == 1) shadow = ShadowFactor(uShadowMap1, sc.xy, sc.z);
            else shadow = ShadowFactor(uShadowMap2, sc.xy, sc.z);
        }
    }
    // The sun term is shadowed; ambient/sky stays unshadowed so shadowed areas
    // read as dim (not black) and match the CPU projected-shadow fallback look.
    color = (color - ambientLight) * shadow + ambientLight;
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
uniform mat4 uViewMatrix;
uniform vec3 uCamPos;
out vec3 vWorldPos;
out vec3 vNormal;
out vec2 vUV;
out vec4 vColor;
out float vViewZ;
void main() {
    vWorldPos = (aInstance * vec4(aPos, 1.0)).xyz;
    vNormal = mat3(aInstance) * aNormal;
    vUV = aUV;
    vColor = aColor;
    vViewZ = (uViewMatrix * vec4(vWorldPos, 1.0)).z;
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
#if defined(NEON_ENABLE_VULKAN)
    if (backendName_ == "vulkan") {
        backend_ = CreateVulkanBackend();
    }
#endif
    if (!backend_) {
        backend_ = CreateOpenGLBackend();
    }
    if (!backend_ || !backend_->Init(window)) {
        NEON_LOG_CAT(neon::core::LogCategory::Gfx, neon::core::LogLevel::Error,
                     "Renderer: %s backend initialization failed",
                     backendName_ == "vulkan" ? "Vulkan" : "OpenGL");
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
    if (skinnedLitShader_.Valid()) backend_->DestroyShader(skinnedLitShader_);
    if (unlitShader_.Valid()) backend_->DestroyShader(unlitShader_);
    if (uiShader_.Valid()) backend_->DestroyShader(uiShader_);
    if (linesShader_.Valid()) backend_->DestroyShader(linesShader_);
    if (litInstancedShader_.Valid()) backend_->DestroyShader(litInstancedShader_);
    if (unlitInstancedShader_.Valid()) backend_->DestroyShader(unlitInstancedShader_);
    if (depthShader_.Valid()) backend_->DestroyShader(depthShader_);
    if (depthInstancedShader_.Valid()) backend_->DestroyShader(depthInstancedShader_);
    if (depthSkinnedShader_.Valid()) backend_->DestroyShader(depthSkinnedShader_);
    if (pointDepthShader_.Valid()) backend_->DestroyShader(pointDepthShader_);
    if (pointDepthInstancedShader_.Valid()) backend_->DestroyShader(pointDepthInstancedShader_);
    if (pointDepthSkinnedShader_.Valid()) backend_->DestroyShader(pointDepthSkinnedShader_);
    if (brightPassShader_.Valid()) backend_->DestroyShader(brightPassShader_);
    if (blurShader_.Valid()) backend_->DestroyShader(blurShader_);
    if (downsampleShader_.Valid()) backend_->DestroyShader(downsampleShader_);
    if (upsampleAddShader_.Valid()) backend_->DestroyShader(upsampleAddShader_);
    if (compositeShader_.Valid()) backend_->DestroyShader(compositeShader_);
    if (probeQuadMesh_.Valid()) backend_->DestroyMesh(probeQuadMesh_);
    if (postQuadMesh_.Valid()) backend_->DestroyMesh(postQuadMesh_);
    for (int i = 0; i < kShadowCascades; ++i) {
        if (shadowRT_[i].Valid()) backend_->DestroyRenderTarget(shadowRT_[i]);
    }
    for (int li = 0; li < kShadowPointLights; ++li) {
        for (int face = 0; face < 6; ++face) {
            if (pointShadowRT_[li][face].Valid())
                backend_->DestroyRenderTarget(pointShadowRT_[li][face]);
        }
    }
    DestroyPostTargets();
    if (white_.Valid()) backend_->DestroyTexture(white_);
    if (iblIrradianceTex_.Valid()) backend_->DestroyTexture(iblIrradianceTex_);
    if (iblPrefilteredTex_.Valid()) backend_->DestroyTexture(iblPrefilteredTex_);
    if (iblBrdfLutTex_.Valid()) backend_->DestroyTexture(iblBrdfLutTex_);
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
    {
        // Skinned lit variant: same source with #define SKINNED 1 inserted
        // right after the #version line (GLSL requires #version first) so the
        // shader enables the joint/weight attributes + uBoneMatrices.
        std::string skinnedSrc(kLitVertexShader);
        size_t versionPos = skinnedSrc.find("#version");
        size_t versionEnd = skinnedSrc.find('\n', versionPos);
        skinnedSrc.insert(versionEnd + 1, "#define SKINNED 1\n");
        skinnedLitShader_ =
            backend_->CreateShader(skinnedSrc.c_str(), kLitFragmentShader, "lit_skinned");
        NEON_LOG_CAT(neon::core::LogCategory::Gfx, neon::core::LogLevel::Info,
                     "Renderer: skinned lit shader %s",
                     skinnedLitShader_.Valid() ? "ok" : "FAILED");
    }
    unlitShader_ = backend_->CreateShader(kUnlitVertexShader, kUnlitFragmentShader, "unlit");
    uiShader_ = backend_->CreateShader(kUIVertexShader, kUIFragmentShader, "ui");
    linesShader_ = backend_->CreateShader(kLineVertexShader, kLineFragmentShader, "lines");
    litInstancedShader_ =
        backend_->CreateShader(kLitInstancedVertexShader, kLitFragmentShader, "lit_instanced");
    unlitInstancedShader_ =
        backend_->CreateShader(kUnlitInstancedVertexShader, kUnlitFragmentShader, "unlit_instanced");
    depthShader_ = backend_->CreateShader(kShadowVertexShader, kShadowFragmentShader, "shadow");
    depthInstancedShader_ =
        backend_->CreateShader(kShadowInstancedVertexShader, kShadowFragmentShader, "shadow_inst");
    depthSkinnedShader_ =
        backend_->CreateShader(kShadowSkinnedVertexShader, kShadowFragmentShader, "shadow_skin");
    pointDepthShader_ =
        backend_->CreateShader(kPointShadowVertexShader, kPointShadowFragmentShader, "point_shadow");
    pointDepthInstancedShader_ = backend_->CreateShader(kPointShadowInstancedVertexShader,
                                                        kPointShadowFragmentShader,
                                                        "point_shadow_inst");
    pointDepthSkinnedShader_ = backend_->CreateShader(kPointShadowSkinnedVertexShader,
                                                      kPointShadowFragmentShader,
                                                      "point_shadow_skin");
    NEON_LOG_CAT(neon::core::LogCategory::Gfx, neon::core::LogLevel::Info,
                 "Renderer: point shadow shaders %s",
                 (pointDepthShader_.Valid() && pointDepthInstancedShader_.Valid() &&
                  pointDepthSkinnedShader_.Valid())
                     ? "ok"
                     : "FAILED");

    // Post-processing shaders (HDR + bloom). Sources live in bloom.hpp so the
    // pure math and the shader tokens are unit-testable headlessly.
    brightPassShader_ =
        backend_->CreateShader(kPostVertexShader, kBrightPassFragmentShader, "bloom_bright");
    blurShader_ = backend_->CreateShader(kPostVertexShader, kBlurFragmentShader, "bloom_blur");
    downsampleShader_ =
        backend_->CreateShader(kPostVertexShader, kDownsampleFragmentShader, "bloom_downsample");
    upsampleAddShader_ =
        backend_->CreateShader(kPostVertexShader, kUpsampleAddFragmentShader, "bloom_upsample_add");
    compositeShader_ =
        backend_->CreateShader(kPostVertexShader, kCompositeFragmentShader, "bloom_composite");
    NEON_LOG_CAT(neon::core::LogCategory::Gfx, neon::core::LogLevel::Info,
                 "Renderer: bloom shaders %s",
                 (brightPassShader_.Valid() && blurShader_.Valid() && downsampleShader_.Valid() &&
                  upsampleAddShader_.Valid() && compositeShader_.Valid())
                     ? "ok"
                     : "FAILED");
    if (std::getenv("NEON_NO_BLOOM")) {
        NEON_LOG_CAT(neon::core::LogCategory::Gfx, neon::core::LogLevel::Warn,
                     "Renderer: bloom disabled by NEON_NO_BLOOM");
        bloomEnabled_ = false;
    }

    // NDC unit quad used by the FBO capability self-test.
    const Vertex3D quadVerts[4] = {
        {{-1, -1, 0}, {}, {}, {1, 1, 1, 1}, {0, 0, 0, 0}, {0, 0, 0, 0}},
        {{1, -1, 0}, {}, {}, {1, 1, 1, 1}, {0, 0, 0, 0}, {0, 0, 0, 0}},
        {{1, 1, 0}, {}, {}, {1, 1, 1, 1}, {0, 0, 0, 0}, {0, 0, 0, 0}},
        {{-1, 1, 0}, {}, {}, {1, 1, 1, 1}, {0, 0, 0, 0}, {0, 0, 0, 0}},
    };
    const uint16_t quadIndices[6] = {0, 1, 2, 0, 2, 3};
    probeQuadMesh_ = backend_->CreateMesh(quadVerts, 4, quadIndices, 6);

    // Fullscreen NDC quad with texture coordinates for the post passes.
    const Vertex3D postVerts[4] = {
        {{-1, -1, 0}, {}, {0, 0}, {1, 1, 1, 1}, {0, 0, 0, 0}, {0, 0, 0, 0}},
        {{1, -1, 0}, {}, {1, 0}, {1, 1, 1, 1}, {0, 0, 0, 0}, {0, 0, 0, 0}},
        {{1, 1, 0}, {}, {1, 1}, {1, 1, 1, 1}, {0, 0, 0, 0}, {0, 0, 0, 0}},
        {{-1, 1, 0}, {}, {0, 1}, {1, 1, 1, 1}, {0, 0, 0, 0}, {0, 0, 0, 0}},
    };
    postQuadMesh_ = backend_->CreateMesh(postVerts, 4, quadIndices, 6);

    // HDR float-target capability (independent of the shadow path, so
    // --no-shadows still gets HDR + bloom). If the driver cannot render into a
    // half-float FBO, the renderer falls back to the legacy direct-to-backbuffer
    // flow and bloom is skipped.
    hdrEnabled_ = TestFloatTargetCapability();

    // MSAA on the HDR scene target (Task 3.7): gated on the float path AND the
    // multisample FBO + blit-resolve self-test. A failure (or --no-msaa) keeps
    // the single-sample HDR target, so every fallback still composites.
    if (hdrEnabled_ && msaaRequested_) {
        msaaEnabled_ = TestMsaaCapability();
        if (!msaaEnabled_) {
            NEON_LOG_CAT(neon::core::LogCategory::Gfx, neon::core::LogLevel::Warn,
                         "Renderer: MSAA unavailable -> single-sample HDR path");
        }
    } else if (hdrEnabled_ && !msaaRequested_) {
        NEON_LOG_CAT(neon::core::LogCategory::Gfx, neon::core::LogLevel::Info,
                     "Renderer: MSAA disabled by flag -> single-sample HDR path");
    }

    if (shadowsForcedOff_) {
        NEON_LOG_CAT(neon::core::LogCategory::Gfx, neon::core::LogLevel::Warn,
                     "Renderer: CSM disabled by flag (--disable-fbo/--no-shadows)");
        return;
    }
    depthAvailable_ = backend_->DepthAvailable();
    for (int i = 0; i < kShadowCascades; ++i) {
        shadowRT_[i] = backend_->CreateRenderTarget(shadowSize_, shadowSize_);
        shadowDepthTex_[i] = backend_->RenderTargetColorTexture(shadowRT_[i]);
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
                    backend_->CreateRenderTarget(kPointShadowSize, kPointShadowSize);
                pointShadowDepthTex_[li][face] =
                    backend_->RenderTargetColorTexture(pointShadowRT_[li][face]);
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

void Renderer::BeginFrame(const Color& clearColor, float clearDepth) {
    stats_ = RenderStats{};
    csmActive_ = false;
    pointShadowsActive_ = false;
    shadowPassRanThisFrame_ = false;
    compositedThisFrame_ = false;
    bloomRanThisFrame_ = false;
    screenW_ = window_ ? window_->Width() : screenW_;
    screenH_ = window_ ? window_->Height() : screenH_;
    uiScale_ = static_cast<float>(screenH_) / static_cast<float>(kDesignHeight);
    uiOffsetX_ = (static_cast<float>(screenW_) - static_cast<float>(kDesignWidth) * uiScale_) * 0.5f;
    uiOffsetY_ = 0.0f;
    EnsurePostTargets();
    if (hdrEnabled_ && hdrRT_.Valid()) {
        // Scene + sky draw into the (multisample when MSAA is active) HDR
        // target; the final composite (bloom -> backbuffer) happens in
        // EndFrame / CaptureFrame after resolving the MSAA samples.
        RebindMainTarget();
        backend_->SetViewport(0, 0, screenW_, screenH_);
        backend_->Clear(clearColor, clearDepth);
    } else {
        backend_->BindDefaultTarget();
        backend_->SetViewport(0, 0, screenW_, screenH_);
        backend_->Clear(clearColor, clearDepth);
    }
}

void Renderer::EndFrame() {
    CompositeFrame();
    backend_->EndFrame();
}

void Renderer::SetCamera(const Camera& camera, float aspect) {
    camera_ = camera;
    viewAspect_ = aspect > 0.01f ? aspect : viewAspect_;
    viewProj_ = camera.ViewProjection(aspect);
    view_ = camera.View();
    camPos_ = camera.position;
    frustum_ = math::Frustum::FromViewProjection(viewProj_);
    frustumValid_ = true;
    // Render the cascade shadow maps now: they are sampled by the main-pass
    // draws that follow this SetCamera. Uses the previous frame's recorded
    // casters (one frame of staleness, imperceptible) and the current camera.
    if (csmEnabled_ && !shadowPassRanThisFrame_) {
        RunShadowPass();
        // RunShadowPass ends with BindDefaultTarget (shadow FBOs unbound);
        // route the main pass back into the HDR target when active.
        RebindMainTarget();
    }
}

void Renderer::SetSky(const Color& top, const Color& horizon) {
    skyTop_ = top;
    skyHorizon_ = horizon;
    // Lazy IBL recompute. The demo animates the sky every frame (a day/night
    // cycle), so the environment is rebuilt only when the sky has actually
    // moved by a cumulative epsilon AND enough SetSky calls have elapsed since
    // the last rebuild - an animated sky then re-precomputes at most once every
    // kIblRecomputeInterval frames (~20ms, logged) and a static sky never does.
    // Re-enabling IBL (strength 0 -> >0) forces a rebuild via iblValid_.
    if (iblStrength_ <= 0.0f) return;
    ++iblFrameCounter_;
    if (!iblValid_) {
        RecomputeIbl(top, horizon);
        return;
    }
    const float delta = std::max({
        std::fabs(top.r - iblLastTop_.r),     std::fabs(top.g - iblLastTop_.g),
        std::fabs(top.b - iblLastTop_.b),     std::fabs(horizon.r - iblLastHorizon_.r),
        std::fabs(horizon.g - iblLastHorizon_.g), std::fabs(horizon.b - iblLastHorizon_.b),
    });
    iblAccumDelta_ += delta;
    if (iblAccumDelta_ >= kIblSkyEpsilon &&
        iblFrameCounter_ - iblLastRecomputeFrame_ >= kIblRecomputeInterval) {
        RecomputeIbl(top, horizon);
    }
}

void Renderer::SetIblStrength(float strength) {
    strength = std::max(0.0f, std::min(1.0f, strength));
    const bool wasZero = iblStrength_ <= 0.0f;
    iblStrength_ = strength;
    if (wasZero && strength > 0.0f) iblValid_ = false; // rebuild on next SetSky
}

void Renderer::RecomputeIbl(const Color& top, const Color& horizon) {
    if (!backend_) return;
    ++iblBuildCount_;
    const auto start = std::chrono::steady_clock::now();

    // BRDF LUT is a pure material term (roughness x NoV), independent of the
    // sky - build and upload it once.
    if (!iblBrdfLutReady_) {
        const std::vector<uint8_t> lut = ibl::BuildBrdfLut();
        if (!lut.empty()) {
            if (iblBrdfLutTex_.Valid()) backend_->DestroyTexture(iblBrdfLutTex_);
            TextureDesc desc;
            desc.width = ibl::kBrdfLutSize;
            desc.height = ibl::kBrdfLutSize;
            desc.rgba = lut.data();
            desc.filter = Filter::Linear;
            iblBrdfLutTex_ = backend_->CreateTexture(desc);
            iblBrdfLutReady_ = iblBrdfLutTex_.Valid();
        }
    }

    // Sky-dependent maps: irradiance (diffuse) + prefiltered specular. Both are
    // RGBA8: the sky gradient is an LDR environment (all texels <= 1) so the
    // 8-bit upload loses nothing; a future HDR environment would need a
    // float-texture path in the backend.
    const std::vector<uint8_t> irr = ibl::BuildIrradianceMap(top, horizon, kIblGradientPower);
    const std::vector<uint8_t> pf = ibl::BuildPrefilteredMap(top, horizon, kIblGradientPower);
    if (iblIrradianceTex_.Valid()) backend_->DestroyTexture(iblIrradianceTex_);
    if (iblPrefilteredTex_.Valid()) backend_->DestroyTexture(iblPrefilteredTex_);
    TextureDesc irrDesc;
    irrDesc.width = 1;
    irrDesc.height = ibl::kEnvRows;
    irrDesc.rgba = irr.data();
    irrDesc.filter = Filter::Linear;
    iblIrradianceTex_ = backend_->CreateTexture(irrDesc);
    TextureDesc pfDesc;
    pfDesc.width = ibl::kRoughnessCols;
    pfDesc.height = ibl::kEnvRows;
    pfDesc.rgba = pf.data();
    pfDesc.filter = Filter::Linear;
    iblPrefilteredTex_ = backend_->CreateTexture(pfDesc);

    iblValid_ = iblIrradianceTex_.Valid() && iblPrefilteredTex_.Valid() && iblBrdfLutTex_.Valid();
    iblLastTop_ = top;
    iblLastHorizon_ = horizon;
    iblAccumDelta_ = 0.0f;
    iblLastRecomputeFrame_ = iblFrameCounter_;

    const double ms =
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
    NEON_LOG_CAT(neon::core::LogCategory::Gfx, neon::core::LogLevel::Info,
                 "Renderer: IBL environment recomputed (sky %.2f,%.2f,%.2f -> %.2f,%.2f,%.2f) in "
                 "%.1f ms (%s)",
                 top.r, top.g, top.b, horizon.r, horizon.g, horizon.b, ms,
                 iblValid_ ? "ok" : "FAILED");
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

void Renderer::SetShadowsEnabled(bool enabled) {
    shadowsForcedOff_ = !enabled;
    if (!enabled) csmEnabled_ = false;
}

void Renderer::SetBloomEnabled(bool enabled) {
    bloomEnabled_ = enabled;
    if (!enabled)
        NEON_LOG_CAT(neon::core::LogCategory::Gfx, neon::core::LogLevel::Info,
                     "Renderer: bloom disabled");
}

void Renderer::SetExposure(float exposure) {
    exposure_ = exposure;
    NEON_LOG_CAT(neon::core::LogCategory::Gfx, neon::core::LogLevel::Info,
                 "Renderer: composite exposure = %.3f", exposure_);
}

void Renderer::SetTonemapEnabled(bool enabled) {
    tonemapEnabled_ = enabled;
    NEON_LOG_CAT(neon::core::LogCategory::Gfx, neon::core::LogLevel::Info,
                 "Renderer: tonemap %s", enabled ? "enabled" : "disabled (legacy clamp)");
}

void Renderer::SetMsaaEnabled(bool enabled) {
    msaaRequested_ = enabled;
    NEON_LOG_CAT(neon::core::LogCategory::Gfx, neon::core::LogLevel::Info,
                 "Renderer: MSAA %s", enabled ? "requested" : "disabled by flag");
}

void Renderer::RunShadowPass() {
    if (!csmEnabled_) return;
    shadowPassRanThisFrame_ = true;

    ComputeCascadeSplits(camera_.nearPlane, camera_.farPlane, cascadeSplits_);
    // Cascade frusta must match the camera projection (which may use the
    // viewport rect's aspect when the editor renders into a sub-viewport).
    const float aspect = viewAspect_;

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
        lightViewProj_[i] =
            ComputeCascadeLightViewProj(sunDir_, camera_, aspect, cascadeSplits_[i],
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
    RunPointShadowPass();
    backend_->BindDefaultTarget();
    shadowCasters_.clear();
    csmActive_ = true;
}

void Renderer::DrawShadowCastersSorted(const math::Mat4& lightVP) {
    if (shadowCasters_.empty()) return;
    // Extract the light view (projection is ortho, translation-only per axis)
    // to sort casters by their distance along the light direction.
    const math::Mat4 lightView = lightVP;
    struct SortKey {
        const ShadowDraw* draw;
        float lightZ;
    };
    std::vector<SortKey> keys;
    keys.reserve(shadowCasters_.size());
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
        keys.push_back({&draw, lightView.TransformPoint(center).z});
    }
    // NDC z grows as light-space z goes negative (ortho slope is negative), so
    // the farthest caster has the largest value; draw it first (last wins).
    std::sort(keys.begin(), keys.end(),
              [](const SortKey& a, const SortKey& b) { return a.lightZ > b.lightZ; });
    for (const SortKey& k : keys) DrawShadowCaster(*k.draw, lightVP);
}

void Renderer::DrawShadowCaster(const ShadowDraw& draw, const math::Mat4& lightVP) {
    if (!draw.mesh.Valid()) return;
    if (!draw.models.empty()) {
        backend_->UseShader(depthInstancedShader_);
        backend_->SetUniformMat4("uMVP", lightVP);
        backend_->DrawMeshInstanced(draw.mesh, draw.models.data(),
                                    static_cast<uint32_t>(draw.models.size()));
    } else if (!draw.bones.empty()) {
        backend_->UseShader(depthSkinnedShader_);
        std::vector<float> flat(static_cast<size_t>(draw.boneCount) * 16);
        for (int i = 0; i < draw.boneCount; ++i)
            std::memcpy(flat.data() + static_cast<size_t>(i) * 16,
                        draw.bones[static_cast<size_t>(i)].Data(), 16 * sizeof(float));
        backend_->SetUniformMat4Array("uBoneMatrices", flat.data(), draw.boneCount);
        backend_->SetUniformMat4("uMVP", lightVP * draw.model);
        backend_->DrawMesh(draw.mesh);
    } else {
        backend_->UseShader(depthShader_);
        backend_->SetUniformMat4("uMVP", lightVP * draw.model);
        backend_->DrawMesh(draw.mesh);
    }
}

void Renderer::RunPointShadowPass() {
    if (!pointShadowsEnabled_) return;
    pointShadowsActive_ = false;
    const int lightCount = std::min(pointCount_, kShadowPointLights);
    for (int li = 0; li < lightCount; ++li) {
        if (pointRadius_[li] <= 0.0f) continue;
        const float range = pointRadius_[li];
        bool allFaces = true;
        for (int face = 0; face < 6; ++face) {
            if (!pointShadowRT_[li][face].Valid()) {
                allFaces = false;
                break;
            }
            pointLightViewProj_[li][face] =
                ComputePointLightFaceViewProj(pointPos_[li], face, kPointShadowNear, range);
        }
        if (!allFaces) continue;
        DrawPointShadowCastersSorted(li);
        pointShadowsActive_ = true;
    }
}

void Renderer::DrawPointShadowCastersSorted(int lightIndex) {
    if (shadowCasters_.empty()) return;
    const math::Vec3 lightPos = pointPos_[lightIndex];
    const float range = pointRadius_[lightIndex];

    // Color-encoded maps have no depth buffer, so draw casters far -> near from
    // the light (last wins = nearest surface). Casters fully outside the
    // light's sphere of influence cannot shadow anything the light reaches.
    struct SortKey {
        const ShadowDraw* draw;
        float dist;
    };
    std::vector<SortKey> keys;
    keys.reserve(shadowCasters_.size());
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
        keys.push_back({&draw, dist});
    }
    std::sort(keys.begin(), keys.end(),
              [](const SortKey& a, const SortKey& b) { return a.dist > b.dist; });

    backend_->SetBlendMode(BlendMode::Opaque);
    backend_->SetCullMode(CullMode::Back);
    backend_->SetDepthTest(false, false);
    for (int face = 0; face < 6; ++face) {
        backend_->BindRenderTarget(pointShadowRT_[lightIndex][face]);
        backend_->Clear({1.0f, 1.0f, 1.0f, 1.0f}, 1.0f);
        for (const SortKey& k : keys)
            DrawPointShadowCaster(*k.draw, pointLightViewProj_[lightIndex][face], lightPos, range);
    }
    backend_->BindDefaultTarget();
}

void Renderer::DrawPointShadowCaster(const ShadowDraw& draw, const math::Mat4& lightVP,
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
        std::vector<float> flat(static_cast<size_t>(draw.boneCount) * 16);
        for (int i = 0; i < draw.boneCount; ++i)
            std::memcpy(flat.data() + static_cast<size_t>(i) * 16,
                        draw.bones[static_cast<size_t>(i)].Data(), 16 * sizeof(float));
        backend_->SetUniformMat4Array("uBoneMatrices", flat.data(), draw.boneCount);
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

bool Renderer::TestDepthTargetCapability() {
    if (!backend_ || !depthShader_.Valid() || !probeQuadMesh_.Valid()) return false;
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
            backend_->DrawMesh(probeQuadMesh_);
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
        backend_->DrawMesh(probeQuadMesh_);
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
                backend_->DrawMesh(probeQuadMesh_);
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

    if (csmEnabled_ && shadowRecording_ && !material.transparent) {
        shadowCasters_.push_back({mesh.Handle(), model, {}, {}, 0, mesh.Bounds()});
    }

    ShaderHandle shader = material.shader.Valid() ? material.shader
                                                  : (material.lit ? litShader_ : unlitShader_);
    ApplyMaterial(material, viewProj_ * model, model, NormalMatrix(model), shader);
    backend_->DrawMesh(mesh.Handle());
    ++stats_.drawCalls;
    stats_.triangles += mesh.TriangleCount();
}

void Renderer::DrawSkinnedMesh(const Mesh& mesh, const Material& material,
                               const math::Mat4& model,
                               const std::vector<math::Mat4>& boneMatrices, int boneCount) {
    if (!mesh.Valid()) return;
    Flush2D();

    if (frustumValid_ && !frustum_.Intersects(math::TransformAABB(mesh.Bounds(), model))) return;

    // Upload up to 64 bone matrices as one contiguous row-major array.
    int count = boneCount >= 0 ? std::min(boneCount, static_cast<int>(boneMatrices.size()))
                               : static_cast<int>(boneMatrices.size());
    count = std::min(count, 64);

    if (csmEnabled_ && shadowRecording_ && !material.transparent) {
        shadowCasters_.push_back({mesh.Handle(), model, {}, boneMatrices, count, mesh.Bounds()});
    }

    ShaderHandle shader = material.shader.Valid() ? material.shader : skinnedLitShader_;
    ApplyMaterial(material, viewProj_ * model, model, NormalMatrix(model), shader);

    if (count > 0) {
        std::vector<float> flat(static_cast<size_t>(count) * 16);
        for (int i = 0; i < count; ++i)
            std::memcpy(flat.data() + static_cast<size_t>(i) * 16,
                        boneMatrices[static_cast<size_t>(i)].Data(), 16 * sizeof(float));
        backend_->SetUniformMat4Array("uBoneMatrices", flat.data(), count);
    }
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

    if (csmEnabled_ && shadowRecording_ && !material.transparent) {
        shadowCasters_.push_back(
            {mesh.Handle(), math::Mat4::Identity(), visible, {}, 0, mesh.Bounds()});
    }

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
        backend_->SetUniformFloat("uAOStrength", material.aoStrength);
        backend_->SetUniformFloat("uEmissiveIntensity", material.emissiveIntensity);
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
        backend_->SetUniformMat4("uViewMatrix", view_);
        {
            float flatVP[3 * 16];
            for (int i = 0; i < kShadowCascades; ++i)
                std::memcpy(flatVP + i * 16, lightViewProj_[i].Data(), 16 * sizeof(float));
            backend_->SetUniformMat4Array("uLightVP", flatVP, kShadowCascades);
        }
        backend_->SetUniformVec4("uCascadeSplits",
                                 {cascadeSplits_[1], cascadeSplits_[2], cascadeSplits_[3],
                                  cascadeSplits_[0]});
        backend_->SetUniformVec2("uShadowTexel",
                                 {1.0f / static_cast<float>(shadowSize_),
                                  1.0f / static_cast<float>(shadowSize_)});
        backend_->SetUniformInt("uShadowEnabled", csmActive_ ? 1 : 0);
        backend_->BindTexture(5, shadowDepthTex_[0]);
        backend_->SetUniformInt("uShadowMap0", 5);
        backend_->BindTexture(6, shadowDepthTex_[1]);
        backend_->SetUniformInt("uShadowMap1", 6);
        backend_->BindTexture(7, shadowDepthTex_[2]);
        backend_->SetUniformInt("uShadowMap2", 7);

        // Point-light cubemap shadows: 2 lights x 6 faces on texture units
        // 8..19. When the pass is inactive the uniforms are set to valid units
        // anyway (harmless: the shader never samples them), so inactive lights
        // only leave their units unbound.
        const int psLightCount =
            pointShadowsActive_ ? std::min(pointCount_, kShadowPointLights) : 0;
        backend_->SetUniformInt("uPointShadowEnabled", pointShadowsActive_ ? 1 : 0);
        backend_->SetUniformInt("uPointShadowLightCount", psLightCount);
        backend_->SetUniformVec2("uPointShadowTexel",
                                 {1.0f / static_cast<float>(kPointShadowSize),
                                  1.0f / static_cast<float>(kPointShadowSize)});
        for (int li = 0; li < kShadowPointLights; ++li) {
            for (int face = 0; face < 6; ++face) {
                const int slot = 8 + li * 6 + face;
                const std::string name = "uPointShadowMap" + std::to_string(li * 6 + face);
                if (li < psLightCount) backend_->BindTexture(slot, pointShadowDepthTex_[li][face]);
                backend_->SetUniformInt(name.c_str(), slot);
            }
        }

        // IBL environment maps (texture units 20..22): irradiance, prefiltered
        // specular, BRDF LUT. When no environment exists yet (IBL off, or
        // recompute pending) the uniforms stay at their GLSL defaults
        // (uIblStrength = 0) so the shader contributes no IBL term.
        if (iblValid_) {
            backend_->SetUniformFloat("uIblStrength", iblStrength_);
            backend_->SetUniformFloat("uRoughnessMin", ibl::kRoughnessMin);
            backend_->BindTexture(20, iblIrradianceTex_);
            backend_->SetUniformInt("uIrradianceMap", 20);
            backend_->BindTexture(21, iblPrefilteredTex_);
            backend_->SetUniformInt("uPrefilteredMap", 21);
            backend_->BindTexture(22, iblBrdfLutTex_);
            backend_->SetUniformInt("uBrdfLUT", 22);
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

Texture Renderer::CreateTextureCompressed(int width, int height, uint32_t format,
                                          const void* data, size_t size) {
    TextureHandle handle = backend_->CreateTextureCompressed(width, height, format, data, size);
    return Texture(handle, width, height);
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

void Renderer::DrawTriangle2D(const math::Vec2& a, const math::Vec2& b, const math::Vec2& c,
                              const Color& color) {
    if (currentUITexture_.Valid() || currentUIBlend_ != BlendMode::Alpha) Flush2D();
    if (uiVerts_.size() + 3 > kMaxUIVertices) Flush2D();
    currentUITexture_ = {};
    currentUIBlend_ = BlendMode::Alpha;

    const math::Vec2 s[3] = {ToScreen(a), ToScreen(b), ToScreen(c)};
    uint16_t base = static_cast<uint16_t>(uiVerts_.size());
    for (int i = 0; i < 3; ++i) {
        UIVertex v;
        v.x = s[i].x;
        v.y = s[i].y;
        v.u = 0.0f;
        v.v = 0.0f;
        v.r = color.r;
        v.g = color.g;
        v.b = color.b;
        v.a = color.a;
        uiVerts_.push_back(v);
    }
    uiIndices_.push_back(base + 0);
    uiIndices_.push_back(base + 1);
    uiIndices_.push_back(base + 2);
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
        if (!g) {
            // Dynamic glyphs: rasterize the missing codepoint into the atlas.
            const_cast<Font&>(font).EnsureGlyph(cp);
            g = font.FindGlyph(cp);
        }
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
    const float ndcX = clip.x / clip.w;
    const float ndcY = clip.y / clip.w;
    const math::Rect2& r = sceneViewport_.w > 0.0f
                               ? sceneViewport_
                               : math::Rect2{0.0f, 0.0f, static_cast<float>(screenW_),
                                             static_cast<float>(screenH_)};
    const float px = r.x + (ndcX * 0.5f + 0.5f) * r.w;
    const float py = r.y + (0.5f - ndcY * 0.5f) * r.h;
    float pixelSize = size * r.h * 0.5f /
                      (std::tan(camera_.fovY * 0.5f) * clip.w);
    math::Vec2 design = ScreenToUI({px, py});
    float designSize = pixelSize / uiScale_;
    DrawQuad(design - math::Vec2{designSize * 0.5f, designSize * 0.5f},
             {designSize, designSize}, color, texture, {0, 1}, {1, 0}, blend);
}

math::Vec2 Renderer::ScreenToUI(const math::Vec2& screenPixels) const {
    return {(screenPixels.x - uiOffsetX_) / uiScale_,
            (screenPixels.y - uiOffsetY_) / uiScale_};
}

bool Renderer::CaptureFrame(std::vector<uint8_t>& out) {
    if (!backend_) return false;
    // The scene lives in the HDR target at this point; composite it (bloom +
    // clamp) to the backbuffer first so the captured pixels are the FINAL
    // rendered image, then flush any pending 2D on top. EndFrame will see
    // compositedThisFrame_ and just swap.
    CompositeFrame();
    out.resize(static_cast<size_t>(screenW_) * screenH_ * 4);
    backend_->CaptureFrame(screenW_, screenH_, out.data());
    return true;
}

math::Vec2 Renderer::ToScreen(const math::Vec2& design) const {
    return {design.x * uiScale_ + uiOffsetX_, design.y * uiScale_ + uiOffsetY_};
}

void Renderer::Set2DViewport(float x, float y, float w, float h, float zoom,
                             const math::Vec2& pan) {
    if (w <= 0.0f || h <= 0.0f || zoom <= 0.0f) {
        Reset2DViewport();
        return;
    }
    const float fitScale =
        std::min(w / static_cast<float>(kDesignWidth), h / static_cast<float>(kDesignHeight));
    uiScale_ = fitScale * zoom;
    // The design point (640 + pan, 360 + pan) sits at the viewport rect center.
    const float designCx = static_cast<float>(kDesignWidth) * 0.5f + pan.x;
    const float designCy = static_cast<float>(kDesignHeight) * 0.5f + pan.y;
    uiOffsetX_ = x + w * 0.5f - designCx * uiScale_;
    uiOffsetY_ = y + h * 0.5f - designCy * uiScale_;
}

void Renderer::Reset2DViewport() {
    uiScale_ = static_cast<float>(screenH_) / static_cast<float>(kDesignHeight);
    uiOffsetX_ = (static_cast<float>(screenW_) - static_cast<float>(kDesignWidth) * uiScale_) * 0.5f;
    uiOffsetY_ = 0.0f;
}

void Renderer::Set2DViewportPixels(float x, float y) {
    uiScale_ = 1.0f;
    uiOffsetX_ = x;
    uiOffsetY_ = y;
}

void Renderer::SetSceneViewport(float x, float y, float w, float h) {
    if (w <= 0.0f || h <= 0.0f) {
        ResetSceneViewport();
        return;
    }
    sceneViewport_ = {x, y, w, h};
    backend_->SetViewport(static_cast<int>(x), static_cast<int>(y),
                          static_cast<int>(w), static_cast<int>(h));
}

void Renderer::ResetSceneViewport() {
    sceneViewport_ = {0.0f, 0.0f, static_cast<float>(screenW_), static_cast<float>(screenH_)};
    backend_->SetViewport(0, 0, screenW_, screenH_);
}

float Renderer::SceneAspect() const {
    if (sceneViewport_.w > 0.0f && sceneViewport_.h > 0.0f) {
        return sceneViewport_.w / sceneViewport_.h;
    }
    return screenH_ > 0 ? static_cast<float>(screenW_) / static_cast<float>(screenH_) : 1.0f;
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
    // The 2D overlay uses full-window pixel coordinates with a full-screen
    // ortho, so it must always rasterize with the FULL backend viewport even
    // when the 3D scene is rendering into a sub-rect (editor viewport dock).
    // Otherwise an early flush (e.g. ApplyMaterial switching to the 3D shader)
    // would squash the overlay into the scene rect. Restore the scene viewport
    // afterwards so the next 3D draw is unaffected.
    const bool sceneVpActive = sceneViewport_.w > 0.0f && sceneViewport_.h > 0.0f &&
                               (sceneViewport_.x != 0.0f || sceneViewport_.y != 0.0f ||
                                sceneViewport_.w != static_cast<float>(screenW_) ||
                                sceneViewport_.h != static_cast<float>(screenH_));
    if (sceneVpActive) backend_->SetViewport(0, 0, screenW_, screenH_);
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
    if (sceneVpActive) {
        backend_->SetViewport(static_cast<int>(sceneViewport_.x),
                              static_cast<int>(sceneViewport_.y),
                              static_cast<int>(sceneViewport_.w),
                              static_cast<int>(sceneViewport_.h));
    }
    uiVerts_.clear();
    uiIndices_.clear();
    currentUITexture_ = {};
    currentUIBlend_ = BlendMode::Alpha;
}

void Renderer::EnsurePostTargets() {
    if (!hdrEnabled_) return;
    if (screenW_ <= 0 || screenH_ <= 0) return;
    if (hdrRT_.Valid() && hdrW_ == screenW_ && hdrH_ == screenH_) return;
    DestroyPostTargets();
    const int hw = std::max(screenW_ / 2, 1);
    const int hh = std::max(screenH_ / 2, 1);
    const int qw = std::max(screenW_ / 4, 1);
    const int qh = std::max(screenH_ / 4, 1);
    hdrRT_ = backend_->CreateRenderTarget(screenW_, screenH_, true);
    if (!hdrRT_.Valid()) {
        NEON_LOG_CAT(neon::core::LogCategory::Gfx, neon::core::LogLevel::Error,
                     "Renderer: HDR target %dx%d creation failed -> HDR/bloom disabled", screenW_,
                     screenH_);
        hdrEnabled_ = false;
        return;
    }
    if (msaaEnabled_) {
        // MSAA scene target: resolves into hdrRT_ (the bloom source) before
        // the bright pass. Only the HDR main target is multisampled.
        hdrMsaaRT_ = backend_->CreateRenderTarget(screenW_, screenH_, true, msaaSamples_);
        if (!hdrMsaaRT_.Valid()) {
            NEON_LOG_CAT(neon::core::LogCategory::Gfx, neon::core::LogLevel::Warn,
                         "Renderer: MSAA target %dx%d (samples=%d) failed -> single-sample HDR",
                         screenW_, screenH_, msaaSamples_);
            msaaEnabled_ = false;
        }
    }
    bloomHalfA_ = backend_->CreateRenderTarget(hw, hh, true);
    bloomHalfB_ = backend_->CreateRenderTarget(hw, hh, true);
    bloomQuarterA_ = backend_->CreateRenderTarget(qw, qh, true);
    bloomQuarterB_ = backend_->CreateRenderTarget(qw, qh, true);
    hdrW_ = screenW_;
    hdrH_ = screenH_;
    NEON_LOG_CAT(neon::core::LogCategory::Gfx, neon::core::LogLevel::Info,
                 "Renderer: HDR target %dx%d (RGBA16F%s) + bloom %dx%d / %dx%d", screenW_,
                 screenH_, msaaEnabled_ ? ", MSAA" : "", hw, hh, qw, qh);
}

void Renderer::DestroyPostTargets() {
    auto destroy = [this](RenderTargetHandle& t) {
        if (t.Valid() && backend_) backend_->DestroyRenderTarget(t);
        t = {};
    };
    destroy(hdrMsaaRT_);
    destroy(hdrRT_);
    destroy(bloomHalfA_);
    destroy(bloomHalfB_);
    destroy(bloomQuarterA_);
    destroy(bloomQuarterB_);
    hdrW_ = 0;
    hdrH_ = 0;
}

bool Renderer::TestFloatTargetCapability() {
    if (!backend_ || !unlitShader_.Valid() || !probeQuadMesh_.Valid()) return false;
    constexpr int kSize = 32;
    RenderTargetHandle rt = backend_->CreateRenderTarget(kSize, kSize, true);
    if (!rt.Valid()) {
        NEON_LOG_CAT(neon::core::LogCategory::Gfx, neon::core::LogLevel::Warn,
                     "Renderer: HDR FBO self-test: float target failed -> HDR/bloom disabled");
        return false;
    }
    backend_->BindRenderTarget(rt);
    backend_->Clear({0.0f, 0.0f, 0.0f, 1.0f}, 1.0f);
    backend_->UseShader(unlitShader_);
    backend_->SetUniformMat4("uMVP", math::Mat4::Identity());
    backend_->SetUniformInt("uHasTexture", 0);
    backend_->SetUniformVec4("uTint", {0.5f, 0.25f, 0.125f, 1.0f});
    backend_->SetCullMode(CullMode::None);
    backend_->SetDepthTest(false, false);
    backend_->SetBlendMode(BlendMode::Opaque);
    backend_->DrawMesh(probeQuadMesh_);
    unsigned char px[4] = {0, 0, 0, 0};
    backend_->ReadCurrentTargetPixel(kSize / 2, kSize / 2, px);
    backend_->DestroyRenderTarget(rt);
    backend_->BindDefaultTarget();
    // Drawn {0.5, 0.25, 0.125} must come back as ~{128, 64, 32} after the
    // float->byte readback; wide-but-specific ranges catch both a non-writing
    // FBO (zeros) and a clamped-to-1 target (255).
    const bool ok = px[0] >= 110 && px[0] <= 150 && px[1] >= 48 && px[1] <= 80 && px[2] >= 16 &&
                    px[2] <= 48;
    NEON_LOG_CAT(neon::core::LogCategory::Gfx, neon::core::LogLevel::Info,
                 "Renderer: HDR FBO self-test: px=%u,%u,%u,%u -> %s", px[0], px[1], px[2], px[3],
                 ok ? "PASS" : "FAIL");
    if (!ok) {
        NEON_LOG_CAT(neon::core::LogCategory::Gfx, neon::core::LogLevel::Warn,
                     "Renderer: HDR float render target unusable -> HDR/bloom disabled");
        return false;
    }
    return true;
}

bool Renderer::TestMsaaCapability() {
    if (!backend_ || !unlitShader_.Valid() || !probeQuadMesh_.Valid()) return false;
    constexpr int kSize = 32;
    // Try 4x first (the target sample count), then 2x for drivers that only
    // handle lower counts; either way the resolved image must round-trip the
    // drawn colour through the same FBO + blit path the frame uses.
    const int attempts[2] = {4, 2};
    for (int samples : attempts) {
        RenderTargetHandle ms = backend_->CreateRenderTarget(kSize, kSize, true, samples);
        RenderTargetHandle ss = backend_->CreateRenderTarget(kSize, kSize, true);
        bool keep = false;
        if (ms.Valid() && ss.Valid()) {
            backend_->BindRenderTarget(ms);
            backend_->Clear({0.0f, 0.0f, 0.0f, 1.0f}, 1.0f);
            backend_->UseShader(unlitShader_);
            backend_->SetUniformMat4("uMVP", math::Mat4::Identity());
            backend_->SetUniformInt("uHasTexture", 0);
            backend_->SetUniformVec4("uTint", {0.5f, 0.25f, 0.125f, 1.0f});
            backend_->SetCullMode(CullMode::None);
            backend_->SetDepthTest(false, false);
            backend_->SetBlendMode(BlendMode::Opaque);
            backend_->DrawMesh(probeQuadMesh_);
            backend_->ResolveRenderTarget(ms, ss);
            unsigned char px[4] = {0, 0, 0, 0};
            backend_->BindRenderTarget(ss);
            backend_->ReadCurrentTargetPixel(kSize / 2, kSize / 2, px);
            // {0.5, 0.25, 0.125} must survive draw -> multisample -> blit ->
            // byte readback as ~{128, 64, 32}; wide-but-specific ranges catch a
            // dead FBO (zeros) and a clamped-to-1 target (255).
            const bool ok = px[0] >= 110 && px[0] <= 150 && px[1] >= 48 && px[1] <= 80 &&
                            px[2] >= 16 && px[2] <= 48;
            NEON_LOG_CAT(neon::core::LogCategory::Gfx, neon::core::LogLevel::Info,
                         "Renderer: MSAA %dx self-test: px=%u,%u,%u,%u -> %s", samples, px[0],
                         px[1], px[2], px[3], ok ? "PASS" : "FAIL");
            keep = ok;
        }
        backend_->DestroyRenderTarget(ss);
        backend_->DestroyRenderTarget(ms);
        backend_->BindDefaultTarget();
        if (keep) {
            msaaSamples_ = samples;
            NEON_LOG_CAT(neon::core::LogCategory::Gfx, neon::core::LogLevel::Info,
                         "Renderer: MSAA %dx HDR target self-test PASS", samples);
            return true;
        }
        NEON_LOG_CAT(neon::core::LogCategory::Gfx, neon::core::LogLevel::Warn,
                     "Renderer: MSAA %dx HDR target self-test FAIL", samples);
    }
    return false;
}

void Renderer::ResolveMainTarget() {
    if (msaaEnabled_ && hdrMsaaRT_.Valid() && hdrRT_.Valid()) {
        backend_->ResolveRenderTarget(hdrMsaaRT_, hdrRT_);
    }
}

void Renderer::RebindMainTarget() {
    if (hdrEnabled_ && hdrRT_.Valid()) {
        backend_->BindRenderTarget(msaaEnabled_ && hdrMsaaRT_.Valid() ? hdrMsaaRT_ : hdrRT_);
    } else {
        backend_->BindDefaultTarget();
    }
}

bool Renderer::RunBloom() {
    if (!bloomEnabled_) return false;
    if (!hdrRT_.Valid() || !bloomHalfA_.Valid() || !bloomHalfB_.Valid() ||
        !bloomQuarterA_.Valid() || !bloomQuarterB_.Valid() || !postQuadMesh_.Valid()) {
        return false;
    }
    if (!brightPassShader_.Valid() || !blurShader_.Valid() || !downsampleShader_.Valid() ||
        !upsampleAddShader_.Valid()) {
        NEON_LOG_CAT(neon::core::LogCategory::Gfx, neon::core::LogLevel::Warn,
                     "Renderer: bloom skipped - post shader missing");
        return false;
    }
    const float halfTexelX = 1.0f / static_cast<float>(std::max(hdrW_ / 2, 1));
    const float halfTexelY = 1.0f / static_cast<float>(std::max(hdrH_ / 2, 1));
    const float quarterTexelX = 1.0f / static_cast<float>(std::max(hdrW_ / 4, 1));
    const float quarterTexelY = 1.0f / static_cast<float>(std::max(hdrH_ / 4, 1));

    auto fullscreen = [this](ShaderHandle shader) {
        backend_->SetBlendMode(BlendMode::Opaque);
        backend_->SetDepthTest(false, false);
        backend_->SetCullMode(CullMode::None);
        backend_->UseShader(shader);
        backend_->SetUniformMat4("uMVP", math::Mat4::Identity());
    };

    // 1. Bright pass: HDR -> halfA (thresholded, only pixels above 1.0).
    backend_->BindRenderTarget(bloomHalfA_);
    fullscreen(brightPassShader_);
    backend_->BindTexture(0, backend_->RenderTargetColorTexture(hdrRT_));
    backend_->SetUniformInt("uTex", 0);
    backend_->SetUniformFloat("uThreshold", kBloomThreshold);
    backend_->DrawMesh(postQuadMesh_);

    // 2. Blur the half-res bright (H then V), ping-ponging halfA/halfB.
    backend_->BindRenderTarget(bloomHalfB_);
    fullscreen(blurShader_);
    backend_->BindTexture(0, backend_->RenderTargetColorTexture(bloomHalfA_));
    backend_->SetUniformInt("uTex", 0);
    backend_->SetUniformVec2("uTexelSize", {halfTexelX, halfTexelY});
    backend_->SetUniformVec2("uDirection", {1.0f, 0.0f});
    backend_->DrawMesh(postQuadMesh_);
    backend_->BindRenderTarget(bloomHalfA_);
    fullscreen(blurShader_);
    backend_->BindTexture(0, backend_->RenderTargetColorTexture(bloomHalfB_));
    backend_->SetUniformInt("uTex", 0);
    backend_->SetUniformVec2("uTexelSize", {halfTexelX, halfTexelY});
    backend_->SetUniformVec2("uDirection", {0.0f, 1.0f});
    backend_->DrawMesh(postQuadMesh_);

    // 3. Downsample: halfA -> quarterA (2x2 box).
    backend_->BindRenderTarget(bloomQuarterA_);
    fullscreen(downsampleShader_);
    backend_->BindTexture(0, backend_->RenderTargetColorTexture(bloomHalfA_));
    backend_->SetUniformInt("uTex", 0);
    backend_->SetUniformVec2("uSrcTexelSize", {halfTexelX, halfTexelY});
    backend_->DrawMesh(postQuadMesh_);

    // 4. Blur the quarter-res level (H then V), ping-ponging quarterA/quarterB.
    backend_->BindRenderTarget(bloomQuarterB_);
    fullscreen(blurShader_);
    backend_->BindTexture(0, backend_->RenderTargetColorTexture(bloomQuarterA_));
    backend_->SetUniformInt("uTex", 0);
    backend_->SetUniformVec2("uTexelSize", {quarterTexelX, quarterTexelY});
    backend_->SetUniformVec2("uDirection", {1.0f, 0.0f});
    backend_->DrawMesh(postQuadMesh_);
    backend_->BindRenderTarget(bloomQuarterA_);
    fullscreen(blurShader_);
    backend_->BindTexture(0, backend_->RenderTargetColorTexture(bloomQuarterB_));
    backend_->SetUniformInt("uTex", 0);
    backend_->SetUniformVec2("uTexelSize", {quarterTexelX, quarterTexelY});
    backend_->SetUniformVec2("uDirection", {0.0f, 1.0f});
    backend_->DrawMesh(postQuadMesh_);

    // 5. Upsample-add (progressive bloom): halfB = halfA + up(quarterA).
    backend_->BindRenderTarget(bloomHalfB_);
    fullscreen(upsampleAddShader_);
    backend_->BindTexture(0, backend_->RenderTargetColorTexture(bloomHalfA_));
    backend_->SetUniformInt("uHalf", 0);
    backend_->BindTexture(1, backend_->RenderTargetColorTexture(bloomQuarterA_));
    backend_->SetUniformInt("uQuarter", 1);
    backend_->DrawMesh(postQuadMesh_);

    bloomRanThisFrame_ = true;
    return true;
}

void Renderer::CompositeToBackbuffer() {
    if (!compositeShader_.Valid() || !postQuadMesh_.Valid() || !hdrRT_.Valid()) return;
    backend_->BindDefaultTarget();
    backend_->SetBlendMode(BlendMode::Opaque);
    backend_->SetDepthTest(false, false);
    backend_->SetCullMode(CullMode::None);
    backend_->UseShader(compositeShader_);
    backend_->SetUniformMat4("uMVP", math::Mat4::Identity());
    backend_->BindTexture(0, backend_->RenderTargetColorTexture(hdrRT_));
    backend_->SetUniformInt("uHdr", 0);
    if (bloomRanThisFrame_ && bloomHalfB_.Valid()) {
        backend_->BindTexture(1, backend_->RenderTargetColorTexture(bloomHalfB_));
        backend_->SetUniformFloat("uStrength", kBloomStrength);
        backend_->SetUniformInt("uBloomEnabled", 1);
    } else {
        // Bloom off / unavailable: bind the HDR texture on the bloom slot too
        // (the program still references the sampler) and skip the term, so the
        // `--no-bloom` image differs only by the bloom contribution.
        backend_->BindTexture(1, backend_->RenderTargetColorTexture(hdrRT_));
        backend_->SetUniformFloat("uStrength", kBloomStrength);
        backend_->SetUniformInt("uBloomEnabled", 0);
    }
    backend_->SetUniformInt("uBloom", 1);
    backend_->SetUniformFloat("uExposure", exposure_);
    backend_->SetUniformInt("uTonemapEnabled", tonemapEnabled_ ? 1 : 0);
    backend_->DrawMesh(postQuadMesh_);
}

void Renderer::CompositeSceneToBackbuffer() {
    if (!hdrEnabled_ || !hdrRT_.Valid()) {
        Flush2D();
        return;
    }
    // The scene rendered into the (possibly multisample) HDR target; resolve
    // into the single-sample bloom source before any pass samples it.
    ResolveMainTarget();
    bloomRanThisFrame_ = RunBloom();
    CompositeToBackbuffer();
    Flush2D();
}

void Renderer::EndScene() {
    if (!hdrEnabled_ || !hdrRT_.Valid()) return; // legacy: 2D already to backbuffer
    if (!compositedThisFrame_) {
        // Any 2D still queued at this point is scene content (billboards,
        // particles, ground marker): flush it into the HDR target so it is
        // bloomed with the scene, then composite to the backbuffer.
        Flush2D();
        CompositeSceneToBackbuffer();
        compositedThisFrame_ = true;
    }
    // From here on every 2D flush goes straight to the backbuffer (unbloomed,
    // on top of the composite): HUD/nameplates/minimap/editor UI.
    backend_->BindDefaultTarget();
}

void Renderer::CompositeFrame() {
    if (!compositedThisFrame_) {
        CompositeSceneToBackbuffer();
        compositedThisFrame_ = true;
    } else {
        // EndScene already composited this frame; just draw any 2D the app
        // pushed after EndScene (the HUD) onto the backbuffer.
        Flush2D();
    }
}

bool Renderer::CaptureBloomComparison(std::vector<uint8_t>& bloomOff,
                                      std::vector<uint8_t>& bloomOn) {
    if (!backend_ || !hdrEnabled_ || !hdrRT_.Valid()) return false;
    const bool savedBloom = bloomEnabled_;
    // Both captures composite the same (resolved) HDR target WITHOUT the 2D
    // overlay, so the two buffers differ only by the bloom term; the HUD is
    // flushed once at the end (it is drawn on top of the composite and is not
    // bloomed).
    ResolveMainTarget();
    bloomEnabled_ = false;
    bloomRanThisFrame_ = false;
    CompositeToBackbuffer();
    bloomOff.resize(static_cast<size_t>(screenW_) * screenH_ * 4);
    backend_->CaptureFrame(screenW_, screenH_, bloomOff.data());
    bloomEnabled_ = savedBloom;
    bloomRanThisFrame_ = RunBloom();
    CompositeToBackbuffer();
    bloomOn.resize(static_cast<size_t>(screenW_) * screenH_ * 4);
    backend_->CaptureFrame(screenW_, screenH_, bloomOn.data());
    compositedThisFrame_ = true;
    Flush2D();
    return true;
}

bool Renderer::CaptureTonemapComparison(std::vector<uint8_t>& clamped,
                                        std::vector<uint8_t>& tonemapped) {
    if (!backend_ || !hdrEnabled_ || !hdrRT_.Valid()) return false;
    const bool savedTonemap = tonemapEnabled_;
    // Same-frame diff of the tone-mapping operator: composite the SAME
    // resolved HDR target twice, once with ACES+exposure and once with the
    // T3.6 clamp reference. Bloom runs once so it is identical in both images.
    ResolveMainTarget();
    bloomRanThisFrame_ = RunBloom();
    tonemapEnabled_ = false;
    CompositeToBackbuffer();
    clamped.resize(static_cast<size_t>(screenW_) * screenH_ * 4);
    backend_->CaptureFrame(screenW_, screenH_, clamped.data());
    tonemapEnabled_ = savedTonemap;
    CompositeToBackbuffer();
    tonemapped.resize(static_cast<size_t>(screenW_) * screenH_ * 4);
    backend_->CaptureFrame(screenW_, screenH_, tonemapped.data());
    compositedThisFrame_ = true;
    Flush2D();
    return true;
}

} // namespace neon::gfx
