#version 450
#extension GL_GOOGLE_include_directive : require
// Lit PBR fragment shader. Ported 1:1 from the engine's GLSL 330 source
// (renderer.cpp kLitFragmentShader): same material / IBL / CSM / point-shadow
// math, uniforms read from the shared EngineUBO, samplers on set 1 bindings
// that match the renderer's texture units.
layout(location = 0) in vec3 vWorldPos;
layout(location = 1) in vec3 vNormal;
layout(location = 2) in vec2 vUV;
layout(location = 3) in vec4 vColor;
layout(location = 4) in float vViewZ;
layout(location = 0) out vec4 FragColor;

#include "engine_ubo.glsl"

layout(set = 1, binding = 0)  uniform sampler2D uAlbedo;
layout(set = 1, binding = 1)  uniform sampler2D uMR;
layout(set = 1, binding = 2)  uniform sampler2D uOcclusion;
layout(set = 1, binding = 3)  uniform sampler2D uEmissive;
layout(set = 1, binding = 4)  uniform sampler2D uShadowMap0;
layout(set = 1, binding = 5)  uniform sampler2D uShadowMap1;
layout(set = 1, binding = 6)  uniform sampler2D uShadowMap2;
layout(set = 1, binding = 8)  uniform sampler2D uPointShadowMap0;
layout(set = 1, binding = 9)  uniform sampler2D uPointShadowMap1;
layout(set = 1, binding = 10) uniform sampler2D uPointShadowMap2;
layout(set = 1, binding = 11) uniform sampler2D uPointShadowMap3;
layout(set = 1, binding = 12) uniform sampler2D uPointShadowMap4;
layout(set = 1, binding = 13) uniform sampler2D uPointShadowMap5;
layout(set = 1, binding = 14) uniform sampler2D uPointShadowMap6;
layout(set = 1, binding = 15) uniform sampler2D uPointShadowMap7;
layout(set = 1, binding = 16) uniform sampler2D uPointShadowMap8;
layout(set = 1, binding = 17) uniform sampler2D uPointShadowMap9;
layout(set = 1, binding = 18) uniform sampler2D uPointShadowMap10;
layout(set = 1, binding = 19) uniform sampler2D uPointShadowMap11;
layout(set = 1, binding = 20) uniform sampler2D uIrradianceMap;
layout(set = 1, binding = 21) uniform sampler2D uPrefilteredMap;
layout(set = 1, binding = 22) uniform sampler2D uBrdfLUT;

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
    float d0 = DecodeDepth(texture(sm, uv));
    float dx = DecodeDepth(texture(sm, uv + vec2(eng.uShadowTexel.x, 0.0)));
    float dy = DecodeDepth(texture(sm, uv + vec2(0.0, eng.uShadowTexel.y)));
    float slope = max(abs(dx - d0), abs(dy - d0));
    float bias = clamp(0.002 + slope, 0.002, 0.02);

    float lit = 0.0;
    for (int x = 0; x < 2; ++x) {
        for (int y = 0; y < 2; ++y) {
            vec2 off = (vec2(float(x), float(y)) - vec2(0.5)) * eng.uShadowTexel;
            lit += DecodeDepth(texture(sm, uv + off)) > lightDepth - bias ? 1.0 : 0.0;
        }
    }
    return lit / 4.0;
}
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
float PointShadowFactor(sampler2D sm, vec2 uv, float current, int taps) {
    float d0 = DecodeDepth(texture(sm, uv));
    float dx = DecodeDepth(texture(sm, uv + vec2(eng.uPointShadowTexel.x, 0.0)));
    float dy = DecodeDepth(texture(sm, uv + vec2(0.0, eng.uPointShadowTexel.y)));
    float slope = max(abs(dx - d0), abs(dy - d0));
    float bias = clamp(0.003 + slope, 0.003, 0.03);
    float lit = 0.0;
    if (taps == 1) {
        lit = d0 > current - bias ? 1.0 : 0.0;
    } else {
        for (int x = 0; x < 2; ++x) {
            for (int y = 0; y < 2; ++y) {
                vec2 off = (vec2(float(x), float(y)) - vec2(0.5)) * eng.uPointShadowTexel;
                lit += DecodeDepth(texture(sm, uv + off)) > current - bias ? 1.0 : 0.0;
            }
        }
        lit /= 4.0;
    }
    return lit;
}
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
    vec4 albedo = (eng.uHasTexture != 0) ? texture(uAlbedo, vUV) : vec4(1.0);
    albedo *= eng.uTint * vColor;
    vec3 N = normalize(vNormal);
    vec3 V = normalize(eng.uCamPos - vWorldPos);
    vec3 L = normalize(-eng.uSunDir);
    float ndl = max(dot(N, L), 0.0);
    vec3 H = normalize(L + V);
    float metallic = (eng.uHasMR != 0) ? texture(uMR, vUV).b : eng.uMetallic;
    float roughness = (eng.uHasMR != 0) ? texture(uMR, vUV).g : eng.uRoughness;
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
    vec3 iblIrradiance = texture(uIrradianceMap, vec2(0.5, N.y * 0.5 + 0.5)).rgb;
    vec3 iblDiffuse = kd * iblIrradiance * albedo.rgb * eng.uIblStrength;
    vec3 R = reflect(-V, N);
    float roughU = clamp((roughness - eng.uRoughnessMin) / (1.0 - eng.uRoughnessMin), 0.0, 1.0);
    vec3 prefiltered = texture(uPrefilteredMap, vec2(roughU, R.y * 0.5 + 0.5)).rgb;
    vec2 brdf = texture(uBrdfLUT, vec2(ndv, roughness)).rg;
    vec3 iblSpecular = prefiltered * (f0 * brdf.x + brdf.y) * eng.uIblStrength;
    vec3 ambientLight = iblDiffuse + iblSpecular + albedo.rgb * eng.uAmbient * (1.0 - eng.uIblStrength);
    if (eng.uHasAO != 0) ambientLight *= mix(1.0, texture(uOcclusion, vUV).r, eng.uAOStrength);
    vec3 color = (kd * albedo.rgb + spec) * eng.uSunColor * ndl + ambientLight;
    if (eng.uHasEmissive != 0) color += texture(uEmissive, vUV).rgb * eng.uEmissiveIntensity;
    for (int i = 0; i < 8; ++i) {
        if (i >= eng.uPointCount) break;
        vec3 toL = eng.uPointPos[i] - vWorldPos;
        float d = length(toL);
        float atten = clamp(1.0 - d / eng.uPointRadius[i], 0.0, 1.0);
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
        vec3 pContrib = (pKd * albedo.rgb + pSpec) * eng.uPointColor[i] * pndl * atten;
        if (eng.uPointShadowEnabled != 0 && i < eng.uPointShadowLightCount) {
            pContrib *= PointShadowForLight(i, vWorldPos, eng.uPointPos[i], eng.uPointRadius[i]);
        }
        color += pContrib;
    }
    if (eng.uPlayerLightEnabled != 0) {
        vec3 toL = eng.uPlayerLightPos - vWorldPos;
        float d = length(toL);
        float atten = clamp(1.0 - d / eng.uPlayerLightRadius, 0.0, 1.0);
        atten *= atten;
        vec3 pl = toL / max(d, 1e-4);
        float pndl = max(dot(N, pl), 0.0);
        color += albedo.rgb * eng.uPlayerLightColor * pndl * atten;
    }
    float dist = length(vWorldPos - eng.uCamPos);
    float fog = smoothstep(eng.uFogStart, eng.uFogEnd, dist);
    color = mix(color, eng.uFogColor, fog);

    float shadow = 1.0;
    if (eng.uShadowEnabled != 0) {
        float viewDepth = -vViewZ;
        int cascade = viewDepth < eng.uCascadeSplits.x ? 0 : (viewDepth < eng.uCascadeSplits.y ? 1 : 2);
        vec4 sp;
        if (cascade == 0) sp = eng.uLightVP[0] * vec4(vWorldPos, 1.0);
        else if (cascade == 1) sp = eng.uLightVP[1] * vec4(vWorldPos, 1.0);
        else sp = eng.uLightVP[2] * vec4(vWorldPos, 1.0);
        vec3 ndc = sp.xyz / sp.w;
        if (ndc.x > -1.0 && ndc.x < 1.0 && ndc.y > -1.0 && ndc.y < 1.0 && ndc.z > -1.0 &&
            ndc.z < 1.0) {
            vec3 sc = ndc * 0.5 + 0.5;
            if (cascade == 0) shadow = ShadowFactor(uShadowMap0, sc.xy, sc.z);
            else if (cascade == 1) shadow = ShadowFactor(uShadowMap1, sc.xy, sc.z);
            else shadow = ShadowFactor(uShadowMap2, sc.xy, sc.z);
        }
    }
    color = (color - ambientLight) * shadow + ambientLight;
    FragColor = vec4(color, albedo.a);
}
