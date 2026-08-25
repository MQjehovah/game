#pragma once

#include <algorithm>
#include <cmath>

#include "neon/math/vec2.hpp"
#include "neon/math/vec3.hpp"
#include "neon/math/vec4.hpp"

namespace neon::gfx {

// Screen-space ambient occlusion (G1-5). Like bloom.hpp, the pure math + the
// built-in shader sources live here (not in renderer.cpp) so the occlusion
// kernel and the depth encode/decode can be unit-tested headlessly with the
// NullBackend fixture. The renderer wires the passes: a color-encoded linear
// camera-depth pass -> AO compute -> separable blur -> composite multiply.

// Modern SSAO uses a few taps in the hemisphere around a fragment. 6 taps is
// a good quality/speed tradeoff for a forward renderer. The kernel is a small
// unit disk sample set biased toward the normal (used in view space here).
constexpr int kSsaoKernelSize = 6;

// Offsets in [-1,1] for one sample [x, y, scale]: a ring of taps plus one
// centre tap. Sparse but produces a soft, broadband occlusion.
inline constexpr float kSsaoKernel[kSsaoKernelSize][3] = {
    {0.0f, 0.0f, 0.5f},  // centre
    {1.0f, 0.0f, 0.9f},  {1.0f, 0.7f, 0.7f},  {0.0f, 1.0f, 0.9f},
    {-1.0f, 0.8f, 0.7f}, {-0.6f, -1.0f, 0.85f},
};

// AO tuning.
constexpr float kSsaoRadius = 0.8f;    // world-units sphere radius
constexpr float kSsaoBias = 0.05f;     // depth bias (fights self-occlusion)
constexpr float kSsaoPower = 1.8f;     // sharpens the occlusion curve
constexpr float kSsaoIntensity = 3.0f; // scales the unoccluded multiplier

// --- Linear depth encode/decode (color-encoded, mirrors the shadow pass) --

// Packs a normalized linear camera depth in [0,1] into 4 RGBA bytes (24-bit
// mantissa) so the scene depth can be stored in an RGBA8 target and sampled in
// the AO pass - the FBO depth TEXTURE is unreliable on the same Intel drivers
// that made the shadow maps use this colour-encoded trick.
inline math::Vec4 EncodeLinearDepth(float d) {
    d = std::clamp(d, 0.0f, 1.0f);
    float r = d * 1.0f;
    float g = std::fmod(d * 255.0f, 1.0f);
    float b = std::fmod(d * 65025.0f, 1.0f);
    float a = std::fmod(d * 16581375.0f, 1.0f);
    return {r, g, b, a};
}

// Reconstruction of the linear depth from the colour-encoded value.
inline float DecodeLinearDepth(const math::Vec4& packed) {
    return packed.x + packed.y / 255.0f + packed.z / 65025.0f + packed.w / 16581375.0f;
}

// The occlusion value for one pixel given a screen-space depth function. This
// is the CPU mirror of the SSAO fragment shader: `sample` returns the decoded
// linear depth at a (x,y) in [0,1] UV space; `depth` is the centre depth.
// Returns >= 0 occlusion (higher = more occluded), eventually clamped to 1 by
// the caller. Callers/tests can inject a depth function to unit-test.
inline float SsaoOcclusion(float depth, float radius, float bias,
                           float (*sample)(const math::Vec2& uv, void* user),
                           void* user, const math::Vec2& uv, float texelSize) {
    float occ = 0.0f;
    int count = 0;
    for (int i = 0; i < kSsaoKernelSize; ++i) {
        const math::Vec2 offset(kSsaoKernel[i][0], kSsaoKernel[i][1]);
        const float scale = kSsaoKernel[i][2];
        // Project the sample onto the fragment's view-space depth plane.
        const float sampleDepth = sample(uv + offset * texelSize * radius * scale, user);
        const float diff = depth - sampleDepth;
        // A neighbour that is closer than the centre (diff > 0) occludes it;
        // a farther one contributes nothing. The bias avoids self-occlusion.
        if (diff > bias) occ += std::pow(1.0f - std::min(diff / radius, 1.0f), kSsaoPower);
        ++count;
    }
    return occ / static_cast<float>(count);
}

// --- Built-in shaders -----------------------------------------------------

// Camera-depth pass: writes a colour-encoded linear camera-space depth. The
// clip-space -w equals the view-space z (linear), so depth/far is [0,1].
inline constexpr const char* kSsaoDepthVertexShader = R"(
#version 330 core
layout(location = 0) in vec3 aPos;
layout(location = 4) in mat4 aInstance;
uniform mat4 uMVP;
out float vViewDepth;
void main() {
    vec4 clip = uMVP * aInstance * vec4(aPos, 1.0);
    vViewDepth = -clip.w; // linear view-space depth (perspective W)
    gl_Position = clip;
}
)";

inline constexpr const char* kSsaoDepthFragmentShader = R"(
#version 330 core
in float vViewDepth;
out vec4 FragColor;
uniform float uFar;
void main() {
    float d = vViewDepth / uFar;
    d = clamp(d, 0.0, 1.0);
    FragColor = vec4(d, fract(d * 255.0), fract(d * 65025.0), fract(d * 16581375.0));
}
)";

// AO compute: samples the colour-encoded depth, reconstructs linear depth and
// accumulates occlusion over the screen-space kernel. Outputs AO in R.
inline constexpr const char* kSsaoFragmentShader = R"(
#version 330 core
in vec2 vUV;
out vec4 FragColor;
uniform sampler2D uDepth;
uniform vec2 uTexelSize;
uniform float uRadius;
uniform float uBias;
uniform float uPower;
uniform float uFar;
vec4 Decode(vec4 p) {
    return vec4(p.r, p.g, p.b, p.a);
}
float LoadDepth(vec2 uv) {
    vec4 p = texture(uDepth, uv);
    return p.r + p.g / 255.0 + p.b / 65025.0 + p.a / 16581375.0;
}
void main() {
    float centre = LoadDepth(vUV);
    if (centre >= 1.0) { FragColor = vec4(1.0, 1.0, 1.0, 1.0); return; } // sky/no geometry
    float occ = 0.0;
    float count = 0.0;
    for (int i = 0; i < 6; ++i) {
        vec2 off; float scale;
        if (i == 0) { off = vec2(0.0); scale = 0.5; }
        else if (i == 1) { off = vec2(1.0, 0.0); scale = 0.9; }
        else if (i == 2) { off = vec2(1.0, 0.7); scale = 0.7; }
        else if (i == 3) { off = vec2(0.0, 1.0); scale = 0.9; }
        else if (i == 4) { off = vec2(-1.0, 0.8); scale = 0.7; }
        else { off = vec2(-0.6, -1.0); scale = 0.85; }
        float s = LoadDepth(vUV + off * uTexelSize * uRadius * scale);
        float diff = centre - s;
        if (diff > uBias) occ += pow(1.0 - min(diff / uRadius, 1.0), uPower);
        count += 1.0;
    }
    float ao = 1.0 - clamp(occ / count, 0.0, 1.0);
    FragColor = vec4(ao, ao, ao, 1.0);
}
)";

// Separable blur for the AO channel (kSsaoBlurTaps taps, one axis per pass).
constexpr int kSsaoBlurTaps = 5;
inline constexpr float kSsaoBlurKernel[kSsaoBlurTaps] = {0.05449f, 0.244202f, 0.402620f,
                                                          0.244202f, 0.05449f};
inline constexpr const char* kSsaoBlurFragmentShader = R"(
#version 330 core
in vec2 vUV;
out vec4 FragColor;
uniform sampler2D uTex;
uniform vec2 uTexelSize;
uniform vec2 uDirection;
void main() {
    vec2 off = uTexelSize * uDirection;
    float c = texture(uTex, vUV - off * 2.0).r * 0.05449
            + texture(uTex, vUV - off).r       * 0.244202
            + texture(uTex, vUV).r             * 0.402620
            + texture(uTex, vUV + off).r       * 0.244202
            + texture(uTex, vUV + off * 2.0).r * 0.05449;
    FragColor = vec4(c);
}
)";

} // namespace neon::gfx
