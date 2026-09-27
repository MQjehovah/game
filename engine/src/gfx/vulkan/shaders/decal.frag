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
        // gl_FragCoord is top-down in Vulkan, but the scene vertex shaders
        // already flip clip y (lit.vert etc.), which cancels the framebuffer
        // row-order difference: the depth texture content at texture-v matches
        // GL's. Rebuilding with an EXTRA flip (1 - scr.y*2) mirrored the
        // projection box and threw decals onto vertically-mirrored positions.
        vec4 clip = vec4(scr.x * 2.0 - 1.0, scr.y * 2.0 - 1.0, d * 2.0 - 1.0, 1.0);
        vec4 wp = eng.uInvViewProj * clip;
        if (abs(wp.w) < 1e-7) discard;
        world = wp.xyz / wp.w;
        // uDecalBias is in WORLD units. Window depth is quadratic in distance,
        // so a constant window bias drifted with camera distance squared (at
        // gameplay range it covered a full world unit and decals painted over
        // the legs of units standing in them). Linearise -> offset -> re-encode
        // keeps the offset constant in world space.
        float nf = eng.uNear * eng.uFar;
        float dist = (2.0 * nf) /
                     (eng.uFar + eng.uNear - (d * 2.0 - 1.0) * (eng.uFar - eng.uNear));
        float biased = max(dist - eng.uDecalBias, eng.uNear * 1.001);
        outDepth = max(eng.uFar / (eng.uFar - eng.uNear) -
                           nf / ((eng.uFar - eng.uNear) * biased), 0.0);
    }
    vec3 local = (eng.uDecalInvModel * vec4(world, 1.0)).xyz;
    if (any(greaterThan(abs(local), vec3(0.5)))) discard; // outside the box
    if (eng.uDecalProject == 1) uv = local.xz + 0.5;
    vec4 tex = texture(uAlbedo, uv);
    vec4 col = tex * eng.uTint;
    if (eng.uDecalAdditive == 1) {
        if (col.a <= 0.0) discard;
        // Straight colour: the engine's additive blend is (SRC_ALPHA, ONE), so
        // premultiplying here squared the alpha (a=0.5 texels contributed 25%).
        FragColor = vec4(col.rgb, col.a);
    } else {
        if (col.a <= 0.002) discard;
        FragColor = col;
    }
    gl_FragDepth = outDepth;
}
