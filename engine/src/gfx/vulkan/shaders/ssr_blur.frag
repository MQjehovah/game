#version 450
#extension GL_GOOGLE_include_directive : require
// Screen-space reflections (VK twin of ssr.hpp's kSsrFragmentShader): reflect
// the view ray off a depth-gradient normal, march in VIEW space with a
// perspective-correct projection of every sample, binary-refine the hit and
// write a fresnel-weighted result for the composite to BLEND with.
layout(location = 0) in vec2 vUV;
layout(location = 0) out vec4 FragColor;

#include "engine_ubo.glsl"

layout(set = 1, binding = 0) uniform sampler2D uTex;

void main() {
    const float w[9] = float[9](0.0048, 0.0287, 0.1028, 0.2210, 0.2854,
                                0.2210, 0.1028, 0.0287, 0.0048);
    vec2 off = eng.uTexelSize * eng.uDirection;
    vec3 psum = vec3(0.0);
    float asum = 0.0;
    for (int i = 0; i < 9; ++i) {
        vec4 s = texture(uTex, vUV + off * float(i - 4));
        psum += s.rgb * s.a * w[i];
        asum += s.a * w[i];
    }
    FragColor = vec4(asum > 1e-4 ? psum / asum : vec3(0.0), asum);
}