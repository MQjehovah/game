#version 450
#extension GL_GOOGLE_include_directive : require
// Re-encodes the resolved main-pass depth into the colour-encoded form the AO
// / SSR / volumetric passes consume (one geometry pass instead of two).
layout(location = 0) in vec2 vUV;
layout(location = 0) out vec4 FragColor;

#include "engine_ubo.glsl"

layout(set = 1, binding = 0) uniform sampler2D uDepthTex;

void main() {
    float ndc = texture(uDepthTex, vUV).r;
    // The depth attachment holds the same window-space [0,1] depth as GL.
    float linear = (2.0 * eng.uNear * eng.uFar) /
                   (eng.uFar + eng.uNear - (2.0 * ndc - 1.0) * (eng.uFar - eng.uNear));
    float d = (ndc >= 1.0) ? 1.0 : clamp(linear / eng.uFar, 0.0, 1.0);
    FragColor = vec4(d, fract(d * 255.0), fract(d * 65025.0), fract(d * 16581375.0));
}
