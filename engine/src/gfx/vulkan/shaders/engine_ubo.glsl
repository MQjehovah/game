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
    // --- quality upgrade (mirrors the GL side) ----------------------------
    // Per-cascade world size of one shadow texel, for the normal-offset bias.
    layout(offset = 5568) vec3 uShadowTexelWorld;
    // 0 disables PCSS and falls back to the 2x2 comparison.
    layout(offset = 5584) float uShadowSoftness;
    // Receiver offset in shadow texels along the shading normal.
    layout(offset = 5600) float uShadowNormalOffset;
    // Non-zero replaces the shaded result with the raw cascade shadow factor.
    layout(offset = 5616) int uShadowDebug;
    // glTF MASK / foliage card cutout threshold (0 disables).
    layout(offset = 5632) float uAlphaTest;
    // --- post-processing / effects ---------------------------------------
    // Camera planes (AO + fog depth linearisation).
    layout(offset = 5648) float uNear;
    layout(offset = 5664) float uFar;
    // SSAO kernel (world units) and its screen-space projection scale.
    layout(offset = 5680) float uRadius;
    layout(offset = 5696) float uBias;
    layout(offset = 5712) float uPower;
    layout(offset = 5728) float uProjScale;
    // Volumetric shafts.
    layout(offset = 5744) float uDensity;
    layout(offset = 5760) float uWeight;
    layout(offset = 5776) float uDecay;
    // Ray-march step count shared by the volumetric and SSR passes.
    layout(offset = 5792) float uSteps;
    layout(offset = 5808) float uThickness;
    layout(offset = 5824) float uMaxDist;
    // Auto-exposure tuning.
    layout(offset = 5840) float uKeyValue;
    layout(offset = 5856) float uExposureMin;
    layout(offset = 5872) float uExposureMax;
    layout(offset = 5888) float uAdaptation;
    // Composite chain strengths / fog.
    layout(offset = 5904) float uAoIntensity;
    layout(offset = 5920) float uVolStrength;
    layout(offset = 5936) float uSsrStrength;
    layout(offset = 5968) float uFogDensity;
    // Display-space colour grading.
    layout(offset = 5984) float uSaturation;
    layout(offset = 6000) float uContrast;
    layout(offset = 6016) float uGain;
    layout(offset = 6032) float uGamma;
    layout(offset = 6048) float uLift;
    // Vignette.
    layout(offset = 6064) float uVignetteRadius;
    layout(offset = 6080) float uVignetteSoftness;
    layout(offset = 6096) float uVignetteIntensity;
    // Soft-particle depth fade + decal depth bias.
    layout(offset = 6112) float uSoftFade;
    layout(offset = 6128) float uDecalBias;
    // Procedural sky (values mirror Skybox settings).
    layout(offset = 6144) float uSunYaw;
    layout(offset = 6160) float uSunPitch;
    layout(offset = 6176) float uCloudCoverage;
    layout(offset = 6192) float uCloudScale;
    layout(offset = 6208) float uTime;
    // Chain enable flags.
    layout(offset = 6224) int uAoEnabled;
    layout(offset = 6240) int uVolEnabled;
    layout(offset = 6256) int uSsrEnabled;
    layout(offset = 6272) int uFogEnabled;
    layout(offset = 6288) int uGradeEnabled;
    layout(offset = 6304) int uAutoExposure;
    layout(offset = 6320) int uVignette;
    // Decal projection mode + blend.
    layout(offset = 6336) int uDecalProject;
    layout(offset = 6352) int uDecalAdditive;
    // Procedural sky toggles.
    layout(offset = 6368) int uSkyTextureValid;
    layout(offset = 6384) int uSunVisible;
    layout(offset = 6400) int uMoonVisible;
    layout(offset = 6416) int uCloudsEnabled;
    // Screen size in pixels (soft particles + decals).
    layout(offset = 6432) vec2 uScreenSize;
    // Sun position in [0,1] UV (screen-space god-ray approximation).
    layout(offset = 6448) vec2 uSunScreen;
    // Procedural sky gradient.
    layout(offset = 6464) vec3 uSkyTop;
    layout(offset = 6480) vec3 uSkyHorizon;
    // Letterboxed scene viewport in normalised HDR-target UV.
    layout(offset = 6496) vec4 uSceneVpRect;
    // Extra matrix palette for the post / effect passes.
    layout(offset = 6512) mat4 uViewProj;
    layout(offset = 6576) mat4 uInvViewProj;
    layout(offset = 6640) mat4 uPrevViewProj;
    layout(offset = 6704) mat4 uPrevModel;
    layout(offset = 6768) mat4 uDecalInvModel;
    // UV repeat multiplier for lit draws (mirrors GL uTiling).
    layout(offset = 6832) vec2 uTiling;
    // Terrain splat layers (mirrors the GL lit_terrain variant).
    layout(offset = 6848) vec4 uDirtColor;
    layout(offset = 6864) vec4 uRockColor;
    layout(offset = 6880) int uHasGrassTex;
    // Temporal AA resolve (uBlend = weight of the current frame).
    layout(offset = 6896) float uBlend;
    layout(offset = 6912) int uValidHistory;
    layout(offset = 6928) int uHasVelocity;
    layout(offset = 6944) float uSharpen;
    // MSAA depth-resolve sample count (backend-internal depth resolve pass).
    layout(offset = 6960) int uSamples;
    // --- lit material / lighting parity with the GL lit shader -----------
    // (A2 normal mapping + A3 hemisphere ambient + probe-field GI +
    //  Material::highlightColor rim glow + Material::receiveShadow)
    layout(offset = 6976) vec3 uAmbientGroundColor;
    layout(offset = 6992) vec3 uHighlightColor;
    layout(offset = 7008) vec3 uLightProbeMin;
    layout(offset = 7024) vec3 uLightProbeExtent;
    layout(offset = 7040) float uNormalScale;
    layout(offset = 7056) float uHighlightStrength;
    layout(offset = 7072) float uLightProbeRes;
    layout(offset = 7088) float uLightProbeInvMax;
    layout(offset = 7104) int uHasNormalMap;
    layout(offset = 7120) int uReceiveShadow;
    layout(offset = 7136) int uLightProbeEnabled;
    // Bloom upsample filter footprint (mirrors GL uBloomWidth; 1 = bilinear).
    layout(offset = 7140) float uBloomWidth;
} eng;
