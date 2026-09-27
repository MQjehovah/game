#version 450
#extension GL_GOOGLE_include_directive : require
// 16x16 uniform subsample of the log-luminance target -> 1x1 average. The old
// 4 taps sat on the centre 2x2 texels (~0.002% of the frame), so the camera
// aim alone swung the whole frame's exposure. Letterbox bars are excluded
// (taps outside the scene rect are skipped) so clear-colour black can't poison
// the mean.
layout(location = 0) in vec2 vUV;
layout(location = 0) out vec4 FragColor;

#include "engine_ubo.glsl"

layout(set = 1, binding = 0) uniform sampler2D uLum;

void main() {
    const int GRID = 16;
    float sum = 0.0;
    float n = 0.0;
    for (int y = 0; y < GRID; ++y) {
        for (int x = 0; x < GRID; ++x) {
            vec2 uv = (vec2(float(x), float(y)) + 0.5) / float(GRID);
            if (uv.x < eng.uSceneVpRect.x || uv.y < eng.uSceneVpRect.y ||
                uv.x > eng.uSceneVpRect.x + eng.uSceneVpRect.z ||
                uv.y > eng.uSceneVpRect.y + eng.uSceneVpRect.w) continue;
            sum += texture(uLum, uv).r;
            n += 1.0;
        }
    }
    float avg = n > 0.0 ? sum / n : log(0.18);
    FragColor = vec4(avg, 0.0, 0.0, 1.0);
}
