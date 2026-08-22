#version 450
#extension GL_GOOGLE_include_directive : require
// Line vertex shader (28-byte stride: pos3 + color4).
layout(location = 0) in vec3 aPos;
layout(location = 1) in vec4 aColor;

layout(location = 0) out vec4 vColor;

#include "engine_ubo.glsl"

void main() {
    vColor = aColor;
    gl_Position = eng.uMVP * vec4(aPos, 1.0);
    gl_Position.y = -gl_Position.y;
    gl_Position.z = gl_Position.z * 0.5 + gl_Position.w * 0.5;
}
