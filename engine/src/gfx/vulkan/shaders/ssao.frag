#version 450
#extension GL_GOOGLE_include_directive : require
// SSAO: decodes the colour-encoded depth to world units and accumulates
// occlusion over a depth-scaled screen-space kernel. AO lands in .r.
layout(location = 0) in vec2 vUV;
layout(location = 0) out vec4 FragColor;

#include "engine_ubo.glsl"

layout(set = 1, binding = 0) uniform sampler2D uDepth;

float RawDepth(vec2 uv) {
    vec4 p = texture(uDepth, uv);
    return p.r + p.g / 255.0 + p.b / 65025.0 + p.a / 16581375.0;
}
// Interleaved gradient noise: rotates the kernel per pixel so the discrete
// tap radii average into a smooth halo instead of concentric outline rings.
float Ign(vec2 p) {
    return fract(52.9829189 * fract(0.06711056 * p.x + 0.00583715 * p.y));
}

// 12 unit hemisphere directions (z > 0), the top of the classic cosine-ish
// sample kernel (GL twin in ssao.hpp).
const int kSsaoSamples = 12;
const vec3 kSsaoKernel[12] = vec3[12](
    vec3( 0.538,  0.283, 0.834), vec3(-0.342, -0.664, 0.666),
    vec3( 0.043,  0.930, 0.365), vec3(-0.696,  0.549, 0.462),
    vec3( 0.706,  0.370, 0.604), vec3(-0.395, -0.184, 0.900),
    vec3(-0.003, -0.372, 0.928), vec3(-0.717,  0.091, 0.691),
    vec3( 0.442,  0.786, 0.434), vec3(-0.347,  0.880, 0.325),
    vec3(-0.870, -0.298, 0.394), vec3(-0.297, -0.902, 0.312));
void main() {
    float raw = RawDepth(vUV);
    if (raw >= 0.9999) { FragColor = vec4(1.0, 1.0, 1.0, 1.0); return; } // sky
    float viewZ = raw * eng.uFar; // positive linear view depth

    // View-space reconstruction (camera at origin, -z forward): neighbours fix
    // the real surface normal, and each kernel sample is placed in view space
    // so the occlusion test compares the SAMPLE POINT's depth with the stored
    // depth. The old planar-extrapolation compared the receiver's own depth,
    // which cancelled the contact signal (AO ~= 1 everywhere).
    float hdrH = 1.0 / eng.uTexelSize.y;
    float tanHalf = 0.5 * hdrH / eng.uProjScale;
    float aspect = eng.uTexelSize.y / eng.uTexelSize.x;
    vec2 scr = vUV * 2.0 - 1.0;
    vec3 ro = vec3(scr.x * tanHalf * aspect, scr.y * tanHalf, -1.0) * viewZ;
    vec2 oX = vec2(2.0 * eng.uTexelSize.x, 0.0);
    vec2 oY = vec2(0.0, 2.0 * eng.uTexelSize.y);
    float zX = RawDepth(vUV + oX) * eng.uFar;
    float zY = RawDepth(vUV + oY) * eng.uFar;
    vec2 sX = scr + 2.0 * oX;
    vec2 sY = scr + 2.0 * oY;
    vec3 pX = vec3(sX.x * tanHalf * aspect, sX.y * tanHalf, -1.0) *
              (zX < eng.uFar * 0.9999 ? zX : viewZ);
    vec3 pY = vec3(sY.x * tanHalf * aspect, sY.y * tanHalf, -1.0) *
              (zY < eng.uFar * 0.9999 ? zY : viewZ);
    vec3 N = normalize(cross(pY - ro, pX - ro));
    if (dot(N, ro) > 0.0) N = -N;

    float ang = Ign(gl_FragCoord.xy) * 6.2831853;
    vec3 rvec = vec3(cos(ang), sin(ang), 0.0);
    vec3 T = normalize(rvec - N * dot(rvec, N));
    vec3 B = cross(N, T);
    mat3 TBN = mat3(T, B, N);

    float occ = 0.0;
    for (int i = 0; i < kSsaoSamples; ++i) {
        float t = (float(i) + 0.5) / float(kSsaoSamples);
        vec3 sv = TBN * (kSsaoKernel[i] * (eng.uRadius * mix(0.25, 1.0, t)));
        vec3 samplePos = ro + sv;
        float sampleZ = -samplePos.z;
        if (sampleZ < 1e-3) continue;
        vec2 suv = vec2(samplePos.x / (-samplePos.z * tanHalf * aspect),
                        samplePos.y / (-samplePos.z * tanHalf)) * 0.5 + 0.5;
        if (suv.x < 0.0 || suv.x > 1.0 || suv.y < 0.0 || suv.y > 1.0) continue;
        float sceneZ = RawDepth(suv) * eng.uFar;
        if (sceneZ >= eng.uFar * 0.9999) continue;
        float rangeCheck = smoothstep(0.0, 1.0,
                                      eng.uRadius / max(abs(viewZ - sceneZ), 1e-4));
        float diff = sampleZ - sceneZ;
        if (diff > eng.uBias) occ += rangeCheck;
    }
    float ao = clamp(1.0 - occ / float(kSsaoSamples), 0.0, 1.0);
    FragColor = vec4(vec3(pow(ao, eng.uPower)), 1.0);
}
