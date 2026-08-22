#version 450
#extension GL_GOOGLE_include_directive : require
// Lit PBR vertex shader, per-instance model matrices (mat4 at location 4).
layout(location = 0) in vec3 aPos;
layout(location = 1) in vec3 aNormal;
layout(location = 2) in vec2 aUV;
layout(location = 3) in vec4 aColor;
layout(location = 4) in mat4 aInstance;

layout(location = 0) out vec3 vWorldPos;
layout(location = 1) out vec3 vNormal;
layout(location = 2) out vec2 vUV;
layout(location = 3) out vec4 vColor;
layout(location = 4) out float vViewZ;

#include "engine_ubo.glsl"

void main() {
    vWorldPos = (aInstance * vec4(aPos, 1.0)).xyz;
    vNormal = mat3(aInstance) * aNormal;
    vUV = aUV;
    vColor = aColor;
    vViewZ = (eng.uViewMatrix * vec4(vWorldPos, 1.0)).z;
    gl_Position = eng.uMVP * aInstance * vec4(aPos, 1.0);
    gl_Position.y = -gl_Position.y;
    gl_Position.z = gl_Position.z * 0.5 + gl_Position.w * 0.5;
}
