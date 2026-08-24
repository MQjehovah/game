#version 450
#extension GL_GOOGLE_include_directive : require
// Progressive bloom accumulation: half-res bloom + upsampled quarter-res bloom.
layout(location = 0) in vec2 vUV;
layout(location = 0) out vec4 FragColor;

#include "engine_ubo.glsl"

layout(set = 1, binding = 0) uniform sampler2D uHalf;
layout(set = 1, binding = 1) uniform sampler2D uQuarter;

void main() {
    vec3 halfB = texture(uHalf, vUV).rgb;
    vec3 quarter = texture(uQuarter, vUV).rgb;
    FragColor = vec4(halfB + quarter, 1.0);
}
