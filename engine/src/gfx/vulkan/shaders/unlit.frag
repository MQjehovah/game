#version 450
#extension GL_GOOGLE_include_directive : require
// Unlit / texture-only fragment shader.
layout(location = 0) in vec2 vUV;
layout(location = 1) in vec4 vColor;
layout(location = 0) out vec4 FragColor;

#include "engine_ubo.glsl"

layout(set = 1, binding = 0) uniform sampler2D uAlbedo;

void main() {
    vec4 albedo = (eng.uHasTexture != 0) ? texture(uAlbedo, vUV) : vec4(1.0);
    FragColor = albedo * eng.uTint * vColor;
}
