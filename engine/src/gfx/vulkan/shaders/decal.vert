#version 450
#extension GL_GOOGLE_include_directive : require
// Depth-projected decal, flat quad on the decal plane. Y is flipped and clip z
// remapped like the scene shaders so the quad rasterises in the right place.
layout(location = 0) in vec3 aPos;
layout(location = 2) in vec2 aUV;

layout(location = 0) out vec3 vWorld;
layout(location = 1) out vec2 vUv;

#include "engine_ubo.glsl"

void main() {
    vWorld = (eng.uModel * vec4(aPos, 1.0)).xyz;
    vUv = aUV;
    gl_Position = eng.uMVP * vec4(aPos, 1.0);
    gl_Position.y = -gl_Position.y;
    gl_Position.z = gl_Position.z * 0.5 + gl_Position.w * 0.5;
}
