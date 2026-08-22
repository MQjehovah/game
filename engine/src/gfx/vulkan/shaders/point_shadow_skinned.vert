#version 450
#extension GL_GOOGLE_include_directive : require
// Point-light shadow vertex shader, skinned.
layout(location = 0) in vec3 aPos;
layout(location = 4) in vec4 aJointIds;
layout(location = 5) in vec4 aWeights;

layout(location = 0) out vec3 vWorldPos;

#include "engine_ubo.glsl"

void main() {
    mat4 skin = mat4(0.0);
    for (int i = 0; i < 4; ++i) {
        int id = int(aJointIds[i]);
        if (id >= 0 && id < 64) skin += aWeights[i] * eng.uBoneMatrices[id];
    }
    vWorldPos = (eng.uModel * skin * vec4(aPos, 1.0)).xyz;
    gl_Position = eng.uMVP * skin * vec4(aPos, 1.0);
    gl_Position.z = gl_Position.z * 0.5 + gl_Position.w * 0.5;
}
