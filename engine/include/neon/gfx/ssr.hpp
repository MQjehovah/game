#pragma once

#include <algorithm>
#include <cmath>

#include "neon/math/vec2.hpp"
#include "neon/math/vec3.hpp"

namespace neon::gfx {

// Screen-space reflections (G1-5 / Godot P2-1). Like bloom.hpp / ssao.hpp, the
// ray-march math + shader source live here so the hit logic is unit-testable
// headlessly with the NullBackend fixture. The pass reuses the colour-encoded
// scene depth (ssaoDepthRT_) plus the resolved HDR scene colour; each pixel
// reflects its view ray off a depth-gradient normal, marches it in screen
// space, and pulls the reflected colour where it hits nearby geometry.

constexpr int kSsrSteps = 48;
constexpr float kSsrThickness = 0.03f; // depth epsilon, RELATIVE to ray depth
constexpr float kSsrMaxDist = 0.6f;   // max screen-space ray length in UV

// Premultiplied-alpha separable blur for the SSR chain. A plain gaussian on
// RGBA smears sparse hits into dark halos: a no-hit texel (0,0,0,0) next to a
// hit (colour, .6) averages toward BLACK, and the composite then blends that
// dimmed colour at the spread alpha - vertical streaks and ghost smears around
// every reflection. Blurring (rgb*a, a) and un-premultiplying keeps black
// texels contributing nothing.
inline constexpr const char* kSsrBlurFragmentShader = R"(
#version 330 core
in vec2 vUV;
out vec4 FragColor;
uniform sampler2D uTex;
uniform vec2 uTexelSize;
uniform vec2 uDirection;
void main() {
    const float w[9] = float[9](0.0048, 0.0287, 0.1028, 0.2210, 0.2854,
                                0.2210, 0.1028, 0.0287, 0.0048);
    vec2 off = uTexelSize * uDirection;
    vec3 psum = vec3(0.0);
    float asum = 0.0;
    for (int i = 0; i < 9; ++i) {
        vec4 s = texture(uTex, vUV + off * float(i - 4));
        psum += s.rgb * s.a * w[i];
        asum += s.a * w[i];
    }
    FragColor = vec4(asum > 1e-4 ? psum / asum : vec3(0.0), asum);
}
)";


// One ray-march step in screen space. `rayDepth` is the ray's view-space depth
// at the current sample, `sceneDepth` the decoded scene depth at the same UV.
// Returns true when the ray is in FRONT of the surface by ~thickness (a hit);
// a sample behind the surface is skipped, a sample "inside" the surface (within
// thickness) is treated as hitting the surface boundary.
inline bool SsrHit(float rayDepth, float sceneDepth, float thickness) {
    return sceneDepth < rayDepth && (rayDepth - sceneDepth) > thickness;
}

// --- Built-in shaders -----------------------------------------------------

inline constexpr const char* kSsrFragmentShader = R"(
#version 330 core
in vec2 vUV;
out vec4 FragColor;
uniform sampler2D uScene;       // resolved HDR colour
uniform sampler2D uDepth;       // colour-encoded scene depth
uniform vec2 uTexelSize;        // FULL-res texel of the depth texture
uniform float uNear;
uniform float uFar;
uniform float uSteps;
uniform float uThickness;       // relative depth epsilon
uniform float uMaxDist;         // ray length in UV
uniform float uProjScale;       // 0.5*height/tan(fovY/2)
float LoadDepth(vec2 uv) {
    vec4 p = texture(uDepth, uv);
    return p.r + p.g / 255.0 + p.b / 65025.0 + p.a / 16581375.0;
}
float ViewDepth(float ndc) {
    // The depth RT stores LINEAR view distance / uFar (SSAO depth encoder).
    return ndc * uFar;
}
// Interleaved gradient noise: jitters the march start per pixel so the
// discrete step bands average into (blur-able) noise instead of jagged edges.
float Ign(vec2 p) {
    return fract(52.9829189 * fract(0.06711056 * p.x + 0.00583715 * p.y));
}
void main() {
    float ndc = LoadDepth(vUV);
    if (ndc >= 1.0) { FragColor = vec4(0.0); return; } // sky -> no reflection
    float viewZ = ViewDepth(ndc);

    // Projection factors: uTexelSize is the full-res depth texel, so
    // 1/uTexelSize.y is the full height and uProjScale = 0.5*H/tan(fovY/2).
    float hdrH = 1.0 / uTexelSize.y;
    float tanHalf = 0.5 * hdrH / uProjScale;
    float aspect = uTexelSize.y / uTexelSize.x;

    // View-space position of this fragment (camera at the origin, looking
    // down -z): the reconstructed ray through the pixel scaled to its depth.
    vec2 scr = vUV * 2.0 - 1.0;
    vec3 ro = vec3(scr.x * tanHalf * aspect, scr.y * tanHalf, -1.0) * viewZ;

    // View-space normal from neighbouring view positions (cross of the
    // deltas, flipped to the camera's hemisphere). A WIDE baseline (4 texels)
    // smooths depth noise; the tilt is clamped so a depth CLIFF cannot
    // explode the normal into a wildly wrong reflection direction (the
    // jagged, shimmering reflection edges).
    vec2 oX = vec2(4.0 * uTexelSize.x, 0.0);
    vec2 oY = vec2(0.0, 4.0 * uTexelSize.y);
    float zX = ViewDepth(LoadDepth(vUV + oX));
    float zY = ViewDepth(LoadDepth(vUV + oY));
    vec2 sX = scr + 2.0 * oX;
    vec2 sY = scr + 2.0 * oY;
    vec3 pX = vec3(sX.x * tanHalf * aspect, sX.y * tanHalf, -1.0) *
              (zX < uFar * 0.9999 ? zX : viewZ);
    vec3 pY = vec3(sY.x * tanHalf * aspect, sY.y * tanHalf, -1.0) *
              (zY < uFar * 0.9999 ? zY : viewZ);
    vec3 n = normalize(cross(pY - ro, pX - ro));
    // Face the camera's hemisphere (the winding alone is arbitrary); a z-sign
    // flip is WRONG here - a floor seen from above has a normal with POSITIVE
    // view z (tilted toward the camera).
    if (dot(n, ro) > 0.0) n = -n;
    // Clamp the tilt to ~63 degrees from the view axis: cliff-corrupted
    // normals scatter the reflections; smooth surfaces never get this steep.
    float tilt = length(n.xy);
    if (tilt > 2.0 * abs(n.z)) {
        n.xy *= (2.0 * abs(n.z)) / max(tilt, 1e-6);
        n = normalize(n);
    }

    vec3 rd = reflect(normalize(ro), n);
    // Tangent-ray fade: rays leaving the surface almost parallel TO it (tiny
    // rd.n) are the ground-reflects-ground grazing family - physically the
    // strongest fresnel, but visually they smear the distant shadowed ground
    // across the reflector in large dark patches at certain view angles.
    // Fade them out; mid-angle object reflections are unaffected.
    float tangentFade = smoothstep(0.12, 0.32, dot(rd, n));
    // NOTE: no rd.z cull. A mirror floor legitimately reflects rays back
    // toward the camera (rd.z > 0) - culling those emptied the SSR entirely.
    // The march's -p.z < uNear break handles rays that pass the camera.

    // March in VIEW space. A straight 3D ray projects to a straight screen
    // line, but its DEPTH along that line is not linear under perspective, so
    // project every sample analytically instead of lerping the depth (the old
    // screen-space march compared against the ORIGIN's depth and happily hit
    // surfaces the ray never crossed - smears and phantom reflections).
    float maxDist = uMaxDist * viewZ * 2.0 * tanHalf * aspect; // world length
    float dt = max(maxDist / uSteps, 1e-3);
    // Deterministic, centred march start. A per-pixel dithered start was tried
    // to break step banding, but it moves each pixel's hit by up to a whole
    // STEP (0.2+ world units) - the sampled reflection colour then varies far
    // beyond what the blur can average, printing view-dependent dense stripes
    // that shifted density while rotating. The binary refinement below already
    // recovers exact crossings, so no dither is needed.
    float t = 0.1 + 0.5 * dt;
    // prev/cur delta = sceneZ - rayZ at consecutive samples; a hit is a SIGN
    // FLIP (ray went from in front of the surface to behind it). A grazing
    // ray that merely skims the ground never flips and never hits.
    float prevDelta = 1e9;
    for (int i = 0; i < int(uSteps); ++i) {
        t += dt;
        vec3 p = ro + rd * t;
        if (-p.z < uNear) break; // slid behind the camera
        vec2 suv = vec2(p.x / (-p.z * tanHalf * aspect),
                        p.y / (-p.z * tanHalf)) * 0.5 + 0.5;
        if (suv.x < 0.0 || suv.x > 1.0 || suv.y < 0.0 || suv.y > 1.0) break;
        float sceneZ = ViewDepth(LoadDepth(suv));
        float rayZ = -p.z;
        // Crossing on the SIGN of delta (sceneZ - rayZ), not a -thick
        // threshold: at grazing angles delta decays slowly and a threshold
        // trigger fired on a coarse dt-grid step, printing view-dependent
        // stripes. Trigger on the true sign flip and bisect to delta == 0.
        float curDelta = sceneZ - rayZ;
        if (prevDelta > 0.0 && curDelta < 0.0) {
            // Binary refinement to the exact crossing (5 bisections).
            float tFar = t;
            float tNear = t - dt;
            for (int r = 0; r < 5; ++r) {
                float tm = 0.5 * (tNear + tFar);
                vec3 pm = ro + rd * tm;
                vec2 sm = vec2(0.5, 0.5);
                bool outside = -pm.z < uNear;
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
            if (-p.z >= uNear) {
                suv = vec2(p.x / (-p.z * tanHalf * aspect),
                           p.y / (-p.z * tanHalf)) * 0.5 + 0.5;
                // Self-reflection guard: reject only hits on the ORIGIN'S OWN
                // SURFACE, detected by DEPTH (the hit lies at essentially the
                // origin's view depth = the same plane), or hits inside a tiny
                // 1-texel-ish UV radius. The old fixed 0.08-UV disc (~100px)
                // also rejected DIFFERENT surfaces near the contact point -
                // a mirror floor at a tower's base lost the tower's contact
                // reflection and kept only a detached ghost further out.
                float hitZ = ViewDepth(LoadDepth(suv));
                bool sameSurface = abs(hitZ - viewZ) < viewZ * 0.02;
                if (suv.x >= 0.0 && suv.x <= 1.0 && suv.y >= 0.0 && suv.y <= 1.0 &&
                    !sameSurface && distance(suv, vUV) >= 0.01) {
                // Fade with march distance and toward the screen edges: samples
                // near the border would extrapolate off-screen geometry.
                float edge = smoothstep(0.0, 0.1, suv.x) * smoothstep(1.0, 0.9, suv.x) *
                             smoothstep(0.0, 0.1, suv.y) * smoothstep(1.0, 0.9, suv.y);
                float fade = max(1.0 - t / maxDist, 0.0) * edge * tangentFade;
                // Schlick-style fresnel with a GAME floor (0.25, not the
                // physical dielectric 0.04): a physical F0 leaves face-on
                // reflections blended at ~4-8% and visually switches the
                // effect off. Grazing angles still dominate.
                float fres = 0.25 + 0.75 * pow(1.0 - max(dot(n, -normalize(ro)), 0.0), 5.0);
                FragColor = vec4(texture(uScene, suv).rgb, fade * fres);
                return;
                }
            }
        }
        prevDelta = curDelta;
    }
    FragColor = vec4(0.0);
}
)";

} // namespace neon::gfx
