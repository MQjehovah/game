#pragma once
#include <algorithm>
#include <cmath>
#include "neon/math/vec3.hpp"

namespace neon::gfx {

// HDR + bloom post-processing (Task 3.6). Pure math + built-in shader sources
// live here (not inside renderer.cpp) so the bright-pass / clamp / combine
// steps and the shader source strings can be unit-tested headlessly with the
// NullBackend fixture (no GL needed), mirroring how csm.hpp / point_shadow.hpp
// expose their math for tests.
//
// Pipeline (all fullscreen passes, RGBA16F targets):
//   1. bright pass:      hdr  -> half   max(color - threshold, 0)
//   2. blur half (H+V)   half  <-> temp   5-tap separable Gaussian
//   3. downsample        half  -> quarter 2x2 box
//   4. blur quarter (H+V)quarter <-> temp  5-tap separable Gaussian
//   5. upsample-add      half' = half + up(quarter)
//   6. composite         out  = ACES( (hdr + up(half') * strength) * exposure )
//
// T3.7 replaced the provisional clamp with the ACES fitted tonemapper
// (Narkowicz); the legacy `min(c,1)` clamp is kept only as the uTonemapEnabled
// == 0 reference branch used by the --tonemap-compare diff. Everything before
// the composite is unchanged.

// Only pixels brighter than this (HDR units) bloom. The scene already sums the
// sun + point + player lights without clamping, so sun-lit surfaces exceed 1.0
// and drive the bloom; sky / fog / dark UI stay below it and are untouched.
constexpr float kBloomThreshold = 1.0f;
// How strongly the accumulated bloom is added back in the composite. Modest on
// purpose: the composite clamps to [0,1], so too much bloom just turns into a
// flat white wash instead of a glow.
constexpr float kBloomStrength = 0.35f;
// Gaussian blur width (taps per direction). Kernel below is normalized to sum
// to 1.0 (sigma = 1 texel); see test_post.cpp for the sum assertion.
constexpr int kBloomBlurTaps = 5;
inline constexpr float kBloomKernel[kBloomBlurTaps] = {
    0.05449f, 0.244202f, 0.402620f, 0.244202f, 0.05449f};

// Bright pass: everything above the threshold, everything else 0.
inline float BrightPass(float value, float threshold) {
    return std::max(value - threshold, 0.0f);
}
inline math::Vec3 BrightPass(const math::Vec3& color, float threshold) {
    return {BrightPass(color.x, threshold), BrightPass(color.y, threshold),
            BrightPass(color.z, threshold)};
}

// LDR clamp kept as the T3.6 reference conversion (the composite's
// uTonemapEnabled == 0 branch). The backbuffer would clamp anyway; the --no-
// tonemap / --tonemap-compare paths use it so the ACES diff isolates only the
// tone-mapping operator.
inline float ClampLdr(float value) { return std::min(value, 1.0f); }
inline math::Vec3 ClampLdr(const math::Vec3& color) {
    return {ClampLdr(color.x), ClampLdr(color.y), ClampLdr(color.z)};
}

// ACES fitted tonemapper ("ACES Film" approximation, Narkowicz 2015): a
// monotonic rational curve that maps [0,inf) HDR into [0,1] with a soft knee
// around mid-tones and no colour shift for neutral inputs. Exactly the curve
// baked into kCompositeFragmentShader so the CPU math stays a faithful model.
// The input is clamped to the half-float max BEFORE the curve so an Inf/NaN
// HDR value (point-light overflow at near-zero distance, a shader NaN, ...)
// cannot feed the rational curve (clamp(NaN) is undefined in GLSL -> flicker).
// The CPU mirror additionally maps NaN inputs to 0. Finite in-range values are
// unchanged.
inline constexpr float kAcesHdrMax = 65504.0f; // half-float max (also baked into the shader)
inline float AcesFilm(float x) {
    if (std::isnan(x)) return 0.0f;
    x = std::clamp(x, 0.0f, kAcesHdrMax);
    const float a = 2.51f, b = 0.03f, c = 2.43f, d = 0.59f, e = 0.14f;
    return std::clamp((x * (a * x + b)) / (x * (c * x + d) + e), 0.0f, 1.0f);
}
inline math::Vec3 AcesFilm(const math::Vec3& color) {
    return {AcesFilm(color.x), AcesFilm(color.y), AcesFilm(color.z)};
}

// Exposure application: exposure multiplies the HDR input BEFORE the curve
// (ACESFilm(color * exposure)); the composite shader does the same. exposure
// == 1 is the identity (no darkening / no wash-out).
inline math::Vec3 ToneMap(float exposure, const math::Vec3& hdr) {
    return AcesFilm(hdr * exposure);
}

// Composite: hdr + bloom * strength, ACES tonemapped with exposure applied.
inline math::Vec3 BloomCombine(const math::Vec3& hdr, const math::Vec3& bloom, float strength,
                               float exposure = 1.0f) {
    return ToneMap(exposure, hdr + bloom * strength);
}

// --- Procedural color grading (A1 / LUT-free "film look") --------------------
// Applied AFTER the ACES tonemap, in display space [0,1], by the composite.
// The CPU model below mirrors the GLSL in kCompositeFragmentShader exactly so
// the grade can be unit-tested headlessly (same idea as BloomCombine/AcesFilm
// being CPU mirrors of shaders). Chosen operators are monotonic per component
// (no banding / no hue flip), and the identity set returns the input unchanged
// so the default RenderStack leaves every existing scene pixel-identical.
//
// Order: white-balance tint -> photographic Lift/Gamma/Gain -> luminance-
// preserving saturation -> contrast around a fixed 0.5 pivot -> clamp.
struct ColorGrade {
    bool enabled = false;      // master switch; off = identity
    float saturation = 1.0f;   // 1 neutral; <1 toward grey; >1 punchier
    float contrast = 0.0f;     // 0 neutral; >0 spreads around the 0.5 pivot
    float gain = 1.0f;         // scales highlights
    float gamma = 1.0f;        // >1 darkens mid-tones; <1 brightens them
    float lift = 0.0f;         // offsets shadows upward (0 = neutral)
    math::Vec3 tint{1.0f, 1.0f, 1.0f}; // per-channel white balance (colour temp)
};

// Neutral-grade fast path: returns the input untouched (matches the shader
// skipping grading when uGradeEnabled == 0).
inline math::Vec3 GradeColor(const math::Vec3& c, const ColorGrade& g) {
    if (!g.enabled) return c;
    // Clamp to display space first so LGG pow() never sees a negative base.
    math::Vec3 x{std::fmin(std::fmax(c.x, 0.0f), 1.0f),
                 std::fmin(std::fmax(c.y, 0.0f), 1.0f),
                 std::fmin(std::fmax(c.z, 0.0f), 1.0f)};
    // White balance.
    x = {x.x * g.tint.x, x.y * g.tint.y, x.z * g.tint.z};
    // Lift/Gamma/Gain (photographic): gamma -> gain -> lift.
    const float invGamma = 1.0f / std::max(g.gamma, 1e-4f);
    x = {std::pow(std::fmin(std::fmax(x.x, 0.0f), 1.0f), invGamma) * g.gain + g.lift,
         std::pow(std::fmin(std::fmax(x.y, 0.0f), 1.0f), invGamma) * g.gain + g.lift,
         std::pow(std::fmin(std::fmax(x.z, 0.0f), 1.0f), invGamma) * g.gain + g.lift};
    // Luminance-preserving saturation.
    const float luma = 0.2126f * x.x + 0.7152f * x.y + 0.0722f * x.z;
    x = {luma + (x.x - luma) * g.saturation, luma + (x.y - luma) * g.saturation,
         luma + (x.z - luma) * g.saturation};
    // Contrast around a 0.5 pivot.
    const float ck = 1.0f + g.contrast;
    x = {(x.x - 0.5f) * ck + 0.5f, (x.y - 0.5f) * ck + 0.5f, (x.z - 0.5f) * ck + 0.5f};
    return {std::fmin(std::fmax(x.x, 0.0f), 1.0f), std::fmin(std::fmax(x.y, 0.0f), 1.0f),
            std::fmin(std::fmax(x.z, 0.0f), 1.0f)};
}

// --- A5 auto-exposure + vignette (post-tonemap LUT-free) ---------------------
// Auto-exposure targets a middle-grey average luminance: the scene's HDR
// average log-luminance is computed by a downsample chain (see PostGraph), and
// the composite derives an exposure multiplier that drives it toward a key
// value. Smoothing (lerp toward the target) is applied in the composite via the
// previous frame's exposure. The CPU models below mirror the GLSL so the
// operators are unit-testable headlessly (same convention as AcesFilm).

// Auto-exposure params (RenderStack / CompositeParams / Renderer state).
struct AutoExposure {
    bool enabled = false;       // off = keep the authored scalar exposure
    float keyValue = 0.18f;     // middle-grey target
    float minExposure = 0.05f;  // clamp the multiplier
    float maxExposure = 20.0f;
    float adaptationSpeed = 0.5f; // lerp factor per frame toward target [0,1]
};

// Vignette params: radial darkening toward the frame corners/edges.
struct Vignette {
    bool enabled = false;
    float radius = 0.6f;   // [0,1] inner radius (1 = no vignette)
    float softness = 0.5f; // blur of the darkening edge
    float intensity = 0.5f;// 0 = none, 1 = strong edge dark
};

// Exposure multiplier from the scene's average log-luminance: scale the HDR
// average so the key value lands mid-grey. avgLum is the average LUMINANCE
// (not log) already averaged by the downsample chain; a degenerate (near-zero)
// average keeps a neutral exposure, and the result is clamped to [min,max].
inline float AutoExposureExposure(float avgLum, const AutoExposure& a) {
    if (!a.enabled || avgLum <= 1e-5f) return 1.0f;
    // EV-based: exposure = key / avgLum, clamped. log2 keeps the perceptual scale
    // so a dark scene lifts and a bright scene compresses symmetrically.
    float ev = std::log2(a.keyValue / avgLum);
    return std::clamp(std::exp2(ev), a.minExposure, a.maxExposure);
}

// Vignette factor at a screen-space UV in [0,1]: 1 in the center, falling to
// (1 - intensity) at the edges. Inner radius + softness shape the falloff;
// intensity scales how dark the edge becomes.
inline float VignetteFactor(const math::Vec2& uv, const Vignette& v) {
    if (!v.enabled) return 1.0f;
    const math::Vec2 d{uv.x - 0.5f, uv.y - 0.5f};
    const float dist = std::sqrt(d.x * d.x + d.y * d.y) * 2.0f; // 0 center, ~1.41 corners
    const float inner = v.radius;
    const float falloff = std::fmax(0.0f, (dist - inner) / std::max(v.softness, 1e-4f));
    const float t = std::clamp(falloff, 0.0f, 1.0f);
    // Smoothstep for a soft edge.
    const float st = t * t * (3.0f - 2.0f * t);
    return 1.0f - st * v.intensity;
}

// --- Built-in post-process shaders ------------------------------------------
// Fullscreen NDC quad (postQuadMesh_ in the renderer) drawn with uMVP =
// identity. Location 0 = position, 2 = uv (matches the engine vertex layout).

inline constexpr const char* kPostVertexShader = R"(
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

inline constexpr const char* kBrightPassFragmentShader = R"(
#version 330 core
in vec2 vUV;
out vec4 FragColor;
uniform sampler2D uTex;
uniform float uThreshold;
void main() {
    vec4 c = texture(uTex, vUV);
    FragColor = vec4(max(c.rgb - vec3(uThreshold), vec3(0.0)), 1.0);
}
)";

// 5-tap separable Gaussian blur, one axis per pass (uDirection selects H/V).
// The kernel is baked in (no dynamic indexing, friendly to every driver).
inline constexpr const char* kBlurFragmentShader = R"(
#version 330 core
in vec2 vUV;
out vec4 FragColor;
uniform sampler2D uTex;
uniform vec2 uTexelSize;  // 1 / target dimensions
uniform vec2 uDirection;  // (1,0) horizontal or (0,1) vertical
void main() {
    vec2 off = uTexelSize * uDirection;
    vec4 c = texture(uTex, vUV - off * 2.0) * 0.05449
           + texture(uTex, vUV - off)       * 0.244202
           + texture(uTex, vUV)             * 0.402620
           + texture(uTex, vUV + off)       * 0.244202
           + texture(uTex, vUV + off * 2.0) * 0.05449;
    FragColor = c;
}
)";

// 2x2 box downsample (half-res -> quarter-res).
inline constexpr const char* kDownsampleFragmentShader = R"(
#version 330 core
in vec2 vUV;
out vec4 FragColor;
uniform sampler2D uTex;
uniform vec2 uSrcTexelSize;  // 1 / source dimensions
void main() {
    vec2 o = uSrcTexelSize * 0.5;
    vec4 s = texture(uTex, vUV + vec2(-o.x, -o.y))
           + texture(uTex, vUV + vec2( o.x, -o.y))
           + texture(uTex, vUV + vec2(-o.x,  o.y))
           + texture(uTex, vUV + vec2( o.x,  o.y));
    FragColor = vec4(s.rgb * 0.25, 1.0);
}
)";

// A5 auto-exposure measurement. Two explicit passes:
//   1. kLuminanceShader: converts HDR -> a 1-channel log-averaged luminance in
//      .x (we average log luminance, the perceptual scalar that auto-exposure
//      keys on). Written into a small (e.g. 16x16) RGBA target.
//   2. kLuminanceReduceShader: box-averages that down to 1x1. The composite then
//      reads that single texel as the scene's average LOG luminance, exponentiates
//      it, and derives the exposure multiplier.
// The log-luminance histogram is approximated by a single average (no histogram,
// fine for a lightweight per-frame estimate). Empty (black) scene yields log ~ -inf;
// the shader clamps the min luminance so it degrades to a neutral exposure.
inline constexpr const char* kLuminanceShader = R"(
#version 330 core
in vec2 vUV;
out vec4 FragColor;
uniform sampler2D uHdr;     // the scene HDR (composite input)
void main() {
    vec3 c = texture(uHdr, vUV).rgb;
    // Perceptual luminance (Rec.709), with a floor to keep log finite on black.
    float lum = max(dot(c, vec3(0.2126, 0.7152, 0.0722)), 1e-4);
    // Average LOG luminance (writes the log so the reduce pass can average it).
    FragColor = vec4(log(lum), 0.0, 0.0, 1.0);
}
)";

inline constexpr const char* kLuminanceReduceShader = R"(
#version 330 core
in vec2 vUV;
out vec4 FragColor;
uniform sampler2D uLum;     // the log-luminance target (small)
uniform vec2 uSrcTexelSize; // 1 / source dimensions
void main() {
    vec2 o = uSrcTexelSize * 0.5;
    vec4 s = texture(uLum, vUV + vec2(-o.x, -o.y))
           + texture(uLum, vUV + vec2( o.x, -o.y))
           + texture(uLum, vUV + vec2(-o.x,  o.y))
           + texture(uLum, vUV + vec2( o.x,  o.y));
    FragColor = vec4(s.rgb * 0.25, 1.0); // averaged log luminance
}
)";

// Progressive bloom accumulation: half-res bloom + upsampled quarter-res bloom.
// The bilinear texture sampler does the upsampling; the quarter pass only ever
// holds 1/4-res data, so its contribution is added at 1/4 resolution and then
// carried up by the half-res target.
inline constexpr const char* kUpsampleAddFragmentShader = R"(
#version 330 core
in vec2 vUV;
out vec4 FragColor;
uniform sampler2D uHalf;
uniform sampler2D uQuarter;
void main() {
    vec3 halfB = texture(uHalf, vUV).rgb;
    vec3 quarter = texture(uQuarter, vUV).rgb;
    FragColor = vec4(halfB + quarter, 1.0);
}
)";

// Final composite: (hdr + bloom * strength) ACES tonemapped with exposure.
// uBloomEnabled == 0 skips the bloom term so the `--no-bloom` screenshot path
// still round-trips through the same HDR target and only the bloom addition
// differs. uTonemapEnabled == 0 uses the legacy clamp reference (T3.6
// behaviour) so the `--tonemap-compare` / `--no-tonemap` diff isolates the
// tonemapping operator; exposure only applies in the ACES branch.
inline constexpr const char* kCompositeFragmentShader = R"(
#version 330 core
in vec2 vUV;
out vec4 FragColor;
uniform sampler2D uHdr;
uniform sampler2D uBloom;
uniform sampler2D uAo;
uniform float uAoIntensity;
uniform int uAoEnabled;
uniform sampler2D uVol;
uniform float uVolStrength;
uniform int uVolEnabled;
uniform sampler2D uSsr;
uniform float uSsrStrength;
uniform int uSsrEnabled;
uniform sampler2D uFogDepth;
uniform vec3 uFogColor;
uniform float uFogDensity;
uniform float uNear;
uniform float uFar;
uniform int uFogEnabled;
uniform float uStrength;
uniform float uExposure;
uniform int uBloomEnabled;
uniform int uTonemapEnabled;
uniform int uGradeEnabled;
uniform float uSaturation;
uniform float uContrast;
uniform float uGain;
uniform float uGamma;
uniform float uLift;
uniform vec3 uTint;
uniform sampler2D uAvgLum;     // A5 1x1 average log-luminance (auto-exposure)
uniform int uAutoExposure;
uniform float uKeyValue;
uniform float uExposureMin;
uniform float uExposureMax;
uniform float uVignetteRadius;
uniform float uVignetteSoftness;
uniform float uVignetteIntensity;
uniform int uVignette;
vec3 ACESFilm(vec3 x) {
    float a = 2.51, b = 0.03, c = 2.43, d = 0.59, e = 0.14;
    // Clamp the input to the half-float max before the curve: an Inf/NaN HDR
    // value (overflow or a shader NaN) would otherwise reach clamp() below,
    // which is undefined for NaN in GLSL (matches the CPU AcesFilm guard).
    x = clamp(x, vec3(0.0), vec3(65504.0));
    return clamp((x * (a * x + b)) / (x * (c * x + d) + e), vec3(0.0), vec3(1.0));
}
void main() {
    vec3 hdr = texture(uHdr, vUV).rgb;
    vec3 c = hdr;
    if (uBloomEnabled != 0) c += texture(uBloom, vUV).rgb * uStrength;
    if (uAoEnabled != 0) {
        float ao = texture(uAo, vUV).r;
        // SSAO lightens the indirect/diffuse contribution. We have no separate
        // ambient term in the composite, so the AO scales the whole in-range
        // colour; a modest intensity keeps it from crushing lit surfaces.
        c *= mix(1.0, ao, uAoIntensity);
    }
    if (uVolEnabled != 0) c += texture(uVol, vUV).rgb * uVolStrength;
    if (uSsrEnabled != 0) c += texture(uSsr, vUV).rgb * uSsrStrength;
    if (uFogEnabled != 0) {
        vec4 dp = texture(uFogDepth, vUV);
        float ndc = dp.r + dp.g / 255.0 + dp.b / 65025.0 + dp.a / 16581375.0;
        if (ndc < 1.0) {
            float z = ndc * 2.0 - 1.0;
            float dist = (2.0 * uNear * uFar) / (uFar + uNear - z * (uFar - uNear));
            float f = 1.0 - exp(-uFogDensity * uFogDensity * dist * dist);
            c = mix(c, uFogColor, clamp(f, 0.0, 1.0));
        }
    }
    if (uTonemapEnabled != 0) {
        // A5 auto-exposure: derive the exposure multiplier from the measured
        // average log-luminance (uAvgLum), smoothed toward the target key value
        // from the previous frame's exposure. When disabled the authored scalar
        // uExposure is used verbatim (the historical pipeline).
        float exposure = uExposure;
        if (uAutoExposure != 0) {
            float avgLog = texture(uAvgLum, vec2(0.5)).r;
            float avgLum = exp(avgLog);
            if (avgLum > 1e-5) exposure = clamp(uKeyValue / avgLum, uExposureMin, uExposureMax);
        }
        vec3 graded = ACESFilm(c * exposure);
        // A1 color grading (post-tonemap, display space). Skipped when disabled
        // so the default RenderStack is pixel-identical (matches GradeColor).
        if (uGradeEnabled != 0) {
            graded = clamp(graded, vec3(0.0), vec3(1.0));
            graded *= uTint;
            float ig = 1.0 / max(uGamma, 1e-4);
            graded = pow(clamp(graded, vec3(0.0), vec3(1.0)), vec3(ig)) * uGain + vec3(uLift);
            float luma = dot(graded, vec3(0.2126, 0.7152, 0.0722));
            graded = mix(vec3(luma), graded, uSaturation);
            float ck = 1.0 + uContrast;
            graded = (graded - vec3(0.5)) * ck + vec3(0.5);
            graded = clamp(graded, vec3(0.0), vec3(1.0));
        }
        // A5 vignette: radial darken toward the frame corners (display space).
        if (uVignette != 0) {
            float dx = vUV.x - 0.5;
            float dy = vUV.y - 0.5;
            float dist = length(vec2(dx, dy)) * 2.0;
            float fall = max((dist - uVignetteRadius) / max(uVignetteSoftness, 1e-4), 0.0);
            float t = clamp(fall, 0.0, 1.0);
            float st = t * t * (3.0 - 2.0 * t);
            graded *= 1.0 - st * uVignetteIntensity;
        }
        FragColor = vec4(graded, 1.0);
    } else {
        FragColor = vec4(min(c, vec3(1.0)), 1.0);
    }
}
)";

} // namespace neon::gfx
