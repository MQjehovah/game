#pragma once

// Step E: temporal anti-aliasing.
//
// The scene is rendered with a per-frame sub-pixel camera jitter; this pass
// resolves the jittered frame against an accumulated history buffer, using a
// camera-only reprojection (the world position is rebuilt from the resolved
// depth, then pushed through the previous frame's view-projection) and a
// neighbourhood clamp that keeps history close to the current 3x3 colour
// footprint.
//
// Camera-only reprojection means moving geometry (units, projectiles) has no
// motion vector of its own: the neighbourhood clamp is what suppresses the
// resulting ghosting, which is why the blend keeps a healthy slice of the
// current frame every tick.
//
// A second, optional pass runs a 3x3 tent unsharp mask over the resolved image.
// Accumulating N jittered frames is what removes the edge aliasing, but it also
// averages away high-frequency texture detail; the unsharp mask puts the detail
// back without re-introducing edge crawl.

#include "neon/core/log.hpp"
#include "neon/gfx/backend.hpp"
#include "neon/math/mat4.hpp"

namespace neon::gfx {

// Resolves a jittered HDR frame against its history. Fullscreen; samples the
// current colour (0), previous output (1) and the resolved depth (2).
inline constexpr const char* kTaaFragmentShader = R"(
#version 330 core
in vec2 vUV;
out vec4 FragColor;
uniform sampler2D uCurrent;
uniform sampler2D uHistory;
uniform sampler2D uDepth;
uniform sampler2D uVelocity;    // xy = offset to ADD to vUV, a > 0.5 = moving object
uniform mat4 uInvViewProj;
uniform mat4 uPrevViewProj;
uniform vec2 uScreenSize;
uniform float uBlend;        // weight of the CURRENT frame (0.1 = 10% new)
uniform int uValidHistory;   // 0 on the first frame / after a resize
uniform int uHasVelocity;    // 1 when the motion-vector target is bound

// Catmull-Rom (bicubic) history tap. The history is consumed at a *fractional*
// sub-pixel offset every frame (the jitter), and the accumulation re-samples it
// with weight (1-uBlend) each tick, so the resampling kernel is applied
// recursively ~1/uBlend times. A bilinear tap has a wide kernel, and that gets
// amplified into several pixels of smear; the sharper Catmull-Rom kernel keeps
// the recursion from destroying texture detail.
float CatmullWeight(float f, int i) {
    float f2 = f * f;
    float f3 = f2 * f;
    if (i == 0) return -0.5 * f3 + f2 - 0.5 * f;
    if (i == 1) return  1.5 * f3 - 2.5 * f2 + 1.0;
    if (i == 2) return -1.5 * f3 + 2.0 * f2 + 0.5 * f;
    return 0.5 * f3 - 0.5 * f2;
}

// Returns a 4-element weight row without needing dynamic vector indexing.
vec4 CatmullRow(float f) {
    return vec4(CatmullWeight(f, 0), CatmullWeight(f, 1),
                CatmullWeight(f, 2), CatmullWeight(f, 3));
}

vec3 HistoryTap(vec2 base, float dx, float dy) {
    vec2 uv = (base + vec2(dx, dy) + 0.5) / uScreenSize;
    return texture(uHistory, clamp(uv, vec2(0.0), vec2(1.0))).rgb;
}

vec3 SampleHistory(vec2 uv) {
    vec2 pos = uv * uScreenSize - 0.5;
    vec2 base = floor(pos);
    vec2 f = pos - base;
    vec4 wx = CatmullRow(f.x);
    vec4 wy = CatmullRow(f.y);
    vec3 acc = vec3(0.0);
    acc += wx.x * (wy.x * HistoryTap(base, -1.0, -1.0) + wy.y * HistoryTap(base, 0.0, -1.0) +
                   wy.z * HistoryTap(base,  1.0, -1.0) + wy.w * HistoryTap(base, 2.0, -1.0));
    acc += wx.y * (wy.x * HistoryTap(base, -1.0,  0.0) + wy.y * HistoryTap(base, 0.0,  0.0) +
                   wy.z * HistoryTap(base,  1.0,  0.0) + wy.w * HistoryTap(base, 2.0,  0.0));
    acc += wx.z * (wy.x * HistoryTap(base, -1.0,  1.0) + wy.y * HistoryTap(base, 0.0,  1.0) +
                   wy.z * HistoryTap(base,  1.0,  1.0) + wy.w * HistoryTap(base, 2.0,  1.0));
    acc += wx.w * (wy.x * HistoryTap(base, -1.0,  2.0) + wy.y * HistoryTap(base, 0.0,  2.0) +
                   wy.z * HistoryTap(base,  1.0,  2.0) + wy.w * HistoryTap(base, 2.0,  2.0));
    return acc;
}

void main() {
    vec3 cur = texture(uCurrent, vUV).rgb;
    bool valid = false;
    vec2 prevUv = vUV;
    if (uValidHistory == 1) {
        // 1) Per-object motion vector: exact for anything the velocity pass
        //    rasterized this frame (moving units, props, VFX quads).
        if (uHasVelocity == 1) {
            vec4 mv = texture(uVelocity, vUV);
            if (mv.a > 0.5) {
                vec2 uv = vUV + mv.xy;
                if (uv.x >= 0.0 && uv.x <= 1.0 && uv.y >= 0.0 && uv.y <= 1.0) {
                    prevUv = uv;
                    valid = true;
                }
            }
        }
        // 2) Otherwise camera-only reprojection from the scene depth: this is
        //    what static geometry (no motion vector written) uses, and the
        //    fallback when a motion vector is missing or off-screen.
        if (!valid) {
            float d = texture(uDepth, vUV).r;
            if (d < 0.99999) {   // d >= 1 == sky / nothing rendered
                vec4 clip = vec4(vUV * 2.0 - 1.0, d * 2.0 - 1.0, 1.0);
                vec4 wp = uInvViewProj * clip;
                if (abs(wp.w) >= 1e-7) {
                    vec4 pc = uPrevViewProj * vec4(wp.xyz / wp.w, 1.0);
                    if (pc.w > 1e-6) {
                        vec2 uv = (pc.xy / pc.w) * 0.5 + 0.5;
                        if (uv.x >= 0.0 && uv.x <= 1.0 && uv.y >= 0.0 && uv.y <= 1.0) {
                            prevUv = uv;
                            valid = true;
                        }
                    }
                }
            }
        }
    }
    vec3 outColor = cur;
    if (valid) {
        vec3 hist = SampleHistory(prevUv);
        // Neighbourhood clip: the 3x3 min/max box in the CURRENT frame bounds
        // what history is allowed to contribute, which is what kills ghosting
        // from geometry that moved without a velocity vector.
        vec3 mn = cur, mx = cur;
        for (int y = -1; y <= 1; ++y) {
            for (int x = -1; x <= 1; ++x) {
                if (x == 0 && y == 0) continue;
                vec3 c = texture(uCurrent, vUV + vec2(float(x), float(y)) / uScreenSize).rgb;
                mn = min(mn, c);
                mx = max(mx, c);
            }
        }
        // Widen the box a touch so flat areas keep accumulating instead of
        // snapping to the current sample.
        vec3 pad = (mx - mn) * 0.15;
        hist = clamp(hist, mn - pad, mx + pad);
        outColor = mix(cur, hist, clamp(1.0 - uBlend, 0.0, 1.0));
    }
    FragColor = vec4(outColor, 1.0);
}
)";

// Unsharp mask applied to the resolved frame (see the header note). 9 taps; the
// amount is a straight multiplier on the high-pass term, 0 = disabled.
inline constexpr const char* kTaaSharpenFragmentShader = R"(
#version 330 core
in vec2 vUV;
out vec4 FragColor;
uniform sampler2D uSource;
uniform vec2 uScreenSize;
uniform float uSharpen;

void main() {
    vec2 t = 1.0 / uScreenSize;
    vec3 c  = texture(uSource, vUV).rgb;
    vec3 n  = texture(uSource, vUV + vec2(0.0, -t.y)).rgb;
    vec3 s  = texture(uSource, vUV + vec2(0.0,  t.y)).rgb;
    vec3 e  = texture(uSource, vUV + vec2( t.x, 0.0)).rgb;
    vec3 w  = texture(uSource, vUV + vec2(-t.x, 0.0)).rgb;
    vec3 ne = texture(uSource, vUV + vec2( t.x, -t.y)).rgb;
    vec3 nw = texture(uSource, vUV + vec2(-t.x, -t.y)).rgb;
    vec3 se = texture(uSource, vUV + vec2( t.x,  t.y)).rgb;
    vec3 sw = texture(uSource, vUV + vec2(-t.x,  t.y)).rgb;
    vec3 blur = (c * 4.0 + (n + s + e + w) * 2.0 + (ne + nw + se + sw)) / 16.0;
    vec3 sharp = c + (c - blur) * uSharpen;
    FragColor = vec4(max(sharp, vec3(0.0)), 1.0);
}
)";

// Owns the ping-pong history targets + the resolve program. The renderer feeds
// it the resolved scene colour, the resolved depth and both (jittered)
// view-projection matrices once per frame; the returned handle is the freshly
// accumulated image, which the post chain then blooms/composites instead of the
// raw scene colour.
class Taa {
public:
    bool Init(IRenderBackend& backend, int width, int height);
    // (Re)allocates the history at the render resolution. Returns false when the
    // targets cannot be created (TAA then stays off).
    bool Resize(IRenderBackend& backend, int width, int height);
    // Releases the history targets; `keepShader` preserves the program so the
    // renderer can keep it across target rebuilds.
    void Shutdown(IRenderBackend& backend, bool keepShader);
    bool Ready() const {
        return shader_.Valid() && history_[0].Valid() && history_[1].Valid();
    }
    int Width() const { return w_; }
    int Height() const { return h_; }
    // High-pass gain of the post-resolve unsharp mask (0 = off, ~0.4 = default).
    void SetSharpen(float amount);
    float Sharpen() const { return sharpen_; }
    RenderTargetHandle Resolve(IRenderBackend& backend, RenderTargetHandle current,
                               TextureHandle depth, const math::Mat4& invViewProj,
                               const math::Mat4& prevViewProj, MeshHandle postQuad, float blend,
                               bool validHistory, TextureHandle velocity);

private:
    ShaderHandle shader_;
    ShaderHandle sharpenShader_;
    RenderTargetHandle history_[2];
    RenderTargetHandle scratch_;
    int write_ = 0;
    int w_ = 0;
    int h_ = 0;
    float sharpen_ = 0.4f;
};

} // namespace neon::gfx