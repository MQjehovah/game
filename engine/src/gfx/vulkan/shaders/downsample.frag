#version 450
#extension GL_GOOGLE_include_directive : require
// 2x2 box downsample (half-res -> quarter-res).
layout(location = 0) in vec2 vUV;
layout(location = 0) out vec4 FragColor;

#include "engine_ubo.glsl"

layout(set = 1, binding = 0) uniform sampler2D uTex;

void main() {
    vec2 o = eng.uSrcTexelSize * 0.5;
    vec4 s = texture(uTex, vUV + vec2(-o.x, -o.y))
           + texture(uTex, vUV + vec2( o.x, -o.y))
           + texture(uTex, vUV + vec2(-o.x,  o.y))
           + texture(uTex, vUV + vec2( o.x,  o.y));
    FragColor = vec4(s.rgb * 0.25, 1.0);
}
