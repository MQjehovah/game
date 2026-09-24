#version 450
// Motion vectors: xy = UV offset to the previous frame, a = 1 marks a pixel the
// velocity pass rasterized (the target is cleared to alpha 0, which the resolve
// reads as "no motion vector, fall back to depth reprojection").
layout(location = 0) in vec2 vMotion;
layout(location = 0) out vec4 FragColor;

void main() {
    FragColor = vec4(vMotion, 0.0, 1.0);
}
