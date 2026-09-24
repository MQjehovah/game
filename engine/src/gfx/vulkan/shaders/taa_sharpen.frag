#version 450
#extension GL_GOOGLE_include_directive : require
// Unsharp mask applied to the resolved TAA frame (see engine/include/neon/gfx/taa.hpp
// for the rationale): accumulating N jittered frames removes the edge aliasing but
// also averages away high-frequency texture detail, so a 3x3 tent high-pass puts the
// detail back without re-introducing edge crawl. 0 = disabled.
layout(location = 0) in vec2 vUV;
layout(location = 0) out vec4 FragColor;

#include "engine_ubo.glsl"

layout(set = 1, binding = 0) uniform sampler2D uSource;

void main() {
    vec2 t = 1.0 / eng.uScreenSize;
    vec3 c  = texture(uSource, vUV).rgb;
    vec3 n  = texture(uSource, vUV + vec2(0.0, -t.y)).rgb;
    vec3 s  = texture(uSource, vUV + vec2(0.0,  t.y)).rgb;
    vec3 e  = texture(uSource, vUV + vec2( t.x, 0.0)).rgb;
    vec3 w  = texture(uSource, vUV + vec2(-t.x, 0.0)).rgb;
    vec3 ne = texture(uSource, vUV + vec2( t.x, -t.y)).rgb;
    vec3 nw = texture(uSource, vUV + vec2(-t.x, -t.y)).rgb;
    vec3 se = texture(uSource, vUV + vec2( t.x,  t.y)).rgb;
    vec3 sw = texture(uSource, vUV + vec2(-t.x,  t.y)).rgb;
    vec3 blur = (c * 4.0 + (n + s + e + w) * 2.0 + (ne + nw + se + sw)) / 16.0;
    vec3 sharp = c + (c - blur) * eng.uSharpen;
    FragColor = vec4(max(sharp, vec3(0.0)), 1.0);
}
