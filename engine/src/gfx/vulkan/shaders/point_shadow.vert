#version 450
#extension GL_GOOGLE_include_directive : require
// Point-light shadow vertex shader. NOT y-flipped: the 6 per-face maps stay in
// GL NDC orientation so the lit shader's PointCubemapFaceUV sampling lines up.
layout(location = 0) in vec3 aPos;

layout(location = 0) out vec3 vWorldPos;

#include "engine_ubo.glsl"

void main() {
    vWorldPos = (eng.uModel * vec4(aPos, 1.0)).xyz;
    gl_Position = eng.uMVP * vec4(aPos, 1.0);
    gl_Position.z = gl_Position.z * 0.5 + gl_Position.w * 0.5;
}
