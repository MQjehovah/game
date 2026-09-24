#version 450
#extension GL_GOOGLE_include_directive : require
// Motion-vector vertex shader (step E2, feeds the TAA resolve). Ported from the
// GL velocity program: the current and previous clip positions are differenced
// and handed to the fragment stage as the screen-space offset to ADD to the
// current UV to find that surface point in the history buffer. The engine
// matrices are GL-convention, so each position is flipped into Vulkan NDC
// before the subtraction - that way the delta already lives in the y-down UV
// space of the velocity target.
layout(location = 0) in vec3 aPos;

layout(location = 0) out vec2 vMotion;

#include "engine_ubo.glsl"

void main() {
    vec4 cur = eng.uViewProj * eng.uModel * vec4(aPos, 1.0);
    vec4 prv = eng.uPrevViewProj * eng.uPrevModel * vec4(aPos, 1.0);
    float cw = abs(cur.w) > 1e-6 ? cur.w : 1e-6;
    float pw = abs(prv.w) > 1e-6 ? prv.w : 1e-6;
    // NDC spans 2 UV units, hence the 0.5.
    vec2 curUv = vec2(cur.x / cw, -cur.y / cw) * 0.5;
    vec2 prvUv = vec2(prv.x / pw, -prv.y / pw) * 0.5;
    vMotion = prvUv - curUv;
    gl_Position = cur;
    gl_Position.y = -gl_Position.y;
    gl_Position.z = gl_Position.z * 0.5 + gl_Position.w * 0.5;
}
