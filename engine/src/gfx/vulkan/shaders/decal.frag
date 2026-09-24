#version 450
#extension GL_GOOGLE_include_directive : require
// Rebuilds the world position of the surface the resolved scene depth shows at
// this pixel, maps it into the decal local box and discards everything outside
// it, so the decal conforms to slopes instead of floating over them. It writes
// gl_FragDepth just in front of the receiving surface so it never z-fights.
layout(location = 0) in vec3 vWorld;
layout(location = 1) in vec2 vUv;
layout(location = 0) out vec4 FragColor;

#include "engine_ubo.glsl"

layout(set = 1, binding = 0) uniform sampler2D uAlbedo;
layout(set = 1, binding = 24) uniform sampler2D uSceneDepth;

void main() {
    vec3 world = vWorld;
    vec2 uv = vUv;
    float outDepth = gl_FragCoord.z;
    if (eng.uDecalProject == 1) {
        vec2 scr = gl_FragCoord.xy / eng.uScreenSize;
        float d = texture(uSceneDepth, scr).r;
        if (d >= 0.99999) discard; // sky / nothing rendered
        // gl_FragCoord is top-down in Vulkan, the engine matrices are GL NDC.
        vec4 clip = vec4(scr.x * 2.0 - 1.0, 1.0 - scr.y * 2.0, d * 2.0 - 1.0, 1.0);
        vec4 wp = eng.uInvViewProj * clip;
        if (abs(wp.w) < 1e-7) discard;
        world = wp.xyz / wp.w;
        outDepth = max(d - eng.uDecalBias, 0.0);
    }
    vec3 local = (eng.uDecalInvModel * vec4(world, 1.0)).xyz;
    if (any(greaterThan(abs(local), vec3(0.5)))) discard; // outside the box
    if (eng.uDecalProject == 1) uv = local.xz + 0.5;
    vec4 tex = texture(uAlbedo, uv);
    vec4 col = tex * eng.uTint;
    if (eng.uDecalAdditive == 1) {
        if (col.a <= 0.0) discard;
        FragColor = vec4(col.rgb * col.a, col.a);
    } else {
        if (col.a <= 0.002) discard;
        FragColor = col;
    }
    gl_FragDepth = outDepth;
}
