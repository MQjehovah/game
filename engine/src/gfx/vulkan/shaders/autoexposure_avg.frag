#version 450
#extension GL_GOOGLE_include_directive : require
// Full average of the log-luminance target -> 1x1. Every texel of the small
// target votes (each is a 32x32 HDR block average); letterbox bars are excluded
// so clear-colour black cannot poison the mean.
layout(location = 0) in vec2 vUV;
layout(location = 0) out vec4 FragColor;

#include "engine_ubo.glsl"

layout(set = 1, binding = 0) uniform sampler2D uLum;

void main() {
    // Average EVERY texel of the small target (each already a 32x32 HDR block
    // average) instead of a fixed 16x16 subsample. Letterbox taps are skipped so
    // clear-colour black cannot poison the mean. GL twin in bloom.hpp.
    ivec2 sz = textureSize(uLum, 0);
    float sum = 0.0;
    float n = 0.0;
    for (int y = 0; y < sz.y; ++y) {
        for (int x = 0; x < sz.x; ++x) {
            vec2 uv = (vec2(float(x), float(y)) + 0.5) / vec2(sz);
            if (uv.x < eng.uSceneVpRect.x || uv.y < eng.uSceneVpRect.y ||
                uv.x > eng.uSceneVpRect.x + eng.uSceneVpRect.z ||
                uv.y > eng.uSceneVpRect.y + eng.uSceneVpRect.w) continue;
            sum += texelFetch(uLum, ivec2(x, y), 0).r;
            n += 1.0;
        }
    }
    float avg = n > 0.0 ? sum / n : log(0.18);
    FragColor = vec4(avg, 0.0, 0.0, 1.0);
}
