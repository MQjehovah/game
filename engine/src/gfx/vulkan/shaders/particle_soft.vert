#version 450
#extension GL_GOOGLE_include_directive : require
// Soft billboard: the atlas variant plus the window depth handed to the
// fragment shader for the scene-depth fade. In Vulkan gl_Position.z / w is
// already the window depth after the clip-space remap below, so the value is
// taken directly (no half-plus-half on top).
layout(location = 0) in vec3 aPos;
layout(location = 2) in vec2 aUV;
layout(location = 3) in vec4 aColor;
layout(location = 4) in mat4 aInstance;
layout(location = 8) in vec4 aInstanceColor;
layout(location = 9) in vec4 aUvRect;

layout(location = 0) out vec2 vUV;
layout(location = 1) out vec4 vColor;
layout(location = 2) out vec4 vInstanceColor;
layout(location = 3) out float vWinDepth;

#include "engine_ubo.glsl"

void main() {
    vUV = aUvRect.xy + aUV * aUvRect.zw;
    vColor = aColor;
    vInstanceColor = aInstanceColor;
    gl_Position = eng.uMVP * aInstance * vec4(aPos, 1.0);
    gl_Position.y = -gl_Position.y;
    gl_Position.z = gl_Position.z * 0.5 + gl_Position.w * 0.5;
    vWinDepth = gl_Position.z / gl_Position.w;
}
