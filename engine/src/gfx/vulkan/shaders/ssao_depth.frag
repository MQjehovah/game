#version 450
#extension GL_GOOGLE_include_directive : require
// Colour-encodes the normalised view depth (4 x 8 bit) so it survives the
// RGBA8 render target with enough precision for the AO kernel.
layout(location = 0) in float vViewDepth;
layout(location = 0) out vec4 FragColor;

#include "engine_ubo.glsl"

void main() {
    float d = vViewDepth / max(eng.uFar, 1e-4);
    d = clamp(d, 0.0, 1.0);
    FragColor = vec4(d, fract(d * 255.0), fract(d * 65025.0), fract(d * 16581375.0));
}
