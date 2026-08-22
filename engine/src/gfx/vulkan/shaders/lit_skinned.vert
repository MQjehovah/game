#version 450
#extension GL_GOOGLE_include_directive : require
// Lit PBR vertex shader, GPU-skinned variant (up to 64 bones, 4 joints/vertex).
layout(location = 0) in vec3 aPos;
layout(location = 1) in vec3 aNormal;
layout(location = 2) in vec2 aUV;
layout(location = 3) in vec4 aColor;
layout(location = 4) in vec4 aJointIds;
layout(location = 5) in vec4 aWeights;

layout(location = 0) out vec3 vWorldPos;
layout(location = 1) out vec3 vNormal;
layout(location = 2) out vec2 vUV;
layout(location = 3) out vec4 vColor;
layout(location = 4) out float vViewZ;

#include "engine_ubo.glsl"

void main() {
    mat4 skin = mat4(0.0);
    for (int i = 0; i < 4; ++i) {
        int id = int(aJointIds[i]);
        if (id >= 0 && id < 64) skin += aWeights[i] * eng.uBoneMatrices[id];
    }
    vec4 p = skin * vec4(aPos, 1.0);
    vec4 n = skin * vec4(aNormal, 0.0);
    vWorldPos = (eng.uModel * p).xyz;
    vNormal = (eng.uNormalMat * n).xyz;
    vUV = aUV;
    vColor = aColor;
    vViewZ = (eng.uViewMatrix * vec4(vWorldPos, 1.0)).z;
    gl_Position = eng.uMVP * p;
    gl_Position.y = -gl_Position.y;
    gl_Position.z = gl_Position.z * 0.5 + gl_Position.w * 0.5;
}
