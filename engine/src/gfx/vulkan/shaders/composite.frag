#version 450
#extension GL_GOOGLE_include_directive : require
// Final composite: HDR scene + bloom, AO / volumetric / SSR terms, distance
// fog, then ACES tonemapping with (optional) auto exposure, colour grading and
// a vignette. Mirrors the engine kCompositeFragmentShader used by the GL path.
layout(location = 0) in vec2 vUV;
layout(location = 0) out vec4 FragColor;

#include "engine_ubo.glsl"

layout(set = 1, binding = 0) uniform sampler2D uHdr;
layout(set = 1, binding = 1) uniform sampler2D uBloom;
layout(set = 1, binding = 2) uniform sampler2D uAo;
layout(set = 1, binding = 3) uniform sampler2D uVol;
layout(set = 1, binding = 4) uniform sampler2D uSsr;
layout(set = 1, binding = 5) uniform sampler2D uFogDepth;
layout(set = 1, binding = 6) uniform sampler2D uAvgLum;
layout(set = 1, binding = 7) uniform sampler2D uGBuffer; // RGB = indirect radiance

vec3 ACESFilm(vec3 x) {
    float a = 2.51, b = 0.03, c = 2.43, d = 0.59, e = 0.14;
    // Clamp before the curve: an Inf/NaN HDR value would otherwise reach the
    // clamp below, which is undefined for NaN in GLSL.
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
// Interleaved gradient noise (Jimenez 2014) - the same per-pixel dither the
// lit shader uses for shadow kernels. Adding +-0.5 LSB of it before the 8-bit
// store turns smooth-gradient posterization (horizontal stripes on large flat
// ground / sky) into imperceptible noise.
float OutDither(vec2 p) {
    return (fract(52.9829189 * fract(dot(p, vec2(0.06711056, 0.00583715)))) - 0.5)
           / 255.0;
}
void main() {
    vec3 hdr = texture(uHdr, vUV).rgb;
    vec3 c = hdr;
    // Depth of field gather (GL parity, mirrors bloom.hpp kCompositeFragmentShader).
    if (eng.uDofEnabled != 0) {
        vec4 cdp = texture(uFogDepth, vUV);
        float cndc = cdp.r + cdp.g / 255.0 + cdp.b / 65025.0 + cdp.a / 16581375.0;
        float cdist = cndc * eng.uFar;
        float coc = clamp(abs(cdist - eng.uFocusDistance) / max(eng.uFocusRange, 1e-3), 0.0, 1.0) *
                    clamp(eng.uDofStrength, 0.0, 1.0);
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
                float sw = clamp(1.0 - abs(sndc * eng.uFar - cdist) / max(eng.uFocusRange, 1e-3),
                                 0.0, 1.0);
                acc += sc * sw;
                wsum += sw;
            }
            c = acc / wsum;
        }
    }
    // SSAO scales ONLY the indirect/ambient term (the G-buffer pass wrote the
    // indirect radiance to uGBuffer.rgb). Without the G-buffer, fall back to the
    // whole-colour multiply. GL twin in bloom.hpp.
    if (eng.uAoEnabled != 0) {
        float aoEff = mix(1.0, texture(uAo, vUV).r, eng.uAoIntensity);
        if (eng.uHasGBuffer != 0) {
            c -= texture(uGBuffer, vUV).rgb * (1.0 - aoEff);
        } else {
            c *= aoEff;
        }
    }
    if (eng.uBloomEnabled != 0) c += texture(uBloom, vUV).rgb * eng.uStrength;
    if (eng.uVolEnabled != 0) c += texture(uVol, vUV).rgb * eng.uVolStrength;
    // SSR BLENDS by its alpha (fresnel * ray fade), it is not ADDed: adding
    // the fully-lit scene colour on top of an already-lit surface double-lit
    // everything and washed reflections out to white.
    if (eng.uSsrEnabled != 0) {
        vec4 ssr = texture(uSsr, vUV);
        c = mix(c, ssr.rgb, clamp(eng.uSsrStrength * ssr.a, 0.0, 1.0));
    }
    if (eng.uFogEnabled != 0) {
        vec4 dp = texture(uFogDepth, vUV);
        float ndc = dp.r + dp.g / 255.0 + dp.b / 65025.0 + dp.a / 16581375.0;
        if (ndc < 1.0) {
            // The scene-depth resource stores LINEAR view distance / uFar (see
            // the SSAO depth encoder), NOT window NDC depth - decoding through
            // the perspective formula collapsed every distance to ~2*uNear.
            float dist = ndc * eng.uFar;
            float f = 1.0 - exp(-eng.uFogDensity * eng.uFogDensity * dist * dist);
            c = mix(c, eng.uFogColor, clamp(f, 0.0, 1.0));
        }
    }
    if (eng.uTonemapEnabled != 0) {
        float exposure = eng.uExposure;
        // The adapted value MULTIPLIES the authored exposure instead of
        // replacing it -- the authored exposure is a scene look.
        if (eng.uAutoExposure != 0)
            exposure = eng.uExposure * max(texture(uAvgLum, vec2(0.5)).r, 1e-4);
        vec3 graded = TonemapColor(eng.uTonemapMode, c * exposure);
        if (eng.uGammaCorrect != 0) graded = LinearToSrgb(graded);
        if (eng.uGradeEnabled != 0) {
            graded = clamp(graded, vec3(0.0), vec3(1.0));
            graded *= eng.uTint.rgb;
            float ig = 1.0 / max(eng.uGamma, 1e-4);
            graded = pow(clamp(graded, vec3(0.0), vec3(1.0)), vec3(ig)) * eng.uGain + vec3(eng.uLift);
            float luma = dot(graded, vec3(0.2126, 0.7152, 0.0722));
            graded = mix(vec3(luma), graded, eng.uSaturation);
            float ck = 1.0 + eng.uContrast;
            graded = (graded - vec3(0.5)) * ck + vec3(0.5);
            graded = clamp(graded, vec3(0.0), vec3(1.0));
        }
        if (eng.uVignette != 0) {
            float dx = vUV.x - 0.5;
            float dy = vUV.y - 0.5;
            float dist = length(vec2(dx, dy)) * 2.0;
            float fall = max((dist - eng.uVignetteRadius) / max(eng.uVignetteSoftness, 1e-4), 0.0);
            float t = clamp(fall, 0.0, 1.0);
            float st = t * t * (3.0 - 2.0 * t);
            graded *= 1.0 - st * eng.uVignetteIntensity;
        }
        FragColor = vec4(graded + OutDither(gl_FragCoord.xy), 1.0);
    } else {
        vec3 outc = min(c, vec3(1.0));
        if (eng.uGammaCorrect != 0) outc = LinearToSrgb(outc);
        FragColor = vec4(outc + OutDither(gl_FragCoord.xy), 1.0);
    }
}
