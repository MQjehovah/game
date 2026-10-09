#version 450
#extension GL_GOOGLE_include_directive : require
// Real water surface shader - SINGLE SOURCE for both backends (see
// tools/gen_vk_shaders.ps1 for SPIR-V and tools/gen_gl_shaders.ps1 for the GL
// transform). Scene uniforms come from the shared EngineUBO (`eng.`); the GL
// generator expands that block into loose uniforms and drops `eng.`.
// Algorithm: animated procedural wave normals + Fresnel blend between a depth
// body colour and the IBL environment reflection + a sharp sun highlight.
layout(location = 0) in vec3 vWorldPos;
layout(location = 1) in vec3 vNormal;
layout(location = 2) in vec2 vUV;
layout(location = 3) in vec4 vColor;
layout(location = 0) out vec4 FragColor;

#include "engine_ubo.glsl"

layout(set = 1, binding = 20) uniform sampler2D uIrradianceMap;
layout(set = 1, binding = 21) uniform sampler2D uPrefilteredMap;
layout(set = 1, binding = 22) uniform sampler2D uBrdfLUT;

// --- Octahedral IBL sampling (matches ibl.cpp OctahedralEncode; wrap-fold
// bilinear keeps the prefiltered atlas seam-free). ---------------------------
vec2 OctWrap(vec2 v) {
    return (1.0 - abs(v.yx)) * vec2(v.x >= 0.0 ? 1.0 : -1.0, v.y >= 0.0 ? 1.0 : -1.0);
}
vec2 OctEncode(vec3 n) {
    n /= max(abs(n.x) + abs(n.y) + abs(n.z), 1e-8);
    if (n.z < 0.0) n.xy = OctWrap(n.xy);
    return n.xy * 0.5 + 0.5;
}
vec3 OctDecode(vec2 uv) {
    vec2 e = uv * 2.0 - 1.0;
    vec3 n = vec3(e.x, e.y, 1.0 - abs(e.x) - abs(e.y));
    if (n.z < 0.0) n.xy = OctWrap(n.xy);
    return normalize(n);
}
vec3 OctBilinear(sampler2D s, vec2 uv, float v0, float v1) {
    ivec2 sz = textureSize(s, 0);
    float res = float(sz.x);
    vec2 p = uv * res - 0.5;
    vec2 f = fract(p);
    vec2 i = floor(p);
    vec3 acc = vec3(0.0);
    for (int j = 0; j < 2; ++j) {
        for (int k = 0; k < 2; ++k) {
            vec2 corner = (i + vec2(float(k), float(j)) + 0.5) / res;
            vec2 e = OctEncode(OctDecode(corner));
            int x = clamp(int(floor(e.x * res)), 0, sz.x - 1);
            int yLocal = clamp(int(floor(e.y * res)), 0, int(res) - 1);
            float vt = (float(yLocal) + 0.5) / res;
            int y = clamp(int(mix(v0, v1, vt) * float(sz.y)), 0, sz.y - 1);
            float w = (k == 0 ? 1.0 - f.x : f.x) * (j == 0 ? 1.0 - f.y : f.y);
            acc += texelFetch(s, ivec2(x, y), 0).rgb * w;
        }
    }
    return acc;
}
vec3 OctIblSampleRough(sampler2D s, vec3 dir, float rough) {
    vec2 ts = vec2(textureSize(s, 0));
    float levels = max(ts.y / max(ts.x, 1.0), 1.0);
    vec2 uv = OctEncode(dir);
    float lv = clamp(rough, 0.0, 1.0) * (levels - 1.0);
    float l0 = floor(lv);
    float l1 = min(l0 + 1.0, levels - 1.0);
    vec3 a = OctBilinear(s, uv, l0 / levels, (l0 + 1.0) / levels);
    vec3 b = OctBilinear(s, uv, l1 / levels, (l1 + 1.0) / levels);
    return mix(a, b, lv - l0);
}

// Three superimposed directional sine waves scrolling over world XZ.
vec3 WaterNormal(vec3 wp) {
    float t = eng.uTime * eng.uWaterParams.y;
    vec2 p = wp.xz * eng.uWaterParams.x;
    float a1 = p.x * 1.0 + t * 1.3;
    float a2 = p.y * 1.3 - t * 1.1;
    float a3 = (p.x + p.y) * 0.7 + t * 0.7;
    vec2 grad = vec2(cos(a1) + cos(a3) * 0.7, cos(a2) * 1.3 + cos(a3) * 0.7);
    grad *= eng.uWaterParams.z;
    return normalize(vec3(-grad.x, 1.0, -grad.y));
}

void main() {
    vec3 V = normalize(eng.uCamPos - vWorldPos);
    vec3 N = normalize(vNormal);
    // Perturb only roughly-horizontal water; vertical water walls keep N.
    if (abs(N.y) > 0.5) N = normalize(WaterNormal(vWorldPos) + N * 0.15);
    if (dot(N, V) < 0.0) N = -N;

    float ndv = max(dot(N, V), 1e-4);
    float fres = 0.02 + 0.98 * pow(1.0 - ndv, 5.0);

    vec3 R = reflect(-V, N);
    float roughU = clamp((0.06 - eng.uRoughnessMin) / max(1.0 - eng.uRoughnessMin, 1e-4), 0.0, 1.0);
    vec3 refl = OctIblSampleRough(uPrefilteredMap, R, roughU) * eng.uIblStrength;

    vec3 body = mix(eng.uWaterDeep.rgb, eng.uWaterShallow.rgb, clamp(1.0 - ndv, 0.0, 1.0));
    vec3 color = mix(body, refl, clamp(fres, 0.0, 1.0));

    vec3 L = normalize(-eng.uSunDir);
    vec3 H = normalize(L + V);
    color += eng.uSunColor * pow(max(dot(N, H), 0.0), eng.uWaterParams2.x) * eng.uWaterParams.w;

    color += body * mix(eng.uAmbientGroundColor, eng.uAmbientColor, clamp(N.y * 0.5 + 0.5, 0.0, 1.0)) *
             eng.uAmbient;
    color *= eng.uTint.rgb;

    float dist = length(vWorldPos - eng.uCamPos);
    float fog = (eng.uFogEnd > eng.uFogStart) ? smoothstep(eng.uFogStart, eng.uFogEnd, dist) : 0.0;
    color = mix(color, eng.uFogColor, fog);

    FragColor = vec4(color, 1.0);
}
