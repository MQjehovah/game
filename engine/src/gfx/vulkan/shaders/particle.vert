#version 450
#extension GL_GOOGLE_include_directive : require
// Billboard particle vertex shader with a per-instance atlas (flipbook) UV
// rectangle. The fragment program is the shared instanced-coloured one.
layout(location = 0) in vec3 aPos;
layout(location = 2) in vec2 aUV;
layout(location = 3) in vec4 aColor;
layout(location = 4) in mat4 aInstance;
layout(location = 8) in vec4 aInstanceColor;
layout(location = 9) in vec4 aUvRect;

layout(location = 0) out vec2 vUV;
layout(location = 1) out vec4 vColor;
layout(location = 2) out vec4 vInstanceColor;

#include "engine_ubo.glsl"

void main() {
    vUV = aUvRect.xy + aUV * aUvRect.zw;
    vColor = aColor;
    vInstanceColor = aInstanceColor;
    gl_Position = eng.uMVP * aInstance * vec4(aPos, 1.0);
    gl_Position.y = -gl_Position.y;
    gl_Position.z = gl_Position.z * 0.5 + gl_Position.w * 0.5;
}
