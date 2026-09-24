#version 450
#extension GL_GOOGLE_include_directive : require
// 2x2 box reduce of the log-luminance target (1x1 result after the last step).
layout(location = 0) in vec2 vUV;
layout(location = 0) out vec4 FragColor;

#include "engine_ubo.glsl"

layout(set = 1, binding = 0) uniform sampler2D uLum;

void main() {
    vec2 o = eng.uSrcTexelSize * 0.5;
    vec4 s = texture(uLum, vUV + vec2(-o.x, -o.y))
           + texture(uLum, vUV + vec2( o.x, -o.y))
           + texture(uLum, vUV + vec2(-o.x,  o.y))
           + texture(uLum, vUV + vec2( o.x,  o.y));
    FragColor = vec4(s.rgb * 0.25, 1.0);
}
