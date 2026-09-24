#version 450
#extension GL_GOOGLE_include_directive : require
// Depth-aware volumetric sun shafts (god rays). Marches the pixel view ray in
// world space, scouts only the empty space in front of the nearest surface
// (viewZ, decoded from the colour-encoded depth) and accumulates the sun phase
// scattered light there, so a canopy truncates the shaft.
layout(location = 0) in vec2 vUV;
layout(location = 0) out vec4 FragColor;

#include "engine_ubo.glsl"

layout(set = 1, binding = 0) uniform sampler2D uScene;
layout(set = 1, binding = 1) uniform sampler2D uDepth;

float LoadDepth(vec2 uv) {
    vec4 p = texture(uDepth, uv);
    return p.r + p.g / 255.0 + p.b / 65025.0 + p.a / 16581375.0;
}
float ViewDepth(float ndc) {
    float z = ndc * 2.0 - 1.0;
    return (2.0 * eng.uNear * eng.uFar) / (eng.uFar + eng.uNear - z * (eng.uFar - eng.uNear));
}
void main() {
    // vUV spans the whole HDR target; the 3D scene only occupies the centred
    // sub-rect, so map into it before reconstructing the view ray.
    vec2 uv = (vUV - eng.uSceneVpRect.xy) / max(eng.uSceneVpRect.zw, vec2(1e-4));
    if (uv.x < 0.0 || uv.x > 1.0 || uv.y < 0.0 || uv.y > 1.0) {
        FragColor = vec4(0.0);
        return;
    }
    // Nearest surface along this pixel, sampled at the full-window UV.
    float ndc = LoadDepth(vUV);
    float viewZ = ndc >= 1.0 ? eng.uFar : ViewDepth(ndc);

    vec4 clip = inverse(eng.uViewProj) * vec4(uv.x * 2.0 - 1.0, uv.y * 2.0 - 1.0, 1.0, 1.0);
    vec3 rayEnd = clip.xyz / clip.w;
    vec3 rayDir = normalize(rayEnd - eng.uCamPos);

    // Henyey-Greenstein forward scatter: a tight sun-facing cone.
    vec3 toSun = normalize(-eng.uSunDir);
    float mu = dot(rayDir, toSun);
    float g = 0.7;
    float gg = g * g;
    float phase = (1.0 - gg) / (4.0 * 3.14159265 * pow(max(1.0 + gg - 2.0 * g * mu, 1e-3), 1.5));
    phase *= 1.5;

    // The scattering medium lives in a thin near-field slab, so sky is not
    // doubly lit and the shaft reads as sun-angled light in the fog.
    float t0 = eng.uNear;
    float t1 = min(eng.uNear + 38.0, min(viewZ, eng.uFar));
    int steps = max(int(eng.uSteps), 1);
    float dt = max((t1 - t0) / float(steps), 1e-4);
    float density = eng.uDensity * 0.05;
    float transmittance = 1.0;
    vec3 acc = vec3(0.0);
    for (int i = 0; i < steps; ++i) {
        float seg = density * dt;
        acc += eng.uSunColor * (phase * seg * transmittance);
        transmittance *= exp(-seg);
    }
    acc *= eng.uWeight;
    FragColor = vec4(acc, 1.0);
}
