#version 450
#extension GL_GOOGLE_include_directive : require
// Fullscreen NDC quad for the procedural sky (uMVP = identity). Not y-flipped,
// matching post.vert; the fragment flips the NDC y itself when rebuilding the
// view ray, because the fragment vUV runs top-down in Vulkan.
layout(location = 0) in vec3 aPos;
layout(location = 2) in vec2 aUV;

layout(location = 0) out vec2 vUV;

#include "engine_ubo.glsl"

void main() {
    vUV = aUV;
    gl_Position = eng.uMVP * vec4(aPos, 1.0);
    gl_Position.z = gl_Position.z * 0.5 + gl_Position.w * 0.5;
}
