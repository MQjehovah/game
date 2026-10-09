#version 450
#extension GL_GOOGLE_include_directive : require
// Procedural sky: vertical gradient (or an equirect HDRI), sun disc + halo,
// moon and a drifting FBM cloud layer.
layout(location = 0) in vec2 vUV;
layout(location = 0) out vec4 FragColor;

#include "engine_ubo.glsl"

layout(set = 1, binding = 0) uniform sampler2D uSkyTexture;

float hash21(vec2 p) {
    p = fract(p * vec2(123.34, 456.21));
    p += dot(p, p + 45.32);
    return fract(p.x * p.y);
}
float vnoise(vec2 p) {
    vec2 i = floor(p);
    vec2 f = fract(p);
    vec2 u = f * f * (3.0 - 2.0 * f);
    float a = hash21(i);
    float b = hash21(i + vec2(1.0, 0.0));
    float c = hash21(i + vec2(0.0, 1.0));
    float d = hash21(i + vec2(1.0, 1.0));
    return mix(mix(a, b, u.x), mix(c, d, u.x), u.y);
}
float fbm(vec2 p) {
    float v = 0.0;
    float amp = 0.5;
    for (int i = 0; i < 4; ++i) {
        v += amp * vnoise(p);
        p *= 2.0;
        amp *= 0.5;
    }
    return v;
}
// ---- Volumetric clouds: a raymarched FBM slab with sun transmittance. ------
float hash31(vec3 p) { return fract(sin(dot(p, vec3(12.9898, 78.233, 37.719))) * 43758.5453); }
float vnoise3(vec3 x) {
    vec3 i = floor(x);
    vec3 f = fract(x);
    f = f * f * (3.0 - 2.0 * f);
    return mix(mix(mix(hash31(i + vec3(0, 0, 0)), hash31(i + vec3(1, 0, 0)), f.x),
                   mix(hash31(i + vec3(0, 1, 0)), hash31(i + vec3(1, 1, 0)), f.x), f.y),
               mix(mix(hash31(i + vec3(0, 0, 1)), hash31(i + vec3(1, 0, 1)), f.x),
                   mix(hash31(i + vec3(0, 1, 1)), hash31(i + vec3(1, 1, 1)), f.x), f.y),
               f.z);
}
float fbm3(vec3 p) {
    float v = 0.0, a = 0.5;
    for (int i = 0; i < 4; ++i) { v += a * vnoise3(p); p *= 2.03; a *= 0.5; }
    return v;
}
const float CLOUD_BASE = 140.0;
const float CLOUD_TOP = 360.0;
float CloudDensity(vec3 p, float coverage) {
    float h = (p.y - CLOUD_BASE) / (CLOUD_TOP - CLOUD_BASE);
    if (h < 0.0 || h > 1.0) return 0.0;
    float profile = smoothstep(0.0, 0.12, h) * smoothstep(1.0, 0.55, h);
    vec3 q = p * 0.01 * max(eng.uCloudScale, 0.1);
    q.xz += vec2(eng.uTime * 0.02, eng.uTime * 0.008);
    float n = fbm3(q);
    n = 1.0 - abs(n * 2.0 - 1.0); // billow: rounded cumulus blobs
    return smoothstep(1.0 - clamp(coverage, 0.0, 1.0), 1.0, n) * profile;
}
float CloudLight(vec3 p, vec3 sunDir, float coverage) {
    float d = 0.0;
    float step = (CLOUD_TOP - CLOUD_BASE) * 0.15;
    vec3 q = p;
    for (int i = 0; i < 4; ++i) { q += sunDir * step; d += CloudDensity(q, coverage); }
    return exp(-d * 0.6);
}
vec3 InverseViewProjRay(vec2 ndc) {
    vec4 p0 = eng.uInvViewProj * vec4(ndc, 1.0, 1.0);
    vec4 p1 = eng.uInvViewProj * vec4(ndc, -1.0, 1.0);
    if (abs(p0.w) < 1e-6 || abs(p1.w) < 1e-6) return vec3(0.0, 0.0, 1.0);
    vec3 nearP = p0.xyz / p0.w;
    vec3 farP = p1.xyz / p1.w;
    return normalize(farP - nearP);
}
void main() {
    // vUV runs top-down in Vulkan, the engine matrices use GL NDC (y up).
    vec2 ndc = vec2(vUV.x * 2.0 - 1.0, 1.0 - vUV.y * 2.0);
    vec3 dir = InverseViewProjRay(ndc);
    float dy = clamp(dir.y * 0.5 + 0.5, 0.0, 1.0);

    vec3 col;
    if (eng.uSkyTextureValid != 0) {
        float u = atan(dir.z, dir.x) / (2.0 * 3.14159265) + 0.5;
        // Equirect v = polar angle from the zenith, using the direction's OWN y
        // ([-1,1]). Passing the gradient's 0..1 `dy` ran acos over [0,1] so v
        // peaked at 0.5: the sky was squeezed into the top half of the image and
        // the horizon sat at v=1/3 (GL twin in skybox.hpp).
        float v = acos(clamp(dir.y, -1.0, 1.0)) / 3.14159265;
        col = texture(uSkyTexture, vec2(u, v)).rgb;
    } else {
        col = mix(eng.uSkyHorizon, eng.uSkyTop, pow(dy, 1.0));
        float horizonGlow = pow(1.0 - abs(dir.y), 4.0);
        col = mix(col, eng.uSkyTop * 0.9 + eng.uSkyHorizon * 0.1, horizonGlow * 0.35);
    }

    if (eng.uSunVisible != 0) {
        vec3 sunDir = normalize(vec3(cos(eng.uSunPitch) * cos(eng.uSunYaw),
                                      sin(eng.uSunPitch),
                                      cos(eng.uSunPitch) * sin(eng.uSunYaw)));
        float cosAng = dot(dir, sunDir);
        float disc = smoothstep(0.9990, 0.9996, cosAng);
        float halo = pow(max(dot(dir, sunDir), 0.0), 16.0) * 0.35;
        col += vec3(1.0, 0.96, 0.85) * (disc * 3.0 + halo);
    }
    if (eng.uMoonVisible != 0) {
        vec3 sunDir = normalize(vec3(cos(eng.uSunPitch) * cos(eng.uSunYaw),
                                      sin(eng.uSunPitch),
                                      cos(eng.uSunPitch) * sin(eng.uSunYaw)));
        vec3 moonDir = mat3(1.0, 0.0, 0.0, 0.0, -1.0, 0.0, 0.0, 0.0, -1.0) * -sunDir;
        float disc = smoothstep(0.9992, 0.9996, dot(dir, moonDir));
        col += vec3(0.85, 0.90, 1.0) * disc * 1.2;
    }
    if (eng.uCloudsEnabled != 0 && dir.y > 0.002) {
        vec3 sunDir = normalize(vec3(cos(eng.uSunPitch) * cos(eng.uSunYaw),
                                      sin(eng.uSunPitch),
                                      cos(eng.uSunPitch) * sin(eng.uSunYaw)));
        vec3 cam = eng.uCamPos;
        float t0 = (CLOUD_BASE - cam.y) / dir.y;
        float t1 = (CLOUD_TOP - cam.y) / dir.y;
        if (t1 > 0.0) {
            t0 = max(t0, 0.0);
            // Fade near the horizon: the slab is effectively at infinity there
            // and under-sampled (banding), and real distant clouds vanish.
            float horizonFade = smoothstep(0.02, 0.16, dir.y);
            const int STEPS = 48;
            float dt = (t1 - t0) / float(STEPS);
            vec3 p = cam + dir * t0;
            float T = 1.0;
            vec3 scatter = vec3(0.0);
            // Henyey-Greenstein forward lobe for the silver lining + isotropic.
            float c = dot(dir, sunDir);
            float g = 0.55;
            float phase = mix((1.0 - g * g) /
                                  (4.0 * 3.14159265 * pow(1.0 + g * g - 2.0 * g * c, 1.5)),
                              1.0 / (4.0 * 3.14159265), 0.5);
            vec3 sunCol = vec3(1.02, 0.98, 0.92) * 3.4;
            vec3 ambCol = mix(eng.uSkyHorizon, eng.uSkyTop, 0.5) * 1.0 + vec3(0.32);
            for (int i = 0; i < STEPS; ++i) {
                float d = CloudDensity(p, eng.uCloudCoverage);
                if (d > 0.001) {
                    float lt = CloudLight(p, sunDir, eng.uCloudCoverage);
                    float e = exp(-d * 8.0 / float(STEPS));
                    scatter += T * (1.0 - e) * (ambCol + sunCol * lt * phase);
                    T *= e;
                    if (T < 0.02) break;
                }
                p += dir * dt;
            }
            col = mix(col, col * T + scatter, horizonFade);
        }
    }
    FragColor = vec4(col, 1.0);
}
