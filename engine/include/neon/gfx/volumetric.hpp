#pragma once

#include <algorithm>

#include "neon/math/vec2.hpp"
#include "neon/math/vec3.hpp"

namespace neon::gfx {

// Screen-space volumetric light shafts (G1-5). Like bloom.hpp, the pure math
// + shader sources live here so the god-ray accumulation can be unit-tested
// headlessly with the NullBackend fixture. This is the stylised, robust
// "crepuscular rays" approximation: the scene colour is sampled radially
// toward the sun, accumulating brightness weighted by an exponential decay, so
// geometry that blocks the sun leaves dark shafts and open sky glows.

constexpr int kVolumetricSteps = 20;      // number of samples along each ray
constexpr float kVolumetricDensity = 0.5f; // ray shortening factor
constexpr float kVolumetricWeight = 0.3f;  // per-sample brightness weight
constexpr float kVolumetricDecay = 0.97f;  // per-sample brightness decay
constexpr float kVolumetricThreshold = 0.9f; // only bright (sun/sky) samples scatter

// One accumulative step of the god-ray integration. `brightness` is the sample
// luminance, `transmittance` is the running decay (updated in place), returns
// the contribution added to the ray. Pure so tests can simulate a dark vs.
// bright ray.
inline float VolumetricStep(float brightness, float weight, float decay,
                            float& transmittance) {
    const float c = std::max(brightness - kVolumetricThreshold, 0.0f) * weight * transmittance;
    transmittance *= decay;
    return c;
}

// --- Built-in shaders -----------------------------------------------------

// Depth-aware volume light-shafts (god-rays). The old screen-space radial
// approximation baked brightness out of the HDR scene and died whenever a tree
// canopy blocked the sky, so sun shafts never appeared in forest scenes. This
// version marches each pixel's view ray in world space: it reconstructs the
// world ray from the camera, scouts only the empty space IN FRONT of the
// nearest surface (viewZ from the colour-encoded depth), and accumulates the
// sun's phase-scattered light there. Geometry ahead of a step (tree canopy)
// truncates the shaft; gaps between canopies let it glow toward the sun — the
// classic crepuscular-ray look.
inline constexpr const char* kVolumetricFragmentShader = R"(
#version 330 core
in vec2 vUV;
out vec4 FragColor;
uniform sampler2D uScene;   // resolved HDR colour (unused here; kept for parity)
uniform sampler2D uDepth;   // colour-encoded scene depth
uniform vec2 uTexelSize;
uniform vec2 uSunScreen;    // sun in [0,1] UV (unused; world march instead)
uniform vec3 uCamPos;
uniform vec3 uSunDir;       // light propagation dir; toward sun = -uSunDir
uniform vec3 uSunColor;
uniform mat4 uViewProj;
uniform vec4 uSceneVpRect;  // scene viewport rect in normalized [0,1] HDR-tgt UV
uniform float uNear;
uniform float uFar;
uniform float uDensity;
uniform float uWeight;
uniform float uDecay;
uniform float uThreshold;
uniform int uSteps;
float LoadDepth(vec2 uv) {
    vec4 p = texture(uDepth, uv);
    return p.r + p.g / 255.0 + p.b / 65025.0 + p.a / 16581375.0;
}
float ViewDepth(float ndc) {
    float z = ndc * 2.0 - 1.0;
    return (2.0 * uNear * uFar) / (uFar + uNear - z * (uFar - uNear));
}
void main() {
    // The 3D scene draws only into the centred letterboxed sub-rect; a ray
    // reconstructed from the FULL-WINDOW UV would be wrong (and ghost into the
    // black bars). Map vUV into the sub-rect's local UV and bail on the bars.
    vec2 uv = (vUV - uSceneVpRect.xy) / max(uSceneVpRect.zw, vec2(1e-4));
    if (uv.x < 0.0 || uv.x > 1.0 || uv.y < 0.0 || uv.y > 1.0) {
        FragColor = vec4(0.0);
        return;
    }

    // Nearest surface along this pixel (view space). Sky (ndc>=1) = open.
    float ndc = LoadDepth(uv);
    float viewZ = ndc >= 1.0 ? uFar : ViewDepth(ndc);

    // World-space view ray for this pixel (from the sub-rect local UV, which
    // the camera viewProj was built for).
    vec4 clip = inverse(uViewProj) * vec4(uv.x * 2.0 - 1.0, uv.y * 2.0 - 1.0, 1.0, 1.0);
    vec3 rayEnd = clip.xyz / clip.w;
    vec3 rayDir = normalize(rayEnd - uCamPos);

    // Henyey-Greenstein forward scatter (tight, sun-facing cone) -> the shaft
    // fans only toward the sun so open sky brightens backlit and stays dark
    // elsewhere, instead of washing the whole frame.
    vec3 toSun = normalize(-uSunDir);
    float mu = dot(rayDir, toSun);
    float g = 0.7;
    float gg = g * g;
    float phase = (1.0 - gg) / (4.0 * 3.14159265 * pow(max(1.0 + gg - 2.0 * g * mu, 1e-3), 1.5));
    phase *= 1.5; // modest; keeps the shaft visible without washing the sky

    // The scattering medium (haze) lives only in a thin near-field slab in
    // front of the camera: every pixel (sky or geometry) passes through ~the
    // same slab, so sky isn't doubly-lit to blow-out — the shaft shows up as
    // sun-angled light in the fog rather than a white sky.
    float t0 = uNear;
    float t1 = min(uNear + 38.0, min(viewZ, uFar));
    float dt = max((t1 - t0) / float(uSteps), 1e-4);
    float density = uDensity * 0.05;
    float transmittance = 1.0;
    vec3 acc = vec3(0.0);
    for (int i = 0; i < uSteps; ++i) {
        float seg = density * dt;
        acc += uSunColor * (phase * seg * transmittance);
        transmittance *= exp(-seg);
    }
    acc *= uWeight;
    FragColor = vec4(acc, 1.0);
}
)";

} // namespace neon::gfx
