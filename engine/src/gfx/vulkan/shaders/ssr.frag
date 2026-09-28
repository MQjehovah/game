#version 450
#extension GL_GOOGLE_include_directive : require
// Screen-space reflections (VK twin of ssr.hpp's kSsrFragmentShader): reflect
// the view ray off a depth-gradient normal and march in VIEW space with a
// perspective-correct projection of every sample.
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

    // Projection factors: uTexelSize is the full-res depth texel.
    float hdrH = 1.0 / eng.uTexelSize.y;
    float tanHalf = 0.5 * hdrH / eng.uProjScale;
    float aspect = eng.uTexelSize.y / eng.uTexelSize.x;

    // View-space position of this fragment (camera at the origin, -z forward).
    vec2 scr = vUV * 2.0 - 1.0;
    vec3 ro = vec3(scr.x * tanHalf * aspect, scr.y * tanHalf, -1.0) * viewZ;

    // View-space normal from the depth field: world depth deltas per texel
    // divided by the world size of a texel at this depth.
    float dzx = ViewDepth(LoadDepth(vUV + vec2(eng.uTexelSize.x, 0.0))) - viewZ;
    float dzy = ViewDepth(LoadDepth(vUV + vec2(0.0, eng.uTexelSize.y))) - viewZ;
    float wx = max(viewZ * 2.0 * tanHalf * aspect * eng.uTexelSize.x, 1e-4);
    float wy = max(viewZ * 2.0 * tanHalf * eng.uTexelSize.y, 1e-4);
    vec3 n = normalize(vec3(-dzx / wx, -dzy / wy, 1.0));

    vec3 rd = reflect(normalize(ro), n);
    if (rd.z > -1e-3) { FragColor = vec4(0.0); return; } // back at the camera

    // March in view space; project each sample (the ray's depth along a
    // screen-space line is NOT linear under perspective).
    float maxDist = eng.uMaxDist * viewZ * 2.0 * tanHalf * aspect;
    float dt = max(maxDist / eng.uSteps, 1e-3);
    float t = 0.1;
    for (int i = 0; i < int(eng.uSteps); ++i) {
        t += dt;
        vec3 p = ro + rd * t;
        if (-p.z < eng.uNear) break;
        vec2 suv = vec2(p.x / (-p.z * tanHalf * aspect),
                        p.y / (-p.z * tanHalf)) * 0.5 + 0.5;
        if (suv.x < 0.0 || suv.x > 1.0 || suv.y < 0.0 || suv.y > 1.0) break;
        float sceneZ = ViewDepth(LoadDepth(suv));
        float rayZ = -p.z;
        float thick = rayZ * (eng.uThickness + 0.01);
        if (sceneZ < rayZ - thick) {
            float edge = smoothstep(0.0, 0.1, suv.x) * smoothstep(1.0, 0.9, suv.x) *
                         smoothstep(0.0, 0.1, suv.y) * smoothstep(1.0, 0.9, suv.y);
            float fade = max(1.0 - t / maxDist, 0.0) * edge;
            FragColor = vec4(texture(uScene, suv).rgb * fade, 1.0);
            return;
        }
    }
    FragColor = vec4(0.0);
}
