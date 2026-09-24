#version 450
#extension GL_GOOGLE_include_directive : require
// Depth resolve for the MSAA HDR target. Vulkan cannot blit or resolve
// depth/stencil images (vkCmdResolveImage is colour-only and vkCmdBlitImage
// rejects multisampled sources), so the multisampled depth attachment is
// collapsed into the single-sample depth target by a depth-writing fullscreen
// pass instead. The nearest sample wins, matching the GL_NEAREST depth blit the
// GL backend uses for the same job.
layout(location = 0) in vec2 vUV;

#include "engine_ubo.glsl"

layout(set = 1, binding = 0) uniform sampler2DMS uDepthMs;

void main() {
    ivec2 p = ivec2(gl_FragCoord.xy);
    int n = max(eng.uSamples, 1);
    float d = 1.0;
    for (int i = 0; i < n; ++i) d = min(d, texelFetch(uDepthMs, p, i).r);
    gl_FragDepth = d;
}
