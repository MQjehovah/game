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
    // Carry-corrected base-255 packing (see the GL twin in ssao.hpp): the
    // naive fract() form left a ~0.5/255 quantisation sawtooth that the
    // AO/SSR/volumetric/fog consumers read as depth-contour stripes.
    vec4 bits = vec4(1.0, 255.0, 65025.0, 16581375.0) * d;
    bits = fract(bits);
    bits -= bits.yzww * vec4(1.0 / 255.0, 1.0 / 255.0, 1.0 / 255.0, 0.0);
    FragColor = bits;
}
