// Shared canonical engine uniform block (Vulkan GLSL).
// Compiled by the shader generator (tools/gen_vk_shaders.cmake) into SPIR-V.
//
// The block is IDENTICAL in every shader so the backend uses one fixed
// descriptor set 0 (a dynamic UBO) for all programs. The offsets are explicit
// (all 16-byte aligned) and mirror the CPU-side std140 layout table in
// vk_backend.cpp (kVkUniformOffsets) exactly. If you change a member here,
// change it in BOTH the shader and the C++ table.
layout(set = 0, binding = 0) uniform EngineUBO {
    layout(offset = 0)    mat4 uMVP;
    layout(offset = 64)   mat4 uModel;
    layout(offset = 128)  mat4 uNormalMat;
    layout(offset = 192)  mat4 uViewMatrix;
    layout(offset = 256)  mat4 uBoneMatrices[64];
    layout(offset = 4352) mat4 uLightVP[3];
    layout(offset = 4544) vec4 uCascadeSplits;
    layout(offset = 4560) vec4 uTint;
    layout(offset = 4576) vec3 uPointPos[8];
    layout(offset = 4704) vec3 uPointColor[8];
    layout(offset = 4832) float uPointRadius[8];
    layout(offset = 4864) vec3 uAmbientColor;
    layout(offset = 4960) vec3 uCamPos;
    layout(offset = 4976) vec3 uSunDir;
    layout(offset = 4992) vec3 uSunColor;
    layout(offset = 5008) vec3 uPlayerLightPos;
    layout(offset = 5024) vec3 uPlayerLightColor;
    layout(offset = 5040) float uPlayerLightRadius;
    layout(offset = 5056) vec3 uFogColor;
    layout(offset = 5072) float uFogStart;
    layout(offset = 5088) float uFogEnd;
    layout(offset = 5104) float uAmbient;
    layout(offset = 5120) float uAOStrength;
    layout(offset = 5136) float uEmissiveIntensity;
    layout(offset = 5152) float uShininess;
    layout(offset = 5168) float uMetallic;
    layout(offset = 5184) float uRoughness;
    layout(offset = 5200) float uRoughnessMin;
    layout(offset = 5216) float uIblStrength;
    layout(offset = 5232) vec2 uShadowTexel;
    layout(offset = 5248) vec2 uPointShadowTexel;
    layout(offset = 5264) int uShadowEnabled;
    layout(offset = 5280) int uPointShadowEnabled;
    layout(offset = 5296) int uPointShadowLightCount;
    layout(offset = 5312) int uPointCount;
    layout(offset = 5328) int uHasTexture;
    layout(offset = 5344) int uHasMR;
    layout(offset = 5360) int uHasAO;
    layout(offset = 5376) int uHasEmissive;
    layout(offset = 5392) int uPlayerLightEnabled;
    layout(offset = 5408) int uBloomEnabled;
    layout(offset = 5424) int uTonemapEnabled;
    layout(offset = 5440) float uThreshold;
    layout(offset = 5456) float uStrength;
    layout(offset = 5472) float uExposure;
    layout(offset = 5488) vec2 uTexelSize;
    layout(offset = 5504) vec2 uDirection;
    layout(offset = 5520) vec2 uSrcTexelSize;
    layout(offset = 5536) vec3 uLightPos;
    layout(offset = 5552) float uLightRange;
} eng;
