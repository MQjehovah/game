#version 450
#extension GL_GOOGLE_include_directive : require
// Soft billboard: the atlas variant plus the particle's view-axis distance
// (perspective clip w) handed to the fragment shader for the scene-depth fade.
// The fade compares against the LINEARISED scene depth; window depth is
// quadratic in distance and made the fade band grow with d^2 (soft particles
// all but vanished at gameplay camera distances).
layout(location = 0) in vec3 aPos;
layout(location = 2) in vec2 aUV;
layout(location = 3) in vec4 aColor;
layout(location = 4) in mat4 aInstance;
layout(location = 8) in vec4 aInstanceColor;
layout(location = 9) in vec4 aUvRect;

layout(location = 0) out vec2 vUV;
layout(location = 1) out vec4 vColor;
layout(location = 2) out vec4 vInstanceColor;
layout(location = 3) out float vViewDist;

#include "engine_ubo.glsl"

void main() {
    vUV = aUvRect.xy + aUV * aUvRect.zw;
    vColor = aColor;
    vInstanceColor = aInstanceColor;
    gl_Position = eng.uMVP * aInstance * vec4(aPos, 1.0);
    gl_Position.y = -gl_Position.y;
    gl_Position.z = gl_Position.z * 0.5 + gl_Position.w * 0.5;
    vViewDist = gl_Position.w;
}
