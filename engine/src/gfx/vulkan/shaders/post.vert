#version 450
#extension GL_GOOGLE_include_directive : require
// Fullscreen post-process vertex shader (draws the NDC quad mesh with
// uMVP = identity). NOT y-flipped: the composite/sampling convention relies on
// the render-target textures being y-down (see the y-flip comments in lit.vert).
layout(location = 0) in vec3 aPos;
layout(location = 2) in vec2 aUV;

layout(location = 0) out vec2 vUV;

#include "engine_ubo.glsl"

void main() {
    vUV = aUV;
    gl_Position = eng.uMVP * vec4(aPos, 1.0);
    gl_Position.z = gl_Position.z * 0.5 + gl_Position.w * 0.5;
}
