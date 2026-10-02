#version 450
#extension GL_GOOGLE_include_directive : require
// Per-pixel LOG luminance of the scene HDR (the reduce pass averages it).
layout(location = 0) in vec2 vUV;
layout(location = 0) out vec4 FragColor;

#include "engine_ubo.glsl"

layout(set = 1, binding = 0) uniform sampler2D uHdr;

void main() {
    // Each output texel stands for a 32x32 block of the full-res HDR (the target
    // is 1/32). Average a 64-tap stratified sample of the block so a small bright
    // emitter anywhere inside it still votes (a single point sample missed
    // anything between texel centres). GL twin in bloom.hpp.
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
    FragColor = vec4(log(lum), 0.0, 0.0, 1.0);
}
