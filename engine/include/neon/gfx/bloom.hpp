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

// Tonemap operators exposed to the composite. ACES (Narkowicz) stays the
// default so existing scenes are unchanged; the others are selectable per
// scene/stack and mirror the GLSL in kCompositeFragmentShader / composite.frag.
enum class TonemapMode : int {
    Linear = 0,   // clamp only (legacy reference)
    Reinhard = 1, // x / (1 + x)
    Filmic = 2,   // Hable "Uncharted 2" curve, white point 11.2
    ACES = 3,     // Narkowicz ACES fit (default)
    AgX = 4,      // Sobotka AgX (compact log2 + contrast approximation)
};

// Guard for every operator: clamp away Inf and map NaN -> 0 before the curve
// (rational/pow ops on NaN are undefined in GLSL -> flicker).
inline math::Vec3 SanitizeHdr(const math::Vec3& x) {
    return {std::isnan(x.x) ? 0.0f : std::clamp(x.x, 0.0f, kAcesHdrMax),
            std::isnan(x.y) ? 0.0f : std::clamp(x.y, 0.0f, kAcesHdrMax),
            std::isnan(x.z) ? 0.0f : std::clamp(x.z, 0.0f, kAcesHdrMax)};
}

inline math::Vec3 ReinhardFilm(const math::Vec3& in) {
    const math::Vec3 x = SanitizeHdr(in);
    return {x.x / (1.0f + x.x), x.y / (1.0f + x.y), x.z / (1.0f + x.z)};
}

inline math::Vec3 Uncharted2Curve(const math::Vec3& x) {
    const float A = 0.15f, B = 0.50f, C = 0.10f, D = 0.20f, E = 0.02f, F = 0.30f;
    auto f = [&](float v) {
        return ((v * (A * v + C * B) + D * E) / (v * (A * v + B) + D * F)) - E / F;
    };
    return {f(x.x), f(x.y), f(x.z)};
}

inline math::Vec3 FilmicFilm(const math::Vec3& in) {
    const math::Vec3 x = SanitizeHdr(in) * 2.0f;
    const float whiteScale = 1.0f / Uncharted2Curve({11.2f, 11.2f, 11.2f}).x;
    const math::Vec3 c = Uncharted2Curve(x) * whiteScale;
    return {std::clamp(c.x, 0.0f, 1.0f), std::clamp(c.y, 0.0f, 1.0f),
            std::clamp(c.z, 0.0f, 1.0f)};
}

// AgX (Sobotka) compact approximation: inset matrix -> log2 encode ->
// polynomial contrast -> outset matrix. Matrices are written as ROWS and
// applied by dot product (identical CPU/GPU, no column/row ambiguity).
inline float AgxContrastApprox(float x) {
    const float x2 = x * x;
    const float x4 = x2 * x2;
    return 15.5f * x4 * x2 - 40.14f * x4 * x + 31.96f * x4 - 6.868f * x2 * x +
           0.4298f * x2 + 0.1191f * x - 0.00232f;
}
inline math::Vec3 AgxFilm(const math::Vec3& in) {
    const math::Vec3 x = SanitizeHdr(in);
    // Inset matrix rows.
    const math::Vec3 r0{0.842479062253094f, 0.0423282422610123f, 0.0423756549057051f};
    const math::Vec3 r1{0.0784335999999992f, 0.878468636469772f, 0.0784336f};
    const math::Vec3 r2{0.0792237451477643f, 0.0791661274605434f, 0.879142973793104f};
    math::Vec3 v{math::Dot(r0, x), math::Dot(r1, x), math::Dot(r2, x)};
    constexpr float minEv = -12.47393f, maxEv = 4.026069f;
    auto enc = [&](float c) {
        const float l = std::log2(std::max(c, 1e-10f));
        return (std::clamp(l, minEv, maxEv) - minEv) / (maxEv - minEv);
    };
    v = {AgxContrastApprox(enc(v.x)), AgxContrastApprox(enc(v.y)), AgxContrastApprox(enc(v.z))};
    // Outset matrix rows (inverse).
    const math::Vec3 o0{1.19687900512017f, -0.0528968517574562f, -0.0529716355144438f};
    const math::Vec3 o1{-0.0980208811401368f, 1.15190312990417f, -0.0980434501171241f};
    const math::Vec3 o2{-0.0990297440797205f, -0.0989611768448433f, 1.15107367264116f};
    v = {math::Dot(o0, v), math::Dot(o1, v), math::Dot(o2, v)};
    return {std::clamp(v.x, 0.0f, 1.0f), std::clamp(v.y, 0.0f, 1.0f),
            std::clamp(v.z, 0.0f, 1.0f)};
}

// Applies the selected operator to an already exposure-scaled HDR colour.
inline math::Vec3 TonemapColor(TonemapMode mode, const math::Vec3& hdr) {
    switch (mode) {
        case TonemapMode::Linear: {
            const math::Vec3 s = SanitizeHdr(hdr);
            return {std::min(s.x, 1.0f), std::min(s.y, 1.0f), std::min(s.z, 1.0f)};
        }
        case TonemapMode::Reinhard:
            return ReinhardFilm(hdr);
        case TonemapMode::Filmic:
            return FilmicFilm(hdr);
        case TonemapMode::AgX:
            return AgxFilm(hdr);
        case TonemapMode::ACES:
        default:
            return AcesFilm(hdr);
    }
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
    float gamma = 1.0f;        // >1 brightens mid-tones (pow(c, 1/gamma));
                               // <1 darkens them
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
    float adaptationSpeed = 0.02f; // lerp factor per frame toward target [0,1]
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
uniform sampler2D uAvgLum;   // 1x1 adapted exposure (auto-exposure on)
uniform float uThreshold;
uniform float uExposure;
uniform int uAutoExposure;
void main() {
    vec4 c = texture(uTex, vUV);
    // The bright pass runs BEFORE the composite applies exposure. Keying the
    // threshold on raw HDR values decoupled bloom from displayed brightness:
    // with auto exposure lifting a dark scene no pixel crossed the threshold
    // and the glow vanished exactly when the image looked brightest. Threshold
    // on the SAME effective exposure the composite will use - but return the
    // result in UNEXPOSED units: the composite adds bloom to the scene before
    // its single `* exposure`, so emitting an exposed bright pass multiplied
    // the glow by exposure a second time (bloom scaled by exposure^2).
    // max(c*e - t, 0)/e == max(c - t/e, 0) for e > 0.
    float exposure = uExposure;
    if (uAutoExposure != 0) exposure = uExposure * max(texture(uAvgLum, vec2(0.5)).r, 1e-4);
    float e = max(exposure, 1e-4);
    FragColor = vec4(max(c.rgb - vec3(uThreshold) / e, vec3(0.0)), 1.0);
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
    // 9-tap gaussian (±4 texels): the SSR march dithers its start per pixel
    // (to break step banding), and a ±2 kernel left that as fine dense
    // stripes. The wider kernel averages the dither away.
    vec4 c = texture(uTex, vUV - off * 4.0) * 0.0048
           + texture(uTex, vUV - off * 3.0) * 0.0287
           + texture(uTex, vUV - off * 2.0) * 0.1028
           + texture(uTex, vUV - off)       * 0.2210
           + texture(uTex, vUV)             * 0.2854
           + texture(uTex, vUV + off)       * 0.2210
           + texture(uTex, vUV + off * 2.0) * 0.1028
           + texture(uTex, vUV + off * 3.0) * 0.0287
           + texture(uTex, vUV + off * 4.0) * 0.0048;
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
    // Each output texel stands for a 32x32 block of the full-res HDR (the target
    // is 1/32). Average a 64-tap stratified sample of that block so a small but
    // bright emitter anywhere in the block still votes - the old single-point
    // sample only saw it when it sat exactly under the texel centre, so exposure
    // ignored muzzle flashes / lamps that fell between samples.
    vec2 texel = 1.0 / vec2(textureSize(uHdr, 0));
    vec3 acc = vec3(0.0);
    for (int y = 0; y < 8; ++y) {
        for (int x = 0; x < 8; ++x) {
            vec2 off = (vec2(float(x), float(y)) - 3.5) * 4.0 * texel;
            acc += texture(uHdr, vUV + off).rgb;
        }
    }
    acc *= (1.0 / 64.0);
    float lum = max(dot(acc, vec3(0.2126, 0.7152, 0.0722)), 1e-4);
    // Average LOG luminance (writes the log so the reduce pass can average it).
    FragColor = vec4(log(lum), 0.0, 0.0, 1.0);
}
)";

inline constexpr const char* kLuminanceReduceShader = R"(
#version 330 core
in vec2 vUV;
out vec4 FragColor;
uniform sampler2D uLum;         // the log-luminance target (small)
uniform vec4 uSceneVpRect;      // letterboxed scene area, normalised HDR UV
void main() {
    // Average EVERY texel of the small target (each already a 32x32 HDR block
    // average), instead of a fixed 16x16 subsample that left most of the target
    // with no vote. Letterbox taps are skipped so clear-colour black can't
    // poison the mean.
    ivec2 sz = textureSize(uLum, 0);
    float sum = 0.0;
    float n = 0.0;
    for (int y = 0; y < sz.y; ++y) {
        for (int x = 0; x < sz.x; ++x) {
            vec2 uv = (vec2(float(x), float(y)) + 0.5) / vec2(sz);
            if (uv.x < uSceneVpRect.x || uv.y < uSceneVpRect.y ||
                uv.x > uSceneVpRect.x + uSceneVpRect.z ||
                uv.y > uSceneVpRect.y + uSceneVpRect.w) continue;
            sum += texelFetch(uLum, ivec2(x, y), 0).r;
            n += 1.0;
        }
    }
    float avg = n > 0.0 ? sum / n : log(0.18);
    FragColor = vec4(avg, 0.0, 0.0, 1.0); // averaged log luminance
}
)";

// Auto-exposure ADAPTATION: turns the raw per-frame target exposure
// (key / avgLum) into a temporally smoothed one by lerping toward it from the
// previous frame's exposure. Without this the raw value follows every bright
// particle/projectile flash and the whole frame strobes.
inline constexpr const char* kExposureAdaptShader = R"(
#version 330 core
in vec2 vUV;
out vec4 FragColor;
uniform sampler2D uAvgLum;       // 1x1 average LOG luminance (this frame)
uniform sampler2D uPrevExposure; // 1x1 previous frame's smoothed exposure
uniform float uKeyValue;
uniform float uExposureMin;
uniform float uExposureMax;
uniform float uAdaptation;       // per-frame lerp toward the target [0,1]
void main() {
    // Clamp the measured average: bright HDR flashes (fireballs, additive
    // particles) would otherwise swing the target exposure with every cast.
    float avgLum = clamp(exp(texture(uAvgLum, vec2(0.5)).r), 0.03, 4.0);
    float target = clamp(uKeyValue / avgLum, uExposureMin, uExposureMax);
    float prev = max(texture(uPrevExposure, vec2(0.5)).r, 1e-4);
    FragColor = vec4(mix(prev, target, clamp(uAdaptation, 0.0, 1.0)), 0.0, 0.0, 1.0);
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
uniform float uBloomWidth;   // filter footprint multiplier (1 = classic bilinear)
void main() {
    vec3 halfB = texture(uHalf, vUV).rgb;
    // 9-tap tent instead of a single bilinear tap: the same target count, but
    // the halo no longer has the tight "dirty" edge a one-tap upsample leaves.
    // A wide low-weight skirt (2.5x footprint on the diagonals) extends the
    // glow outward, which is what makes bright VFX read as a real bloom rather
    // than a local blur. Weights sum to 1 so the bloom energy is unchanged.
    vec2 t = uBloomWidth / vec2(textureSize(uQuarter, 0));
    vec3 quarter = texture(uQuarter, vUV).rgb * 0.25
                 + (texture(uQuarter, vUV + vec2( t.x, 0.0)).rgb +
                    texture(uQuarter, vUV + vec2(-t.x, 0.0)).rgb +
                    texture(uQuarter, vUV + vec2(0.0,  t.y)).rgb +
                    texture(uQuarter, vUV + vec2(0.0, -t.y)).rgb) * 0.125
                 + (texture(uQuarter, vUV + vec2( t.x,  t.y) * 2.5).rgb +
                    texture(uQuarter, vUV + vec2(-t.x,  t.y) * 2.5).rgb +
                    texture(uQuarter, vUV + vec2( t.x, -t.y) * 2.5).rgb +
                    texture(uQuarter, vUV + vec2(-t.x, -t.y) * 2.5).rgb) * 0.0625;
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
uniform sampler2D uGBuffer;      // G-buffer: RGB = indirect radiance (A = roughness)
uniform int uHasGBuffer;         // 1 when uGBuffer is bound (SSAO indirect-only path)
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
uniform int uTonemapMode;      // 0 Linear, 1 Reinhard, 2 Filmic, 3 ACES
uniform int uGammaCorrect;     // 1 = encode display output from linear to sRGB
uniform int uDofEnabled;
uniform float uFocusDistance;  // world units
uniform float uFocusRange;     // world units to reach max blur
uniform float uDofStrength;    // 0..1 blur amount
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
vec3 ReinhardFilm(vec3 x) {
    x = clamp(x, vec3(0.0), vec3(65504.0));
    return x / (vec3(1.0) + x);
}
vec3 Uncharted2Curve(vec3 x) {
    const float A = 0.15, B = 0.50, C = 0.10, D = 0.20, E = 0.02, F = 0.30;
    return ((x * (A * x + C * B) + D * E) / (x * (A * x + B) + D * F)) - E / F;
}
vec3 FilmicFilm(vec3 x) {
    x = clamp(x, vec3(0.0), vec3(65504.0)) * 2.0;
    float whiteScale = 1.0 / Uncharted2Curve(vec3(11.2)).x;
    return clamp(Uncharted2Curve(x) * whiteScale, vec3(0.0), vec3(1.0));
}
float AgxContrastApprox(float x) {
    float x2 = x * x;
    float x4 = x2 * x2;
    return 15.5 * x4 * x2 - 40.14 * x4 * x + 31.96 * x4 - 6.868 * x2 * x +
           0.4298 * x2 + 0.1191 * x - 0.00232;
}
vec3 AgxFilm(vec3 x) {
    x = clamp(x, vec3(0.0), vec3(65504.0));
    // Inset matrix (rows) applied by dot product.
    vec3 r0 = vec3(0.842479062253094, 0.0423282422610123, 0.0423756549057051);
    vec3 r1 = vec3(0.0784335999999992, 0.878468636469772, 0.0784336);
    vec3 r2 = vec3(0.0792237451477643, 0.0791661274605434, 0.879142973793104);
    vec3 v = vec3(dot(r0, x), dot(r1, x), dot(r2, x));
    const float minEv = -12.47393;
    const float maxEv = 4.026069;
    v = vec3(
        AgxContrastApprox((clamp(log2(max(v.x, 1e-10)), minEv, maxEv) - minEv) / (maxEv - minEv)),
        AgxContrastApprox((clamp(log2(max(v.y, 1e-10)), minEv, maxEv) - minEv) / (maxEv - minEv)),
        AgxContrastApprox((clamp(log2(max(v.z, 1e-10)), minEv, maxEv) - minEv) / (maxEv - minEv)));
    // Outset matrix (rows).
    vec3 o0 = vec3(1.19687900512017, -0.0528968517574562, -0.0529716355144438);
    vec3 o1 = vec3(-0.0980208811401368, 1.15190312990417, -0.0980434501171241);
    vec3 o2 = vec3(-0.0990297440797205, -0.0989611768448433, 1.15107367264116);
    v = vec3(dot(o0, v), dot(o1, v), dot(o2, v));
    return clamp(v, vec3(0.0), vec3(1.0));
}
// 0 Linear, 1 Reinhard, 2 Filmic, 3 ACES, 4 AgX (mirrors bloom.hpp TonemapColor).
vec3 TonemapColor(int mode, vec3 x) {
    if (mode == 0) return clamp(x, vec3(0.0), vec3(1.0));
    if (mode == 1) return ReinhardFilm(x);
    if (mode == 2) return FilmicFilm(x);
    if (mode == 4) return AgxFilm(x);
    return ACESFilm(x);
}
// Linear -> sRGB display encode (gamma-correct output, off by default).
vec3 LinearToSrgb(vec3 c) {
    c = clamp(c, vec3(0.0), vec3(1.0));
    return mix(c * 12.92, 1.055 * pow(c, vec3(1.0 / 2.4)) - 0.055, step(vec3(0.0031308), c));
}
// Interleaved gradient noise - +-0.5 LSB of it before the 8-bit store turns
// smooth-gradient posterization (horizontal stripes on large flat ground /
// sky) into imperceptible noise. Matches the Vulkan composite.
float OutDither(vec2 p) {
    return (fract(52.9829189 * fract(dot(p, vec2(0.06711056, 0.00583715)))) - 0.5)
           / 255.0;
}
void main() {
    vec3 hdr = texture(uHdr, vUV).rgb;
    vec3 c = hdr;
    // Depth of field: gather the HDR within a circle of confusion derived from
    // the linear view depth (the same depth encoding the fog term uses). Taps
    // are weighted by depth proximity so out-of-focus background does not bleed
    // sharply onto an in-focus foreground.
    if (uDofEnabled != 0) {
        vec4 cdp = texture(uFogDepth, vUV);
        float cndc = cdp.r + cdp.g / 255.0 + cdp.b / 65025.0 + cdp.a / 16581375.0;
        float cdist = cndc * uFar;
        float coc = clamp(abs(cdist - uFocusDistance) / max(uFocusRange, 1e-3), 0.0, 1.0) *
                    clamp(uDofStrength, 0.0, 1.0);
        if (coc > 0.001) {
            vec2 texel = 1.0 / vec2(textureSize(uHdr, 0));
            float radius = coc * 24.0;
            vec3 acc = hdr;
            float wsum = 1.0;
            for (int i = 0; i < 12; ++i) {
                float ang = float(i) * 2.39996323;
                float r = sqrt((float(i) + 0.5) / 12.0) * radius;
                vec2 off = vec2(cos(ang), sin(ang)) * r * texel;
                vec3 sc = texture(uHdr, vUV + off).rgb;
                vec4 sp = texture(uFogDepth, vUV + off);
                float sndc = sp.r + sp.g / 255.0 + sp.b / 65025.0 + sp.a / 16581375.0;
                float sw = clamp(1.0 - abs(sndc * uFar - cdist) / max(uFocusRange, 1e-3),
                                 0.0, 1.0);
                acc += sc * sw;
                wsum += sw;
            }
            c = acc / wsum;
        }
    }
    // SSAO scales ONLY the indirect/ambient term. The G-buffer pass (forward
    // path has no MRT) wrote the indirect radiance to uGBuffer.rgb, so we remove
    // indirect*(1-AO) and leave direct sunlight intact. Without the G-buffer
    // (e.g. SSAO on a backend that skipped it) fall back to the whole-colour
    // multiply, which is the pre-G-buffer behaviour.
    if (uAoEnabled != 0) {
        float aoEff = mix(1.0, texture(uAo, vUV).r, uAoIntensity);
        if (uHasGBuffer != 0) {
            c -= texture(uGBuffer, vUV).rgb * (1.0 - aoEff);
        } else {
            c *= aoEff;
        }
    }
    if (uBloomEnabled != 0) c += texture(uBloom, vUV).rgb * uStrength;
    if (uVolEnabled != 0) c += texture(uVol, vUV).rgb * uVolStrength;
    // SSR BLENDS by its alpha (fresnel * ray fade), it is not ADDed: adding
    // the fully-lit scene colour on top of an already-lit surface double-lit
    // everything and washed reflections out to white.
    if (uSsrEnabled != 0) {
        vec4 ssr = texture(uSsr, vUV);
        c = mix(c, ssr.rgb, clamp(uSsrStrength * ssr.a, 0.0, 1.0));
    }
    if (uFogEnabled != 0) {
        vec4 dp = texture(uFogDepth, vUV);
        float ndc = dp.r + dp.g / 255.0 + dp.b / 65025.0 + dp.a / 16581375.0;
        if (ndc < 1.0) {
            // The scene-depth resource stores LINEAR view distance / uFar (see
            // the SSAO depth encoder), NOT window NDC depth. Decoding it through
            // the perspective formula collapsed every distance to ~2*uNear and
            // the exp^2 density fog never appeared.
            float dist = ndc * uFar;
            float f = 1.0 - exp(-uFogDensity * uFogDensity * dist * dist);
            c = mix(c, uFogColor, clamp(f, 0.0, 1.0));
        }
    }
    if (uTonemapEnabled != 0) {
        // A5 auto-exposure: uAvgLum carries the TEMPORALLY SMOOTHED exposure
        // produced by the adaptation pass (raw key/avgLum strobed on bright
        // flashes). The adapted value MULTIPLIES the authored exposure instead
        // of replacing it -- the author's exposure is a scene look, and the old
        // replace silently discarded it whenever auto exposure was on.
        float exposure = uExposure;
        if (uAutoExposure != 0) exposure = uExposure * max(texture(uAvgLum, vec2(0.5)).r, 1e-4);
        vec3 graded = TonemapColor(uTonemapMode, c * exposure);
        if (uGammaCorrect != 0) graded = LinearToSrgb(graded);
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
        FragColor = vec4(graded + OutDither(gl_FragCoord.xy), 1.0);
    } else {
        vec3 outc = min(c, vec3(1.0));
        if (uGammaCorrect != 0) outc = LinearToSrgb(outc);
        FragColor = vec4(outc + OutDither(gl_FragCoord.xy), 1.0);
    }
}
)";

// FXAA (Lottes) resolve, run as the terminal pass after the composite has
// written the display-referred LDR image. A strength of 0 returns the centre
// texel untouched (an exact copy), so the pass can stay in the graph while
// anti-aliasing is off. uTexelSize is 1/resolution.
inline constexpr const char* kFxaaFragmentShader = R"(
#version 330 core
in vec2 vUV;
out vec4 FragColor;
uniform sampler2D uColor;
uniform vec2 uTexelSize;
uniform float uFxaaStrength;
float FxaaLuma(vec3 c) { return dot(c, vec3(0.299, 0.587, 0.114)); }
void main() {
    vec3 rgbM = texture(uColor, vUV).rgb;
    if (uFxaaStrength <= 0.0) {
        FragColor = vec4(rgbM, 1.0);
        return;
    }
    vec3 rgbNW = texture(uColor, vUV + vec2(-1.0, -1.0) * uTexelSize).rgb;
    vec3 rgbNE = texture(uColor, vUV + vec2( 1.0, -1.0) * uTexelSize).rgb;
    vec3 rgbSW = texture(uColor, vUV + vec2(-1.0,  1.0) * uTexelSize).rgb;
    vec3 rgbSE = texture(uColor, vUV + vec2( 1.0,  1.0) * uTexelSize).rgb;
    float lM = FxaaLuma(rgbM);
    float lNW = FxaaLuma(rgbNW), lNE = FxaaLuma(rgbNE);
    float lSW = FxaaLuma(rgbSW), lSE = FxaaLuma(rgbSE);
    float lMin = min(lM, min(min(lNW, lNE), min(lSW, lSE)));
    float lMax = max(lM, max(max(lNW, lNE), max(lSW, lSE)));
    // Early out on flat areas (also protects mostly-uniform dark frames).
    float range = lMax - lMin;
    if (range < max(0.0312, lMax * 0.125)) {
        FragColor = vec4(rgbM, 1.0);
        return;
    }
    vec2 dir = vec2(-((lNW + lNE) - (lSW + lSE)), ((lNW + lSW) - (lNE + lSE)));
    float dirReduce = max((lNW + lNE + lSW + lSE) * 0.25 * 0.125, 1.0 / 128.0);
    float rcp = 1.0 / (min(abs(dir.x), abs(dir.y)) + dirReduce);
    dir = clamp(dir * rcp, vec2(-8.0), vec2(8.0)) * uTexelSize;
    vec3 rgbA = 0.5 * (texture(uColor, vUV + dir * (1.0 / 3.0 - 0.5)).rgb +
                       texture(uColor, vUV + dir * (2.0 / 3.0 - 0.5)).rgb);
    vec3 rgbB = rgbA * 0.5 +
                0.25 * (texture(uColor, vUV + dir * -0.5).rgb +
                        texture(uColor, vUV + dir * 0.5).rgb);
    float lB = FxaaLuma(rgbB);
    vec3 result = (lB < lMin || lB > lMax) ? rgbA : rgbB;
    FragColor = vec4(mix(rgbM, result, uFxaaStrength), 1.0);
}
)";

} // namespace neon::gfx
