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

// Binding index == renderer texture unit (the backend writes descriptor i to
// dstBinding i). Units 1 is the terrain grass splat and 5..7 are the CSM
// cascades, so these must NOT be renumbered to 1..6.
layout(set = 1, binding = 0)  uniform sampler2D uAlbedo;
layout(set = 1, binding = 1)  uniform sampler2D uGrassTex; // TERRAIN_SPLAT only
layout(set = 1, binding = 2)  uniform sampler2D uMR;
layout(set = 1, binding = 3)  uniform sampler2D uOcclusion;
layout(set = 1, binding = 4)  uniform sampler2D uEmissive;
layout(set = 1, binding = 5)  uniform sampler2D uShadowMap0;
layout(set = 1, binding = 6)  uniform sampler2D uShadowMap1;
layout(set = 1, binding = 7)  uniform sampler2D uShadowMap2;
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
// Material maps the renderer binds at their texture units: 23 = normal map
// (Material::normalMap) and 24 = baked light-probe irradiance atlas. The
// set-1 layout declares all 25 units, and an unbound unit reads the white
// fallback texture, so both stay inert until the renderer hands one over.
layout(set = 1, binding = 23) uniform sampler2D uNormalMap;
layout(set = 1, binding = 24) uniform sampler2D uLightProbeAtlas;

// A3 probe-field GI: trilinear-sample the baked 2D irradiance atlas by world
// position (mirrors CPU light_probe.cpp SampleProbeField exactly). The atlas is
// res x (res*res): tile (i,j,k) at texel (i, k*res + j). Irradiance is encoded
// LDR (value * uLightProbeInvMax). Disabled when uLightProbeEnabled == 0.
vec3 SampleLightProbeAtlas(vec3 wp) {
    vec3 u = clamp((wp - eng.uLightProbeMin) /
                       max(eng.uLightProbeExtent, vec3(1e-5)) * eng.uLightProbeRes - 0.5,
                   vec3(0.0), vec3(eng.uLightProbeRes - 1.0));
    ivec3 i0 = ivec3(floor(u));
    ivec3 i1 = min(i0 + ivec3(1), ivec3(ivec3(eng.uLightProbeRes) - 1));
    vec3 f = u - vec3(i0);
    float invRes = 1.0 / eng.uLightProbeRes;
    float invRsq = 1.0 / (eng.uLightProbeRes * eng.uLightProbeRes);
    vec3 c000 = texture(uLightProbeAtlas, vec2((float(i0.x) + 0.5) * invRes,
                        (float(i0.z) * eng.uLightProbeRes + float(i0.y) + 0.5) * invRsq)).rgb;
    vec3 c100 = texture(uLightProbeAtlas, vec2((float(i1.x) + 0.5) * invRes,
                        (float(i0.z) * eng.uLightProbeRes + float(i0.y) + 0.5) * invRsq)).rgb;
    vec3 c010 = texture(uLightProbeAtlas, vec2((float(i0.x) + 0.5) * invRes,
                        (float(i0.z) * eng.uLightProbeRes + float(i1.y) + 0.5) * invRsq)).rgb;
    vec3 c110 = texture(uLightProbeAtlas, vec2((float(i1.x) + 0.5) * invRes,
                        (float(i0.z) * eng.uLightProbeRes + float(i1.y) + 0.5) * invRsq)).rgb;
    vec3 c001 = texture(uLightProbeAtlas, vec2((float(i0.x) + 0.5) * invRes,
                        (float(i1.z) * eng.uLightProbeRes + float(i0.y) + 0.5) * invRsq)).rgb;
    vec3 c101 = texture(uLightProbeAtlas, vec2((float(i1.x) + 0.5) * invRes,
                        (float(i1.z) * eng.uLightProbeRes + float(i0.y) + 0.5) * invRsq)).rgb;
    vec3 c011 = texture(uLightProbeAtlas, vec2((float(i0.x) + 0.5) * invRes,
                        (float(i1.z) * eng.uLightProbeRes + float(i1.y) + 0.5) * invRsq)).rgb;
    vec3 c111 = texture(uLightProbeAtlas, vec2((float(i1.x) + 0.5) * invRes,
                        (float(i1.z) * eng.uLightProbeRes + float(i1.y) + 0.5) * invRsq)).rgb;
    vec3 x00 = mix(c000, c100, f.x);
    vec3 x01 = mix(c010, c110, f.x);
    vec3 x10 = mix(c001, c101, f.x);
    vec3 x11 = mix(c011, c111, f.x);
    vec3 y0 = mix(x00, x01, f.y);
    vec3 y1 = mix(x10, x11, f.y);
    // The atlas stores irradiance PRE-multiplied by 1/maxIrr; divide to undo
    // the encode scale (multiplying again darkened GI by maxIrr^2).
    return mix(y0, y1, f.z) / eng.uLightProbeInvMax;
}

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
// Interleaved gradient noise (Jimenez 2014): a cheap screen-space dither used
// to rotate the shadow kernel per pixel.
float Ign(vec2 p) {
    return fract(52.9829189 * fract(dot(p, vec2(0.06711056, 0.00583715))));
}
// 12-tap Poisson disk of unit radius, rotated per pixel by Ign above.
const vec2 kShadowDisk[12] = vec2[12](
    vec2(-0.326, -0.406), vec2(-0.840, -0.074), vec2(-0.696, 0.457),
    vec2(-0.203, 0.621), vec2(0.962, -0.195), vec2(0.473, -0.480),
    vec2(0.519, 0.767), vec2(0.185, -0.893), vec2(0.507, 0.064),
    vec2(0.896, 0.412), vec2(-0.322, -0.933), vec2(-0.792, -0.598));
// Rotated-Poisson PCF with a PCSS penumbra estimate (see the GL twin in
// builtin_shaders.hpp for the full rationale): the same 12 taps both estimate
// visibility and average the occluder depth, so the blocker search is free.
float ShadowFactor(sampler2D sm, vec2 uv, float lightDepth, float biasUnit) {
    float d0 = DecodeDepth(texture(sm, uv));
    float dx = DecodeDepth(texture(sm, uv + vec2(eng.uShadowTexel.x, 0.0)));
    float dy = DecodeDepth(texture(sm, uv + vec2(0.0, eng.uShadowTexel.y)));
    float slope = max(abs(dx - d0), abs(dy - d0));
    // biasUnit = one shadow texel in this cascade's NORMALIZED depth
    // (texelWorld / zRange): clamping to whole texels keeps the world-space
    // bias ~[0.2, 8] texels no matter how far the cascade's depth range
    // stretches (raw normalized constants became whole world units on a large
    // scene - contact shadows detached from their casters).
    float bias = clamp(0.5 * slope + 0.5 * biasUnit, 0.2 * biasUnit, 8.0 * biasUnit);

    float ang = Ign(gl_FragCoord.xy) * 6.2831853;
    float ca = cos(ang);
    float sa = sin(ang);
    mat2 rot = mat2(ca, sa, -sa, ca);
    // Minimum 1.5-texel penumbra (see the GL twin): hides IGN grain and the
    // coarse cascades' stair-steps; uShadowSoftness still scales it up.
    vec2 base = eng.uShadowTexel * max(eng.uShadowSoftness, 0.0);
    if (eng.uShadowSoftness <= 0.0) {
        float lit1 = 0.0;
        for (int x = 0; x < 2; ++x) {
            for (int y = 0; y < 2; ++y) {
                vec2 off = (vec2(float(x), float(y)) - vec2(0.5)) * eng.uShadowTexel;
                lit1 += DecodeDepth(texture(sm, uv + off)) > lightDepth - bias ? 1.0 : 0.0;
            }
        }
        return lit1 / 4.0;
    }

    float lit = 0.0;
    float sumBlocker = 0.0;
    int blockers = 0;
    for (int i = 0; i < 12; ++i) {
        float d = DecodeDepth(texture(sm, uv + rot * kShadowDisk[i] * base));
        lit += d > lightDepth - bias ? 1.0 : 0.0;
        if (d < lightDepth - bias) {
            sumBlocker += d;
            blockers += 1;
        }
    }
    float shadow = lit / 12.0;
    if (blockers == 0) return shadow;

    float avgBlocker = sumBlocker / float(blockers);
    float penumbra = clamp((lightDepth - avgBlocker) / max(avgBlocker, 1e-4), 0.0, 1.0);
    float radius = penumbra * 6.0;
    if (radius < 0.75) return shadow;
    vec2 wide = eng.uShadowTexel * radius;
    float soft = 0.0;
    for (int i = 0; i < 8; ++i) {
        float d = DecodeDepth(texture(sm, uv + rot * kShadowDisk[i] * wide));
        soft += d > lightDepth - bias ? 1.0 : 0.0;
    }
    return mix(shadow, min(shadow, soft / 8.0), clamp(radius / 3.0, 0.0, 1.0));
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
// Shadow factor from one directional cascade: projects the world position into
// the cascade's light space and resolves shadow + penumbra. Fragments outside
// the cascade's ortho box return "lit" - the box always covers the whole camera
// slice now (see csm.cpp), so that only happens past the shadow distance.
float CascadeShadow(int c, vec3 worldPos, vec3 norm) {
    float texelWorld = c == 0 ? eng.uShadowTexelWorld.x
                      : (c == 1 ? eng.uShadowTexelWorld.y : eng.uShadowTexelWorld.z);
    // Cascade depth range from the light VP: the z row of (ortho*lightView)
    // is (2/zRange) * unit-light-forward, so its LENGTH recovers the range the
    // packed RGBA8 depth is normalized over. (abs([2].z) alone overestimated
    // zRange ~2x for a tilted sun - the bias collapsed, receivers self-shadowed.)
    float zRange = 2.0 / max(length(vec3(eng.uLightVP[c][0].z, eng.uLightVP[c][1].z,
                                         eng.uLightVP[c][2].z)), 1e-8);
    float biasUnit = texelWorld / zRange;
    vec3 p = worldPos + norm * texelWorld * eng.uShadowNormalOffset;
    vec4 sp = eng.uLightVP[c] * vec4(p, 1.0);
    vec3 ndc = sp.xyz / sp.w;
    if (!(ndc.x > -1.0 && ndc.x < 1.0 && ndc.y > -1.0 && ndc.y < 1.0 &&
          ndc.z > -1.0 && ndc.z < 1.0)) {
        return 1.0;
    }
    vec3 sc = ndc * 0.5 + 0.5;
    if (c == 0) return ShadowFactor(uShadowMap0, sc.xy, sc.z, biasUnit);
    if (c == 1) return ShadowFactor(uShadowMap1, sc.xy, sc.z, biasUnit);
    return ShadowFactor(uShadowMap2, sc.xy, sc.z, biasUnit);
}
void main() {
#ifdef TERRAIN_SPLAT
    // G4 terrain splatmap (GL parity): layer a grass texture, a dirt colour and
    // a rock colour by the vertex splat weights (vColor.r = grass, .g = dirt,
    // .b = rock). Terrain chunks draw with this variant; the plain lit path is
    // unchanged.
    vec3 grassAlbedo = (eng.uHasGrassTex != 0) ? texture(uGrassTex, vUV).rgb : vec3(1.0);
    vec3 splatAlbedo = grassAlbedo * vColor.r + eng.uDirtColor.rgb * vColor.g +
                       eng.uRockColor.rgb * vColor.b;
    vec4 albedo = vec4(splatAlbedo, 1.0);
    albedo *= eng.uTint;
#else
    vec4 albedo = (eng.uHasTexture != 0) ? texture(uAlbedo, vUV) : vec4(1.0);
    albedo *= eng.uTint * vColor;
    // glTF MASK / foliage card cutout: discard transparent fragments so leaf
    // blades keep a crisp edge instead of a translucent quad outline.
    if (eng.uAlphaTest > 0.0 && albedo.a < eng.uAlphaTest) discard;
#endif
    vec3 N = normalize(vNormal);
    // A2 normal mapping without per-vertex tangents: reconstruct the tangent
    // basis from screen-space derivatives of world position + UV (the standard
    // dFdx/dFdy triangle method, good for the common single-UV mesh). The map's
    // z is the geometric normal axis by construction, so orthonormalizing
    // against N avoids the flipping artifact along UV seams. When no map is
    // bound (uHasNormalMap == 0) N is left as authored.
    if (eng.uHasNormalMap != 0) {
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
        vec3 nrm = normalize(texture(uNormalMap, vUV).rgb * 2.0 - 1.0);
        nrm.xy *= eng.uNormalScale;
        N = normalize(nrm.x * tangent + nrm.y * bitangent + nrm.z * N);
    }
    vec3 V = normalize(eng.uCamPos - vWorldPos);
    // Two-sided shading normal: a mirrored transform, an inverted-winding mesh
    // or a plane seen from behind leaves dot(N, V) < 0, which lit the surface
    // with a normal pointing away from the camera (flat IBL-only ground) and
    // buried the shadow receiver under its own depth via the normal offset.
    if (dot(N, V) < 0.0) N = -N;
    // Receiver-offset normal for shadow sampling: the GEOMETRIC normal, not the
    // normal-mapped one (a bumped normal's lateral component slides the sample
    // along the surface - light leaks at silhouettes / acne bands). Viewer-
    // flipped like the shading normal above.
    vec3 shadowNormal = normalize(vNormal);
    if (dot(shadowNormal, V) < 0.0) shadowNormal = -shadowNormal;
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
    // A3 hemisphere ambient: split the flat ambient into a sky/ground gradient by
    // the world normal's Y so upward faces take the sky tint (uAmbientColor) and
    // downward faces a ground bounce (uAmbientGroundColor). Equal colours
    // reproduce the old flat ambient exactly.
    vec3 hemiAmbient =
        mix(eng.uAmbientGroundColor, eng.uAmbientColor, clamp(N.y * 0.5 + 0.5, 0.0, 1.0));
    vec3 ambientLight = iblDiffuse + iblSpecular +
                        albedo.rgb * hemiAmbient * eng.uAmbient * (1.0 - eng.uIblStrength);
    // A3 probe-field GI: baked scene-local indirect light on top of the sky IBL,
    // weighted by the diffuse term only so specular is not double counted.
    if (eng.uLightProbeEnabled != 0) {
        ambientLight += kd * SampleLightProbeAtlas(vWorldPos) * albedo.rgb;
    }
    if (eng.uHasAO != 0) ambientLight *= mix(1.0, texture(uOcclusion, vUV).r, eng.uAOStrength);
    vec3 color = (kd * albedo.rgb + spec) * eng.uSunColor * ndl + ambientLight;
    // Emissive, tint self-glow, point lights and the player light are NOT part
    // of the sun term: kept out of the sun shadow composite below (a glowing
    // pickup or torch-lit wall must not go dark because the sun is blocked).
    vec3 extraLight = vec3(0.0);
    if (eng.uHasEmissive != 0) extraLight += texture(uEmissive, vUV).rgb * eng.uEmissiveIntensity;
    // Tint self-glow: tint components pushed above 1.0 emit light directly (beacon
    // lamps, glowing pickups). The term lands in the HDR target, so intensity above
    // the bloom threshold reads as an actual light source.
    vec3 tintGlow = max(eng.uTint.rgb - vec3(1.0), vec3(0.0));
    if (tintGlow.r + tintGlow.g + tintGlow.b > 0.0) {
        extraLight += tintGlow * albedo.rgb * eng.uEmissiveIntensity;
    }
    for (int i = 0; i < 8; ++i) {
        if (i >= eng.uPointCount) break;
        if (eng.uPointRadius[i] <= 0.0) continue; // unset/off slot (kills 0/0 NaN)
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
        extraLight += pContrib;
    }
    if (eng.uPlayerLightEnabled != 0) {
        vec3 toL = eng.uPlayerLightPos - vWorldPos;
        float d = length(toL);
        float atten = clamp(1.0 - d / eng.uPlayerLightRadius, 0.0, 1.0);
        atten *= atten;
        vec3 pl = toL / max(d, 1e-4);
        float pndl = max(dot(N, pl), 0.0);
        extraLight += albedo.rgb * eng.uPlayerLightColor * pndl * atten;
    }
    float dist = length(vWorldPos - eng.uCamPos);
    // A degenerate range (end <= start, e.g. 0/0 from a data-driven stack) must
    // mean 'no fog': smoothstep(0, 0, d) divides by zero and washes the frame out.
    float fog = (eng.uFogEnd > eng.uFogStart) ? smoothstep(eng.uFogStart, eng.uFogEnd, dist) : 0.0;

    float shadow = 1.0;
    if (eng.uShadowEnabled != 0) {
        float viewDepth = -vViewZ;
        float s0 = eng.uCascadeSplits.x;
        float s1 = eng.uCascadeSplits.y;
        float s2 = eng.uCascadeSplits.z;
        int cascade = viewDepth < s0 ? 0 : (viewDepth < s1 ? 1 : 2);
        shadow = CascadeShadow(cascade, vWorldPos, shadowNormal);
        // Cross-fade across each split so the cascade switch - which also changes
        // the texel size and thus the penumbra - is not a hard seam. The mix is
        // DITHERED with the per-pixel IGN below: the two cascades' shadow factors
        // disagree slightly inside the band (different texel size), and a solid
        // mix of that disagreement reads as a horizontal stripe across flat
        // ground. Dithering + a wider band turns the stripe into fine noise.
        float b0 = max(s0 * 0.25, 0.02);
        float b1 = max(s1 * 0.25, 0.02);
        float dth = Ign(gl_FragCoord.xy) - 0.5;
        if (viewDepth > s0 - b0 && viewDepth < s0 + b0) {
            float t = clamp(smoothstep(s0 - b0, s0 + b0, viewDepth) + dth * 0.7,
                            0.0, 1.0);
            shadow = mix(CascadeShadow(0, vWorldPos, shadowNormal),
                         CascadeShadow(1, vWorldPos, shadowNormal), t);
        } else if (viewDepth > s1 - b1 && viewDepth < s1 + b1) {
            float t = clamp(smoothstep(s1 - b1, s1 + b1, viewDepth) + dth * 0.7,
                            0.0, 1.0);
            shadow = mix(CascadeShadow(1, vWorldPos, shadowNormal),
                         CascadeShadow(2, vWorldPos, shadowNormal), t);
        }
        // Fade the shadow out at the end of the last cascade so the shadow
        // distance does not end in a hard line across the terrain.
        shadow = mix(1.0, shadow,
                     1.0 - smoothstep(s2 * 0.85, s2, viewDepth));
        // NEON_SHADOW_DEBUG=2: cascade index view (blue = 0, green = 1, red = 2).
        if (eng.uShadowDebug == 2) {
            FragColor = vec4(viewDepth < s0 ? vec3(0.1, 0.1, 0.9)
                             : (viewDepth < s1 ? vec3(0.1, 0.8, 0.1)
                                               : vec3(0.9, 0.1, 0.1)),
                             1.0);
            return;
        }
        // NEON_SHADOW_DEBUG=3: albedo ALPHA visualization (white = a=1,
        // black = a=0). Diagnoses MASK materials where the cutout behaves
        // differently per backend (the Rift water-layer hunt).
        if (eng.uShadowDebug == 3) {
            FragColor = vec4(vec3(albedo.a), 1.0);
            return;
        }
        // NEON_SHADOW_DEBUG=4: sampled texture LOD (black=0 .. white=9+).
        if (eng.uShadowDebug == 4) {
            FragColor = vec4(vec3(clamp(textureQueryLod(uAlbedo, vUV).x * 0.1, 0.0, 1.0)), 1.0);
            return;
        }
    }
    if (eng.uReceiveShadow == 0) shadow = 1.0;
    // NEON_SHADOW_DEBUG=1: show the raw cascade shadow factor instead of the
    // shaded result (white = sun reaches the surface, black = fully occluded).
    if (eng.uShadowDebug != 0) { FragColor = vec4(vec3(shadow), 1.0); return; }
    // Only the sun term is shadowed; ambient/sky stays unshadowed so shadowed
    // areas read as dim rather than black. Clamping the sun term to >= 0 first
    // keeps a surface whose lit colour is darker than the ambient from turning
    // BRIGHTER where it is shadowed (pale "ghost" shadows).
    vec3 sunTerm = max(color - ambientLight, vec3(0.0));
    color = sunTerm * shadow + ambientLight;
    // Emissive / glow / local lights survive the sun shadow; then distance fog
    // fades BOTH the shadowed and lit result (fog used to be mixed in before
    // the composite, which let the sun-shadow term darken the fog itself).
    color += extraLight;
    color = mix(color, eng.uFogColor, fog);
    // Selection / edge glow: Fresnel rim on the silhouette. Applied last (after
    // fog + shadow) so a highlighted unit stays visible in shadow or fog; the
    // albedo tint keeps it reading as the mesh's edge rather than a flat disc.
    if (eng.uHighlightStrength > 0.0) {
        float rim = 1.0 - clamp(dot(normalize(N), normalize(V)), 0.0, 1.0);
        color += eng.uHighlightColor * mix(albedo.rgb, vec3(1.0), 0.5) *
                 pow(rim, 2.5) * eng.uHighlightStrength;
    }
    FragColor = vec4(color, albedo.a);
}
