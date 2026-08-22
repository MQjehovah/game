#version 450
#extension GL_GOOGLE_include_directive : require
// CSM shadow vertex shader, instanced (per-instance model matrices).
layout(location = 0) in vec3 aPos;
layout(location = 4) in mat4 aInstance;

#include "engine_ubo.glsl"

void main() {
    gl_Position = eng.uMVP * aInstance * vec4(aPos, 1.0);
    gl_Position.z = gl_Position.z * 0.5 + gl_Position.w * 0.5;
}
