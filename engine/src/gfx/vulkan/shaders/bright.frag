#version 450
#extension GL_GOOGLE_include_directive : require
// HDR bright pass. The threshold is keyed on the EXPOSED colour (so the cut
// tracks displayed brightness, and auto-exposure cannot vanish the glow), but
// the result is returned UNEXPOSED. The composite adds the bloom to the scene
// before its single `* exposure`, so emitting an exposed bright pass scaled the
// glow by exposure a second time (bloom ~ exposure^2). max(c*e - t, 0)/e ==
// max(c - t/e, 0) for e > 0; GL twin in bloom.hpp.
layout(location = 0) in vec2 vUV;
layout(location = 0) out vec4 FragColor;

#include "engine_ubo.glsl"

layout(set = 1, binding = 0) uniform sampler2D uTex;
layout(set = 1, binding = 1) uniform sampler2D uAvgLum;

void main() {
    vec4 c = texture(uTex, vUV);
    float exposure = eng.uExposure;
    if (eng.uAutoExposure != 0)
        exposure = eng.uExposure * max(texture(uAvgLum, vec2(0.5)).r, 1e-4);
    float e = max(exposure, 1e-4);
    FragColor = vec4(max(c.rgb - vec3(eng.uThreshold) / e, vec3(0.0)), 1.0);
}
