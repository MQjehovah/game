#version 450
#extension GL_GOOGLE_include_directive : require
// Screen-space reflections: rebuilds a crude depth-gradient normal, marches the
// reflected view ray in screen space and pulls the HDR colour at the hit.
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
    // The depth RT stores LINEAR view distance / uFar (SSAO depth encoder).
    return ndc * eng.uFar;
}
void main() {
    float ndc = LoadDepth(vUV);
    if (ndc >= 1.0) { FragColor = vec4(0.0); return; } // sky -> no reflection
    float viewZ = ViewDepth(ndc);

    float dzx = ViewDepth(LoadDepth(vUV + vec2(eng.uTexelSize.x, 0.0))) - viewZ;
    float dzy = ViewDepth(LoadDepth(vUV + vec2(0.0, eng.uTexelSize.y))) - viewZ;
    vec3 n = normalize(vec3(-dzx * eng.uMaxDist, -dzy * eng.uMaxDist, eng.uTexelSize.x));

    vec3 viewDir = vec3(0.0, 0.0, 1.0); // camera looks along +viewZ
    vec3 refl = reflect(viewDir, n);
    if (refl.z <= 0.0) { FragColor = vec4(0.0); return; } // reflected away

    int steps = max(int(eng.uSteps), 1);
    vec2 stepVec = refl.xy / refl.z * eng.uMaxDist / float(steps);
    vec2 uv = vUV;
    vec3 result = vec3(0.0);
    for (int i = 0; i < steps; ++i) {
        uv += stepVec;
        if (uv.x < 0.0 || uv.x > 1.0 || uv.y < 0.0 || uv.y > 1.0) break;
        float sceneZ = ViewDepth(LoadDepth(uv));
        if (sceneZ > 0.001 && sceneZ < (viewZ - eng.uThickness)) {
            result = texture(uScene, uv).rgb;
            break;
        }
    }
    FragColor = vec4(result, 1.0);
}
