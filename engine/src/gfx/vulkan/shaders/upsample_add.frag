#version 450
#extension GL_GOOGLE_include_directive : require
// Progressive bloom accumulation: half-res bloom + upsampled quarter-res bloom.
// The quarter contribution uses the same 9-tap tent + wide low-weight skirt as
// the GL path (weights sum to 1): a single bilinear tap leaves a tight "dirty"
// edge on the halo, and uBloomWidth scales the skirt footprint outward.
layout(location = 0) in vec2 vUV;
layout(location = 0) out vec4 FragColor;

#include "engine_ubo.glsl"

layout(set = 1, binding = 0) uniform sampler2D uHalf;
layout(set = 1, binding = 1) uniform sampler2D uQuarter;

void main() {
    vec3 halfB = texture(uHalf, vUV).rgb;
    vec2 t = eng.uBloomWidth / vec2(textureSize(uQuarter, 0));
    vec3 quarter = texture(uQuarter, vUV).rgb * 0.25
                 + (texture(uQuarter, vUV + vec2( t.x, 0.0)).rgb +
                    texture(uQuarter, vUV + vec2(-t.x, 0.0)).rgb +
                    texture(uQuarter, vUV + vec2(0.0,  t.y)).rgb +
                    texture(uQuarter, vUV + vec2(0.0, -t.y)).rgb) * 0.125
                 + (texture(uQuarter, vUV + vec2( t.x,  t.y) * 2.5).rgb +
                    texture(uQuarter, vUV + vec2(-t.x,  t.y) * 2.5).rgb +
                    texture(uQuarter, vUV + vec2( t.x, -t.y) * 2.5).rgb +
                    texture(uQuarter, vUV + vec2(-t.x, -t.y) * 2.5).rgb) * 0.0625;
    FragColor = vec4(halfB + quarter, 1.0);
}
