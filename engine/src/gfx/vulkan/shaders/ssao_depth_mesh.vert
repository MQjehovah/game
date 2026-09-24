#version 450
#extension GL_GOOGLE_include_directive : require
// SSAO depth pre-pass, non-instanced single-mesh variant (see ssao_depth.vert).
layout(location = 0) in vec3 aPos;

layout(location = 0) out float vViewDepth;

#include "engine_ubo.glsl"

void main() {
    vec4 clip = eng.uMVP * vec4(aPos, 1.0);
    vViewDepth = clip.w; // positive view distance
    gl_Position = clip;
    gl_Position.y = -gl_Position.y;
    gl_Position.z = gl_Position.z * 0.5 + gl_Position.w * 0.5;
}
