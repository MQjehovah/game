#version 450
#extension GL_GOOGLE_include_directive : require
// FXAA resolve (GL parity with kFxaaFragmentShader in bloom.hpp). Terminal
// pass after the composite writes the display-referred LDR image. Strength 0
// returns the centre texel (exact copy). uTexelSize = 1/resolution.
layout(location = 0) in vec2 vUV;
layout(location = 0) out vec4 FragColor;

#include "engine_ubo.glsl"

layout(set = 1, binding = 0) uniform sampler2D uColor;

float FxaaLuma(vec3 c) { return dot(c, vec3(0.299, 0.587, 0.114)); }

void main() {
    vec3 rgbM = texture(uColor, vUV).rgb;
    if (eng.uFxaaStrength <= 0.0) {
        FragColor = vec4(rgbM, 1.0);
        return;
    }
    vec2 ts = eng.uTexelSize;
    vec3 rgbNW = texture(uColor, vUV + vec2(-1.0, -1.0) * ts).rgb;
    vec3 rgbNE = texture(uColor, vUV + vec2( 1.0, -1.0) * ts).rgb;
    vec3 rgbSW = texture(uColor, vUV + vec2(-1.0,  1.0) * ts).rgb;
    vec3 rgbSE = texture(uColor, vUV + vec2( 1.0,  1.0) * ts).rgb;
    float lM = FxaaLuma(rgbM);
    float lNW = FxaaLuma(rgbNW), lNE = FxaaLuma(rgbNE);
    float lSW = FxaaLuma(rgbSW), lSE = FxaaLuma(rgbSE);
    float lMin = min(lM, min(min(lNW, lNE), min(lSW, lSE)));
    float lMax = max(lM, max(max(lNW, lNE), max(lSW, lSE)));
    float range = lMax - lMin;
    if (range < max(0.0312, lMax * 0.125)) {
        FragColor = vec4(rgbM, 1.0);
        return;
    }
    vec2 dir = vec2(-((lNW + lNE) - (lSW + lSE)), ((lNW + lSW) - (lNE + lSE)));
    float dirReduce = max((lNW + lNE + lSW + lSE) * 0.25 * 0.125, 1.0 / 128.0);
    float rcp = 1.0 / (min(abs(dir.x), abs(dir.y)) + dirReduce);
    dir = clamp(dir * rcp, vec2(-8.0), vec2(8.0)) * ts;
    vec3 rgbA = 0.5 * (texture(uColor, vUV + dir * (1.0 / 3.0 - 0.5)).rgb +
                       texture(uColor, vUV + dir * (2.0 / 3.0 - 0.5)).rgb);
    vec3 rgbB = rgbA * 0.5 +
                0.25 * (texture(uColor, vUV + dir * -0.5).rgb +
                        texture(uColor, vUV + dir * 0.5).rgb);
    float lB = FxaaLuma(rgbB);
    vec3 result = (lB < lMin || lB > lMax) ? rgbA : rgbB;
    FragColor = vec4(mix(rgbM, result, eng.uFxaaStrength), 1.0);
}
