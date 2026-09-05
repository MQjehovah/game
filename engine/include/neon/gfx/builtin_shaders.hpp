#pragma once

// Built-in shader sources owned by the Renderer facade's draw path (3D mesh,
// instanced + lines programs). Kept in a dedicated header - exactly like
// bloom.hpp/ssao.hpp/volumetric.hpp/ssr.hpp hold their post sources - so
// renderer.cpp stays a manageable facade instead of carrying ~450 lines of
// raw GLSL string literals. Only the Renderer includes this.
//
// The SHADOW (depth/point-depth) and UI sources live next to their owning
// subsystems (shadow_system.cpp / draw_batch2d.cpp) instead.

namespace neon::gfx {

inline constexpr const char* kLitVertexShader = R"(
#version 330 core
layout(location = 0) in vec3 aPos;
layout(location = 1) in vec3 aNormal;
layout(location = 2) in vec2 aUV;
layout(location = 3) in vec4 aColor;
uniform vec2 uTiling; // UV repeat multiplier (default 1,1)
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
    vUV = aUV * uTiling;
    vColor = aColor;
    vViewZ = (uViewMatrix * vec4(vWorldPos, 1.0)).z;
    gl_Position = uMVP * p;
#else
    vWorldPos = (uModel * vec4(aPos, 1.0)).xyz;
    vNormal = (uNormalMat * vec4(aNormal, 0.0)).xyz;
    vUV = aUV * uTiling;
    vColor = aColor;
    vViewZ = (uViewMatrix * vec4(vWorldPos, 1.0)).z;
    gl_Position = uMVP * vec4(aPos, 1.0);
#endif
}
)";

inline constexpr const char* kLitFragmentShader = R"(
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
uniform sampler2D uGrassTex;
uniform sampler2D uNormalMap;
uniform vec4 uDirtColor;
uniform vec4 uRockColor;
uniform bool uHasGrassTex;
uniform vec4 uTint;
uniform bool uHasTexture;
uniform bool uHasMR;
uniform bool uHasAO;
uniform bool uHasEmissive;
uniform bool uHasNormalMap;
uniform float uNormalScale;
// glTF MASK / foliage card cutout: when uAlphaTest > 0 fragments with an albedo
// alpha below the threshold are discarded (crisp silhouette for grass/leaf/hair
// cards instead of a soft translucent stack).
uniform float uAlphaTest;
uniform float uAOStrength;
uniform float uEmissiveIntensity;
uniform float uShininess;
uniform float uMetallic;
uniform float uRoughness;
uniform vec3 uSunDir;
uniform vec3 uSunColor;
uniform float uAmbient;
uniform vec3 uAmbientColor;
uniform vec3 uAmbientGroundColor;
uniform sampler2D uIrradianceMap;
uniform sampler2D uPrefilteredMap;
uniform sampler2D uBrdfLUT;
uniform sampler2D uLightProbeAtlas;
uniform float uIblStrength;
uniform float uLightProbeRes;
uniform float uLightProbeInvMax;
uniform int uLightProbeEnabled;
uniform vec3 uLightProbeMin;
uniform vec3 uLightProbeExtent;
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
)" R"(
// A3 probe-field GI: trilinear-sample the baked 2D irradiance atlas by world
// position (mirrors CPU light_probe.cpp SampleProbeField exactly). The atlas is
// res x (res*res): tile (i,j,k) at texel (i, k*res + j). Irradiance is encoded
// LDR (value * uLightProbeInvMax). Disabled when uLightProbeEnabled == 0.
vec3 SampleLightProbeAtlas(vec3 wp) {
    vec3 u = clamp((wp - uLightProbeMin) / max(uLightProbeExtent, vec3(1e-5)) * uLightProbeRes -
                       0.5,
                   vec3(0.0), vec3(uLightProbeRes - 1.0));
    ivec3 i0 = ivec3(floor(u));
    ivec3 i1 = min(i0 + ivec3(1), ivec3(ivec3(uLightProbeRes) - 1));
    vec3 f = u - vec3(i0);
    float invRes = 1.0 / uLightProbeRes;
    float invRsq = 1.0 / (uLightProbeRes * uLightProbeRes);
    vec3 c000 = texture(uLightProbeAtlas, vec2((float(i0.x) + 0.5) * invRes,
                        (float(i0.z) * uLightProbeRes + float(i0.y) + 0.5) * invRsq)).rgb;
    vec3 c100 = texture(uLightProbeAtlas, vec2((float(i1.x) + 0.5) * invRes,
                        (float(i0.z) * uLightProbeRes + float(i0.y) + 0.5) * invRsq)).rgb;
    vec3 c010 = texture(uLightProbeAtlas, vec2((float(i0.x) + 0.5) * invRes,
                        (float(i0.z) * uLightProbeRes + float(i1.y) + 0.5) * invRsq)).rgb;
    vec3 c110 = texture(uLightProbeAtlas, vec2((float(i1.x) + 0.5) * invRes,
                        (float(i0.z) * uLightProbeRes + float(i1.y) + 0.5) * invRsq)).rgb;
    vec3 c001 = texture(uLightProbeAtlas, vec2((float(i0.x) + 0.5) * invRes,
                        (float(i1.z) * uLightProbeRes + float(i0.y) + 0.5) * invRsq)).rgb;
    vec3 c101 = texture(uLightProbeAtlas, vec2((float(i1.x) + 0.5) * invRes,
                        (float(i1.z) * uLightProbeRes + float(i0.y) + 0.5) * invRsq)).rgb;
    vec3 c011 = texture(uLightProbeAtlas, vec2((float(i0.x) + 0.5) * invRes,
                        (float(i1.z) * uLightProbeRes + float(i1.y) + 0.5) * invRsq)).rgb;
    vec3 c111 = texture(uLightProbeAtlas, vec2((float(i1.x) + 0.5) * invRes,
                        (float(i1.z) * uLightProbeRes + float(i1.y) + 0.5) * invRsq)).rgb;
    vec3 x00 = mix(c000, c100, f.x);
    vec3 x01 = mix(c010, c110, f.x);
    vec3 x10 = mix(c001, c101, f.x);
    vec3 x11 = mix(c011, c111, f.x);
    vec3 y0 = mix(x00, x01, f.y);
    vec3 y1 = mix(x10, x11, f.y);
    return mix(y0, y1, f.z) * uLightProbeInvMax;
}
void main() {
#ifdef TERRAIN_SPLAT
    // G4 terrain splatmap: layer a realistic grass texture, dirt color and rock
    // color by the vertex splat weights (vColor.r = grass, .g = dirt, .b = rock).
    vec3 grassAlbedo = uHasGrassTex ? texture(uGrassTex, vUV).rgb : vec3(1.0);
    vec3 albedoSplat = grassAlbedo * vColor.r + uDirtColor.rgb * vColor.g +
                       uRockColor.rgb * vColor.b;
    vec4 albedo = vec4(albedoSplat, 1.0);
    albedo *= uTint;
#else
    vec4 albedo = uHasTexture ? texture(uAlbedo, vUV) : vec4(1.0);
    albedo *= uTint * vColor;
    // Alpha cutout (grass/leaf cards): discard transparent fragments so the
    // blades have a crisp edge rather than a translucent quad outline.
    if (uAlphaTest > 0.0 && albedo.a < uAlphaTest) discard;
#endif
    vec3 N = normalize(vNormal);
    // A2 normal mapping without per-vertex tangents: reconstruct the tangent
    // basis from screen-space derivatives of world position + UV (the standard
    // dFdx/dFdy triangle method, good for the common single-UV mesh). The
    // normal map's z is the geometric normal axis by construction, so
    // orthonormalizing against N (rather than using B directly) avoids the
    // flipping artifact along UV seams for a backfacing/ignoring pitch. When no
    // map is bound (uHasNormalMap == 0) N is left as-authored.
    if (uHasNormalMap) {
        vec3 dp1 = dFdx(vWorldPos);
        vec3 dp2 = dFdy(vWorldPos);
        vec2 duv1 = dFdx(vUV);
        vec2 duv2 = dFdy(vUV);
        vec3 dp2perp = cross(dp2, N);
        vec3 dp1perp = cross(N, dp1);
        vec3 tangent = dp2perp * duv1.x + dp1perp * duv2.x;
        vec3 bitangent = dp2perp * duv1.y + dp1perp * duv2.y;
        float invMax = inversesqrt(max(dot(tangent, tangent), dot(bitangent, bitangent)));
        tangent *= invMax;
        bitangent *= invMax;
        // Re-orthonormalize bitangent against N so frames align with the
        // geometry normal (avoids shear on smoothed/skinned meshes).
        vec3 nrm = normalize(texture(uNormalMap, vUV).rgb * 2.0 - 1.0);
        nrm.xy *= uNormalScale;
        N = normalize(nrm.x * tangent + nrm.y * bitangent + nrm.z * N);
    }
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
    // A3 hemisphere ambient: split the legacy flat ambient into a sky/ground
    // gradient by the world normal's Y so upward-facing surfaces take the sky
    // tint (uAmbientColor) and downward-facing take a ground bounce
    // (uAmbientGroundColor). This is the cheap indirect-lighting quality step
    // that removes the flat grey-shaded look; a flat ambient is reproduced when
    // the ground color equals the sky color.
    vec3 hemiAmbient = mix(uAmbientGroundColor, uAmbientColor, clamp(N.y * 0.5 + 0.5, 0.0, 1.0));
    vec3 ambientLight = iblDiffuse + iblSpecular +
                        albedo.rgb * hemiAmbient * uAmbient * (1.0 - uIblStrength);
    // A3 probe-field GI: the baked light-probe irradiance adds scene-local
    // indirect light (point-light / sun bounce baked per probe) on top of the
    // sky-based IBL. Weighted by iblDiffuse's diffuse term only (kd) so it
    // fills the indirect contribution without double-counting specular.
    if (uLightProbeEnabled != 0) {
        ambientLight += kd * SampleLightProbeAtlas(vWorldPos) * albedo.rgb;
    }
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

inline constexpr const char* kUnlitVertexShader = R"(
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

inline constexpr const char* kUnlitFragmentShader = R"(
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

inline constexpr const char* kLitInstancedVertexShader = R"(
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

inline constexpr const char* kUnlitInstancedVertexShader = R"(
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

// Instanced variant with a per-instance RGBA color (attribute 8) for
// sprite/billboard particles that tint independently per instance.
inline constexpr const char* kUnlitInstancedColoredVertexShader = R"(
#version 330 core
layout(location = 0) in vec3 aPos;
layout(location = 2) in vec2 aUV;
layout(location = 3) in vec4 aColor;
layout(location = 4) in mat4 aInstance;
layout(location = 8) in vec4 aInstanceColor;
uniform mat4 uMVP;
out vec2 vUV;
out vec4 vColor;
out vec4 vInstanceColor;
void main() {
    vUV = aUV;
    vColor = aColor;
    vInstanceColor = aInstanceColor;
    gl_Position = uMVP * aInstance * vec4(aPos, 1.0);
}
)";

inline constexpr const char* kUnlitInstancedColoredFragmentShader = R"(
#version 330 core
in vec2 vUV;
in vec4 vColor;
in vec4 vInstanceColor;
out vec4 FragColor;
uniform sampler2D uAlbedo;
uniform vec4 uTint;
uniform bool uHasTexture;
void main() {
    vec4 tex = uHasTexture ? texture(uAlbedo, vUV) : vec4(1.0);
    FragColor = tex * uTint * vColor * vInstanceColor;
}
)";

inline constexpr const char* kLineVertexShader = R"(
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

inline constexpr const char* kLineFragmentShader = R"(
#version 330 core
in vec4 vColor;
out vec4 FragColor;
void main() {
    FragColor = vColor;
}
)";

} // namespace neon::gfx
