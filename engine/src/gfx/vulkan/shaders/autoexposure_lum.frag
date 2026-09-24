#version 450
#extension GL_GOOGLE_include_directive : require
// Per-pixel LOG luminance of the scene HDR (the reduce pass averages it).
layout(location = 0) in vec2 vUV;
layout(location = 0) out vec4 FragColor;

#include "engine_ubo.glsl"

layout(set = 1, binding = 0) uniform sampler2D uHdr;

void main() {
    vec3 c = texture(uHdr, vUV).rgb;
    float lum = max(dot(c, vec3(0.2126, 0.7152, 0.0722)), 1e-4);
    FragColor = vec4(log(lum), 0.0, 0.0, 1.0);
}
