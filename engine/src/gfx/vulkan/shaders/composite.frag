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

vec3 ACESFilm(vec3 x) {
    float a = 2.51, b = 0.03, c = 2.43, d = 0.59, e = 0.14;
    // Clamp before the curve: an Inf/NaN HDR value would otherwise reach the
    // clamp below, which is undefined for NaN in GLSL.
    x = clamp(x, vec3(0.0), vec3(65504.0));
    return clamp((x * (a * x + b)) / (x * (c * x + d) + e), vec3(0.0), vec3(1.0));
}
void main() {
    vec3 hdr = texture(uHdr, vUV).rgb;
    vec3 c = hdr;
    // SSAO scales the LIT SCENE COLOUR only; after the additive terms it also
    // darkened bloom / volumetric / SSR with occlusion.
    if (eng.uAoEnabled != 0) {
        float ao = texture(uAo, vUV).r;
        c *= mix(1.0, ao, eng.uAoIntensity);
    }
    if (eng.uBloomEnabled != 0) c += texture(uBloom, vUV).rgb * eng.uStrength;
    if (eng.uVolEnabled != 0) c += texture(uVol, vUV).rgb * eng.uVolStrength;
    if (eng.uSsrEnabled != 0) c += texture(uSsr, vUV).rgb * eng.uSsrStrength;
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
        vec3 graded = ACESFilm(c * exposure);
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
        FragColor = vec4(graded, 1.0);
    } else {
        FragColor = vec4(min(c, vec3(1.0)), 1.0);
    }
}
