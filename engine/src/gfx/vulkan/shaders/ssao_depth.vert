#version 450
#extension GL_GOOGLE_include_directive : require
// SSAO depth pre-pass (instanced): encodes the camera view depth of every
// caster into RGBA so the AO / SSR / volumetric passes can decode it. Y is
// flipped like the scene shaders so this target matches the HDR scene
// orientation, and clip z is remapped into Vulkan window space.
layout(location = 0) in vec3 aPos;
layout(location = 4) in mat4 aInstance;

layout(location = 0) out float vViewDepth;

#include "engine_ubo.glsl"

void main() {
    vec4 clip = eng.uMVP * aInstance * vec4(aPos, 1.0);
    vViewDepth = clip.w; // positive view distance (GL-convention matrices)
    gl_Position = clip;
    gl_Position.y = -gl_Position.y;
    gl_Position.z = gl_Position.z * 0.5 + gl_Position.w * 0.5;
}
