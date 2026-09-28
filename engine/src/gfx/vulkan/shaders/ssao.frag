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

void main() {
    float raw = RawDepth(vUV);
    if (raw >= 0.9999) { FragColor = vec4(1.0, 1.0, 1.0, 1.0); return; } // sky
    float centre = raw * eng.uFar; // world units
    // Planar-depth rejection: reconstruct the surface's local depth slope and
    // subtract it from the expected neighbour depth so a slanted plane occludes
    // nothing however obliquely it is viewed. Without it the plane's own depth
    // gradient across the kernel was counted as occlusion and the resulting AO
    // gradient quantised into view-angle-dependent horizontal stripes.
    float rx = RawDepth(vUV + vec2(eng.uTexelSize.x, 0.0));
    float ry = RawDepth(vUV + vec2(0.0, eng.uTexelSize.y));
    float gradX = (rx >= 0.9999) ? 0.0 : (rx - raw) * eng.uFar;
    float gradY = (ry >= 0.9999) ? 0.0 : (ry - raw) * eng.uFar;
    // Project the world radius to screen space at the centre depth.
    float pixels = clamp(eng.uRadius * eng.uProjScale / max(centre, 0.001), 1.0, 128.0);
    vec2 texel = eng.uTexelSize * pixels;
    float occ = 0.0;
    for (int i = 0; i < 6; ++i) {
        vec2 off; float k;
        if (i == 0) { off = vec2(0.0); k = 0.5; }
        else if (i == 1) { off = vec2(1.0, 0.0); k = 0.9; }
        else if (i == 2) { off = vec2(1.0, 0.7); k = 0.7; }
        else if (i == 3) { off = vec2(0.0, 1.0); k = 0.9; }
        else if (i == 4) { off = vec2(-1.0, 0.8); k = 0.7; }
        else { off = vec2(-0.6, -1.0); k = 0.85; }
        float planar = clamp((gradX * off.x + gradY * off.y) * pixels * k,
                             -eng.uRadius, eng.uRadius);
        vec2 uv2 = vUV + off * texel * k;
        float s = RawDepth(uv2) * eng.uFar;
        if (s >= eng.uFar * 0.9999) continue; // sky sample
        float diff = (centre + planar) - s; // positive: neighbour is closer
        if (diff > eng.uBias) occ += pow(1.0 - min(diff / eng.uRadius, 1.0), eng.uPower);
    }
    float ao = clamp(1.0 - eng.uPower * (occ / 6.0), 0.0, 1.0);
    FragColor = vec4(ao, ao, ao, 1.0);
}
