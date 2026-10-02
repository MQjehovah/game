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
constexpr float kSsaoBias = 0.05f;     // depth bias (CPU mirror, normalised depth)
// GPU AO bias in WORLD units: the shader decodes the normalised scene depth back
// to world units, so the bias must be world-scaled too (a grazing ground plane's
// own depth gradient across a tap is ~0.1..0.3 units). Kept separate from
// kSsaoBias, which the CPU mirror test drives in normalised units.
// GPU AO bias in WORLD units: the shader now compares each hemisphere sample's
// own view depth against the stored depth, so the bias only has to absorb depth
// quantisation (the 24-bit encoded depth is ~sub-millimetre at these ranges) -
// a large bias here would reject the contact occlusion the pass exists to add.
constexpr float kSsaoBiasWorld = 0.05f;
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
// `depthGradient` is the surface's local depth slope (depth per UV unit); it is
// subtracted as the expected planar depth at each tap so a slanted plane does
// not occlude itself (see the GPU shader's planar-depth rejection). Returns >= 0
// occlusion (higher = more occluded), eventually clamped to 1 by the caller.
// Callers/tests can inject a depth function to unit-test.
inline float SsaoOcclusion(float depth, float radius, float bias,
                           float (*sample)(const math::Vec2& uv, void* user),
                           void* user, const math::Vec2& uv, float texelSize,
                           const math::Vec2& depthGradient = {0.0f, 0.0f}) {
    float occ = 0.0f;
    int count = 0;
    for (int i = 0; i < kSsaoKernelSize; ++i) {
        const math::Vec2 offset(kSsaoKernel[i][0], kSsaoKernel[i][1]);
        const float scale = kSsaoKernel[i][2];
        // Project the sample onto the fragment's view-space depth plane.
        const math::Vec2 tap = offset * texelSize * radius * scale;
        const float sampleDepth = sample(uv + tap, user);
        // Expected planar depth at the tap: the centre depth plus the surface
        // slope projected on the tap offset. On a flat/sloped plane this
        // cancels the geometric depth change, so only real occluders remain.
        const float expected = depth + depthGradient.x * tap.x + depthGradient.y * tap.y;
        const float diff = expected - sampleDepth;
        // A neighbour that is closer than the expected plane (diff > 0) occludes
        // it; a farther one contributes nothing. The bias avoids self-occlusion.
        if (diff > bias) occ += std::pow(1.0f - std::min(diff / radius, 1.0f), kSsaoPower);
        ++count;
    }
    return occ / static_cast<float>(count);
}

// --- Built-in shaders -----------------------------------------------------

// Camera-depth pass: writes a colour-encoded linear camera depth. In OpenGL the
// clip-space w equals -viewZ, and the view looks down -Z, so clip.w is the
// POSITIVE view distance: `vViewDepth = clip.w`. (The previous `-clip.w` was
// negative and clamped to 0, so every fragment encoded depth 0.)
inline constexpr const char* kSsaoDepthVertexShader = R"(
#version 330 core
layout(location = 0) in vec3 aPos;
layout(location = 4) in mat4 aInstance;
uniform mat4 uMVP;
out float vViewDepth;
void main() {
    vec4 clip = uMVP * aInstance * vec4(aPos, 1.0);
    vViewDepth = clip.w; // positive view distance
    gl_Position = clip;
}
)";

// Non-instanced variant for a single-mesh draw (no per-instance matrix).
inline constexpr const char* kSsaoDepthMeshVertexShader = R"(
#version 330 core
layout(location = 0) in vec3 aPos;
uniform mat4 uMVP;
out float vViewDepth;
void main() {
    vec4 clip = uMVP * vec4(aPos, 1.0);
    vViewDepth = clip.w; // positive view distance
    gl_Position = clip;
}
)";

// GPU-skinned variant for the depth pre-pass fallback. The shadow skinned
// program encodes WINDOW depth (correct for CSM depth compare) — reusing it
// here fed the post chain quadratic window depth that consumers decoded as
// linear view distance (skinned characters turned into solid fog silhouettes).
inline constexpr const char* kSsaoDepthSkinnedVertexShader = R"(
#version 330 core
layout(location = 0) in vec3 aPos;
layout(location = 4) in vec4 aJointIds;
layout(location = 5) in vec4 aWeights;
uniform mat4 uBoneMatrices[128];
uniform mat4 uMVP;
out float vViewDepth;
void main() {
    mat4 skin = mat4(0.0);
    for (int i = 0; i < 4; ++i) {
        int id = int(aJointIds[i]);
        if (id >= 0 && id < 128) skin += aWeights[i] * uBoneMatrices[id];
    }
    vec4 clip = uMVP * skin * vec4(aPos, 1.0);
    vViewDepth = clip.w; // positive view distance (same as the static variant)
    gl_Position = clip;
}
)";

inline constexpr const char* kSsaoDepthFragmentShader = R"(
#version 330 core
in float vViewDepth;
out vec4 FragColor;
uniform float uFar;
// Carry-corrected base-255 packing (same as the shadow encoder). The naive
// `vec4(d, fract(d*255), ...)` form has each channel rounded INDEPENDENTLY by
// the RGBA8 quantizer, so the decode r+g/255+... sawtooths by up to ~0.5/255
// of the range (~0.8 world units at uFar 800) with a period of 1/255 of depth.
// The AO/SSR/volumetric/fog consumers compare that depth with world-unit
// thresholds, so the sawtooth surfaced as false occlusion / fake surfaces
// along constant-depth contours - horizontal stripes whose pitch and strength
// differed per consumer. The carry correction makes every stored channel a
// UNORM8-exact multiple so the decode error collapses to ~1/16M.
vec4 EncodeDepth(float d) {
    vec4 bits = vec4(1.0, 255.0, 65025.0, 16581375.0) * d;
    bits = fract(bits);
    bits -= bits.yzww * vec4(1.0 / 255.0, 1.0 / 255.0, 1.0 / 255.0, 0.0);
    return bits;
}
void main() {
    float d = vViewDepth / uFar;
    d = clamp(d, 0.0, 1.0);
    FragColor = EncodeDepth(d);
}
)";

// AO compute: samples the colour-encoded depth, decodes it to WORLD units and
// accumulates occlusion over a depth-scaled screen-space kernel. Outputs AO in R.
inline constexpr const char* kSsaoFragmentShader = R"(
#version 330 core
in vec2 vUV;
out vec4 FragColor;
uniform sampler2D uDepth;
uniform vec2 uTexelSize;
uniform float uRadius;     // sampling radius, world units
uniform float uBias;       // depth bias, world units
uniform float uPower;
uniform float uFar;
uniform float uProjScale;  // pixels per world unit at depth 1 (0.5*height/tan(fovY/2))
float RawDepth(vec2 uv) {
    vec4 p = texture(uDepth, uv);
    return p.r + p.g / 255.0 + p.b / 65025.0 + p.a / 16581375.0;
}
// Interleaved gradient noise (same as the lit shader's PCSS rotation):
// rotates the kernel per pixel so the DISCRETE tap radii average into a
// smooth halo instead of drawing concentric dark outline rings around every
// silhouette (the "multiple black contour lines" artifact).
float Ign(vec2 p) {
    return fract(52.9829189 * fract(0.06711056 * p.x + 0.00583715 * p.y));
}
// 12 unit hemisphere directions (z > 0), the top of the classic cosine-ish
// sample kernel. Sparse but, combined with the per-pixel IGN rotation, a
// broadband occlusion.
const int kSsaoSamples = 12;
const vec3 kSsaoKernel[12] = vec3[12](
    vec3( 0.538,  0.283, 0.834), vec3(-0.342, -0.664, 0.666),
    vec3( 0.043,  0.930, 0.365), vec3(-0.696,  0.549, 0.462),
    vec3( 0.706,  0.370, 0.604), vec3(-0.395, -0.184, 0.900),
    vec3(-0.003, -0.372, 0.928), vec3(-0.717,  0.091, 0.691),
    vec3( 0.442,  0.786, 0.434), vec3(-0.347,  0.880, 0.325),
    vec3(-0.870, -0.298, 0.394), vec3(-0.297, -0.902, 0.312));
void main() {
    float raw = RawDepth(vUV);
    if (raw >= 0.9999) { FragColor = vec4(1.0, 1.0, 1.0, 1.0); return; } // sky/no geometry
    float viewZ = raw * uFar; // linear view distance (world units)

    // View-space reconstruction (camera at origin, -z forward): a fragment and
    // its neighbours fix an actual surface normal instead of relying on a
    // depth-gradient plane, and each kernel sample is placed in VIEW space so
    // the occlusion test can compare the SAMPLE POINT's depth with the stored
    // depth - the comparison the old planar-extrapolation code got backwards
    // (it compared the receiver's own extrapolated depth, which cancels the
    // contact signal and left AO ~= 1 everywhere).
    float hdrH = 1.0 / uTexelSize.y;
    float tanHalf = 0.5 * hdrH / uProjScale;
    float aspect = uTexelSize.y / uTexelSize.x;
    vec2 scr = vUV * 2.0 - 1.0;
    vec3 ro = vec3(scr.x * tanHalf * aspect, scr.y * tanHalf, -1.0) * viewZ;
    vec2 oX = vec2(2.0 * uTexelSize.x, 0.0);
    vec2 oY = vec2(0.0, 2.0 * uTexelSize.y);
    float zX = RawDepth(vUV + oX) * uFar;
    float zY = RawDepth(vUV + oY) * uFar;
    vec2 sX = scr + 2.0 * oX;
    vec2 sY = scr + 2.0 * oY;
    vec3 pX = vec3(sX.x * tanHalf * aspect, sX.y * tanHalf, -1.0) *
              (zX < uFar * 0.9999 ? zX : viewZ);
    vec3 pY = vec3(sY.x * tanHalf * aspect, sY.y * tanHalf, -1.0) *
              (zY < uFar * 0.9999 ? zY : viewZ);
    vec3 N = normalize(cross(pY - ro, pX - ro));
    if (dot(N, ro) > 0.0) N = -N; // face the camera's hemisphere

    // Per-pixel rotation of a random tangent vector, so the 12 fixed directions
    // average into a smooth halo instead of imprinting rings on silhouettes.
    float ang = Ign(gl_FragCoord.xy) * 6.2831853;
    vec3 rvec = vec3(cos(ang), sin(ang), 0.0);
    vec3 T = normalize(rvec - N * dot(rvec, N));
    vec3 B = cross(N, T);
    mat3 TBN = mat3(T, B, N);

    float occ = 0.0;
    for (int i = 0; i < kSsaoSamples; ++i) {
        // Linear-sample the kernel toward the origin so occlusion is weighted
        // toward the contact point.
        float t = (float(i) + 0.5) / float(kSsaoSamples);
        vec3 sv = TBN * (kSsaoKernel[i] * (uRadius * mix(0.25, 1.0, t)));
        vec3 samplePos = ro + sv;
        float sampleZ = -samplePos.z; // positive view depth of the sample
        if (sampleZ < 1e-3) continue;  // behind the camera
        vec2 suv = vec2(samplePos.x / (-samplePos.z * tanHalf * aspect),
                        samplePos.y / (-samplePos.z * tanHalf)) * 0.5 + 0.5;
        if (suv.x < 0.0 || suv.x > 1.0 || suv.y < 0.0 || suv.y > 1.0) continue;
        float sceneZ = RawDepth(suv) * uFar;
        if (sceneZ >= uFar * 0.9999) continue; // sky sample
        // Range check: only occlude with geometry inside the sample radius.
        float rangeCheck = smoothstep(0.0, 1.0, uRadius / max(abs(viewZ - sceneZ), 1e-4));
        // Occluded when the stored surface is CLOSER than the sample point.
        float diff = sampleZ - sceneZ;
        if (diff > uBias) occ += rangeCheck;
    }
    float ao = clamp(1.0 - occ / float(kSsaoSamples), 0.0, 1.0);
    FragColor = vec4(vec3(pow(ao, uPower)), 1.0);
}
)";

// Fullscreen variant of the depth pre-pass (B4): instead of redrawing the
// scene's casters, encode the main pass's RESOLVED depth attachment into the same
// colour-encoded RGBA8 the AO/SSR/volumetric consumers read. This reuses the
// geometry the main pass already drew (one geometry pass instead of two).
inline constexpr const char* kDepthEncodeFragmentShader = R"(
#version 330 core
in vec2 vUV;
out vec4 FragColor;
uniform sampler2D uDepthTex; // raw depth attachment (NDC depth in .r)
uniform float uNear;
uniform float uFar;
void main() {
    float ndc = texture(uDepthTex, vUV).r;
    // GL depth is nonlinear NDC [0,1]; invert to a positive view distance.
    float linear = (2.0 * uNear * uFar) /
                   (uFar + uNear - (2.0 * ndc - 1.0) * (uFar - uNear));
    // Sky / no geometry: the depth buffer sits at the far plane (ndc == 1).
    float d = (ndc >= 1.0) ? 1.0 : clamp(linear / uFar, 0.0, 1.0);
    // Carry-corrected packing (see kSsaoDepthFragmentShader): the naive
    // fract() form quantised to a ~0.5/255 sawtooth that the AO/SSR/vol/fog
    // consumers read as horizontal depth-contour stripes.
    vec4 bits = vec4(1.0, 255.0, 65025.0, 16581375.0) * d;
    bits = fract(bits);
    bits -= bits.yzww * vec4(1.0 / 255.0, 1.0 / 255.0, 1.0 / 255.0, 0.0);
    FragColor = bits;
}
)";

// Separable blur for the AO channel (kSsaoBlurTaps taps, one axis per pass).
// 9 taps (±4 texels, gaussian): the AO kernel's features are up to `pixels`
// texels wide (≤128), and the old ±2 blur left the rotated kernel's residual
// structure and silhouette halos unsmoothed.
constexpr int kSsaoBlurTaps = 9;
inline constexpr float kSsaoBlurKernel[kSsaoBlurTaps] = {
    0.0048f, 0.0287f, 0.1028f, 0.2210f, 0.2854f, 0.2210f, 0.1028f, 0.0287f, 0.0048f};
inline constexpr const char* kSsaoBlurFragmentShader = R"(
#version 330 core
in vec2 vUV;
out vec4 FragColor;
uniform sampler2D uTex;
uniform vec2 uTexelSize;
uniform vec2 uDirection;
void main() {
    vec2 off = uTexelSize * uDirection;
    float c = texture(uTex, vUV - off * 4.0).r * 0.0048
            + texture(uTex, vUV - off * 3.0).r * 0.0287
            + texture(uTex, vUV - off * 2.0).r * 0.1028
            + texture(uTex, vUV - off).r       * 0.2210
            + texture(uTex, vUV).r             * 0.2854
            + texture(uTex, vUV + off).r       * 0.2210
            + texture(uTex, vUV + off * 2.0).r * 0.1028
            + texture(uTex, vUV + off * 3.0).r * 0.0287
            + texture(uTex, vUV + off * 4.0).r * 0.0048;
    FragColor = vec4(c);
}
)";

} // namespace neon::gfx
