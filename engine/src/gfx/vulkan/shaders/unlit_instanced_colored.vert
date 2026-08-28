#version 450
#extension GL_GOOGLE_include_directive : require
// Unlit vertex shader, per-instance model matrices + per-instance RGBA color
// (attribute 8) for sprite/billboard particles tinted per instance.
layout(location = 0) in vec3 aPos;
layout(location = 2) in vec2 aUV;
layout(location = 3) in vec4 aColor;
layout(location = 4) in mat4 aInstance;
layout(location = 8) in vec4 aInstanceColor;

layout(location = 0) out vec2 vUV;
layout(location = 1) out vec4 vColor;
layout(location = 2) out vec4 vInstanceColor;

#include "engine_ubo.glsl"

void main() {
    vUV = aUV;
    vColor = aColor;
    vInstanceColor = aInstanceColor;
    gl_Position = eng.uMVP * aInstance * vec4(aPos, 1.0);
    gl_Position.y = -gl_Position.y;
    gl_Position.z = gl_Position.z * 0.5 + gl_Position.w * 0.5;
}
