#version 450
#extension GL_GOOGLE_include_directive : require
// Screen-space reflections (VK twin of ssr.hpp's kSsrFragmentShader): reflect
// the view ray off a depth-gradient normal, march in VIEW space with a
// perspective-correct projection of every sample, binary-refine the hit and
// write a fresnel-weighted result for the composite to BLEND with.
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
float Ign(vec2 p) {
    return fract(52.9829189 * fract(0.06711056 * p.x + 0.00583715 * p.y));
}

void main() {
    float ndc = LoadDepth(vUV);
    if (ndc >= 1.0) { FragColor = vec4(0.0); return; } // sky -> no reflection
    float viewZ = ViewDepth(ndc);

    float hdrH = 1.0 / eng.uTexelSize.y;
    float tanHalf = 0.5 * hdrH / eng.uProjScale;
    float aspect = eng.uTexelSize.y / eng.uTexelSize.x;

    // View-space position of this fragment (camera at the origin, -z forward).
    vec2 scr = vUV * 2.0 - 1.0;
    vec3 ro = vec3(scr.x * tanHalf * aspect, scr.y * tanHalf, -1.0) * viewZ;

    // View-space normal: cross neighbouring view positions (wide baseline,
    // tilt-clamped against depth cliffs), flipped to the camera's hemisphere.
    vec2 oX = vec2(4.0 * eng.uTexelSize.x, 0.0);
    vec2 oY = vec2(0.0, 4.0 * eng.uTexelSize.y);
    float zX = ViewDepth(LoadDepth(vUV + oX));
    float zY = ViewDepth(LoadDepth(vUV + oY));
    vec2 sX = scr + 2.0 * oX;
    vec2 sY = scr + 2.0 * oY;
    vec3 pX = vec3(sX.x * tanHalf * aspect, sX.y * tanHalf, -1.0) *
              (zX < eng.uFar * 0.9999 ? zX : viewZ);
    vec3 pY = vec3(sY.x * tanHalf * aspect, sY.y * tanHalf, -1.0) *
              (zY < eng.uFar * 0.9999 ? zY : viewZ);
    vec3 n = normalize(cross(pY - ro, pX - ro));
    if (dot(n, ro) > 0.0) n = -n;
    float tilt = length(n.xy);
    if (tilt > 2.0 * abs(n.z)) {
        n.xy *= (2.0 * abs(n.z)) / max(tilt, 1e-6);
        n = normalize(n);
    }

    vec3 rd = reflect(normalize(ro), n);
    // Tangent-ray fade: rays leaving the surface almost parallel TO it are the
    // ground-reflects-ground grazing family - they smear the distant shadowed
    // ground across the reflector in large dark patches at certain angles.
    float tangentFade = smoothstep(0.12, 0.32, dot(rd, n));

    float maxDist = eng.uMaxDist * viewZ * 2.0 * tanHalf * aspect;
    float dt = max(maxDist / eng.uSteps, 1e-3);
    // Sub-step per-pixel jitter (GL twin in ssr.hpp): a deterministic start
    // makes adjacent rows coherently hit/miss, printing horizontal stripes.
    float t = 0.1 + (0.25 + 0.5 * Ign(gl_FragCoord.xy)) * dt;
    // Crossing detection: hit only when the ray goes from BEHIND the surface
    // to IN FRONT beyond the thickness - grazing rays that merely skim the
    // ground must not hit (they smeared the ground's dark regions in large
    // patches across the reflection).
    float prevDelta = 1e9;
    for (int i = 0; i < int(eng.uSteps); ++i) {
        t += dt;
        vec3 p = ro + rd * t;
        if (-p.z < eng.uNear) break;
        vec2 suv = vec2(p.x / (-p.z * tanHalf * aspect),
                        p.y / (-p.z * tanHalf)) * 0.5 + 0.5;
        if (suv.x < 0.0 || suv.x > 1.0 || suv.y < 0.0 || suv.y > 1.0) break;
        float sceneZ = ViewDepth(LoadDepth(suv));
        float rayZ = -p.z;
        // Crossing on the SIGN of delta, not a -thick threshold (a threshold
        // trigger fired on the coarse dt grid at grazing angles and printed
        // view-dependent stripes); bisect to the exact delta == 0 crossing.
        float curDelta = sceneZ - rayZ;
        if (prevDelta > 0.0 && curDelta < 0.0) {
            float tFar = t;
            float tNear = t - dt;
            for (int r = 0; r < 5; ++r) {
                float tm = 0.5 * (tNear + tFar);
                vec3 pm = ro + rd * tm;
                vec2 sm = vec2(0.5, 0.5);
                bool outside = -pm.z < eng.uNear;
                if (!outside) {
                    sm = vec2(pm.x / (-pm.z * tanHalf * aspect),
                              pm.y / (-pm.z * tanHalf)) * 0.5 + 0.5;
                    outside = sm.x < 0.0 || sm.x > 1.0 || sm.y < 0.0 || sm.y > 1.0;
                }
                if (outside) { tFar = tm; continue; }
                float szm = ViewDepth(LoadDepth(sm));
                if (szm - (-pm.z) < 0.0) tFar = tm; else tNear = tm;
            }
            t = 0.5 * (tNear + tFar);
            p = ro + rd * t;
            if (-p.z >= eng.uNear) {
                suv = vec2(p.x / (-p.z * tanHalf * aspect),
                           p.y / (-p.z * tanHalf)) * 0.5 + 0.5;
                // Self-reflection guard (depth-based, GL twin in ssr.hpp):
                // reject only hits on the ORIGIN'S OWN surface (same view
                // depth = same plane) or within a 1-texel-ish UV radius. The
                // old fixed 0.08-UV disc also rejected DIFFERENT surfaces near
                // the contact point - a mirror at an object's base lost its
                // contact reflection and kept a detached ghost further out.
                float hitZ = ViewDepth(LoadDepth(suv));
                bool sameSurface = abs(hitZ - viewZ) < viewZ * 0.02;
                if (suv.x >= 0.0 && suv.x <= 1.0 && suv.y >= 0.0 && suv.y <= 1.0 &&
                    !sameSurface && distance(suv, vUV) >= 0.01) {
                    float edge = smoothstep(0.0, 0.1, suv.x) * smoothstep(1.0, 0.9, suv.x) *
                                 smoothstep(0.0, 0.1, suv.y) * smoothstep(1.0, 0.9, suv.y);
                    float fade = max(1.0 - t / maxDist, 0.0) * edge * tangentFade;
                    // Schlick Fresnel with the PHYSICAL water/dielectric F0
                    // (0.02) - see the GL twin for the full rationale (the
                    // 0.25 floor turned rough ground into mirrors).
                    float fres = 0.02 + 0.98 * pow(1.0 - max(dot(n, -normalize(ro)), 0.0), 5.0);
                    FragColor = vec4(texture(uScene, suv).rgb, fade * fres);
                    return;
                }
            }
        }
        prevDelta = curDelta;
    }
    FragColor = vec4(0.0);
}
