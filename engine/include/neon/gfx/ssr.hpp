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
// reflects its view ray off a crude depth-gradient normal, marches it in screen
// space, and pulls the reflected colour where it hits nearby geometry.

constexpr int kSsrSteps = 16;
constexpr float kSsrThickness = 0.03f; // depth epsilon (prevents self-hit)
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
uniform vec2 uTexelSize;
uniform float uNear;
uniform float uFar;
uniform float uSteps;
uniform float uThickness;
uniform float uMaxDist;
float LoadDepth(vec2 uv) {
    vec4 p = texture(uDepth, uv);
    return p.r + p.g / 255.0 + p.b / 65025.0 + p.a / 16581375.0;
}
float ViewDepth(float ndc) {
    // Invert the perspective z-mapping: ndc in [0,1] -> positive view depth.
    float z = ndc * 2.0 - 1.0;
    return (2.0 * uNear * uFar) / (uFar + uNear - z * (uFar - uNear));
}
void main() {
    float ndc = LoadDepth(vUV);
    if (ndc >= 1.0) { FragColor = vec4(0.0); return; } // sky -> no reflection
    float viewZ = ViewDepth(ndc);

    // Screen-space normal from depth gradients (approx, no G-buffer).
    float dzx = ViewDepth(LoadDepth(vUV + vec2(uTexelSize.x, 0.0))) - viewZ;
    float dzy = ViewDepth(LoadDepth(vUV + vec2(0.0, uTexelSize.y))) - viewZ;
    vec3 n = normalize(vec3(-dzx * uMaxDist, -dzy * uMaxDist, uTexelSize.x));

    // Reflect the view ray: camera looks along +viewZ (into the screen).
    vec3 viewDir = vec3(0.0, 0.0, 1.0);
    vec3 refl = reflect(viewDir, n);
    if (refl.z <= 0.0) { FragColor = vec4(0.0); return; } // reflected away

    vec2 stepVec = refl.xy / refl.z * uMaxDist / uSteps;
    vec2 uv = vUV;
    vec3 result = vec3(0.0);
    for (int i = 0; i < uSteps; ++i) {
        uv += stepVec;
        if (uv.x < 0.0 || uv.x > 1.0 || uv.y < 0.0 || uv.y > 1.0) break;
        float sceneZ = ViewDepth(LoadDepth(uv));
        // The reflected ray travels roughly laterally, staying near the
        // surface depth; a hit is where a nearer surface crosses it.
        if (sceneZ > 0.001 && sceneZ < (viewZ - uThickness)) {
            result = texture(uScene, uv).rgb;
            break;
        }
    }
    FragColor = vec4(result, 1.0);
}
)";

} // namespace neon::gfx
