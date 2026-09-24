#version 450
#extension GL_GOOGLE_include_directive : require
// Temporal AA resolve (step E). Port of the GL program in
// engine/include/neon/gfx/taa.hpp with the two Vulkan orientation changes the
// post passes share: UVs are y-down, so the world position is rebuilt from a
// y-flipped clip position through the engine GL-convention inverse
// view-projection, and the reprojected previous UV is flipped back. Everything
// else (Catmull-Rom history tap, neighbourhood clip, blend) is identical.
layout(location = 0) in vec2 vUV;
layout(location = 0) out vec4 FragColor;

#include "engine_ubo.glsl"

layout(set = 1, binding = 0) uniform sampler2D uCurrent;
layout(set = 1, binding = 1) uniform sampler2D uHistory;
layout(set = 1, binding = 2) uniform sampler2D uDepth;
layout(set = 1, binding = 3) uniform sampler2D uVelocity; // xy = offset to ADD to vUV, a > 0.5 = moving object

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

vec4 CatmullRow(float f) {
    return vec4(CatmullWeight(f, 0), CatmullWeight(f, 1),
                CatmullWeight(f, 2), CatmullWeight(f, 3));
}

vec3 HistoryTap(vec2 base, float dx, float dy) {
    vec2 uv = (base + vec2(dx, dy) + 0.5) / eng.uScreenSize;
    return texture(uHistory, clamp(uv, vec2(0.0), vec2(1.0))).rgb;
}

vec3 SampleHistory(vec2 uv) {
    vec2 pos = uv * eng.uScreenSize - 0.5;
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
    if (eng.uValidHistory == 1) {
        // 1) Per-object motion vector: exact for anything the velocity pass
        //    rasterized this frame (moving units, props, VFX quads).
        if (eng.uHasVelocity == 1) {
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
                vec4 clip = vec4(vUV.x * 2.0 - 1.0, 1.0 - vUV.y * 2.0, d * 2.0 - 1.0, 1.0);
                vec4 wp = eng.uInvViewProj * clip;
                if (abs(wp.w) >= 1e-7) {
                    vec4 pc = eng.uPrevViewProj * vec4(wp.xyz / wp.w, 1.0);
                    if (pc.w > 1e-6) {
                        vec2 ndc = pc.xy / pc.w;
                        vec2 uv = vec2(ndc.x * 0.5 + 0.5, 0.5 - ndc.y * 0.5);
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
                vec3 c = texture(uCurrent, vUV + vec2(float(x), float(y)) / eng.uScreenSize).rgb;
                mn = min(mn, c);
                mx = max(mx, c);
            }
        }
        vec3 pad = (mx - mn) * 0.15;
        hist = clamp(hist, mn - pad, mx + pad);
        outColor = mix(cur, hist, clamp(1.0 - eng.uBlend, 0.0, 1.0));
    }
    FragColor = vec4(outColor, 1.0);
}
