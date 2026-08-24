#version 450
#extension GL_GOOGLE_include_directive : require
// HDR bright pass: max(color - threshold, 0).
layout(location = 0) in vec2 vUV;
layout(location = 0) out vec4 FragColor;

#include "engine_ubo.glsl"

layout(set = 1, binding = 0) uniform sampler2D uTex;

void main() {
    vec4 c = texture(uTex, vUV);
    FragColor = vec4(max(c.rgb - vec3(eng.uThreshold), vec3(0.0)), 1.0);
}
