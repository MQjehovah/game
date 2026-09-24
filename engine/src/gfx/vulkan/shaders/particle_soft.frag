#version 450
#extension GL_GOOGLE_include_directive : require
// Soft-particle fragment: multiplies the atlas colour by the tint and fades
// the alpha where the quad crosses scene geometry (uSceneDepth carries the
// resolved main-pass depth). uSoftFade <= 0 disables the fade.
layout(location = 0) in vec2 vUV;
layout(location = 1) in vec4 vColor;
layout(location = 2) in vec4 vInstanceColor;
layout(location = 3) in float vWinDepth;
layout(location = 0) out vec4 FragColor;

#include "engine_ubo.glsl"

layout(set = 1, binding = 0) uniform sampler2D uAlbedo;
layout(set = 1, binding = 22) uniform sampler2D uSceneDepth;

void main() {
    vec4 tex = eng.uHasTexture != 0 ? texture(uAlbedo, vUV) : vec4(1.0);
    vec4 col = tex * eng.uTint * vColor * vInstanceColor;
    if (eng.uSoftFade > 0.0) {
        float scene = texture(uSceneDepth, gl_FragCoord.xy / eng.uScreenSize).r;
        col.a *= clamp((scene - vWinDepth) / eng.uSoftFade, 0.0, 1.0);
    }
    FragColor = col;
}
