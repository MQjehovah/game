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

inline constexpr const char* kVolumetricFragmentShader = R"(
#version 330 core
in vec2 vUV;
out vec4 FragColor;
uniform sampler2D uScene;
uniform vec2 uSunScreen;   // sun position in [0,1] UV (bottom-left convention)
uniform vec2 uTexelSize;
uniform float uDensity;
uniform float uWeight;
uniform float uDecay;
uniform float uThreshold;
uniform int uSteps;
void main() {
    vec2 coord = vUV;
    vec2 delta = (vUV - uSunScreen) * uDensity / float(uSteps);
    float decay = 1.0;
    vec3 color = vec3(0.0);
    for (int i = 0; i < uSteps; ++i) {
        coord -= delta;
        // Progressive blur: offset the sample by a growing texel so the shaft
        // softens from the source (cheap, keeps it screen-space and stable).
        coord += vec2((0.5 - vUV.y), (vUV.x - 0.5)) * uTexelSize * float(i) * 0.25;
        float lum = dot(texture(uScene, coord).rgb, vec3(0.333));
        color += vec3(max(lum - uThreshold, 0.0) * uWeight * decay);
        decay *= uDecay;
    }
    color *= uDensity / float(uSteps) * 2.0;
    FragColor = vec4(color, 1.0);
}
)";

} // namespace neon::gfx
