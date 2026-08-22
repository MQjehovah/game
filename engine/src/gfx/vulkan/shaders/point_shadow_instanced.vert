#version 450
#extension GL_GOOGLE_include_directive : require
// Point-light shadow vertex shader, instanced.
layout(location = 0) in vec3 aPos;
layout(location = 4) in mat4 aInstance;

layout(location = 0) out vec3 vWorldPos;

#include "engine_ubo.glsl"

void main() {
    vWorldPos = (aInstance * vec4(aPos, 1.0)).xyz;
    gl_Position = eng.uMVP * aInstance * vec4(aPos, 1.0);
    gl_Position.z = gl_Position.z * 0.5 + gl_Position.w * 0.5;
}
