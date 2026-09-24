#version 450
#extension GL_GOOGLE_include_directive : require
// Temporal adaptation of the auto exposure: lerps the previous frame exposure
// toward the raw key / avgLum target so bright flashes do not strobe.
layout(location = 0) in vec2 vUV;
layout(location = 0) out vec4 FragColor;

#include "engine_ubo.glsl"

layout(set = 1, binding = 0) uniform sampler2D uAvgLum;
layout(set = 1, binding = 1) uniform sampler2D uPrevExposure;

void main() {
    float avgLum = clamp(exp(texture(uAvgLum, vec2(0.5)).r), 0.03, 4.0);
    float target = clamp(eng.uKeyValue / avgLum, eng.uExposureMin, eng.uExposureMax);
    float prev = max(texture(uPrevExposure, vec2(0.5)).r, 1e-4);
    FragColor = vec4(mix(prev, target, clamp(eng.uAdaptation, 0.0, 1.0)), 0.0, 0.0, 1.0);
}
