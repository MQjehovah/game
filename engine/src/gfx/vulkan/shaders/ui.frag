#version 450
#extension GL_GOOGLE_include_directive : require
// 2D UI / overlay fragment shader (also used for ImGui).
layout(location = 0) in vec2 vUV;
layout(location = 1) in vec4 vColor;
layout(location = 0) out vec4 FragColor;

#include "engine_ubo.glsl"

layout(set = 0, binding = 1) uniform sampler2D uTex;

void main() {
    FragColor = vColor * texture(uTex, vUV);
}
