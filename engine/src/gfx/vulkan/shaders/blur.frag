#version 450
#extension GL_GOOGLE_include_directive : require
// 5-tap separable Gaussian blur (H or V per uDirection).
layout(location = 0) in vec2 vUV;
layout(location = 0) out vec4 FragColor;

#include "engine_ubo.glsl"

layout(set = 0, binding = 1) uniform sampler2D uTex;

void main() {
    vec2 off = eng.uTexelSize * eng.uDirection;
    vec4 c = texture(uTex, vUV - off * 2.0) * 0.05449
           + texture(uTex, vUV - off)       * 0.244202
           + texture(uTex, vUV)             * 0.402620
           + texture(uTex, vUV + off)       * 0.244202
           + texture(uTex, vUV + off * 2.0) * 0.05449;
    FragColor = c;
}
