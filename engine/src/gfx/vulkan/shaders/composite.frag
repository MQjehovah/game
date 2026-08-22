#version 450
#extension GL_GOOGLE_include_directive : require
// Final composite: ACES((hdr + bloom * strength) * exposure) or the legacy
// clamp reference (uTonemapEnabled == 0), mirroring the engine's
// kCompositeFragmentShader.
layout(location = 0) in vec2 vUV;
layout(location = 0) out vec4 FragColor;

#include "engine_ubo.glsl"

layout(set = 0, binding = 1) uniform sampler2D uHdr;
layout(set = 1, binding = 1) uniform sampler2D uBloom;

vec3 ACESFilm(vec3 x) {
    float a = 2.51, b = 0.03, c = 2.43, d = 0.59, e = 0.14;
    x = clamp(x, vec3(0.0), vec3(65504.0));
    return clamp((x * (a * x + b)) / (x * (c * x + d) + e), vec3(0.0), vec3(1.0));
}
void main() {
    vec3 hdr = texture(uHdr, vUV).rgb;
    vec3 c = hdr;
    if (eng.uBloomEnabled != 0) c += texture(uBloom, vUV).rgb * eng.uStrength;
    if (eng.uTonemapEnabled != 0) {
        FragColor = vec4(ACESFilm(c * eng.uExposure), 1.0);
    } else {
        FragColor = vec4(min(c, vec3(1.0)), 1.0);
    }
}
