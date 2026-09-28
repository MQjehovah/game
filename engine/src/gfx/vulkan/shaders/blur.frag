#version 450
#extension GL_GOOGLE_include_directive : require
// 5-tap separable Gaussian blur (H or V per uDirection).
layout(location = 0) in vec2 vUV;
layout(location = 0) out vec4 FragColor;

#include "engine_ubo.glsl"

layout(set = 1, binding = 0) uniform sampler2D uTex;

void main() {
    vec2 off = eng.uTexelSize * eng.uDirection;
    // 9-tap gaussian (±4 texels): the SSR march dithers its start per pixel;
    // a ±2 kernel left the dither as fine dense stripes.
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
