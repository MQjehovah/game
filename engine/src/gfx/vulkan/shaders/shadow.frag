#version 450
// CSM shadow fragment shader: packs the window depth into RGBA8
// (color-encoded, matching the GL shadow-map path). Vulkan's default depth
// range [0,1] means gl_FragCoord.z already equals GL's window depth.
layout(location = 0) out vec4 FragColor;

vec4 EncodeDepth(float d) {
    vec4 bits = vec4(1.0, 255.0, 65025.0, 16581375.0) * d;
    bits = fract(bits);
    bits -= bits.yzww * vec4(1.0 / 255.0, 1.0 / 255.0, 1.0 / 255.0, 0.0);
    return bits;
}
void main() {
    FragColor = EncodeDepth(gl_FragCoord.z);
}
