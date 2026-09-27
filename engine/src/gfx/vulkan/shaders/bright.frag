#version 450
#extension GL_GOOGLE_include_directive : require
// HDR bright pass: max(color * effectiveExposure - threshold, 0). The bright
// pass runs BEFORE the composite applies exposure; keying the threshold on raw
// HDR values decoupled bloom from displayed brightness (with auto exposure
// lifting a dark scene the glow vanished exactly when the image looked
// brightest), so the same effective exposure is applied here.
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
    FragColor = vec4(max(c.rgb * exposure - vec3(eng.uThreshold), vec3(0.0)), 1.0);
}
