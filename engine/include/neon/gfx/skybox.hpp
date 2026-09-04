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
    vec2 ndc = vUV * 2.0 - 1.0;
    vec3 dir = InverseViewProjRay(ndc);
    float dy = clamp(dir.y * 0.5 + 0.5, 0.0, 1.0);

    // HDRI (equirect) source when provided; otherwise the vertical gradient.
    vec3 col;
    if (uSkyTextureValid != 0) {
        float u = atan(dir.z, dir.x) / (2.0 * 3.14159265) + 0.5;
        float v = acos(clamp(dy, 0.0, 1.0)) / 3.14159265;
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
    if (uCloudsEnabled != 0) {
        // Project the ray onto a plane above the horizon (y = 1) and sample FBM
        // noise in world XZ. Clouds are brighter above, fade near the horizon.
        float t = 1.0 / max(dir.y, 0.02);
        vec2 uv = (dir.xz) * t * uCloudScale + vec2(uTime * 0.01, 0.0);
        float n = fbm(uv);
        float coverage = uCloudCoverage;
        float cloud = smoothstep(coverage, coverage + 0.35, n);
        float fade = smoothstep(0.0, 0.15, dir.y);
        col = mix(col, mix(col, vec3(1.0, 1.0, 1.0), 0.8), cloud * fade);
    }
    FragColor = vec4(col, 1.0);
}
)";

} // namespace neon::gfx
