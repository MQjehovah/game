#version 450
#extension GL_GOOGLE_include_directive : require
// 2D UI / overlay vertex shader. Shares the UI program with ImGui.
layout(location = 0) in vec2 aPos;
layout(location = 1) in vec2 aUV;
layout(location = 2) in vec4 aColor;

layout(location = 0) out vec2 vUV;
layout(location = 1) out vec4 vColor;

#include "engine_ubo.glsl"

void main() {
    vUV = aUV;
    vColor = aColor;
    gl_Position = eng.uMVP * vec4(aPos, 0.0, 1.0);
    gl_Position.y = -gl_Position.y;
    gl_Position.z = gl_Position.z * 0.5 + gl_Position.w * 0.5;
}
