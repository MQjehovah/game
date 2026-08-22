#version 450
#extension GL_GOOGLE_include_directive : require
// Unlit / texture-only vertex shader.
layout(location = 0) in vec3 aPos;
layout(location = 2) in vec2 aUV;
layout(location = 3) in vec4 aColor;

layout(location = 0) out vec2 vUV;
layout(location = 1) out vec4 vColor;

#include "engine_ubo.glsl"

void main() {
    vUV = aUV;
    vColor = aColor;
    gl_Position = eng.uMVP * vec4(aPos, 1.0);
    gl_Position.y = -gl_Position.y;
    gl_Position.z = gl_Position.z * 0.5 + gl_Position.w * 0.5;
}
