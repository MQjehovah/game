#pragma once
#include <cmath>
#include "neon/math/vec3.hpp"

// A4 procedural skybox. The sky is drawn as a fullscreen background quad whose
// fragment shader reconstructs the world-space view ray per pixel (from the
// camera inverse view-projection) and evaluates a procedural sky: the vertical
// skyTop/skyHorizon gradient, a sun disc + halo (from the directional-light
// direction), a moon disc (opposite the sun), and a simple FBM cloud layer.
//
// Living here (not inside renderer.cpp) so the GLSL source strings stay a
// self-contained unit the renderer can compile, mirroring how bloom.hpp /
// ssao.hpp keep their post sources. The CPU-side helper (InverseViewProjRay)
// mirrors the shader's ray reconstruction exactly so tests can pin the math
// headlessly without a GL context.
//
// View-direction awareness replaces the old screen-space vertical gradient
// (which did NOT follow the camera: rotating yaw/pitch left the gradient glued
// to the screen). Sun/moon/clouds are new. The gradient + IBL paths are
// unchanged; this shader only adds atmosphere when a directional/sun is set.

namespace neon::gfx {

// Reconstructs the camera-space ray direction at normalized device coords
// (x,y in [-1,1]) using the inverse of the (view * projection) matrix used to
// render the scene (CLIP row = {m3, m7, m11, m15} is the camera-space
// translation). Mirrors the GLSL `InverseViewProjRay` in kSkyboxFragmentShader
// so the CPU model (used by tests/any host-side probe) matches the GPU exactly.
inline math::Vec3 InverseViewProjRay(const math::Mat4& invViewProj, float ndcX, float ndcY) {
    const math::Vec4 p0 = invViewProj.TransformVec4({ndcX, ndcY, 1.0f, 1.0f});
    const math::Vec4 p1 = invViewProj.TransformVec4({ndcX, ndcY, -1.0f, 1.0f});
    if (std::fabs(p0.w) < 1e-6f || std::fabs(p1.w) < 1e-6f) return {0.0f, 0.0f, 1.0f};
    const math::Vec3 nearP{p0.x / p0.w, p0.y / p0.w, p0.z / p0.w};
    const math::Vec3 farP{p1.x / p1.w, p1.y / p1.w, p1.z / p1.w};
    return (farP - nearP).Normalized();
}

// --- Built-in skybox shaders ------------------------------------------------
// Fullscreen NDC quad (postQuadMesh_, uv 0..1, uMVP = identity). The vertex
// shader mirrors kPostVertexShader; the fragment reconstructs the view ray and
// evaluates the procedural sky. uInvViewProj inverts (view * projection) for
// the active 3D camera.

inline constexpr const char* kSkyboxVertexShader = R"(
#version 330 core
layout(location = 0) in vec3 aPos;
layout(location = 2) in vec2 aUV;
uniform mat4 uMVP;
out vec2 vUV;
void main() {
    vUV = aUV;
    gl_Position = uMVP * vec4(aPos, 1.0);
}
)";

inline constexpr const char* kSkyboxFragmentShader = R"(
#version 330 core
in vec2 vUV;
out vec4 FragColor;
uniform mat4 uInvViewProj;      // inverse(view * projection) for the scene camera
uniform vec3 uCamPos;
// World-space view rays for the 4 NDC corners (bilinear-interpolated in the
// fragment). Passed explicitly because the affine Mat4 inverse is wrong for a
// perspective VP and reconstructing in-shader is error-prone.
uniform vec3 uRayBL;
uniform vec3 uRayBR;
uniform vec3 uRayTR;
uniform vec3 uRayTL;
uniform sampler2D uSkyTexture;  // optional HDRI (equirect); unused when invalid
uniform int uSkyTextureValid;
uniform vec3 uSkyTop;           // zenith colour
uniform vec3 uSkyHorizon;       // horizon colour
uniform float uSunYaw;          // sun azimuth (radians)
uniform float uSunPitch;        // sun elevation (radians); + = above horizon
uniform int uSunVisible;        // 1 = draw the sun disc + halo
uniform int uMoonVisible;       // 1 = draw the moon disc (locked opposite sun)
uniform int uCloudsEnabled;     // 1 = procedural cloud layer
uniform float uCloudCoverage;   // 0..1 target cloudiness
uniform float uCloudScale;      // noise frequency
uniform float uTime;            // seconds (drift)
// 3D value noise + FBM (seeded 2D hash, tri-linear smoothstep interp).
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
// Volumetric clouds: raymarched FBM slab with sun transmittance.
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
    for (int i = 0; i < 5; ++i) { v += a * vnoise3(p); p *= 2.03; a *= 0.5; }
    return v;
}
const float CLOUD_BASE = 140.0;
const float CLOUD_TOP = 360.0;
float CloudDensity(vec3 p, float coverage) {
    float h = (p.y - CLOUD_BASE) / (CLOUD_TOP - CLOUD_BASE);
    if (h < 0.0 || h > 1.0) return 0.0;
    float profile = smoothstep(0.0, 0.12, h) * smoothstep(1.0, 0.55, h);
    vec3 q = p * 0.014 * max(uCloudScale, 0.1);
    q.xz += vec2(uTime * 0.02, uTime * 0.008);
    // Plain fbm + a low coverage threshold gives distinct blobs (billow peaks at
    // the noise mean and would fill every gap). Fine octave breaks the rims.
    float n = fbm3(q) + vnoise3(q * 5.0) * 0.06;
    float cov = clamp(coverage, 0.0, 1.0);
    float thr = 0.62 - cov * 0.4;
    return smoothstep(thr, thr + 0.10, n) * profile;
}
float CloudLight(vec3 p, vec3 sunDir, float coverage) {
    float d = 0.0;
    float step = (CLOUD_TOP - CLOUD_BASE) * 0.15;
    vec3 q = p;
    for (int i = 0; i < 4; ++i) { q += sunDir * step; d += CloudDensity(q, coverage); }
    return exp(-d * 0.6);
}
// Reconstructs the world-space ray direction at the fragment's NDC from the
// inverse(view*proj): unproject both -z (near) and +z (far) and take the
// direction. Mirrors CPU gfx::InverseViewProjRay.
vec3 InverseViewProjRay(vec2 ndc) {
    vec4 p0 = uInvViewProj * vec4(ndc, 1.0, 1.0);
    vec4 p1 = uInvViewProj * vec4(ndc, -1.0, 1.0);
    if (abs(p0.w) < 1e-6 || abs(p1.w) < 1e-6) return vec3(0.0, 0.0, 1.0);
    vec3 nearP = p0.xyz / p0.w;
    vec3 farP = p1.xyz / p1.w;
    return normalize(farP - nearP);
}
void main() {
    // The scene projection is Y-flipped (NDC +y is screen-down), so the corner
    // computed at ndc.y=-1 is the TOP ray and ndc.y=+1 the bottom one.
    vec3 dir = normalize(mix(mix(uRayTL, uRayTR, vUV.x),
                             mix(uRayBL, uRayBR, vUV.x), vUV.y));
    float dy = clamp(dir.y * 0.5 + 0.5, 0.0, 1.0);

    // HDRI (equirect) source when provided; otherwise the vertical gradient.
    vec3 col;
    if (uSkyTextureValid != 0) {
        float u = atan(dir.z, dir.x) / (2.0 * 3.14159265) + 0.5;
        // Equirect v = polar angle from the zenith, from the direction's OWN y
        // (range [-1,1]): zen=0, horizon=0.5, nadir=1. Passing the gradient's
        // 0..1 remap `dy` here ran acos over [0,1] -> v only reached 0.5, so the
        // whole sky was squeezed into the top image half and the horizon landed
        // at v=1/3 instead of 1/2.
        float v = acos(clamp(dir.y, -1.0, 1.0)) / 3.14159265;
        col = texture(uSkyTexture, vec2(u, v)).rgb;
    } else {
        col = mix(uSkyHorizon, uSkyTop, pow(dy, 1.0));
        // Soft banding-free horizon line: brighten the sky near the horizon.
        float horizonGlow = pow(1.0 - abs(dir.y), 4.0);
        col = mix(col, uSkyTop * 0.9 + uSkyHorizon * 0.1, horizonGlow * 0.35);
    }

    // Sun disc + halo, projected on the ray direction.
    if (uSunVisible != 0) {
        vec3 sunDir = normalize(vec3(cos(uSunPitch) * cos(uSunYaw),
                                      sin(uSunPitch),
                                      cos(uSunPitch) * sin(uSunYaw)));
        float cosAng = dot(dir, sunDir);
        // Tight flat disc with a hot core + wide soft halo.
        float disc = smoothstep(0.9990, 0.9996, cosAng);
        float halo = pow(max(dot(dir, sunDir), 0.0), 16.0) * 0.35;
        vec3 sunCol = vec3(1.0, 0.96, 0.85);
        col += sunCol * (disc * 3.0 + halo);
    }
    // Moon disc: opposite the sun (a cold, dimmer body).
    if (uMoonVisible != 0) {
        vec3 sunDir = normalize(vec3(cos(uSunPitch) * cos(uSunYaw),
                                      sin(uSunPitch),
                                      cos(uSunPitch) * sin(uSunYaw)));
        vec3 moonDir = mat3(1.0, 0.0, 0.0, 0.0, -1.0, 0.0, 0.0, 0.0, -1.0) * -sunDir;
        // Clamp moon above horizon-ish; hide if it's below the sun's line.
        float disc = smoothstep(0.9992, 0.9996, dot(dir, moonDir));
        col += vec3(0.85, 0.90, 1.0) * disc * 1.2;
    }
    if (uCloudsEnabled != 0 && dir.y > 0.002) {
        vec3 sunDir = normalize(vec3(cos(uSunPitch) * cos(uSunYaw),
                                      sin(uSunPitch),
                                      cos(uSunPitch) * sin(uSunYaw)));
        float t0 = (CLOUD_BASE - uCamPos.y) / dir.y;
        float t1 = (CLOUD_TOP - uCamPos.y) / dir.y;
        if (t1 > 0.0) {
            t0 = max(t0, 0.0);
            float horizonFade = smoothstep(0.02, 0.16, dir.y);
            const int STEPS = 48;
            float dt = (t1 - t0) / float(STEPS);
            vec3 p = uCamPos + dir * t0;
            float T = 1.0;
            vec3 acc = vec3(0.0);
            float c = dot(dir, sunDir);
            float g = 0.55;
            float phase = mix((1.0 - g * g) /
                                  (4.0 * 3.14159265 * pow(1.0 + g * g - 2.0 * g * c, 1.5)),
                              1.0 / (4.0 * 3.14159265), 0.5);
            vec3 sunCol = vec3(1.02, 0.98, 0.92) * 3.4;
            vec3 ambCol = mix(uSkyHorizon, uSkyTop, 0.5) * 1.0 + vec3(0.32);
            for (int i = 0; i < STEPS; ++i) {
                float d = CloudDensity(p, uCloudCoverage);
                if (d > 0.001) {
                    float lt = CloudLight(p, sunDir, uCloudCoverage);
                    float e = exp(-d * 15.0 / float(STEPS));
                    acc += T * (1.0 - e) * (ambCol + sunCol * lt * phase);
                    T *= e;
                    if (T < 0.02) break;
                }
                p += dir * dt;
            }
            col = mix(col, col * T + acc, horizonFade);
        }
    }
    FragColor = vec4(col, 1.0);
}
)";

} // namespace neon::gfx
