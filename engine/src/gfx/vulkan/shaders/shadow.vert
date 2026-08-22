#version 450
#extension GL_GOOGLE_include_directive : require
// CSM shadow vertex shader. NOT y-flipped: the shadow maps stay in GL NDC
// orientation so the lit shader's uLightVP sampling (also unflipped) lines up.
layout(location = 0) in vec3 aPos;

#include "engine_ubo.glsl"

void main() {
    gl_Position = eng.uMVP * vec4(aPos, 1.0);
    gl_Position.z = gl_Position.z * 0.5 + gl_Position.w * 0.5;
}
