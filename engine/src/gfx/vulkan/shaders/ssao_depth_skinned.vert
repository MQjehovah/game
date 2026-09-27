#version 450
#extension GL_GOOGLE_include_directive : require
// SSAO depth pre-pass, GPU-skinned variant (up to 64 bones, 4 joints/vertex).
// Emits the same linear view depth as the static variants — the shadow skinned
// program's window-depth encoding is wrong for the post chain's consumers.
layout(location = 0) in vec3 aPos;
layout(location = 4) in vec4 aJointIds;
layout(location = 5) in vec4 aWeights;

layout(location = 0) out float vViewDepth;

#include "engine_ubo.glsl"

void main() {
    mat4 skin = mat4(0.0);
    for (int i = 0; i < 4; ++i) {
        int id = int(aJointIds[i]);
        if (id >= 0 && id < 64) skin += aWeights[i] * eng.uBoneMatrices[id];
    }
    vec4 clip = eng.uMVP * skin * vec4(aPos, 1.0);
    vViewDepth = clip.w; // positive view distance
    gl_Position = clip;
    gl_Position.y = -gl_Position.y;
    gl_Position.z = gl_Position.z * 0.5 + gl_Position.w * 0.5;
}
