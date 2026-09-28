#pragma once

#include <algorithm>
#include <cmath>

#include "neon/math/vec2.hpp"
#include "neon/math/vec3.hpp"

namespace neon::gfx {

// Screen-space reflections (G1-5 / Godot P2-1). Like bloom.hpp / ssao.hpp, the
// ray-march math + shader source live here so the hit logic is unit-testable
// headlessly with the NullBackend fixture. The pass reuses the colour-encoded
// scene depth (ssaoDepthRT_) plus the resolved HDR scene colour; each pixel
// reflects its view ray off a depth-gradient normal, marches it in screen
// space, and pulls the reflected colour where it hits nearby geometry.

constexpr int kSsrSteps = 24;
constexpr float kSsrThickness = 0.03f; // depth epsilon, RELATIVE to ray depth
constexpr float kSsrMaxDist = 0.35f;   // max screen-space ray length in UV

// One ray-march step in screen space. `rayDepth` is the ray's view-space depth
// at the current sample, `sceneDepth` the decoded scene depth at the same UV.
// Returns true when the ray is in FRONT of the surface by ~thickness (a hit);
// a sample behind the surface is skipped, a sample "inside" the surface (within
// thickness) is treated as hitting the surface boundary.
inline bool SsrHit(float rayDepth, float sceneDepth, float thickness) {
    return sceneDepth < rayDepth && (rayDepth - sceneDepth) > thickness;
}

// --- Built-in shaders -----------------------------------------------------

inline constexpr const char* kSsrFragmentShader = R"(
#version 330 core
in vec2 vUV;
out vec4 FragColor;
uniform sampler2D uScene;       // resolved HDR colour
uniform sampler2D uDepth;       // colour-encoded scene depth
uniform vec2 uTexelSize;        // FULL-res texel of the depth texture
uniform float uNear;
uniform float uFar;
uniform float uSteps;
uniform float uThickness;       // relative depth epsilon
uniform float uMaxDist;         // ray length in UV
uniform float uProjScale;       // 0.5*height/tan(fovY/2)
float LoadDepth(vec2 uv) {
    vec4 p = texture(uDepth, uv);
    return p.r + p.g / 255.0 + p.b / 65025.0 + p.a / 16581375.0;
}
float ViewDepth(float ndc) {
    // The depth RT stores LINEAR view distance / uFar (SSAO depth encoder).
    return ndc * uFar;
}
void main() {
    float ndc = LoadDepth(vUV);
    if (ndc >= 1.0) { FragColor = vec4(0.0); return; } // sky -> no reflection
    float viewZ = ViewDepth(ndc);

    // Projection factors: uTexelSize is the full-res depth texel, so
    // 1/uTexelSize.y is the full height and uProjScale = 0.5*H/tan(fovY/2).
    float hdrH = 1.0 / uTexelSize.y;
    float tanHalf = 0.5 * hdrH / uProjScale;
    float aspect = uTexelSize.y / uTexelSize.x;

    // View-space position of this fragment (camera at the origin, looking
    // down -z): the reconstructed ray through the pixel scaled to its depth.
    vec2 scr = vUV * 2.0 - 1.0;
    vec3 ro = vec3(scr.x * tanHalf * aspect, scr.y * tanHalf, -1.0) * viewZ;

    // View-space normal from the depth field. dzx/dzy are WORLD depth deltas
    // over one texel, so they must be divided by the WORLD size of a texel at
    // this depth (z * 2*tanHalf*aspect / W and z * 2*tanHalf / H) - mixing
    // them with raw UV scales (the old form) produced wildly wrong normals
    // and reflections that pointed anywhere.
    float dzx = ViewDepth(LoadDepth(vUV + vec2(uTexelSize.x, 0.0))) - viewZ;
    float dzy = ViewDepth(LoadDepth(vUV + vec2(0.0, uTexelSize.y))) - viewZ;
    float wx = max(viewZ * 2.0 * tanHalf * aspect * uTexelSize.x, 1e-4);
    float wy = max(viewZ * 2.0 * tanHalf * uTexelSize.y, 1e-4);
    vec3 n = normalize(vec3(-dzx / wx, -dzy / wy, 1.0));

    vec3 rd = reflect(normalize(ro), n);
    // A reflection pointing back toward the camera leaves the depth buffer.
    if (rd.z > -1e-3) { FragColor = vec4(0.0); return; }

    // March in VIEW space. A straight 3D ray projects to a straight screen
    // line, but its DEPTH along that line is not linear under perspective, so
    // project every sample analytically instead of lerping the depth (the old
    // screen-space march compared against the ORIGIN's depth and happily hit
    // surfaces the ray never crossed - smears and phantom reflections).
    float maxDist = uMaxDist * viewZ * 2.0 * tanHalf * aspect; // world length
    float dt = max(maxDist / uSteps, 1e-3);
    float t = 0.1;
    for (int i = 0; i < int(uSteps); ++i) {
        t += dt;
        vec3 p = ro + rd * t;
        if (-p.z < uNear) break; // slid behind the camera
        vec2 suv = vec2(p.x / (-p.z * tanHalf * aspect),
                        p.y / (-p.z * tanHalf)) * 0.5 + 0.5;
        if (suv.x < 0.0 || suv.x > 1.0 || suv.y < 0.0 || suv.y > 1.0) break;
        float sceneZ = ViewDepth(LoadDepth(suv));
        float rayZ = -p.z;
        // Depth epsilon grows with depth: a fixed world constant self-hits
        // far surfaces (sub-texel depth steps per march step are large).
        float thick = rayZ * (uThickness + 0.01);
        if (sceneZ < rayZ - thick) {
            // Fade with march distance and toward the screen edges: samples
            // near the border would extrapolate off-screen geometry.
            float edge = smoothstep(0.0, 0.1, suv.x) * smoothstep(1.0, 0.9, suv.x) *
                         smoothstep(0.0, 0.1, suv.y) * smoothstep(1.0, 0.9, suv.y);
            float fade = max(1.0 - t / maxDist, 0.0) * edge;
            FragColor = vec4(texture(uScene, suv).rgb * fade, 1.0);
            return;
        }
    }
    FragColor = vec4(0.0);
}
)";

} // namespace neon::gfx
