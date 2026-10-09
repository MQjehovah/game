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

// Four superimposed directional sine waves scrolling over world XZ. Returns
// xy = surface height gradient, z = relative height (used for crest foam).
vec3 WaterWaveSample(vec3 wp) {
    float t = eng.uTime * eng.uWaterParams.y;
    vec2 p = wp.xz * eng.uWaterParams.x;
    vec2 dirs[4] = vec2[4](normalize(vec2(1.0, 0.2)), normalize(vec2(-0.6, 0.9)),
                           normalize(vec2(0.3, -1.0)), normalize(vec2(0.8, 0.6)));
    float freqs[4] = float[4](1.0, 1.7, 2.6, 3.9);
    float amps[4] = float[4](1.0, 0.6, 0.4, 0.25);
    vec2 grad = vec2(0.0);
    float height = 0.0;
    for (int i = 0; i < 4; ++i) {
        float ph = dot(p, dirs[i]) * freqs[i] + t * freqs[i] * 0.9;
        grad += dirs[i] * (cos(ph) * amps[i] * freqs[i]);
        height += sin(ph) * amps[i];
    }
    grad *= eng.uWaterParams.z * 0.25;
    return vec3(grad, height);
}

void main() {
    vec3 V = normalize(eng.uCamPos - vWorldPos);
    vec3 N = normalize(vNormal);
    // Perturb only roughly-horizontal water; vertical water walls keep N.
    vec3 wave = WaterWaveSample(vWorldPos);
    if (abs(N.y) > 0.5) N = normalize(vec3(-wave.x, 1.0, -wave.y) + N * 0.15);
    if (dot(N, V) < 0.0) N = -N;

    float ndv = max(dot(N, V), 1e-4);
    float fres = 0.02 + 0.98 * pow(1.0 - ndv, 5.0);
    float dist = length(vWorldPos - eng.uCamPos);

    // Reflection: the environment map, progressively rougher with distance so
    // far water reflects a soft blur instead of a razor-sharp sky (and the
    // wave normals still break the near reflection up into ripples).
    vec3 R = reflect(-V, N);
    float reflRough = clamp(0.04 + dist * 0.004, 0.04, 0.45);
    float roughU = clamp((reflRough - eng.uRoughnessMin) / max(1.0 - eng.uRoughnessMin, 1e-4),
                         0.0, 1.0);
    vec3 refl = OctIblSampleRough(uPrefilteredMap, R, roughU) * eng.uIblStrength;

    vec3 body = mix(eng.uWaterDeep.rgb, eng.uWaterShallow.rgb, clamp(1.0 - ndv, 0.0, 1.0));
    vec3 color = mix(body, refl, clamp(fres, 0.0, 1.0));

    // Sun glitter: a sharp specular lobe on the wave normals.
    vec3 L = normalize(-eng.uSunDir);
    vec3 H = normalize(L + V);
    color += eng.uSunColor * pow(max(dot(N, H), 0.0), eng.uWaterParams2.x) * eng.uWaterParams.w;

    // Foam on wave crests (height above the mean), strongest at grazing view.
    float foam = smoothstep(1.3, 2.1, wave.z) * (0.25 + 0.5 * fres);
    color = mix(color, vec3(0.9, 0.95, 1.0), clamp(foam, 0.0, 0.85));

    color += body * mix(eng.uAmbientGroundColor, eng.uAmbientColor, clamp(N.y * 0.5 + 0.5, 0.0, 1.0)) *
             eng.uAmbient;
    color *= eng.uTint.rgb;

    float fog = (eng.uFogEnd > eng.uFogStart) ? smoothstep(eng.uFogStart, eng.uFogEnd, dist) : 0.0;
    color = mix(color, eng.uFogColor, fog);

    FragColor = vec4(color, 1.0);
}
