#version 450
#extension GL_GOOGLE_include_directive : require
// Separable 5-tap blur for the AO channel (one axis per pass).
layout(location = 0) in vec2 vUV;
layout(location = 0) out vec4 FragColor;

#include "engine_ubo.glsl"

layout(set = 1, binding = 0) uniform sampler2D uTex;

void main() {
    vec2 off = eng.uTexelSize * eng.uDirection;
    float c = texture(uTex, vUV - off * 2.0).r * 0.05449
            + texture(uTex, vUV - off).r       * 0.244202
            + texture(uTex, vUV).r             * 0.402620
            + texture(uTex, vUV + off).r       * 0.244202
            + texture(uTex, vUV + off * 2.0).r * 0.05449;
    FragColor = vec4(c);
}
