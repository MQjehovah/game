#version 450
#extension GL_GOOGLE_include_directive : require
// Point-light shadow fragment shader: encodes the linear distance/range.
layout(location = 0) in vec3 vWorldPos;
layout(location = 0) out vec4 FragColor;

#include "engine_ubo.glsl"

vec4 EncodeDepth(float d) {
    vec4 bits = vec4(1.0, 255.0, 65025.0, 16581375.0) * d;
    bits = fract(bits);
    bits -= bits.yzww * vec4(1.0 / 255.0, 1.0 / 255.0, 1.0 / 255.0, 0.0);
    return bits;
}
void main() {
    FragColor = EncodeDepth(clamp(length(vWorldPos - eng.uLightPos) / max(eng.uLightRange, 1e-4), 0.0, 1.0));
}
