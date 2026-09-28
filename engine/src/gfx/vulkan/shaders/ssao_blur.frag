#version 450
#extension GL_GOOGLE_include_directive : require
// Separable 9-tap gaussian blur for the AO channel (one axis per pass): the
// AO kernel's features span many texels and the old +-2 blur left the rotated
// kernel's residual structure and silhouette halos unsmoothed.
layout(location = 0) in vec2 vUV;
layout(location = 0) out vec4 FragColor;

#include "engine_ubo.glsl"

layout(set = 1, binding = 0) uniform sampler2D uTex;

void main() {
    vec2 off = eng.uTexelSize * eng.uDirection;
    float c = texture(uTex, vUV - off * 4.0).r * 0.0048
            + texture(uTex, vUV - off * 3.0).r * 0.0287
            + texture(uTex, vUV - off * 2.0).r * 0.1028
            + texture(uTex, vUV - off).r       * 0.2210
            + texture(uTex, vUV).r             * 0.2854
            + texture(uTex, vUV + off).r       * 0.2210
            + texture(uTex, vUV + off * 2.0).r * 0.1028
            + texture(uTex, vUV + off * 3.0).r * 0.0287
            + texture(uTex, vUV + off * 4.0).r * 0.0048;
    FragColor = vec4(c);
}
