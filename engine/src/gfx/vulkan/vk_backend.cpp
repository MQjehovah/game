// Vulkan render backend (T7.1).
//
// A real, SDK-free Vulkan implementation of IRenderBackend. Vulkan entry
// points are loaded dynamically at runtime via volk (vendored), so no Vulkan
// SDK / loader library is needed - only the platform's vulkan-1.dll (present
// on any machine with a Vulkan driver).
//
// Rendering model:
//  - One graphics+present queue. The frame's work is split into one command
//    buffer per render-target switch; each submission chains onto the previous
//    via a semaphore (so sampler reads see prior writes). This is deliberate:
//    the tested Intel driver device-losts when the whole frame is recorded into
//    a single command buffer, and a fence wait alone does not guarantee
//    GPU-to-GPU memory visibility.
//  - Every program shares ONE pipeline layout: set 0 = dynamic uniform buffer
//    holding the engine's canonical "EngineUBO" block (see
//    shaders/engine_ubo.glsl; the CPU-side offsets are the kUniformOffsets
//    table below - keep the two in sync), set 1 = 25 combined image samplers
//    whose bindings match the renderer's texture slots 0..22.
//  - Uniforms are written into a per-frame ring by name (SetUniform*), and
//    every draw records a snapshot into the ring bound via the dynamic offset.
//  - Pipelines are created lazily per (program, render-pass kind, blend mode,
//    depth test/write, cull mode). All color render targets carry a real depth
//    attachment, so DepthAvailable() == true (better than the GL path, whose
//    window/FBO depth is broken on the tested Intel driver).
//
// Orientation / NDC: the engine's matrices follow GL conventions (NDC y-up).
// The scene vertex shaders flip gl_Position.y so Vulkan's y-down rasterization
// matches; the shadow-map and post passes deliberately do NOT flip so the
// unflipped light matrices in the lit shader sample the maps with the same
// convention as GL. All vertex shaders remap clip z to Vulkan's [0,w] range.
//
// Render-target color images live in VK_IMAGE_LAYOUT_GENERAL for their whole
// life (rendering + resolving + sampling use the same layout), because the
// tested Intel driver is unreliable with color-attachment<->shader-read-only
// transitions. Sampler descriptor layouts use GENERAL for render-target
// textures and SHADER_READ_ONLY for uploaded textures.
//
// HDR: CreateRenderTarget(floatColor=true) allocates a real
// VK_FORMAT_R16G16B16A16_SFLOAT image, so the scene target, the bloom
// pyramid and the composite all carry values above 1.0 (the test machine's
// RTX 2060 passes the renderer's float-target self-test). A driver that
// cannot SAMPLE an SFLOAT image (the Intel this engine grew up on returned
// black) fails that self-test, and the renderer then falls back to the
// legacy non-HDR path instead of silently shading a clamped LDR target.
//
// Degradations vs the GL backend (documented):
//  - Custom shaders with sources outside the built-in set are rejected
//    (CreateShader returns an invalid handle) - the renderer falls back to its
//    built-in programs. No custom shaders are created by neon_rush/neon_editor.
//  - Compressed (BC1) uploads work when the driver exposes BC1 sampling; a
//    probe at Init gates them (the asset layer falls back to RGBA8 otherwise).
//  - The unused depth-texture targets (CreateDepthTarget/BeginDepthPass) are
//    implemented but never exercised by the renderer (shadow maps use the
//    color-encoded RGBA8 path).

#include "neon/gfx/backend.hpp"

#ifndef _WIN32
#error "The Vulkan backend currently supports Windows only (VK_KHR_win32_surface)"
#endif

#define VK_NO_PROTOTYPES
#define VK_USE_PLATFORM_WIN32_KHR
#ifndef NOMINMAX
#define NOMINMAX // windows.h min/max macros break std::min/std::max below
#endif

#include <windows.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <string>
#include <unordered_map>
#include <vector>

#include "volk.h"
#include "vulkan/vulkan.h"

#include "neon/core/log.hpp"
#include "neon/gfx/mesh.hpp"

#include "vk_shaders.hpp"

namespace neon::gfx {
namespace {

constexpr VkFormat kDepthFormats[] = {VK_FORMAT_D24_UNORM_S8_UINT, VK_FORMAT_D32_SFLOAT,
                                      VK_FORMAT_D16_UNORM};
constexpr VkFormat kSwapchainFormats[] = {VK_FORMAT_B8G8R8A8_UNORM, VK_FORMAT_R8G8B8A8_UNORM,
                                          VK_FORMAT_B8G8R8A8_SRGB};
constexpr uint32_t kMaxSamplerSlots = 25;  // renderer texture units 0..24
constexpr size_t kUniformBlockSize = 7152; // EngineUBO std140 size (engine_ubo.glsl)
constexpr uint32_t kFramesInFlight = 2;
constexpr uint64_t kScratchBytes = 16ull * 1024 * 1024;
constexpr uint64_t kUboBytes = 16ull * 1024 * 1024;

// ---------------------------------------------------------------------------
// Uniform layout table. Offsets MUST match shaders/engine_ubo.glsl exactly.
// ---------------------------------------------------------------------------
enum class UniKind { Mat4, Mat4Arr, Vec4, Vec3, Vec2, Float, Int, Num, Sampler };
struct UniEntry {
    const char* name;
    UniKind kind;
    uint32_t offset;
    uint32_t bytes;
    int count; // array count (1 for scalars)
};

const UniEntry kUniformOffsets[] = {
    {"uMVP", UniKind::Mat4, 0, 64, 1},
    {"uModel", UniKind::Mat4, 64, 64, 1},
    {"uNormalMat", UniKind::Mat4, 128, 64, 1},
    {"uViewMatrix", UniKind::Mat4, 192, 64, 1},
    {"uBoneMatrices", UniKind::Mat4Arr, 256, 4096, 64},
    {"uLightVP", UniKind::Mat4Arr, 4352, 192, 3},
    {"uCascadeSplits", UniKind::Vec4, 4544, 16, 1},
    {"uTint", UniKind::Vec4, 4560, 16, 1},
    {"uPointPos[0]", UniKind::Vec3, 4576, 12, 8},
    {"uPointColor[0]", UniKind::Vec3, 4704, 12, 8},
    {"uPointRadius[0]", UniKind::Float, 4832, 4, 8},
    {"uAmbientColor", UniKind::Vec3, 4864, 12, 1},
    {"uCamPos", UniKind::Vec3, 4960, 12, 1},
    {"uSunDir", UniKind::Vec3, 4976, 12, 1},
    {"uSunColor", UniKind::Vec3, 4992, 12, 1},
    {"uPlayerLightPos", UniKind::Vec3, 5008, 12, 1},
    {"uPlayerLightColor", UniKind::Vec3, 5024, 12, 1},
    {"uPlayerLightRadius", UniKind::Float, 5040, 4, 1},
    {"uFogColor", UniKind::Vec3, 5056, 12, 1},
    {"uFogStart", UniKind::Float, 5072, 4, 1},
    {"uFogEnd", UniKind::Float, 5088, 4, 1},
    {"uAmbient", UniKind::Float, 5104, 4, 1},
    {"uAOStrength", UniKind::Float, 5120, 4, 1},
    {"uEmissiveIntensity", UniKind::Float, 5136, 4, 1},
    {"uShininess", UniKind::Float, 5152, 4, 1},
    {"uMetallic", UniKind::Float, 5168, 4, 1},
    {"uRoughness", UniKind::Float, 5184, 4, 1},
    {"uRoughnessMin", UniKind::Float, 5200, 4, 1},
    {"uIblStrength", UniKind::Float, 5216, 4, 1},
    {"uShadowTexel", UniKind::Vec2, 5232, 8, 1},
    {"uPointShadowTexel", UniKind::Vec2, 5248, 8, 1},
    {"uShadowEnabled", UniKind::Int, 5264, 4, 1},
    {"uPointShadowEnabled", UniKind::Int, 5280, 4, 1},
    {"uPointShadowLightCount", UniKind::Int, 5296, 4, 1},
    {"uPointCount", UniKind::Int, 5312, 4, 1},
    {"uHasTexture", UniKind::Int, 5328, 4, 1},
    {"uHasMR", UniKind::Int, 5344, 4, 1},
    {"uHasAO", UniKind::Int, 5360, 4, 1},
    {"uHasEmissive", UniKind::Int, 5376, 4, 1},
    {"uPlayerLightEnabled", UniKind::Int, 5392, 4, 1},
    {"uBloomEnabled", UniKind::Int, 5408, 4, 1},
    {"uTonemapEnabled", UniKind::Int, 5424, 4, 1},
    {"uThreshold", UniKind::Float, 5440, 4, 1},
    {"uStrength", UniKind::Float, 5456, 4, 1},
    {"uExposure", UniKind::Float, 5472, 4, 1},
    {"uTexelSize", UniKind::Vec2, 5488, 8, 1},
    {"uDirection", UniKind::Vec2, 5504, 8, 1},
    {"uSrcTexelSize", UniKind::Vec2, 5520, 8, 1},
    {"uLightPos", UniKind::Vec3, 5536, 12, 1},
    {"uLightRange", UniKind::Float, 5552, 4, 1},
    {"uShadowTexelWorld", UniKind::Vec3, 5568, 12, 1},
    {"uShadowSoftness", UniKind::Float, 5584, 4, 1},
    {"uShadowNormalOffset", UniKind::Float, 5600, 4, 1},
    {"uShadowDebug", UniKind::Int, 5616, 4, 1},
    {"uAlphaTest", UniKind::Float, 5632, 4, 1},
    // --- post-processing / effects (offsets mirror engine_ubo.glsl) -------
    {"uNear", UniKind::Float, 5648, 4, 1},
    {"uFar", UniKind::Float, 5664, 4, 1},
    {"uRadius", UniKind::Float, 5680, 4, 1},
    {"uBias", UniKind::Float, 5696, 4, 1},
    {"uPower", UniKind::Float, 5712, 4, 1},
    {"uProjScale", UniKind::Float, 5728, 4, 1},
    {"uDensity", UniKind::Float, 5744, 4, 1},
    {"uWeight", UniKind::Float, 5760, 4, 1},
    {"uDecay", UniKind::Float, 5776, 4, 1},
    {"uSteps", UniKind::Num, 5792, 4, 1},
    {"uThickness", UniKind::Float, 5808, 4, 1},
    {"uMaxDist", UniKind::Float, 5824, 4, 1},
    {"uKeyValue", UniKind::Float, 5840, 4, 1},
    {"uExposureMin", UniKind::Float, 5856, 4, 1},
    {"uExposureMax", UniKind::Float, 5872, 4, 1},
    {"uAdaptation", UniKind::Float, 5888, 4, 1},
    {"uAoIntensity", UniKind::Float, 5904, 4, 1},
    {"uVolStrength", UniKind::Float, 5920, 4, 1},
    {"uSsrStrength", UniKind::Float, 5936, 4, 1},
    {"uFogDensity", UniKind::Float, 5968, 4, 1},
    {"uSaturation", UniKind::Float, 5984, 4, 1},
    {"uContrast", UniKind::Float, 6000, 4, 1},
    {"uGain", UniKind::Float, 6016, 4, 1},
    {"uGamma", UniKind::Float, 6032, 4, 1},
    {"uLift", UniKind::Float, 6048, 4, 1},
    {"uVignetteRadius", UniKind::Float, 6064, 4, 1},
    {"uVignetteSoftness", UniKind::Float, 6080, 4, 1},
    {"uVignetteIntensity", UniKind::Float, 6096, 4, 1},
    {"uSoftFade", UniKind::Float, 6112, 4, 1},
    {"uDecalBias", UniKind::Float, 6128, 4, 1},
    {"uSunYaw", UniKind::Float, 6144, 4, 1},
    {"uSunPitch", UniKind::Float, 6160, 4, 1},
    {"uCloudCoverage", UniKind::Float, 6176, 4, 1},
    {"uCloudScale", UniKind::Float, 6192, 4, 1},
    {"uTime", UniKind::Float, 6208, 4, 1},
    {"uAoEnabled", UniKind::Int, 6224, 4, 1},
    {"uVolEnabled", UniKind::Int, 6240, 4, 1},
    {"uSsrEnabled", UniKind::Int, 6256, 4, 1},
    {"uFogEnabled", UniKind::Int, 6272, 4, 1},
    {"uGradeEnabled", UniKind::Int, 6288, 4, 1},
    {"uAutoExposure", UniKind::Int, 6304, 4, 1},
    {"uVignette", UniKind::Int, 6320, 4, 1},
    {"uDecalProject", UniKind::Int, 6336, 4, 1},
    {"uDecalAdditive", UniKind::Int, 6352, 4, 1},
    {"uSkyTextureValid", UniKind::Int, 6368, 4, 1},
    {"uSunVisible", UniKind::Int, 6384, 4, 1},
    {"uMoonVisible", UniKind::Int, 6400, 4, 1},
    {"uCloudsEnabled", UniKind::Int, 6416, 4, 1},
    {"uScreenSize", UniKind::Vec2, 6432, 8, 1},
    {"uSunScreen", UniKind::Vec2, 6448, 8, 1},
    {"uSkyTop", UniKind::Vec3, 6464, 12, 1},
    {"uSkyHorizon", UniKind::Vec3, 6480, 12, 1},
    {"uSceneVpRect", UniKind::Vec4, 6496, 16, 1},
    {"uViewProj", UniKind::Mat4, 6512, 64, 1},
    {"uInvViewProj", UniKind::Mat4, 6576, 64, 1},
    {"uPrevViewProj", UniKind::Mat4, 6640, 64, 1},
    {"uPrevModel", UniKind::Mat4, 6704, 64, 1},
    {"uDecalInvModel", UniKind::Mat4, 6768, 64, 1},
    {"uTiling", UniKind::Vec2, 6832, 8, 1},
    {"uDirtColor", UniKind::Vec4, 6848, 16, 1},
    {"uRockColor", UniKind::Vec4, 6864, 16, 1},
    {"uHasGrassTex", UniKind::Int, 6880, 4, 1},
    {"uBlend", UniKind::Float, 6896, 4, 1},
    {"uValidHistory", UniKind::Int, 6912, 4, 1},
    {"uHasVelocity", UniKind::Int, 6928, 4, 1},
    {"uSharpen", UniKind::Float, 6944, 4, 1},
    {"uSamples", UniKind::Int, 6960, 4, 1},
    {"uAmbientGroundColor", UniKind::Vec3, 6976, 12, 1},
    {"uHighlightColor", UniKind::Vec3, 6992, 12, 1},
    {"uLightProbeMin", UniKind::Vec3, 7008, 12, 1},
    {"uLightProbeExtent", UniKind::Vec3, 7024, 12, 1},
    {"uNormalScale", UniKind::Float, 7040, 4, 1},
    {"uHighlightStrength", UniKind::Float, 7056, 4, 1},
    {"uLightProbeRes", UniKind::Float, 7072, 4, 1},
    {"uLightProbeInvMax", UniKind::Float, 7088, 4, 1},
    {"uHasNormalMap", UniKind::Int, 7104, 4, 1},
    {"uReceiveShadow", UniKind::Int, 7120, 4, 1},
    {"uLightProbeEnabled", UniKind::Int, 7136, 4, 1},
    {"uAlbedo", UniKind::Sampler, 0, 0, 1},
    {"uGrassTex", UniKind::Sampler, 0, 0, 1},
    {"uCurrent", UniKind::Sampler, 0, 0, 1},
    {"uHistory", UniKind::Sampler, 0, 0, 1},
    {"uVelocity", UniKind::Sampler, 0, 0, 1},
    {"uSource", UniKind::Sampler, 0, 0, 1},
    {"uDepthMs", UniKind::Sampler, 0, 0, 1},
    {"uMR", UniKind::Sampler, 0, 0, 1},
    {"uOcclusion", UniKind::Sampler, 0, 0, 1},
    {"uEmissive", UniKind::Sampler, 0, 0, 1},
    {"uTex", UniKind::Sampler, 0, 0, 1},
    {"uHdr", UniKind::Sampler, 0, 0, 1},
    {"uBloom", UniKind::Sampler, 0, 0, 1},
    {"uHalf", UniKind::Sampler, 0, 0, 1},
    {"uQuarter", UniKind::Sampler, 0, 0, 1},
    {"uShadowMap0", UniKind::Sampler, 0, 0, 1},
    {"uShadowMap1", UniKind::Sampler, 0, 0, 1},
    {"uShadowMap2", UniKind::Sampler, 0, 0, 1},
    {"uIrradianceMap", UniKind::Sampler, 0, 0, 1},
    {"uPrefilteredMap", UniKind::Sampler, 0, 0, 1},
    {"uBrdfLUT", UniKind::Sampler, 0, 0, 1},
    {"uNormalMap", UniKind::Sampler, 0, 0, 1},         // lit unit 23
    {"uLightProbeAtlas", UniKind::Sampler, 0, 0, 1},   // lit unit 24
    // post / effect samplers (bindings equal the renderer texture unit)
    {"uDepthTex", UniKind::Sampler, 0, 0, 1},
    {"uDepth", UniKind::Sampler, 0, 0, 1},
    {"uScene", UniKind::Sampler, 0, 0, 1},
    {"uSceneDepth", UniKind::Sampler, 0, 0, 1},
    {"uLum", UniKind::Sampler, 0, 0, 1},
    {"uAvgLum", UniKind::Sampler, 0, 0, 1},
    {"uPrevExposure", UniKind::Sampler, 0, 0, 1},
    {"uAo", UniKind::Sampler, 0, 0, 1},
    {"uVol", UniKind::Sampler, 0, 0, 1},
    {"uSsr", UniKind::Sampler, 0, 0, 1},
    {"uFogDepth", UniKind::Sampler, 0, 0, 1},
    {"uSkyTexture", UniKind::Sampler, 0, 0, 1},
};

// Opt-in render trace: NEON_VK_TRACE=1 logs backend entry points so a crash can be
// localised to the last completed call.
bool VkTraceEnabled() {
    static const bool enabled = []() {
        const char* v = std::getenv("NEON_VK_TRACE");
        return v && v[0] && v[0] != 48;
    }();
    return enabled;
}

#define NEON_VK_TRACE(...) do { if (VkTraceEnabled()) { std::printf(__VA_ARGS__); std::fflush(stdout); } } while (0)

const UniEntry* FindUniform(const char* name) {
    for (const UniEntry& e : kUniformOffsets) {
        if (std::strcmp(e.name, name) == 0) return &e;
    }
    for (const UniEntry& e : kUniformOffsets) {
        if (e.count <= 1) continue;
        const char* stemEnd = std::strchr(e.name, '[');
        if (!stemEnd) continue;
        const size_t stemLen = static_cast<size_t>(stemEnd - e.name + 1);
        if (std::strncmp(e.name, name, stemLen) == 0 && name[stemLen] != '\0') return &e;
    }
    return nullptr;
}

uint32_t ElementOffset(const UniEntry* e, const char* name) {
    if (e->count <= 1) return e->offset;
    const char* bracket = std::strchr(name, '[');
    if (!bracket) return e->offset;
    int index = 0;
    if (std::sscanf(bracket + 1, "%d", &index) != 1 || index < 0 || index >= e->count)
        return e->offset;
    return e->offset + static_cast<uint32_t>(index) * 16u;
}

void TransposeMat4(float* dst, const float* src) {
    for (int r = 0; r < 4; ++r)
        for (int c = 0; c < 4; ++c) dst[c * 4 + r] = src[r * 4 + c];
}

// IEEE 754 half -> float (A2: HDR target readback conversion).
float HalfToFloat(uint16_t h) {
    const uint32_t sign = static_cast<uint32_t>(h >> 15) & 1u;
    const uint32_t exp = (static_cast<uint32_t>(h) >> 10) & 0x1Fu;
    const uint32_t frac = static_cast<uint32_t>(h) & 0x3FFu;
    uint32_t bits = 0;
    if (exp == 0) {
        if (frac == 0) {
            bits = sign << 31; // +/-0
        } else {
            // Subnormal half: normalize into a float subnormal.
            int e = -1;
            uint32_t f = frac;
            do {
                f <<= 1;
                ++e;
            } while ((f & 0x400u) == 0);
            bits = (sign << 31) | (static_cast<uint32_t>(127 - 15 - e) << 23) | ((f & 0x3FFu) << 13);
        }
    } else if (exp == 31) {
        bits = (sign << 31) | 0x7F800000u | (frac << 13); // inf / nan
    } else {
        bits = (sign << 31) | ((exp - 15 + 127) << 23) | (frac << 13);
    }
    float out;
    std::memcpy(&out, &bits, 4);
    return out;
}

// True when the physical device exposes the named device extension.
bool DeviceHasExtension(VkPhysicalDevice dev, const char* name) {
    uint32_t count = 0;
    if (vkEnumerateDeviceExtensionProperties(dev, nullptr, &count, nullptr) != VK_SUCCESS) return false;
    std::vector<VkExtensionProperties> exts(count);
    vkEnumerateDeviceExtensionProperties(dev, nullptr, &count, exts.data());
    for (const VkExtensionProperties& e : exts) {
        if (std::strcmp(e.extensionName, name) == 0) return true;
    }
    return false;
}

// Graphics queue family index that can also present to `surface`, or UINT32_MAX.
uint32_t FindGraphicsPresentQueueFamily(VkPhysicalDevice dev, VkSurfaceKHR surface) {
    uint32_t count = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(dev, &count, nullptr);
    std::vector<VkQueueFamilyProperties> fams(count);
    vkGetPhysicalDeviceQueueFamilyProperties(dev, &count, fams.data());
    for (uint32_t i = 0; i < count; ++i) {
        if (!(fams[i].queueFlags & VK_QUEUE_GRAPHICS_BIT)) continue;
        VkBool32 present = VK_FALSE;
        vkGetPhysicalDeviceSurfaceSupportKHR(dev, i, surface, &present);
        if (present) return i;
    }
    return UINT32_MAX;
}

// Swapchain/present capability probe used to score candidate devices.
int DevicePresentScore(VkPhysicalDevice dev, VkSurfaceKHR surface, char* why, size_t whyLen) {
    VkPhysicalDeviceProperties props{};
    vkGetPhysicalDeviceProperties(dev, &props);
    int typeScore = 0;
    switch (props.deviceType) {
        case VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU: typeScore = 400; break;
        case VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU: typeScore = 300; break;
        case VK_PHYSICAL_DEVICE_TYPE_VIRTUAL_GPU: typeScore = 200; break;
        case VK_PHYSICAL_DEVICE_TYPE_CPU: typeScore = 100; break;
        default: typeScore = 50; break;
    }
    if (!DeviceHasExtension(dev, VK_KHR_SWAPCHAIN_EXTENSION_NAME)) {
        std::snprintf(why, whyLen, "no VK_KHR_swapchain");
        return -1;
    }
    if (FindGraphicsPresentQueueFamily(dev, surface) == UINT32_MAX) {
        std::snprintf(why, whyLen, "no graphics+present queue family");
        return -1;
    }
    uint32_t formatCount = 0;
    uint32_t modeCount = 0;
    vkGetPhysicalDeviceSurfaceFormatsKHR(dev, surface, &formatCount, nullptr);
    vkGetPhysicalDeviceSurfacePresentModesKHR(dev, surface, &modeCount, nullptr);
    if (formatCount == 0 || modeCount == 0) {
        std::snprintf(why, whyLen, "surface formats=%u modes=%u", formatCount, modeCount);
        return -1;
    }
    VkSurfaceCapabilitiesKHR caps{};
    if (vkGetPhysicalDeviceSurfaceCapabilitiesKHR(dev, surface, &caps) != VK_SUCCESS) {
        std::snprintf(why, whyLen, "surface capabilities unavailable");
        return -1;
    }
    if (caps.supportedUsageFlags == 0 || caps.supportedCompositeAlpha == 0) {
        std::snprintf(why, whyLen, "unsupported usage/alpha bits");
        return -1;
    }
    std::snprintf(why, whyLen, "ok formats=%u modes=%u", formatCount, modeCount);
    return typeScore;
}


VkFormat SwapchainFormat(const std::vector<VkSurfaceFormatKHR>& formats) {
    for (VkFormat want : kSwapchainFormats) {
        for (const VkSurfaceFormatKHR& f : formats) {
            if (f.format == want) return want;
        }
    }
    return formats.empty() ? VK_FORMAT_B8G8R8A8_UNORM : formats[0].format;
}

const char* VkResultName(VkResult r) {
    switch (r) {
        case VK_SUCCESS: return "VK_SUCCESS";
        case VK_NOT_READY: return "VK_NOT_READY";
        case VK_TIMEOUT: return "VK_TIMEOUT";
        case VK_ERROR_OUT_OF_HOST_MEMORY: return "VK_ERROR_OUT_OF_HOST_MEMORY";
        case VK_ERROR_OUT_OF_DEVICE_MEMORY: return "VK_ERROR_OUT_OF_DEVICE_MEMORY";
        case VK_ERROR_INITIALIZATION_FAILED: return "VK_ERROR_INITIALIZATION_FAILED";
        case VK_ERROR_DEVICE_LOST: return "VK_ERROR_DEVICE_LOST";
        case VK_ERROR_LAYER_NOT_PRESENT: return "VK_ERROR_LAYER_NOT_PRESENT";
        case VK_ERROR_EXTENSION_NOT_PRESENT: return "VK_ERROR_EXTENSION_NOT_PRESENT";
        case VK_ERROR_FEATURE_NOT_PRESENT: return "VK_ERROR_FEATURE_NOT_PRESENT";
        case VK_ERROR_INCOMPATIBLE_DRIVER: return "VK_ERROR_INCOMPATIBLE_DRIVER";
        case VK_ERROR_TOO_MANY_OBJECTS: return "VK_ERROR_TOO_MANY_OBJECTS";
        case VK_ERROR_FORMAT_NOT_SUPPORTED: return "VK_ERROR_FORMAT_NOT_SUPPORTED";
        case VK_ERROR_SURFACE_LOST_KHR: return "VK_ERROR_SURFACE_LOST_KHR";
        case VK_ERROR_NATIVE_WINDOW_IN_USE_KHR: return "VK_ERROR_NATIVE_WINDOW_IN_USE_KHR";
        case VK_ERROR_OUT_OF_DATE_KHR: return "VK_ERROR_OUT_OF_DATE_KHR";
        case VK_ERROR_INCOMPATIBLE_DISPLAY_KHR: return "VK_ERROR_INCOMPATIBLE_DISPLAY_KHR";
        default: break;
    }
    static char buf[48];
    std::snprintf(buf, sizeof(buf), "VkResult(%d)", static_cast<int>(r));
    return buf;
}

struct Texture {
    VkImage image = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;
    VkSampler sampler = VK_NULL_HANDLE;
    VkFormat format = VK_FORMAT_UNDEFINED;
    int width = 0;
    int height = 0;
    int mipLevels = 1;
    VkImageLayout layout = VK_IMAGE_LAYOUT_UNDEFINED;
    bool owned = true;
};

struct Mesh {
    VkBuffer vbo = VK_NULL_HANDLE;
    VkDeviceMemory vboMem = VK_NULL_HANDLE;
    VkBuffer ibo = VK_NULL_HANDLE;
    VkDeviceMemory iboMem = VK_NULL_HANDLE;
    uint32_t indexCount = 0;
    uint32_t vertexCount = 0;
    // 0 = uint16 indices, 1 = uint32 (large merged geometry, see CreateMeshU32).
    uint32_t indexType = 0;
};

enum class RpKind : uint8_t { Color1, Float1, Float2, Float4, DepthOnly };

struct Target {
    uint32_t id = 0;
    VkImage colorImage = VK_NULL_HANDLE;
    VkDeviceMemory colorMem = VK_NULL_HANDLE;
    VkImageView colorView = VK_NULL_HANDLE;
    VkImage depthImage = VK_NULL_HANDLE;
    VkDeviceMemory depthMem = VK_NULL_HANDLE;
    VkImageView depthView = VK_NULL_HANDLE;
    VkFramebuffer framebuffer = VK_NULL_HANDLE;
    VkRenderPass rpClear = VK_NULL_HANDLE;
    VkRenderPass rpLoad = VK_NULL_HANDLE;
    RpKind kind = RpKind::Color1;
    VkFormat colorFormat = VK_FORMAT_UNDEFINED;
    int width = 0;
    int height = 0;
    int samples = 1;
    bool floatColor = false;
    bool depthOnly = false;
    bool swapchain = false;
    // Per-target pending clear (see Clear()). The renderer binds a target,
    // clears it, and then binds OTHER targets before drawing into it again
    // (the TAA velocity pass, the soft-depth resolve). A single global latch
    // was dropped by the next bind, so the HDR colour and depth began their
    // pass with LOAD_OP_LOAD over undefined memory - every fragment then failed
    // the depth test and only the sky survived. Keeping the flag on the target
    // makes Clear() match GL exactly: it applies to whatever target was bound
    // when it was called, whenever that target next begins a render pass.
    bool clearPending = false;
    VkClearColorValue pendingClearColor = {{0.02f, 0.03f, 0.08f, 1.0f}};
    float pendingClearDepth = 1.0f;
    uint32_t colorTexId = 0;
    uint32_t depthTexId = 0;
    VkImageLayout layout = VK_IMAGE_LAYOUT_UNDEFINED;
    VkImageLayout depthLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    std::vector<VkImageView> swapViews;
    std::vector<VkFramebuffer> swapFramebuffers;
};

enum class BlendState : uint8_t { Opaque, Alpha, Additive, Premultiplied };
enum class VertexVariant : uint8_t { V3d, Instanced, InstancedColored, Ui, Lines };

struct Program {
    uint32_t id = 0;
    std::string name;
    VkShaderModule vert = VK_NULL_HANDLE;
    VkShaderModule frag = VK_NULL_HANDLE;
    bool flipped = true;
    VertexVariant variant = VertexVariant::V3d;
};

struct PipelineKey {
    uint32_t programId;
    uint8_t rp;
    uint8_t blend;
    uint8_t depthTest;
    uint8_t depthWrite;
    uint8_t cull;
    bool operator==(const PipelineKey& o) const {
        return programId == o.programId && rp == o.rp && blend == o.blend &&
               depthTest == o.depthTest && depthWrite == o.depthWrite && cull == o.cull;
    }
};
struct PipelineKeyHash {
    size_t operator()(const PipelineKey& k) const {
        uint64_t v = k.programId;
        v = v * 31 + k.rp;
        v = v * 31 + k.blend;
        v = v * 31 + k.depthTest;
        v = v * 31 + k.depthWrite;
        v = v * 31 + k.cull;
        return std::hash<uint64_t>{}(v);
    }
};

struct RenderPassKey {
    VkFormat format;
    uint32_t samples;
    bool operator==(const RenderPassKey& o) const {
        return format == o.format && samples == o.samples;
    }
};
struct RenderPassKeyHash {
    size_t operator()(const RenderPassKey& k) const {
        return std::hash<uint64_t>{}((static_cast<uint64_t>(k.format) << 16) | k.samples);
    }
};
struct RenderPassPair {
    VkRenderPass clear = VK_NULL_HANDLE;
    VkRenderPass load = VK_NULL_HANDLE;
};

struct Frame {
    // A1: texture descriptor sets live in a PER-FRAME pool. The frame's own
    // fence (waited in EnsureFrameStarted) covers exactly the sets it
    // allocated, so resetting here can never invalidate sets still in flight.
    // The cache is rebuilt on demand -- allocation churn per frame is bounded
    // by the number of distinct texture-slot combinations actually drawn.
    VkDescriptorPool texPool = VK_NULL_HANDLE;
    VkCommandPool pool = VK_NULL_HANDLE;
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    bool cmdOpen = false;
    VkDescriptorPool descPool = VK_NULL_HANDLE;
    VkBuffer uboRing = VK_NULL_HANDLE;
    VkDeviceMemory uboMem = VK_NULL_HANDLE;
    uint8_t* uboPtr = nullptr;
    uint64_t uboCursor = 0;
    VkBuffer scratch = VK_NULL_HANDLE;
    VkDeviceMemory scratchMem = VK_NULL_HANDLE;
    uint8_t* scratchPtr = nullptr;
    uint64_t scratchCursor = 0;
    VkDescriptorSet uboSet = VK_NULL_HANDLE;
    VkSemaphore imageAvailable = VK_NULL_HANDLE;
    VkSemaphore renderFinished = VK_NULL_HANDLE;
    VkSemaphore progress = VK_NULL_HANDLE;
    bool progressSignaled = false;
    VkFence fence = VK_NULL_HANDLE;
    VkFence readFence = VK_NULL_HANDLE;
    bool acquiredWaited = false;
    struct TexKey {
        uint32_t ids[kMaxSamplerSlots];
        bool operator==(const TexKey& o) const {
            return std::memcmp(ids, o.ids, sizeof(ids)) == 0;
        }
    };
    struct TexKeyHash {
        size_t operator()(const TexKey& k) const {
            uint64_t h = 1469598103934665603ull;
            for (uint32_t v : k.ids) {
                h ^= v;
                h *= 1099511628211ull;
            }
            return std::hash<uint64_t>{}(h);
        }
    };
};

class VulkanBackend : public IRenderBackend {
public:
    ~VulkanBackend() override { Shutdown(); }

    bool Init(platform::IWindow* window) override {
        window_ = window;
        if (!window_) return false;
        hwnd_ = reinterpret_cast<HWND>(window_->NativeHandle());
        if (!hwnd_) {
            NEON_LOG_CAT(neon::core::LogCategory::Gfx, neon::core::LogLevel::Error,
                         "Vulkan: window has no native handle (Win32 only)");
            return false;
        }

        if (volkInitialize() != VK_SUCCESS) {
            NEON_LOG_CAT(neon::core::LogCategory::Gfx, neon::core::LogLevel::Error,
                         "Vulkan: volkInitialize failed - is vulkan-1.dll present?");
            return false;
        }

        if (!CreateInstance()) return false;
        if (!CreateDevice()) return false;
        if (!CreateRenderPassesAndObjects()) return false;
        if (!CreateSwapchain()) return false;
        if (!CreateFrames()) return false;

        // Backend-internal resources for the MSAA depth resolve (ResolveDepth).
        // Built once at startup so a missing program is reported once instead of
        // every frame; the quad is the same NDC unit quad the FBO self-tests use
        // (post.vert maps it to the full target when uMVP is the identity).
        depthResolveShader_ = CreateShader("", "", "depth_resolve");
        {
            const Vertex3D verts[4] = {
                {{-1, -1, 0}, {}, {0, 0}, {1, 1, 1, 1}, {0, 0, 0, 0}, {0, 0, 0, 0}},
                {{1, -1, 0}, {}, {1, 0}, {1, 1, 1, 1}, {0, 0, 0, 0}, {0, 0, 0, 0}},
                {{1, 1, 0}, {}, {1, 1}, {1, 1, 1, 1}, {0, 0, 0, 0}, {0, 0, 0, 0}},
                {{-1, 1, 0}, {}, {0, 1}, {1, 1, 1, 1}, {0, 0, 0, 0}, {0, 0, 0, 0}},
            };
            const uint16_t indices[6] = {0, 1, 2, 0, 2, 3};
            resolveQuad_ = CreateMesh(verts, 4, indices, 6);
        }

        const uint8_t whitePx[4] = {255, 255, 255, 255};
        TextureDesc whiteDesc;
        whiteDesc.width = 1;
        whiteDesc.height = 1;
        whiteDesc.rgba = whitePx;
        white_ = CreateTexture(whiteDesc);

        {
            VkFormatProperties props{};
            vkGetPhysicalDeviceFormatProperties(physicalDevice_, VK_FORMAT_BC1_RGBA_UNORM_BLOCK,
                                                 &props);
            compressedTexSupported_ =
                (props.optimalTilingFeatures & VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT) != 0;
            NEON_LOG_CAT(neon::core::LogCategory::Gfx, neon::core::LogLevel::Info,
                         "Vulkan: compressed textures (BC1): %s",
                         compressedTexSupported_ ? "supported" : "NOT supported - RGBA8 fallback");
        }

        VkPhysicalDeviceProperties props;
        vkGetPhysicalDeviceProperties(physicalDevice_, &props);
        NEON_LOG_CAT(neon::core::LogCategory::Gfx, neon::core::LogLevel::Info,
                     "Vulkan backend: %s (%s)", props.deviceName, Name());
        return true;
    }

    void Shutdown() override {
        if (!device_) return;
        vkDeviceWaitIdle(device_);

        for (auto& [id, prog] : programs_) {
            if (prog.vert) vkDestroyShaderModule(device_, prog.vert, nullptr);
            if (prog.frag) vkDestroyShaderModule(device_, prog.frag, nullptr);
        }
        programs_.clear();
        for (auto& [key, pipe] : pipelines_) vkDestroyPipeline(device_, pipe, nullptr);
        pipelines_.clear();

        for (auto& [id, mesh] : meshes_) {
            if (mesh.vbo) vkDestroyBuffer(device_, mesh.vbo, nullptr);
            if (mesh.vboMem) vkFreeMemory(device_, mesh.vboMem, nullptr);
            if (mesh.ibo) vkDestroyBuffer(device_, mesh.ibo, nullptr);
            if (mesh.iboMem) vkFreeMemory(device_, mesh.iboMem, nullptr);
        }
        meshes_.clear();

        for (auto& [id, tex] : textures_) DestroyTextureInternal(tex);
        textures_.clear();
        white_ = {};

        for (auto& [id, rt] : renderTargets_) DestroyTargetInternal(rt);
        renderTargets_.clear();

        for (auto& [key, pair] : renderPasses_) {
            vkDestroyRenderPass(device_, pair.clear, nullptr);
            vkDestroyRenderPass(device_, pair.load, nullptr);
        }
        renderPasses_.clear();
        if (depthOnlyPass_) vkDestroyRenderPass(device_, depthOnlyPass_, nullptr);

        DestroySwapchain();
        DestroyFrames();

        if (samplerNearest_) vkDestroySampler(device_, samplerNearest_, nullptr);
        if (samplerLinear_) vkDestroySampler(device_, samplerLinear_, nullptr);
        if (samplerRepeatNearest_) vkDestroySampler(device_, samplerRepeatNearest_, nullptr);
        if (samplerRepeatLinear_) vkDestroySampler(device_, samplerRepeatLinear_, nullptr);
        if (pipelineCache_) vkDestroyPipelineCache(device_, pipelineCache_, nullptr);
        if (descLayoutSet0_) vkDestroyDescriptorSetLayout(device_, descLayoutSet0_, nullptr);
        if (descLayoutSet1_) vkDestroyDescriptorSetLayout(device_, descLayoutSet1_, nullptr);
        if (pipelineLayout_) vkDestroyPipelineLayout(device_, pipelineLayout_, nullptr);

        vkDestroyDevice(device_, nullptr);
        vkDestroySurfaceKHR(instance_, surface_, nullptr);
        vkDestroyInstance(instance_, nullptr);
        volkFinalize();
        device_ = VK_NULL_HANDLE;
        instance_ = VK_NULL_HANDLE;
        surface_ = VK_NULL_HANDLE;
        window_ = nullptr;
    }

    const char* Name() const override { return "Vulkan 1.1 (volk)"; }

    // ------------------------------------------------------------------
    // Render targets
    // ------------------------------------------------------------------
    RenderTargetHandle CreateRenderTarget(int width, int height, bool floatColor,
                                          int samples) override {
        NEON_VK_TRACE("vt: CreateRenderTarget %dx%d float=%d\n", width, height, (int)floatColor);
        if (width <= 0 || height <= 0) return {};
        // A2: honor floatColor with real R16G16B16A16_SFLOAT so the HDR +
        // bloom pipeline is not silently clamped to LDR. Drivers whose SFLOAT
        // SAMPLING is broken (the Intel this engine grew up on) are detected
        // by the renderer's float-target self-test, which then disables HDR
        // and falls back to the direct composite path (same adaptive pattern
        // as the GL backend).
        const VkFormat colorFormat =
            floatColor ? VK_FORMAT_R16G16B16A16_SFLOAT : VK_FORMAT_R8G8B8A8_UNORM;
        samples = ClampSamples(samples);
        RpKind kind;
        if (floatColor) {
            kind = samples >= 4 ? RpKind::Float4 : (samples >= 2 ? RpKind::Float2 : RpKind::Float1);
        } else {
            kind = RpKind::Color1;
        }

        Target rt;
        rt.width = width;
        rt.height = height;
        rt.samples = samples;
        rt.floatColor = floatColor;
        rt.kind = kind;
        rt.colorFormat = colorFormat;
        rt.layout = VK_IMAGE_LAYOUT_GENERAL;

        const VkImageUsageFlags colorUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
                                             VK_IMAGE_USAGE_SAMPLED_BIT |
                                             VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                                             VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        if (!CreateImage(width, height, colorFormat, samples, colorUsage, VK_IMAGE_TILING_OPTIMAL,
                         &rt.colorImage, &rt.colorMem, nullptr, 1)) {
            return {};
        }
        rt.colorView =
            CreateView(rt.colorImage, colorFormat, VK_IMAGE_VIEW_TYPE_2D, VK_IMAGE_ASPECT_COLOR_BIT);
        if (!rt.colorView) return {};
        // SAMPLED so the MSAA depth can be read back by the depth-resolve pass
        // (see ResolveDepth); the single-sample attachments get the same flag for
        // free, which keeps the usage list uniform.
        if (!CreateImage(width, height, depthFormat_, samples,
                         VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT |
                             VK_IMAGE_USAGE_SAMPLED_BIT,
                         VK_IMAGE_TILING_OPTIMAL, &rt.depthImage, &rt.depthMem, &rt.depthView, 1)) {
            return {};
        }
        // Multisampled depth needs an explicit resolve pass before anything can
        // sample it, so expose it as a texture here. Single-sample targets keep
        // depthTexId clear: RenderTargetDepthTexture must go on returning the
        // colour-encoded depth for the shadow / post-graph targets.
        if (samples > 1) {
            Texture dtex;
            dtex.image = rt.depthImage;
            dtex.view = rt.depthView;
            dtex.sampler = samplerNearest_;
            dtex.format = depthFormat_;
            dtex.width = width;
            dtex.height = height;
            dtex.mipLevels = 1;
            // The colour render pass declares this image as already being in the
            // depth attachment layout and never transitions it, so the tracked
            // value must claim the same: the resolve pass samples the depth and
            // would otherwise transition from UNDEFINED and discard it.
            dtex.layout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
            rt.depthLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
            dtex.owned = false;
            rt.depthTexId = ++nextTextureId_;
            textures_[rt.depthTexId] = dtex;
        }

        RenderPassPair rp = GetRenderPasses(colorFormat, samples);
        rt.rpClear = rp.clear;
        rt.rpLoad = rp.load;
        rt.framebuffer = CreateFramebuffer(rt.width, rt.height, rp.clear, rt.colorView,
                                           rt.depthView);
        if (!rt.framebuffer) return {};

        rt.colorTexId = ++nextTextureId_;
        Texture tex;
        tex.image = rt.colorImage;
        tex.view = rt.colorView;
        tex.sampler = floatColor ? samplerLinear_ : samplerNearest_;
        tex.format = colorFormat;
        tex.width = width;
        tex.height = height;
        tex.mipLevels = 1;
        tex.layout = VK_IMAGE_LAYOUT_GENERAL;
        tex.owned = false;
        textures_[rt.colorTexId] = tex;

        rt.id = ++nextTargetId_;
        renderTargets_[rt.id] = rt;
        return {rt.id};
    }

    // Every Vulkan colour target created above already carries a D32 depth
    // attachment (see CreateRenderTarget), so the CSM cascade pass can depth-TEST
    // instead of sorting by painters order. That matters for level geometry that
    // interpenetrates inside a single merged mesh (terrain + trees + towers):
    // painters order needs one key per object and therefore drops most casters.
    RenderTargetHandle CreateRenderTargetWithDepth(int width, int height) override {
        NEON_VK_TRACE("vt: CreateRenderTargetWithDepth %dx%d\n", width, height);
        return CreateRenderTarget(width, height, false, 1);
    }

    void DestroyRenderTarget(RenderTargetHandle target) override {
        NEON_VK_TRACE("bt: DestroyRenderTarget %u\n", target.id);
        auto it = renderTargets_.find(target.id);
        if (it == renderTargets_.end()) return;
        FlushPendingWorkBeforeDestroy(&it->second);
        if (it->second.colorTexId) textures_.erase(it->second.colorTexId);
        if (it->second.depthTexId) textures_.erase(it->second.depthTexId);
        DestroyTargetInternal(it->second);
        renderTargets_.erase(it);
    }

    void BindRenderTarget(RenderTargetHandle target) override {
        NEON_VK_TRACE("vt: BindRenderTarget %u\n", target.id);
        auto it = renderTargets_.find(target.id);
        if (it == renderTargets_.end()) return;
        BindTarget(&it->second);
    }

    void BindDefaultTarget() override {
        NEON_VK_TRACE("vt: BindDefaultTarget\n");
        if (!swapTarget_) return;
        BindTarget(swapTarget_.get());
    }

    void ResolveRenderTarget(RenderTargetHandle src, RenderTargetHandle dst) override {
        auto sit = renderTargets_.find(src.id);
        auto dit = renderTargets_.find(dst.id);
        if (sit == renderTargets_.end() || dit == renderTargets_.end()) return;
        Target& s = sit->second;
        Target& d = dit->second;
        if (s.samples <= 1 || s.depthOnly || d.depthOnly) return;
        if (s.width != d.width || s.height != d.height) return;

        Frame& f = CurrentFrame();
        EndRenderPassIfActive(f);
        EnsureFrameStarted();
        OpenCmd(f);

        TransitionImage(f.cmd, s.colorImage, s.layout, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                        VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                        VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
                        VK_ACCESS_TRANSFER_READ_BIT, VK_IMAGE_ASPECT_COLOR_BIT);
        s.layout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        TransitionImage(f.cmd, d.colorImage, d.layout, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                        VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0,
                        VK_ACCESS_TRANSFER_WRITE_BIT, VK_IMAGE_ASPECT_COLOR_BIT);
        d.layout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;

        VkImageResolve region{};
        region.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        region.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        region.extent = {static_cast<uint32_t>(s.width), static_cast<uint32_t>(s.height), 1};
        vkCmdResolveImage(f.cmd, s.colorImage, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, d.colorImage,
                          VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);

        TransitionImage(f.cmd, s.colorImage, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                        VK_IMAGE_LAYOUT_GENERAL, VK_PIPELINE_STAGE_TRANSFER_BIT,
                        VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_TRANSFER_READ_BIT,
                        VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_IMAGE_ASPECT_COLOR_BIT);
        s.layout = VK_IMAGE_LAYOUT_GENERAL;
        TransitionImage(f.cmd, d.colorImage, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                        VK_IMAGE_LAYOUT_GENERAL, VK_PIPELINE_STAGE_TRANSFER_BIT,
                        VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_TRANSFER_WRITE_BIT,
                        VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_IMAGE_ASPECT_COLOR_BIT);
        d.layout = VK_IMAGE_LAYOUT_GENERAL;
    }

    TextureHandle RenderTargetColorTexture(RenderTargetHandle target) const override {
        auto it = renderTargets_.find(target.id);
        if (it == renderTargets_.end() || it->second.depthOnly) return {};
        return {it->second.colorTexId};
    }

    TextureHandle RenderTargetDepthTexture(RenderTargetHandle target) const override {
        auto it = renderTargets_.find(target.id);
        if (it == renderTargets_.end()) return {};
        if (it->second.depthTexId) return {it->second.depthTexId};
        return {it->second.colorTexId};
    }

    // Depth resolve for the MSAA HDR target (the GL backend blits
    // GL_DEPTH_BUFFER_BIT here). Vulkan has no depth blit and vkCmdResolveImage
    // is colour-only, so the multisampled attachment is collapsed into the
    // single-sample depth target by a depth-writing fullscreen pass
    // (shaders/depth_resolve.frag): the post chain, soft particles and the TAA
    // reprojection then read the main-pass depth instead of it being redrawn.
    // Returns false when the resolve is not possible, which keeps the renderer
    // on its colour-encoded caster fallback.
    bool ResolveDepth(RenderTargetHandle src, RenderTargetHandle dst) override {
        auto sit = renderTargets_.find(src.id);
        auto dit = renderTargets_.find(dst.id);
        if (sit == renderTargets_.end() || dit == renderTargets_.end()) return false;
        Target& s = sit->second;
        Target& d = dit->second;
        if (s.samples <= 1 || s.depthOnly || !d.depthOnly) return false;
        if (!s.depthTexId || !d.depthImage || !d.depthView) return false;
        if (s.width != d.width || s.height != d.height) return false;
        if (!depthResolveShader_.Valid() || !resolveQuad_.Valid()) return false;

        static int resolveCount = 0;
        NEON_VK_TRACE("vt: ResolveDepth #%d src=%u dst=%u depthImg=%p tex=%u\\n",
                      ++resolveCount, src.id, dst.id, (void*)s.depthImage, s.depthTexId);
        Frame& f = CurrentFrame();
        Target* prev = target_;
        const int vx = viewportX_;
        const int vy = viewportY_;
        const int vw = viewportWidth_;
        const int vh = viewportHeight_;
        EndRenderPassIfActive(f);
        EnsureFrameStarted();
        BindTarget(&d);
        // BindTarget flushes and ENDS the pending command buffer when the target
        // changes, and the descriptor transition PrepareDraw records (sampling the
        // multisampled depth) would otherwise be recorded into a closed buffer.
        OpenCmd(f);
        SetViewport(0, 0, d.width, d.height);
        // Depth test off / write on: the pass overwrites every destination
        // texel (the quad covers the target), so nothing must reject it.
        SetDepthTest(false, true);
        SetCullMode(CullMode::None);
        SetBlendMode(BlendMode::Opaque);
        UseShader(depthResolveShader_);
        SetUniformMat4("uMVP", math::Mat4::Identity());
        SetUniformInt("uSamples", static_cast<int>(s.samples));
        BindTexture(0, TextureHandle{s.depthTexId});
        SetUniformInt("uDepthMs", 0);
        DrawMesh(resolveQuad_);
        // Sampling the multisampled depth moved it out of the depth-attachment
        // layout the main colour pass declares as its initial layout. Put it back
        // before returning: the TAA velocity pass re-binds the main target for
        // every moving object, and a mid-frame re-bind LOADs the depth buffer -
        // with a mismatched layout the driver may hand back undefined depth and
        // every fragment then fails the depth test (just the sky survived).
        EndRenderPassIfActive(f);
        TransitionImage(f.cmd, s.depthImage, TrackedLayout(s.depthImage),
                        VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
                        VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                        VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT |
                            VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT,
                        VK_ACCESS_SHADER_READ_BIT,
                        VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT |
                            VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
                        DepthAspect());
        SetTrackedLayout(s.depthImage, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL);
        if (prev && prev != &d) BindTarget(prev);
        else if (!prev) BindDefaultTarget();
        SetViewport(vx, vy, vw, vh);
        return true;
    }

    RenderTargetHandle CreateDepthTarget(int width, int height) override {
        Target rt;
        rt.width = width;
        rt.height = height;
        rt.kind = RpKind::DepthOnly;
        rt.depthOnly = true;
        rt.colorFormat = depthFormat_;
        if (!CreateImage(width, height, depthFormat_, 1,
                         VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                         VK_IMAGE_TILING_OPTIMAL, &rt.depthImage, &rt.depthMem, &rt.depthView, 1)) {
            return {};
        }
        rt.rpClear = rt.rpLoad = depthOnlyPass_;
        rt.framebuffer =
            CreateFramebuffer(width, height, depthOnlyPass_, VK_NULL_HANDLE, rt.depthView);
        if (!rt.framebuffer) return {};

        Texture tex;
        tex.image = rt.depthImage;
        tex.view = rt.depthView;
        tex.sampler = samplerNearest_;
        tex.format = depthFormat_;
        tex.width = width;
        tex.height = height;
        tex.mipLevels = 1;
        tex.layout = VK_IMAGE_LAYOUT_UNDEFINED;
        tex.owned = false;
        rt.depthTexId = ++nextTextureId_;
        rt.colorTexId = rt.depthTexId;
        textures_[rt.depthTexId] = tex;

        rt.id = ++nextTargetId_;
        renderTargets_[rt.id] = rt;
        return {rt.id};
    }

    void BeginDepthPass(RenderTargetHandle target) override {
        auto it = renderTargets_.find(target.id);
        if (it == renderTargets_.end()) return;
        BindTarget(&it->second);
        SetViewport(0, 0, it->second.width, it->second.height);
    }

    void EndDepthPass() override { BindDefaultTarget(); }

    void BindShadowMap(int slot, RenderTargetHandle target) override {
        auto it = renderTargets_.find(target.id);
        if (it == renderTargets_.end()) return;
        BindTexture(slot, RenderTargetDepthTexture(target));
    }

    bool ReadCurrentTargetDepth(int, int, float*) override { return false; }


    // ------------------------------------------------------------------
    // Shaders
    // ------------------------------------------------------------------
    ShaderHandle CreateShader(const char* vertexSource, const char* fragmentSource,
                              const char* debugName) override {
        (void)vertexSource;
        (void)fragmentSource;
        if (!debugName) return {};

        const vk::VkShaderSpv* spv = FindSpv(debugName);
        if (!spv) {
            NEON_LOG_CAT(neon::core::LogCategory::Gfx, neon::core::LogLevel::Error,
                         "Vulkan: CreateShader('%s'): no precompiled SPIR-V for this program "
                         "(custom shaders are not supported by the Vulkan backend)",
                         debugName);
            return {};
        }

        Program prog;
        prog.name = debugName;
        prog.vert = CreateModule(spv->vert, spv->vertWords);
        prog.frag = CreateModule(spv->frag, spv->fragWords);
        if (!prog.vert || !prog.frag) {
            if (prog.vert) vkDestroyShaderModule(device_, prog.vert, nullptr);
            if (prog.frag) vkDestroyShaderModule(device_, prog.frag, nullptr);
            return {};
        }
        prog.flipped = IsFlippedProgram(debugName);
        prog.variant = VertexVariantFor(debugName);
        prog.id = ++nextShaderId_;
        programs_[prog.id] = prog;
        return {prog.id};
    }

    void DestroyShader(ShaderHandle shader) override {
        auto it = programs_.find(shader.id);
        if (it == programs_.end()) return;
        if (it->second.vert) vkDestroyShaderModule(device_, it->second.vert, nullptr);
        if (it->second.frag) vkDestroyShaderModule(device_, it->second.frag, nullptr);
        std::vector<PipelineKey> dead;
        for (auto& [key, pipe] : pipelines_) {
            if (key.programId == shader.id) {
                vkDestroyPipeline(device_, pipe, nullptr);
                dead.push_back(key);
            }
        }
        for (const PipelineKey& k : dead) pipelines_.erase(k);
        programs_.erase(it);
        if (currentShader_.id == shader.id) currentShader_ = {};
    }

    // ------------------------------------------------------------------
    // Textures
    // ------------------------------------------------------------------
    TextureHandle CreateTexture(const TextureDesc& desc) override {
        if (!desc.rgba || desc.width <= 0 || desc.height <= 0) return {};

        const VkFormat format = VK_FORMAT_R8G8B8A8_UNORM;
        const int levels = desc.mipmaps ? MipLevels(desc.width, desc.height) : 1;

        VkImage image;
        VkDeviceMemory mem;
        if (!CreateImage(desc.width, desc.height, format, 1,
                         VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                             VK_IMAGE_USAGE_SAMPLED_BIT,
                         VK_IMAGE_TILING_OPTIMAL, &image, &mem, nullptr, levels)) {
            return {};
        }
        VkImageView view =
            CreateView(image, format, VK_IMAGE_VIEW_TYPE_2D, VK_IMAGE_ASPECT_COLOR_BIT, levels);
        if (!view) {
            vkDestroyImage(device_, image, nullptr);
            vkFreeMemory(device_, mem, nullptr);
            return {};
        }

        const size_t dataSize = static_cast<size_t>(desc.width) * desc.height * 4;
        VkBuffer staging;
        VkDeviceMemory stagingMem;
        if (!CreateHostBuffer(dataSize, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, &staging, &stagingMem)) {
            vkDestroyImageView(device_, view, nullptr);
            vkDestroyImage(device_, image, nullptr);
            vkFreeMemory(device_, mem, nullptr);
            return {};
        }
        void* mapped = nullptr;
        vkMapMemory(device_, stagingMem, 0, dataSize, 0, &mapped);
        std::memcpy(mapped, desc.rgba, dataSize);
        vkUnmapMemory(device_, stagingMem);

        VkCommandPool pool;
        CreateCommandPool(&pool);
        VkCommandBuffer cmd = BeginOneShot(pool);
        TransitionImage(cmd, image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                        VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0,
                        VK_ACCESS_TRANSFER_WRITE_BIT, VK_IMAGE_ASPECT_COLOR_BIT);
        VkBufferImageCopy region{};
        region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        region.imageExtent = {static_cast<uint32_t>(desc.width),
                              static_cast<uint32_t>(desc.height), 1};
        vkCmdCopyBufferToImage(cmd, staging, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1,
                               &region);
        if (levels > 1) GenerateMips(cmd, image, format, desc.width, desc.height, levels);
        TransitionImage(cmd, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                        VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_TRANSFER_BIT,
                        VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT,
                        VK_ACCESS_SHADER_READ_BIT, VK_IMAGE_ASPECT_COLOR_BIT);
        vkEndCommandBuffer(cmd);
        SubmitQueue(CurrentFrame(), cmd);
        vkFreeCommandBuffers(device_, pool, 1, &cmd);
        vkDestroyCommandPool(device_, pool, nullptr);
        vkDestroyBuffer(device_, staging, nullptr);
        vkFreeMemory(device_, stagingMem, nullptr);

        Texture tex;
        tex.image = image;
        tex.view = view;
        if (desc.wrap == Wrap::Repeat)
            tex.sampler = desc.filter == Filter::Nearest ? samplerRepeatNearest_
                                                         : samplerRepeatLinear_;
        else
            tex.sampler = desc.filter == Filter::Nearest ? samplerNearest_ : samplerLinear_;
        tex.format = format;
        tex.width = desc.width;
        tex.height = desc.height;
        tex.mipLevels = levels;
        tex.layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        tex.owned = true;
        textures_[++nextTextureId_] = tex;
        return {nextTextureId_};
    }

    void DestroyTexture(TextureHandle texture) override {
        FlushPendingWorkBeforeDestroy(nullptr);
        auto it = textures_.find(texture.id);
        if (it == textures_.end()) return;
        DestroyTextureInternal(it->second);
        textures_.erase(it);
    }

    // Dynamic font atlases are GL-path only today; Vulkan keeps the texture
    // as created (dynamic glyph baking falls back to a static atlas).
    // A2: dynamic texture region update (font glyph atlas). Stages through
    // the frame's host-visible scratch buffer and copies into the sub-rectangle.
    void UpdateTextureRegion(TextureHandle handle, int x, int y, int width, int height,
                             const void* data) override {
        auto it = textures_.find(handle.id);
        if (it == textures_.end() || !data || width <= 0 || height <= 0) return;
        Texture& tex = it->second;
        if (tex.format != VK_FORMAT_R8G8B8A8_UNORM || !tex.owned) return; // RGBA8 uploads only
        const VkDeviceSize bytes = static_cast<VkDeviceSize>(width) * height * 4;

        Frame& f = CurrentFrame();
        EnsureFrameStarted();
        OpenCmd(f);
        uint64_t off = 0;
        if (!ScratchAlloc(f, static_cast<size_t>(bytes), 16, &off)) return;
        std::memcpy(f.scratchPtr + off, data, static_cast<size_t>(bytes));

        const VkImageAspectFlags aspect = VK_IMAGE_ASPECT_COLOR_BIT;
        const VkImageLayout from = TrackedLayout(tex.image);
        TransitionImage(f.cmd, tex.image, from, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                        VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                        VK_ACCESS_SHADER_READ_BIT, VK_ACCESS_TRANSFER_WRITE_BIT, aspect);
        VkBufferImageCopy region{};
        region.bufferOffset = off;
        region.bufferRowLength = 0;
        region.bufferImageHeight = 0;
        region.imageSubresource = {aspect, 0, 0, 1};
        region.imageOffset = {x, y, 0};
        region.imageExtent = {static_cast<uint32_t>(width), static_cast<uint32_t>(height), 1};
        vkCmdCopyBufferToImage(f.cmd, f.scratch, tex.image,
                               VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
        const VkImageLayout back =
            tex.owned ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL : VK_IMAGE_LAYOUT_GENERAL;
        TransitionImage(f.cmd, tex.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, back,
                        VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                        VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT, aspect);
        SetTrackedLayout(tex.image, back);
    }

    TextureHandle CreateTextureCompressed(int width, int height, uint32_t format, const void* data,
                                          size_t size) override {
        if (!data || width <= 0 || height <= 0 || size == 0) return {};
        if (format != 0x83F1u) {
            return {};
        }
        if (!compressedTexSupported_) return {};
        const int bw = (width + 3) & ~3;
        const int bh = (height + 3) & ~3;

        VkImage image;
        VkDeviceMemory mem;
        if (!CreateImage(bw, bh, VK_FORMAT_BC1_RGBA_UNORM_BLOCK, 1,
                         VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                         VK_IMAGE_TILING_OPTIMAL, &image, &mem, nullptr, 1)) {
            return {};
        }
        VkImageView view = CreateView(image, VK_FORMAT_BC1_RGBA_UNORM_BLOCK, VK_IMAGE_VIEW_TYPE_2D,
                                      VK_IMAGE_ASPECT_COLOR_BIT);
        if (!view) {
            vkDestroyImage(device_, image, nullptr);
            vkFreeMemory(device_, mem, nullptr);
            return {};
        }

        VkBuffer staging;
        VkDeviceMemory stagingMem;
        if (!CreateHostBuffer(size, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, &staging, &stagingMem)) {
            return {};
        }
        void* mapped = nullptr;
        vkMapMemory(device_, stagingMem, 0, size, 0, &mapped);
        std::memcpy(mapped, data, size);
        vkUnmapMemory(device_, stagingMem);

        VkCommandPool pool;
        CreateCommandPool(&pool);
        VkCommandBuffer cmd = BeginOneShot(pool);
        TransitionImage(cmd, image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                        VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0,
                        VK_ACCESS_TRANSFER_WRITE_BIT, VK_IMAGE_ASPECT_COLOR_BIT);
        VkBufferImageCopy region{};
        region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        region.imageExtent = {static_cast<uint32_t>(bw), static_cast<uint32_t>(bh), 1};
        vkCmdCopyBufferToImage(cmd, staging, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1,
                               &region);
        TransitionImage(cmd, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                        VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_TRANSFER_BIT,
                        VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT,
                        VK_ACCESS_SHADER_READ_BIT, VK_IMAGE_ASPECT_COLOR_BIT);
        vkEndCommandBuffer(cmd);
        SubmitQueue(CurrentFrame(), cmd);
        vkFreeCommandBuffers(device_, pool, 1, &cmd);
        vkDestroyCommandPool(device_, pool, nullptr);
        vkDestroyBuffer(device_, staging, nullptr);
        vkFreeMemory(device_, stagingMem, nullptr);

        Texture tex;
        tex.image = image;
        tex.view = view;
        tex.sampler = samplerLinear_;
        tex.format = VK_FORMAT_BC1_RGBA_UNORM_BLOCK;
        tex.width = width;
        tex.height = height;
        tex.mipLevels = 1;
        tex.layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        tex.owned = true;
        textures_[++nextTextureId_] = tex;
        return {nextTextureId_};
    }

    // ------------------------------------------------------------------
    // Meshes
    // ------------------------------------------------------------------
    MeshHandle CreateMesh(const void* vertices, uint32_t vertexCount, const uint16_t* indices,
                          uint32_t indexCount) override {
        if (!vertices || vertexCount == 0) return {};
        Mesh m;
        m.vertexCount = vertexCount;
        m.indexCount = indexCount;

        const size_t vbytes = static_cast<size_t>(vertexCount) * sizeof(Vertex3D);
        if (!CreateDeviceBuffer(vbytes, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, vertices, &m.vbo,
                                &m.vboMem)) {
            return {};
        }
        if (indices && indexCount > 0) {
            const size_t ibytes = static_cast<size_t>(indexCount) * sizeof(uint16_t);
            if (!CreateDeviceBuffer(ibytes, VK_BUFFER_USAGE_INDEX_BUFFER_BIT, indices, &m.ibo,
                                    &m.iboMem)) {
                vkDestroyBuffer(device_, m.vbo, nullptr);
                vkFreeMemory(device_, m.vboMem, nullptr);
                return {};
            }
        }
        meshes_[++nextMeshId_] = m;
        return {nextMeshId_, nextMeshId_, nextMeshId_, m.indexCount, m.indexType};
    }

    MeshHandle CreateMeshU32(const void* vertices, uint32_t vertexCount,
                             const uint32_t* indices, uint32_t indexCount) override {
        // 32-bit index path for meshes with more than 65535 vertices (large
        // merged glTF / FBX geometry). Without it those meshes failed to create
        // and their draws silently disappeared (the Summoner Rift map border
        // was the visible symptom).
        if (!vertices || vertexCount == 0) return {};
        Mesh m;
        m.vertexCount = vertexCount;
        m.indexCount = indexCount;
        m.indexType = 1;

        const size_t vbytes = static_cast<size_t>(vertexCount) * sizeof(Vertex3D);
        if (!CreateDeviceBuffer(vbytes, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, vertices, &m.vbo,
                                &m.vboMem)) {
            return {};
        }
        if (indices && indexCount > 0) {
            const size_t ibytes = static_cast<size_t>(indexCount) * sizeof(uint32_t);
            if (!CreateDeviceBuffer(ibytes, VK_BUFFER_USAGE_INDEX_BUFFER_BIT, indices, &m.ibo,
                                    &m.iboMem)) {
                vkDestroyBuffer(device_, m.vbo, nullptr);
                vkFreeMemory(device_, m.vboMem, nullptr);
                return {};
            }
        }
        meshes_[++nextMeshId_] = m;
        return {nextMeshId_, nextMeshId_, nextMeshId_, m.indexCount, m.indexType};
    }

    void DestroyMesh(const MeshHandle& mesh) override {
        FlushPendingWorkBeforeDestroy(nullptr);
        auto it = meshes_.find(mesh.vao);
        if (it == meshes_.end()) return;
        if (it->second.vbo) vkDestroyBuffer(device_, it->second.vbo, nullptr);
        if (it->second.vboMem) vkFreeMemory(device_, it->second.vboMem, nullptr);
        if (it->second.ibo) vkDestroyBuffer(device_, it->second.ibo, nullptr);
        if (it->second.iboMem) vkFreeMemory(device_, it->second.iboMem, nullptr);
        meshes_.erase(it);
    }

    void UpdateMeshVertices(const MeshHandle& mesh, const void* vertices,
                            uint32_t vertexCount) override {
        if (!vertices || vertexCount == 0) return;
        auto it = meshes_.find(mesh.vao);
        if (it == meshes_.end()) return;
        const size_t vbytes = static_cast<size_t>(vertexCount) * sizeof(Vertex3D);
        VkBuffer vbo;
        VkDeviceMemory mem;
        if (!CreateDeviceBuffer(vbytes, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, vertices, &vbo, &mem)) {
            return;
        }
        vkDeviceWaitIdle(device_);
        if (it->second.vbo) vkDestroyBuffer(device_, it->second.vbo, nullptr);
        if (it->second.vboMem) vkFreeMemory(device_, it->second.vboMem, nullptr);
        it->second.vbo = vbo;
        it->second.vboMem = mem;
        it->second.vertexCount = vertexCount;
    }

    // ------------------------------------------------------------------
    // State
    // ------------------------------------------------------------------
    void SetBlendMode(BlendMode mode) override {
        switch (mode) {
            case BlendMode::Opaque: currentBlend_ = BlendState::Opaque; break;
            case BlendMode::Alpha: currentBlend_ = BlendState::Alpha; break;
            case BlendMode::Additive: currentBlend_ = BlendState::Additive; break;
            case BlendMode::Premultiplied: currentBlend_ = BlendState::Premultiplied; break;
        }
    }

    void SetDepthTest(bool enabled, bool write) override {
        currentDepthTest_ = enabled;
        currentDepthWrite_ = write;
    }

    void SetCullMode(CullMode mode) override {
        currentCull_ = mode == CullMode::Back ? 1 : (mode == CullMode::Front ? 2 : 0);
    }

    void SetViewport(int x, int y, int width, int height) override {
        viewportX_ = x;
        viewportY_ = y;
        viewportWidth_ = width;
        viewportHeight_ = height;
    }

    void SetScissor(int x, int y, int width, int height, bool enabled) override {
        scissorEnabled_ = enabled;
        scissorX_ = x;
        scissorY_ = y;
        scissorW_ = width;
        scissorH_ = height;
    }

    void Clear(const Color& color, float depth) override {
        NEON_VK_TRACE("vt: Clear\n");
        Frame& f = CurrentFrame();
        EndRenderPassIfActive(f);
        EnsureFrameStarted();
        if (!target_) return; // nothing bound (GL would clear the backbuffer)
        target_->clearPending = true;
        target_->pendingClearColor = {color.r, color.g, color.b, color.a};
        target_->pendingClearDepth = depth;
    }

    // ------------------------------------------------------------------
    // Uniforms
    // ------------------------------------------------------------------
    void UseShader(ShaderHandle shader) override {
        currentShader_ = shader;
        currentProgramId_ = shader.id;
    }

    void SetUniformMat4(const char* name, const math::Mat4& value) override {
        const UniEntry* e = FindUniform(name);
        if (!e || e->kind != UniKind::Mat4) return;
        TransposeMat4(reinterpret_cast<float*>(uniforms_ + e->offset), value.Data());
        uniformsDirty_ = true;
    }

    void SetUniformMat4Array(const char* name, const float* values, int count) override {
        const UniEntry* e = FindUniform(name);
        if (!e || e->kind != UniKind::Mat4Arr || !values || count <= 0) return;
        const int n = std::min(count, e->count);
        float* dst = reinterpret_cast<float*>(uniforms_ + e->offset);
        for (int i = 0; i < n; ++i) TransposeMat4(dst + i * 16, values + i * 16);
        uniformsDirty_ = true;
    }

    void SetUniformVec4(const char* name, const math::Vec4& value) override {
        const UniEntry* e = FindUniform(name);
        if (!e || e->kind != UniKind::Vec4) return;
        std::memcpy(uniforms_ + e->offset, &value, 16);
        uniformsDirty_ = true;
    }

    void SetUniformVec3(const char* name, const math::Vec3& value) override {
        const UniEntry* e = FindUniform(name);
        // A vec3 write into a vec4 slot is allowed (the composite sets the
        // colour-grade tint while the lit path uses the same slot as a vec4).
        if (!e || (e->kind != UniKind::Vec3 && e->kind != UniKind::Vec4)) return;
        float* dst = reinterpret_cast<float*>(uniforms_ + ElementOffset(e, name));
        dst[0] = value.x;
        dst[1] = value.y;
        dst[2] = value.z;
        uniformsDirty_ = true;
    }

    void SetUniformVec2(const char* name, const math::Vec2& value) override {
        const UniEntry* e = FindUniform(name);
        if (!e || e->kind != UniKind::Vec2) return;
        std::memcpy(uniforms_ + e->offset, &value, 8);
        uniformsDirty_ = true;
    }

    void SetUniformFloat(const char* name, float value) override {
        const UniEntry* e = FindUniform(name);
        if (!e || (e->kind != UniKind::Float && e->kind != UniKind::Num)) return;
        float v = value;
        std::memcpy(uniforms_ + ElementOffset(e, name), &v, 4);
        uniformsDirty_ = true;
    }

    void SetUniformInt(const char* name, int value) override {
        const UniEntry* e = FindUniform(name);
        if (!e) return;
        if (e->kind == UniKind::Num) {
            const float v = static_cast<float>(value);
            std::memcpy(uniforms_ + e->offset, &v, 4);
            uniformsDirty_ = true;
            return;
        }
        if (e->kind != UniKind::Int) return;
        std::memcpy(uniforms_ + e->offset, &value, 4);
        uniformsDirty_ = true;
    }

    void BindTexture(int slot, TextureHandle texture) override {
        if (slot < 0 || slot >= static_cast<int>(kMaxSamplerSlots)) return;
        boundTextures_[slot] = texture.id;
        texSetsDirty_ = true;
    }

    // ------------------------------------------------------------------
    // Drawing
    // ------------------------------------------------------------------
    void DrawMesh(const MeshHandle& mesh) override {
        NEON_VK_TRACE("vt: DrawMesh vao=%u\n", mesh.vao);
        auto it = meshes_.find(mesh.vao);
        if (it == meshes_.end()) {
            // A silently dropped draw is invisible in a screenshot (and cost a
            // full afternoon of shadow debugging on the GL path).
            NEON_LOG_CAT(neon::core::LogCategory::Gfx, neon::core::LogLevel::Warn,
                         "Vulkan: DrawMesh with unknown mesh handle vao=%u", mesh.vao);
            return;
        }
        DrawIndexed(it->second, 1, 0);
    }

    void DrawMeshInstanced(const MeshHandle& mesh, const math::Mat4* models,
                           uint32_t count) override {
        auto it = meshes_.find(mesh.vao);
        if (it == meshes_.end() || !models || count == 0) return;

        const size_t bytes = static_cast<size_t>(count) * 64;
        Frame& f = CurrentFrame();
        uint64_t offset = 0;
        if (!ScratchAlloc(f, bytes, 16, &offset)) return;
        for (uint32_t i = 0; i < count; ++i) {
            TransposeMat4(reinterpret_cast<float*>(f.scratchPtr + offset + i * 64),
                          models[i].Data());
        }
        DrawIndexed(it->second, count, offset);
    }

    // A2/G1-5: instanced draw with per-instance RGBA. Matrices go to binding 1
    // (transposed), colors to binding 2 (raw vec4s).
    void DrawMeshInstancedColored(const MeshHandle& mesh, const math::Mat4* models,
                                  const math::Vec4* colors, uint32_t count) override {
        auto it = meshes_.find(mesh.vao);
        if (it == meshes_.end() || !models || !colors || count == 0) return;

        const size_t matBytes = static_cast<size_t>(count) * 64;
        const size_t colBytes = static_cast<size_t>(count) * 16;
        Frame& f = CurrentFrame();
        uint64_t matOffset = 0, colOffset = 0;
        if (!ScratchAlloc(f, matBytes, 16, &matOffset)) return;
        if (!ScratchAlloc(f, colBytes, 16, &colOffset)) return;
        for (uint32_t i = 0; i < count; ++i) {
            TransposeMat4(reinterpret_cast<float*>(f.scratchPtr + matOffset + i * 64),
                          models[i].Data());
            std::memcpy(f.scratchPtr + colOffset + i * 16, &colors[i], 16);
        }
        DrawIndexed(it->second, count, matOffset, colOffset);
    }

    void DrawPrimitives(const void* vertices, uint32_t vertexCount, uint32_t stride,
                        const uint16_t* indices, uint32_t indexCount,
                        PrimitiveTopology topology) override {
        if (!vertices || vertexCount == 0) return;
        Frame& f = CurrentFrame();

        const bool useIndexed = indices && indexCount > 0;
        const size_t vbytes = static_cast<size_t>(vertexCount) * stride;
        uint64_t voffset = 0;
        if (!ScratchAlloc(f, vbytes, 16, &voffset)) return;
        std::memcpy(f.scratchPtr + voffset, vertices, vbytes);

        uint64_t ioffset = 0;
        if (useIndexed) {
            const size_t ibytes = static_cast<size_t>(indexCount) * 2;
            if (!ScratchAlloc(f, ibytes, 2, &ioffset)) return;
            std::memcpy(f.scratchPtr + ioffset, indices, ibytes);
        }

        if (!currentProgramId_) return;
        Program* prog = GetProgram(currentProgramId_);
        if (!prog) return;
        if (!PrepareDraw(f)) return;
        EnsureRenderPass(f);
        VkPipeline pipeline = GetPipeline(prog);
        if (!pipeline) return;
        BindPipeline(f, pipeline);

        VkBuffer buffers[1] = {f.scratch};
        VkDeviceSize offsets[1] = {voffset};
        vkCmdBindVertexBuffers(f.cmd, 0, 1, buffers, offsets);
        if (useIndexed) {
            vkCmdBindIndexBuffer(f.cmd, f.scratch, ioffset, VK_INDEX_TYPE_UINT16);
            vkCmdDrawIndexed(f.cmd, indexCount, 1, 0, 0, 0);
        } else {
            vkCmdDraw(f.cmd, vertexCount, 1, 0, 0);
        }
        (void)topology;
    }

    // ------------------------------------------------------------------
    // Frame
    // ------------------------------------------------------------------
    void BeginFrame() override {}

    void EndFrame() override {
        Frame& f = CurrentFrame();
        EndRenderPassIfActive(f);
        inFrame_ = false;
        frameReady_ = false;
        if (!acquired_ && swapchain_) AcquireSwapchainImage(f);
        acquired_ = false;

        VkImage swapImage = imageIndex_ < swapImages_.size() ? swapImages_[imageIndex_]
                                                             : VK_NULL_HANDLE;
        // Re-open the frame command buffer instead of testing f.cmdOpen: a
        // mid-frame readback (screenshot capture, ReadImage) ends and SUBMITS
        // f.cmd, and the old guard then skipped the PRESENT_SRC transition below.
        // Presenting an image that is still in GENERAL is undefined, and the
        // driver answered it with VK_ERROR_DEVICE_LOST on every later submit.
        // Resetting an already-submitted ONE_TIME buffer is legal here because
        // SubmitQueue waited on its fence before returning.
        OpenCmd(f);
        if (swapImage && swapImageLayout_ != VK_IMAGE_LAYOUT_PRESENT_SRC_KHR) {
            TransitionImage(f.cmd, swapImage, swapImageLayout_, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
                            VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                            VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
                            VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, 0, VK_IMAGE_ASPECT_COLOR_BIT);
            swapImageLayout_ = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
        }
        if (f.cmdOpen) {
            vkEndCommandBuffer(f.cmd);
            f.cmdOpen = false;
        }

        VkSubmitInfo si{};
        si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        VkSemaphore waitSems[2] = {VK_NULL_HANDLE, VK_NULL_HANDLE};
        VkPipelineStageFlags waitStages[2] = {VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                                              VK_PIPELINE_STAGE_ALL_COMMANDS_BIT};
        uint32_t waitCount = 0;
        if (f.progressSignaled) waitSems[waitCount++] = f.progress;
        if (!f.acquiredWaited && f.imageAvailable) waitSems[waitCount++] = f.imageAvailable;
        if (waitCount > 0) {
            si.waitSemaphoreCount = waitCount;
            si.pWaitSemaphores = waitSems;
            si.pWaitDstStageMask = waitStages;
            f.acquiredWaited = true;
        }
        if (f.cmd) {
            si.commandBufferCount = 1;
            si.pCommandBuffers = &f.cmd;
        }
        si.signalSemaphoreCount = 1;
        si.pSignalSemaphores = &f.renderFinished;
        const VkResult submitRes = vkQueueSubmit(queue_, 1, &si, f.fence);
        if (submitRes != VK_SUCCESS) {
            NEON_LOG_CAT(neon::core::LogCategory::Gfx, neon::core::LogLevel::Error,
                         "Vulkan: EndFrame queue submit failed: %s (%d)",
                         VkResultName(submitRes), static_cast<int>(submitRes));
        }

        if (swapImage) {
            VkPresentInfoKHR pi{};
            pi.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
            pi.waitSemaphoreCount = 1;
            pi.pWaitSemaphores = &f.renderFinished;
            pi.swapchainCount = 1;
            pi.pSwapchains = &swapchain_;
            pi.pImageIndices = &imageIndex_;
            VkResult pr = vkQueuePresentKHR(queue_, &pi);
            if (pr == VK_ERROR_OUT_OF_DATE_KHR || pr == VK_SUBOPTIMAL_KHR) {
                RecreateSwapchain();
            }
        }
    }

    void CaptureFrame(int width, int height, void* rgba) override {
        if (!rgba || width <= 0 || height <= 0 || imageIndex_ >= swapImages_.size()) return;
        AcquireIfNeeded();
        ReadImage(swapImages_[imageIndex_], swapImageLayout_, swapchainFormat_, width, height, rgba,
                  0, 0, true);
    }

    void ReadCurrentTargetPixel(int x, int y, unsigned char* rgba) override {
        NEON_VK_TRACE("vt: ReadCurrentTargetPixel %d,%d target=%p\n", x, y, (void*)target_);
        if (!rgba) return;
        if (target_ && !target_->swapchain && target_->colorImage) {
            ReadImage(target_->colorImage, TrackedLayout(target_->colorImage),
                      target_->colorFormat, 1, 1, rgba, x, y);
        } else if (target_ && target_->swapchain && imageIndex_ < swapImages_.size()) {
            AcquireIfNeeded();
            ReadImage(swapImages_[imageIndex_], swapImageLayout_, swapchainFormat_, 1, 1, rgba, x,
                      y);
        }
    }

    bool DepthAvailable() const override { return depthUsable_; }


private:
    // ------------------------------------------------------------------
    // Init helpers
    // ------------------------------------------------------------------
    bool CreateInstance() {
        uint32_t extCount = 0;
        vkEnumerateInstanceExtensionProperties(nullptr, &extCount, nullptr);
        std::vector<VkExtensionProperties> exts(extCount);
        vkEnumerateInstanceExtensionProperties(nullptr, &extCount, exts.data());

        bool hasSurface = false;
        bool hasWin32 = false;
        for (const auto& e : exts) {
            if (std::strcmp(e.extensionName, VK_KHR_SURFACE_EXTENSION_NAME) == 0) hasSurface = true;
            if (std::strcmp(e.extensionName, VK_KHR_WIN32_SURFACE_EXTENSION_NAME) == 0)
                hasWin32 = true;
        }
        if (!hasSurface || !hasWin32) {
            NEON_LOG_CAT(neon::core::LogCategory::Gfx, neon::core::LogLevel::Error,
                         "Vulkan: instance extensions VK_KHR_surface/win32_surface missing");
            return false;
        }

        VkApplicationInfo app{};
        app.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
        app.pApplicationName = "NeonEngine";
        app.applicationVersion = VK_MAKE_VERSION(0, 1, 0);
        app.pEngineName = "NeonEngine";
        app.engineVersion = VK_MAKE_VERSION(0, 1, 0);
        app.apiVersion = VK_API_VERSION_1_1;

        const char* enabledExts[] = {VK_KHR_SURFACE_EXTENSION_NAME,
                                     VK_KHR_WIN32_SURFACE_EXTENSION_NAME};
        VkInstanceCreateInfo ci{};
        ci.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
        ci.pApplicationInfo = &app;
        ci.enabledExtensionCount = 2;
        ci.ppEnabledExtensionNames = enabledExts;

        VkResult r = vkCreateInstance(&ci, nullptr, &instance_);
        if (r != VK_SUCCESS) {
            NEON_LOG_CAT(neon::core::LogCategory::Gfx, neon::core::LogLevel::Error,
                         "Vulkan: vkCreateInstance failed (%s)", VkResultName(r));
            return false;
        }
        volkLoadInstance(instance_);
        return true;
    }

    bool CreateDevice() {
        VkWin32SurfaceCreateInfoKHR sci{};
        sci.sType = VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR;
        sci.hwnd = hwnd_;
        sci.hinstance = GetModuleHandle(nullptr);
        if (vkCreateWin32SurfaceKHR(instance_, &sci, nullptr, &surface_) != VK_SUCCESS) {
            NEON_LOG_CAT(neon::core::LogCategory::Gfx, neon::core::LogLevel::Error,
                         "Vulkan: vkCreateWin32SurfaceKHR failed");
            return false;
        }

        uint32_t deviceCount = 0;
        vkEnumeratePhysicalDevices(instance_, &deviceCount, nullptr);
        if (deviceCount == 0) {
            NEON_LOG_CAT(neon::core::LogCategory::Gfx, neon::core::LogLevel::Error,
                         "Vulkan: no physical devices");
            return false;
        }
        std::vector<VkPhysicalDevice> devices(deviceCount);
        vkEnumeratePhysicalDevices(instance_, &deviceCount, devices.data());

        // Pick a device that can actually present to this window: devices[0] is
        // often a software adapter or a display-less GPU whose surface support
        // cannot back a swapchain for this HWND.
        int bestScore = -1;
        for (uint32_t i = 0; i < deviceCount; ++i) {
            VkPhysicalDeviceProperties props{};
            vkGetPhysicalDeviceProperties(devices[i], &props);
            char why[96] = {0};
            const int score = DevicePresentScore(devices[i], surface_, why, sizeof(why));
            NEON_LOG_CAT(neon::core::LogCategory::Gfx, neon::core::LogLevel::Info,
                         "Vulkan: device[%u] \"%s\" type=%d -> %s", i, props.deviceName,
                         static_cast<int>(props.deviceType), why);
            if (score < 0) continue;
            if (score > bestScore) {
                bestScore = score;
                physicalDevice_ = devices[i];
            }
        }
        if (physicalDevice_ == VK_NULL_HANDLE) {
            NEON_LOG_CAT(neon::core::LogCategory::Gfx, neon::core::LogLevel::Warn,
                         "Vulkan: no device advertised swapchain support; falling back to devices[0]");
            physicalDevice_ = devices[0];
        }
        if (!DeviceHasExtension(physicalDevice_, VK_KHR_SWAPCHAIN_EXTENSION_NAME)) {
            NEON_LOG_CAT(neon::core::LogCategory::Gfx, neon::core::LogLevel::Error,
                         "Vulkan: selected device does not support VK_KHR_swapchain");
            return false;
        }

        queueFamily_ = FindGraphicsPresentQueueFamily(physicalDevice_, surface_);
        if (queueFamily_ == UINT32_MAX) {
            NEON_LOG_CAT(neon::core::LogCategory::Gfx, neon::core::LogLevel::Error,
                         "Vulkan: no queue family with graphics+present support");
            return false;
        }

        const float priority = 1.0f;
        VkDeviceQueueCreateInfo qci{};
        qci.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
        qci.queueFamilyIndex = queueFamily_;
        qci.queueCount = 1;
        qci.pQueuePriorities = &priority;

        VkPhysicalDeviceFeatures features{};
        features.fillModeNonSolid = VK_TRUE;
        features.imageCubeArray = VK_TRUE;

        const char* deviceExts[] = {VK_KHR_SWAPCHAIN_EXTENSION_NAME};
        VkDeviceCreateInfo dci{};
        dci.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
        dci.queueCreateInfoCount = 1;
        dci.pQueueCreateInfos = &qci;
        dci.pEnabledFeatures = &features;
        dci.enabledExtensionCount = 1;
        dci.ppEnabledExtensionNames = deviceExts;

        const VkResult devRes = vkCreateDevice(physicalDevice_, &dci, nullptr, &device_);
        if (devRes != VK_SUCCESS) {
            NEON_LOG_CAT(neon::core::LogCategory::Gfx, neon::core::LogLevel::Error,
                         "Vulkan: vkCreateDevice failed (%s)", VkResultName(devRes));
            return false;
        }
        volkLoadDevice(device_);
        vkGetDeviceQueue(device_, queueFamily_, 0, &queue_);

        VkPhysicalDeviceProperties props;
        vkGetPhysicalDeviceProperties(physicalDevice_, &props);
        minUboAlignment_ = std::max<uint32_t>(props.limits.minUniformBufferOffsetAlignment, 16u);
        maxSampleCounts_ = props.limits.framebufferColorSampleCounts;
        NEON_LOG_CAT(neon::core::LogCategory::Gfx, neon::core::LogLevel::Info,
                     "Vulkan: using \"%s\" apiVersion=%u.%u.%u", props.deviceName,
                     VK_VERSION_MAJOR(props.apiVersion), VK_VERSION_MINOR(props.apiVersion),
                     VK_VERSION_PATCH(props.apiVersion));
        return true;
    }


    bool CreateSwapchain() { return RecreateSwapchain(); }

    bool RecreateSwapchain() {
        vkDeviceWaitIdle(device_);
        if (swapchain_) DestroySwapchain();

        VkSurfaceCapabilitiesKHR caps{};
        const VkResult capsRes =
            vkGetPhysicalDeviceSurfaceCapabilitiesKHR(physicalDevice_, surface_, &caps);
        uint32_t formatCount = 0;
        vkGetPhysicalDeviceSurfaceFormatsKHR(physicalDevice_, surface_, &formatCount, nullptr);
        std::vector<VkSurfaceFormatKHR> formats(formatCount);
        if (formatCount)
            vkGetPhysicalDeviceSurfaceFormatsKHR(physicalDevice_, surface_, &formatCount,
                                                 formats.data());
        uint32_t modeCount = 0;
        vkGetPhysicalDeviceSurfacePresentModesKHR(physicalDevice_, surface_, &modeCount, nullptr);
        std::vector<VkPresentModeKHR> modes(modeCount);
        if (modeCount)
            vkGetPhysicalDeviceSurfacePresentModesKHR(physicalDevice_, surface_, &modeCount,
                                                      modes.data());

        if (capsRes != VK_SUCCESS || formats.empty() || modes.empty()) {
            NEON_LOG_CAT(neon::core::LogCategory::Gfx, neon::core::LogLevel::Error,
                         "Vulkan: surface is not swapchain-capable (caps=%s formats=%u modes=%u)",
                         VkResultName(capsRes), formatCount, modeCount);
            return false;
        }

        // Present mode is negotiated below: some drivers advertise MAILBOX/IMMEDIATE but
        // reject them at swapchain creation time (seen on RTX 20xx drivers, which return
        // VK_ERROR_NATIVE_WINDOW_IN_USE_KHR), so FIFO stays as the guaranteed fallback.
        VkPresentModeKHR presentMode = VK_PRESENT_MODE_FIFO_KHR;

        uint32_t w = caps.currentExtent.width != UINT32_MAX
                         ? caps.currentExtent.width
                         : static_cast<uint32_t>(window_->Width());
        uint32_t h = caps.currentExtent.height != UINT32_MAX
                         ? caps.currentExtent.height
                         : static_cast<uint32_t>(window_->Height());
        w = std::clamp(w, caps.minImageExtent.width, caps.maxImageExtent.width);
        h = std::clamp(h, caps.minImageExtent.height, caps.maxImageExtent.height);

        uint32_t imageCount = caps.minImageCount + 1;
        if (caps.maxImageCount > 0 && imageCount > caps.maxImageCount)
            imageCount = caps.maxImageCount;
        imageCount = std::max(imageCount, 2u);

        VkCompositeAlphaFlagBitsKHR composite = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
        if (!(caps.supportedCompositeAlpha & composite)) {
            for (uint32_t bit = 1; bit <= VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR; bit <<= 1) {
                if (caps.supportedCompositeAlpha & bit) {
                    composite = static_cast<VkCompositeAlphaFlagBitsKHR>(bit);
                    break;
                }
            }
        }
        VkImageUsageFlags usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
        if (caps.supportedUsageFlags & VK_IMAGE_USAGE_TRANSFER_SRC_BIT)
            usage |= VK_IMAGE_USAGE_TRANSFER_SRC_BIT;

        // Preferred formats first, then everything the surface actually advertises.
        std::vector<VkFormat> candidates(
            kSwapchainFormats,
            kSwapchainFormats + sizeof(kSwapchainFormats) / sizeof(kSwapchainFormats[0]));
        for (const VkSurfaceFormatKHR& f : formats) {
            if (std::find(candidates.begin(), candidates.end(), f.format) == candidates.end())
                candidates.push_back(f.format);
        }

        std::vector<VkPresentModeKHR> presentCandidates;
        for (VkPresentModeKHR m : modes) {
            if (m == VK_PRESENT_MODE_MAILBOX_KHR) presentCandidates.push_back(m);
        }
        for (VkPresentModeKHR m : modes) {
            if (m == VK_PRESENT_MODE_FIFO_RELAXED_KHR) presentCandidates.push_back(m);
        }
        presentCandidates.push_back(VK_PRESENT_MODE_FIFO_KHR);

        VkResult scRes = VK_ERROR_INITIALIZATION_FAILED;
        for (VkPresentModeKHR mode : presentCandidates) {
            for (VkFormat fmt : candidates) {
                VkSwapchainCreateInfoKHR sci{};
                sci.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR;
                sci.surface = surface_;
                sci.minImageCount = imageCount;
                sci.imageFormat = fmt;
                sci.imageColorSpace = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
                sci.imageExtent = {w, h};
                sci.imageArrayLayers = 1;
                sci.imageUsage = usage;
                sci.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
                sci.preTransform = caps.currentTransform;
                sci.compositeAlpha = composite;
                sci.presentMode = mode;
                sci.clipped = VK_TRUE;

                scRes = vkCreateSwapchainKHR(device_, &sci, nullptr, &swapchain_);
                if (scRes == VK_SUCCESS) {
                    swapchainFormat_ = fmt;
                    presentMode = mode;
                    break;
                }
                NEON_LOG_CAT(neon::core::LogCategory::Gfx, neon::core::LogLevel::Warn,
                             "Vulkan: swapchain present=%d format=%d rejected (%s)",
                             static_cast<int>(mode), static_cast<int>(fmt), VkResultName(scRes));
            }
            if (scRes == VK_SUCCESS) break;
            if (mode != VK_PRESENT_MODE_FIFO_KHR) {
                NEON_LOG_CAT(neon::core::LogCategory::Gfx, neon::core::LogLevel::Warn,
                             "Vulkan: present mode %d is advertised but unusable (%s); retrying with a simpler mode",
                             static_cast<int>(mode), VkResultName(scRes));
            }
        }
        if (scRes != VK_SUCCESS) {
            NEON_LOG_CAT(
                neon::core::LogCategory::Gfx, neon::core::LogLevel::Error,
                "Vulkan: vkCreateSwapchainKHR failed (%s) extent=%ux%u range=[%u..%u]x[%u..%u] images=%u present=%d alpha=0x%x usage=0x%x supported=0x%x formats=%u modes=%u",
                VkResultName(scRes), w, h, caps.minImageExtent.width, caps.maxImageExtent.width,
                caps.minImageExtent.height, caps.maxImageExtent.height, imageCount,
                static_cast<int>(presentMode), static_cast<unsigned>(composite),
                static_cast<unsigned>(usage), static_cast<unsigned>(caps.supportedUsageFlags),
                formatCount, modeCount);
            return false;
        }

        uint32_t count = 0;
        vkGetSwapchainImagesKHR(device_, swapchain_, &count, nullptr);
        swapImages_.resize(count);
        vkGetSwapchainImagesKHR(device_, swapchain_, &count, swapImages_.data());

        VkImage depthImage;
        VkImageView depthView;
        if (!CreateImage(w, h, depthFormat_, 1, VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT,
                         VK_IMAGE_TILING_OPTIMAL, &depthImage, &swapDepthMem_, &depthView, 1)) {
            return false;
        }
        swapDepthImage_ = depthImage;
        swapDepthView_ = depthView;
        swapDepthLayout_ = VK_IMAGE_LAYOUT_UNDEFINED;

        RenderPassPair rp = GetRenderPasses(swapchainFormat_, 1);

        swapTarget_ = std::make_unique<Target>();
        swapTarget_->swapchain = true;
        swapTarget_->kind = RpKind::Color1;
        swapTarget_->colorFormat = swapchainFormat_;
        swapTarget_->width = static_cast<int>(w);
        swapTarget_->height = static_cast<int>(h);
        swapTarget_->samples = 1;
        swapTarget_->floatColor = false;
        swapTarget_->rpClear = rp.clear;
        swapTarget_->rpLoad = rp.load;
        swapTarget_->swapViews.resize(count);
        swapTarget_->swapFramebuffers.resize(count);
        for (uint32_t i = 0; i < count; ++i) {
            swapTarget_->swapViews[i] =
                CreateView(swapImages_[i], swapchainFormat_, VK_IMAGE_VIEW_TYPE_2D,
                           VK_IMAGE_ASPECT_COLOR_BIT);
            swapTarget_->swapFramebuffers[i] =
                CreateFramebuffer(w, h, rp.clear, swapTarget_->swapViews[i], depthView);
        }
        swapImageLayout_ = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
        NEON_LOG_CAT(neon::core::LogCategory::Gfx, neon::core::LogLevel::Info,
                     "Vulkan: swapchain %ux%u format=%d images=%u", w, h, swapchainFormat_, count);
        return true;
    }

    void DestroySwapchain() {
        if (swapTarget_) {
            for (VkFramebuffer fb : swapTarget_->swapFramebuffers)
                if (fb) vkDestroyFramebuffer(device_, fb, nullptr);
            for (VkImageView v : swapTarget_->swapViews)
                if (v) vkDestroyImageView(device_, v, nullptr);
            swapTarget_.reset();
        }
        if (swapDepthView_) vkDestroyImageView(device_, swapDepthView_, nullptr);
        if (swapDepthImage_) vkDestroyImage(device_, swapDepthImage_, nullptr);
        if (swapDepthMem_) vkFreeMemory(device_, swapDepthMem_, nullptr);
        swapDepthView_ = VK_NULL_HANDLE;
        swapDepthImage_ = VK_NULL_HANDLE;
        swapDepthMem_ = VK_NULL_HANDLE;
        if (swapchain_) {
            vkDestroySwapchainKHR(device_, swapchain_, nullptr);
            swapchain_ = VK_NULL_HANDLE;
        }
        swapImages_.clear();
        imageIndex_ = 0;
    }

    bool CreateRenderPassesAndObjects() {
        VkSamplerCreateInfo si{};
        si.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
        si.magFilter = VK_FILTER_NEAREST;
        si.minFilter = VK_FILTER_NEAREST;
        si.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
        si.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        si.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        si.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        si.maxLod = VK_LOD_CLAMP_NONE;
        if (vkCreateSampler(device_, &si, nullptr, &samplerNearest_) != VK_SUCCESS) return false;
        si.addressModeU = VK_SAMPLER_ADDRESS_MODE_REPEAT;
        si.addressModeV = VK_SAMPLER_ADDRESS_MODE_REPEAT;
        si.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT;
        if (vkCreateSampler(device_, &si, nullptr, &samplerRepeatNearest_) != VK_SUCCESS)
            return false;
        si.magFilter = VK_FILTER_LINEAR;
        si.minFilter = VK_FILTER_LINEAR;
        si.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
        if (vkCreateSampler(device_, &si, nullptr, &samplerLinear_) != VK_SUCCESS) return false;
        if (vkCreateSampler(device_, &si, nullptr, &samplerRepeatLinear_) != VK_SUCCESS)
            return false;

        depthFormat_ = VK_FORMAT_UNDEFINED;
        // (the chosen depth format is logged after the probe below)
        for (VkFormat f : kDepthFormats) {
            VkFormatProperties props;
            vkGetPhysicalDeviceFormatProperties(physicalDevice_, f, &props);
            if (props.optimalTilingFeatures & VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT) {
                depthFormat_ = f;
                break;
            }
        }
        if (depthFormat_ == VK_FORMAT_UNDEFINED) {
            NEON_LOG_CAT(neon::core::LogCategory::Gfx, neon::core::LogLevel::Error,
                         "Vulkan: no usable depth format");
            return false;
        }

        {
            VkDescriptorSetLayoutBinding bindings0{};
            bindings0.binding = 0;
            bindings0.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
            bindings0.descriptorCount = 1;
            bindings0.stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
            VkDescriptorSetLayoutCreateInfo dl{};
            dl.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
            dl.bindingCount = 1;
            dl.pBindings = &bindings0;
            if (vkCreateDescriptorSetLayout(device_, &dl, nullptr, &descLayoutSet0_) != VK_SUCCESS)
                return false;

            std::vector<VkDescriptorSetLayoutBinding> bindings1;
            for (uint32_t i = 0; i < kMaxSamplerSlots; ++i) {
                bindings1.push_back({i, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1,
                                     VK_SHADER_STAGE_FRAGMENT_BIT, nullptr});
            }
            VkDescriptorSetLayoutCreateInfo dl1{};
            dl1.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
            dl1.bindingCount = static_cast<uint32_t>(bindings1.size());
            dl1.pBindings = bindings1.data();
            if (vkCreateDescriptorSetLayout(device_, &dl1, nullptr, &descLayoutSet1_) !=
                VK_SUCCESS)
                return false;
        }

        VkDescriptorSetLayout layouts[] = {descLayoutSet0_, descLayoutSet1_};
        VkPipelineLayoutCreateInfo pl{};
        pl.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
        pl.setLayoutCount = 2;
        pl.pSetLayouts = layouts;
        if (vkCreatePipelineLayout(device_, &pl, nullptr, &pipelineLayout_) != VK_SUCCESS)
            return false;

        VkPipelineCacheCreateInfo pc{};
        pc.sType = VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO;
        vkCreatePipelineCache(device_, &pc, nullptr, &pipelineCache_);

        VkAttachmentDescription depthAtt{};
        depthAtt.format = depthFormat_;
        depthAtt.samples = VK_SAMPLE_COUNT_1_BIT;
        depthAtt.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        depthAtt.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        depthAtt.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        depthAtt.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        // GENERAL: the only layout that is valid both as a depth attachment and
        // as a sampled texture, so the resolved depth never needs an
        // attachment -> sample transition (see the note in ResolveDepth).
        depthAtt.initialLayout = VK_IMAGE_LAYOUT_GENERAL;
        depthAtt.finalLayout = VK_IMAGE_LAYOUT_GENERAL;
        VkAttachmentReference depthRef{0, VK_IMAGE_LAYOUT_GENERAL};
        VkSubpassDescription subpass{};
        subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
        subpass.colorAttachmentCount = 0;
        subpass.pDepthStencilAttachment = &depthRef;
        VkRenderPassCreateInfo rpci{};
        rpci.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
        rpci.attachmentCount = 1;
        rpci.pAttachments = &depthAtt;
        rpci.subpassCount = 1;
        rpci.pSubpasses = &subpass;
        if (vkCreateRenderPass(device_, &rpci, nullptr, &depthOnlyPass_) != VK_SUCCESS)
            return false;

        return true;
    }

    bool CreateFrames() {
        frames_.resize(kFramesInFlight);
        for (uint32_t i = 0; i < kFramesInFlight; ++i) {
            Frame& f = frames_[i];
            if (!CreateCommandPool(&f.pool)) return false;
            VkCommandBufferAllocateInfo ai{};
            ai.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
            ai.commandPool = f.pool;
            ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
            ai.commandBufferCount = 1;
            if (vkAllocateCommandBuffers(device_, &ai, &f.cmd) != VK_SUCCESS) return false;

            if (!CreateDescriptorPool(&f.descPool)) return false;

            // A1: per-frame texture sampler pool (see struct Frame).
            VkDescriptorPoolSize tps[]{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                                       1024 * kMaxSamplerSlots};
            VkDescriptorPoolCreateInfo tpci{};
            tpci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
            tpci.maxSets = 2048;
            tpci.poolSizeCount = 1;
            tpci.pPoolSizes = tps;
            if (vkCreateDescriptorPool(device_, &tpci, nullptr, &f.texPool) != VK_SUCCESS)
                return false;

            if (!CreateHostBuffer(kUboBytes, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, &f.uboRing,
                                  &f.uboMem)) {
                return false;
            }
            vkMapMemory(device_, f.uboMem, 0, kUboBytes, 0,
                        reinterpret_cast<void**>(&f.uboPtr));
            if (!CreateHostBuffer(kScratchBytes,
                                  VK_BUFFER_USAGE_VERTEX_BUFFER_BIT |
                                      VK_BUFFER_USAGE_INDEX_BUFFER_BIT |
                                      VK_BUFFER_USAGE_TRANSFER_DST_BIT |
                                      // A2: staging source for texture region
                                      // updates (dynamic glyph atlas).
                                      VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                                  &f.scratch, &f.scratchMem)) {
                return false;
            }
            vkMapMemory(device_, f.scratchMem, 0, kScratchBytes, 0,
                        reinterpret_cast<void**>(&f.scratchPtr));

            VkSemaphoreCreateInfo sem{};
            sem.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
            vkCreateSemaphore(device_, &sem, nullptr, &f.imageAvailable);
            vkCreateSemaphore(device_, &sem, nullptr, &f.renderFinished);
            vkCreateSemaphore(device_, &sem, nullptr, &f.progress);

            VkFenceCreateInfo fci{};
            fci.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
            fci.flags = VK_FENCE_CREATE_SIGNALED_BIT;
            vkCreateFence(device_, &fci, nullptr, &f.fence);
            fci.flags = 0;
            vkCreateFence(device_, &fci, nullptr, &f.readFence);
        }
        return true;
    }

    void DestroyFrames() {
        texSetCache_.clear();
        for (Frame& f : frames_) {
            if (f.cmd && f.pool) vkFreeCommandBuffers(device_, f.pool, 1, &f.cmd);
            if (f.pool) vkDestroyCommandPool(device_, f.pool, nullptr);
            if (f.descPool) vkDestroyDescriptorPool(device_, f.descPool, nullptr);
            if (f.texPool) vkDestroyDescriptorPool(device_, f.texPool, nullptr);
            if (f.uboPtr) vkUnmapMemory(device_, f.uboMem);
            if (f.uboRing) vkDestroyBuffer(device_, f.uboRing, nullptr);
            if (f.uboMem) vkFreeMemory(device_, f.uboMem, nullptr);
            if (f.scratchPtr) vkUnmapMemory(device_, f.scratchMem);
            if (f.scratch) vkDestroyBuffer(device_, f.scratch, nullptr);
            if (f.scratchMem) vkFreeMemory(device_, f.scratchMem, nullptr);
            if (f.imageAvailable) vkDestroySemaphore(device_, f.imageAvailable, nullptr);
            if (f.renderFinished) vkDestroySemaphore(device_, f.renderFinished, nullptr);
            if (f.progress) vkDestroySemaphore(device_, f.progress, nullptr);
            if (f.fence) vkDestroyFence(device_, f.fence, nullptr);
            if (f.readFence) vkDestroyFence(device_, f.readFence, nullptr);
        }
        frames_.clear();
    }

    bool AcquireSwapchainImage(Frame& f) {
        if (!swapchain_) return false;
        VkResult r = vkAcquireNextImageKHR(device_, swapchain_, UINT64_MAX, f.imageAvailable,
                                           VK_NULL_HANDLE, &imageIndex_);
        if (r == VK_ERROR_OUT_OF_DATE_KHR) {
            RecreateSwapchain();
            r = vkAcquireNextImageKHR(device_, swapchain_, UINT64_MAX, f.imageAvailable,
                                      VK_NULL_HANDLE, &imageIndex_);
        }
        if (r != VK_SUCCESS && r != VK_SUBOPTIMAL_KHR) {
            NEON_LOG_CAT(neon::core::LogCategory::Gfx, neon::core::LogLevel::Error,
                         "Vulkan: acquire next image failed (%s)", VkResultName(r));
            return false;
        }
        swapImageLayout_ = VK_IMAGE_LAYOUT_UNDEFINED;
        return true;
    }

    // ------------------------------------------------------------------
    // Small Vulkan helpers
    // ------------------------------------------------------------------
    Frame& CurrentFrame() { return frames_[frameIndex_]; }

    uint32_t ClampSamples(int requested) const {
        if (requested <= 1) return 1;
        const VkSampleCountFlags bits = static_cast<VkSampleCountFlags>(
            maxSampleCounts_ & (VK_SAMPLE_COUNT_2_BIT | VK_SAMPLE_COUNT_4_BIT));
        if (requested >= 4 && (bits & VK_SAMPLE_COUNT_4_BIT)) return 4;
        if (requested >= 2 && (bits & VK_SAMPLE_COUNT_2_BIT)) return 2;
        return 1;
    }

    VkSampleCountFlagBits SamplesFlag(uint32_t samples) const {
        return samples >= 4 ? VK_SAMPLE_COUNT_4_BIT
                            : (samples == 2 ? VK_SAMPLE_COUNT_2_BIT : VK_SAMPLE_COUNT_1_BIT);
    }

    bool CreateCommandPool(VkCommandPool* out) {
        VkCommandPoolCreateInfo ci{};
        ci.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
        ci.queueFamilyIndex = queueFamily_;
        ci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        return vkCreateCommandPool(device_, &ci, nullptr, out) == VK_SUCCESS;
    }

    VkCommandBuffer BeginOneShot(VkCommandPool pool) {
        VkCommandBufferAllocateInfo ai{};
        ai.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        ai.commandPool = pool;
        ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        ai.commandBufferCount = 1;
        VkCommandBuffer cmd;
        vkAllocateCommandBuffers(device_, &ai, &cmd);
        VkCommandBufferBeginInfo bi{};
        bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        vkBeginCommandBuffer(cmd, &bi);
        return cmd;
    }

    bool CreateDescriptorPool(VkDescriptorPool* out) {
        VkDescriptorPoolSize sizes[2] = {
            {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, 64},
            {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 8192 * kMaxSamplerSlots},
        };
        VkDescriptorPoolCreateInfo ci{};
        ci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
        ci.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
        ci.maxSets = 16384;
        ci.poolSizeCount = 2;
        ci.pPoolSizes = sizes;
        return vkCreateDescriptorPool(device_, &ci, nullptr, out) == VK_SUCCESS;
    }

    void AllocUboSet(Frame& f) {
        VkDescriptorSetAllocateInfo ai{};
        ai.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        ai.descriptorPool = f.descPool;
        ai.descriptorSetCount = 1;
        ai.pSetLayouts = &descLayoutSet0_;
        if (vkAllocateDescriptorSets(device_, &ai, &f.uboSet) != VK_SUCCESS) {
            f.uboSet = VK_NULL_HANDLE;
            return;
        }
        VkDescriptorBufferInfo info{};
        info.buffer = f.uboRing;
        info.offset = 0;
        info.range = kUniformBlockSize;
        VkWriteDescriptorSet w{};
        w.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        w.dstSet = f.uboSet;
        w.dstBinding = 0;
        w.descriptorCount = 1;
        w.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
        w.pBufferInfo = &info;
        vkUpdateDescriptorSets(device_, 1, &w, 0, nullptr);
    }

    uint32_t FindMemoryType(uint32_t typeBits, VkMemoryPropertyFlags flags) {
        VkPhysicalDeviceMemoryProperties props;
        vkGetPhysicalDeviceMemoryProperties(physicalDevice_, &props);
        for (uint32_t i = 0; i < props.memoryTypeCount; ++i) {
            if ((typeBits & (1u << i)) && (props.memoryTypes[i].propertyFlags & flags) == flags)
                return i;
        }
        return UINT32_MAX;
    }

    bool AllocMemory(VkMemoryRequirements req, VkMemoryPropertyFlags flags, VkDeviceMemory* out) {
        uint32_t type = FindMemoryType(req.memoryTypeBits, flags);
        if (type == UINT32_MAX) {
            return false;
        }
        VkMemoryAllocateInfo ai{};
        ai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        ai.allocationSize = req.size;
        ai.memoryTypeIndex = type;
        return vkAllocateMemory(device_, &ai, nullptr, out) == VK_SUCCESS;
    }

    bool CreateHostBuffer(size_t size, VkBufferUsageFlags usage, VkBuffer* buffer,
                          VkDeviceMemory* mem) {
        VkBufferCreateInfo bi{};
        bi.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        bi.size = size;
        bi.usage = usage;
        bi.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        if (vkCreateBuffer(device_, &bi, nullptr, buffer) != VK_SUCCESS) return false;
        VkMemoryRequirements req;
        vkGetBufferMemoryRequirements(device_, *buffer, &req);
        if (!AllocMemory(req, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                  VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                         mem)) {
            vkDestroyBuffer(device_, *buffer, nullptr);
            *buffer = VK_NULL_HANDLE;
            return false;
        }
        vkBindBufferMemory(device_, *buffer, *mem, 0);
        return true;
    }

    bool CreateDeviceBuffer(size_t size, VkBufferUsageFlags usage, const void* data,
                            VkBuffer* buffer, VkDeviceMemory* mem) {
        VkBufferCreateInfo bi{};
        bi.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        bi.size = size;
        bi.usage = usage | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
        bi.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        if (vkCreateBuffer(device_, &bi, nullptr, buffer) != VK_SUCCESS) return false;
        VkMemoryRequirements req;
        vkGetBufferMemoryRequirements(device_, *buffer, &req);
        if (!AllocMemory(req, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, mem)) {
            vkDestroyBuffer(device_, *buffer, nullptr);
            *buffer = VK_NULL_HANDLE;
            return false;
        }
        vkBindBufferMemory(device_, *buffer, *mem, 0);

        VkBuffer staging;
        VkDeviceMemory stagingMem;
        if (!CreateHostBuffer(size, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, &staging, &stagingMem)) {
            return false;
        }
        void* mapped = nullptr;
        vkMapMemory(device_, stagingMem, 0, size, 0, &mapped);
        std::memcpy(mapped, data, size);
        vkUnmapMemory(device_, stagingMem);

        VkCommandPool pool;
        CreateCommandPool(&pool);
        VkCommandBuffer cmd = BeginOneShot(pool);
        VkBufferCopy copy{};
        copy.size = size;
        vkCmdCopyBuffer(cmd, staging, *buffer, 1, &copy);
        vkEndCommandBuffer(cmd);
        SubmitQueue(CurrentFrame(), cmd);
        vkFreeCommandBuffers(device_, pool, 1, &cmd);
        vkDestroyCommandPool(device_, pool, nullptr);
        vkDestroyBuffer(device_, staging, nullptr);
        vkFreeMemory(device_, stagingMem, nullptr);
        return true;
    }

    bool CreateImage(int width, int height, VkFormat format, uint32_t samples,
                     VkImageUsageFlags usage, VkImageTiling tiling, VkImage* image,
                     VkDeviceMemory* mem, VkImageView* depthView, int mipLevels = 1) {
        VkImageCreateInfo ci{};
        ci.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
        ci.imageType = VK_IMAGE_TYPE_2D;
        ci.format = format;
        ci.extent = {static_cast<uint32_t>(width), static_cast<uint32_t>(height), 1};
        ci.mipLevels = static_cast<uint32_t>(mipLevels);
        ci.arrayLayers = 1;
        ci.samples = SamplesFlag(samples);
        ci.tiling = tiling;
        ci.usage = usage;
        ci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        ci.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        if (vkCreateImage(device_, &ci, nullptr, image) != VK_SUCCESS) return false;

        if (mem) {
            VkMemoryRequirements req;
            vkGetImageMemoryRequirements(device_, *image, &req);
            if (!AllocMemory(req, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, mem)) {
                vkDestroyImage(device_, *image, nullptr);
                *image = VK_NULL_HANDLE;
                return false;
            }
            vkBindImageMemory(device_, *image, *mem, 0);
        }

        if (depthView) {
            VkImageViewCreateInfo vi{};
            vi.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
            vi.image = *image;
            vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
            vi.format = format;
            vi.subresourceRange = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 1};
            if (vkCreateImageView(device_, &vi, nullptr, depthView) != VK_SUCCESS) {
                if (mem) vkFreeMemory(device_, *mem, nullptr);
                vkDestroyImage(device_, *image, nullptr);
                *image = VK_NULL_HANDLE;
                return false;
            }
        }
        return true;
    }

    VkImageView CreateView(VkImage image, VkFormat format, VkImageViewType type,
                           VkImageAspectFlags aspect, int mipLevels = 1) {
        VkImageViewCreateInfo vi{};
        vi.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        vi.image = image;
        vi.viewType = type;
        vi.format = format;
        vi.subresourceRange = {aspect, 0, static_cast<uint32_t>(mipLevels), 0, 1};
        VkImageView view;
        if (vkCreateImageView(device_, &vi, nullptr, &view) != VK_SUCCESS) return VK_NULL_HANDLE;
        return view;
    }

    VkFramebuffer CreateFramebuffer(int width, int height, VkRenderPass rp, VkImageView colorView,
                                    VkImageView depthView) {
        std::vector<VkImageView> attachments;
        if (colorView) attachments.push_back(colorView);
        if (depthView) attachments.push_back(depthView);
        VkFramebufferCreateInfo fci{};
        fci.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
        fci.renderPass = rp;
        fci.attachmentCount = static_cast<uint32_t>(attachments.size());
        fci.pAttachments = attachments.empty() ? nullptr : attachments.data();
        fci.width = static_cast<uint32_t>(width);
        fci.height = static_cast<uint32_t>(height);
        fci.layers = 1;
        VkFramebuffer fb;
        if (vkCreateFramebuffer(device_, &fci, nullptr, &fb) != VK_SUCCESS) return VK_NULL_HANDLE;
        return fb;
    }

    // ------------------------------------------------------------------
    // Render passes
    // ------------------------------------------------------------------
    RenderPassPair GetRenderPasses(VkFormat colorFormat, uint32_t samples) {
        RenderPassKey key{colorFormat, samples};
        auto it = renderPasses_.find(key);
        if (it != renderPasses_.end()) return it->second;

        const VkSampleCountFlagBits sampleFlag = SamplesFlag(samples);
        RenderPassPair pair;
        pair.clear = CreateColorRenderPass(colorFormat, sampleFlag, true);
        pair.load = CreateColorRenderPass(colorFormat, sampleFlag, false);
        renderPasses_[key] = pair;
        return pair;
    }

    VkRenderPass CreateColorRenderPass(VkFormat colorFormat, VkSampleCountFlagBits samples,
                                       bool clearLoad) {
        VkAttachmentDescription color{};
        color.format = colorFormat;
        color.samples = samples;
        color.loadOp = clearLoad ? VK_ATTACHMENT_LOAD_OP_CLEAR : VK_ATTACHMENT_LOAD_OP_LOAD;
        color.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        color.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        color.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        color.initialLayout = VK_IMAGE_LAYOUT_GENERAL;
        color.finalLayout = VK_IMAGE_LAYOUT_GENERAL;

        VkAttachmentDescription depth{};
        depth.format = depthFormat_;
        depth.samples = samples;
        depth.loadOp = clearLoad ? VK_ATTACHMENT_LOAD_OP_CLEAR : VK_ATTACHMENT_LOAD_OP_LOAD;
        depth.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        depth.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        depth.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        depth.initialLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
        depth.finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;

        VkAttachmentReference colorRef{0, VK_IMAGE_LAYOUT_GENERAL};
        VkAttachmentReference depthRef{1, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL};

        VkSubpassDescription subpass{};
        subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
        subpass.colorAttachmentCount = 1;
        subpass.pColorAttachments = &colorRef;
        subpass.pDepthStencilAttachment = &depthRef;

        VkSubpassDependency dependency{};
        dependency.srcSubpass = VK_SUBPASS_EXTERNAL;
        dependency.dstSubpass = 0;
        dependency.srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT |
                                  VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
        dependency.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT |
                                   VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
        dependency.dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT |
                                  VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT;
        dependency.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_READ_BIT |
                                   VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT |
                                   VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT |
                                   VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;

        VkAttachmentDescription attachments[2] = {color, depth};
        VkRenderPassCreateInfo rpci{};
        rpci.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
        rpci.attachmentCount = 2;
        rpci.pAttachments = attachments;
        rpci.subpassCount = 1;
        rpci.pSubpasses = &subpass;
        if (!clearLoad) {
            rpci.dependencyCount = 1;
            rpci.pDependencies = &dependency;
        }
        VkRenderPass rp;
        if (vkCreateRenderPass(device_, &rpci, nullptr, &rp) != VK_SUCCESS) return VK_NULL_HANDLE;
        return rp;
    }

    static int MipLevels(int w, int h) {
        int levels = 1;
        int dim = std::max(w, h);
        while (dim > 1) {
            dim >>= 1;
            ++levels;
        }
        return levels;
    }

    void GenerateMips(VkCommandBuffer cmd, VkImage image, VkFormat format, int width, int height,
                      int levels) {
        (void)format;
        int mipW = width;
        int mipH = height;
        for (int level = 1; level < levels; ++level) {
            VkImageMemoryBarrier barrier{};
            barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
            barrier.image = image;
            barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT,
                                        static_cast<uint32_t>(level - 1), 1, 0, 1};
            barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
            barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
            barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
            barrier.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
            vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                                 0, 0, nullptr, 0, nullptr, 1, &barrier);

            VkImageBlit blit{};
            blit.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT,
                                   static_cast<uint32_t>(level - 1), 0, 1};
            blit.srcOffsets[0] = {0, 0, 0};
            blit.srcOffsets[1] = {mipW, mipH, 1};
            const int nextW = std::max(mipW / 2, 1);
            const int nextH = std::max(mipH / 2, 1);
            blit.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, static_cast<uint32_t>(level), 0, 1};
            blit.dstOffsets[0] = {0, 0, 0};
            blit.dstOffsets[1] = {nextW, nextH, 1};
            vkCmdBlitImage(cmd, image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, image,
                           VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit, VK_FILTER_LINEAR);

            barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT,
                                        static_cast<uint32_t>(level - 1), 1, 0, 1};
            barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
            barrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            barrier.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
            barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
            vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
                                 VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, nullptr, 0, nullptr,
                                 1, &barrier);

            mipW = nextW;
            mipH = nextH;
        }
        VkImageMemoryBarrier barrier{};
        barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        barrier.image = image;
        barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT,
                                    static_cast<uint32_t>(levels - 1), 1, 0, 1};
        barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        barrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
                             VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, nullptr, 0, nullptr, 1,
                             &barrier);
    }

    // Aspect mask for the engine depth format. A combined depth-stencil image
    // must be transitioned with both aspects whenever either layout is a
    // combined depth-stencil layout, which the depth-attachment layouts are:
    // the NVIDIA driver crashes on the depth-only form.
    VkImageAspectFlags DepthAspect() const {
        switch (depthFormat_) {
            case VK_FORMAT_D16_UNORM_S8_UINT:
            case VK_FORMAT_D24_UNORM_S8_UINT:
            case VK_FORMAT_D32_SFLOAT_S8_UINT:
                return VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT;
            default:
                return VK_IMAGE_ASPECT_DEPTH_BIT;
        }
    }

    void TransitionImage(VkCommandBuffer cmd, VkImage image, VkImageLayout oldLayout,
                         VkImageLayout newLayout, VkPipelineStageFlags srcStage,
                         VkPipelineStageFlags dstStage, VkAccessFlags srcAccess,
                         VkAccessFlags dstAccess, VkImageAspectFlags aspect) {
        VkImageMemoryBarrier barrier{};
        barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        barrier.image = image;
        barrier.oldLayout = oldLayout;
        barrier.newLayout = newLayout;
        barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.srcAccessMask = srcAccess;
        barrier.dstAccessMask = dstAccess;
        barrier.subresourceRange = {aspect, 0, VK_REMAINING_MIP_LEVELS, 0, 1};
        NEON_VK_TRACE("vt: barrier pre old=%d new=%d aspect=%u cmd=%p\n", (int)oldLayout, (int)newLayout, (unsigned)aspect, (void*)cmd);
        vkCmdPipelineBarrier(cmd, srcStage, dstStage, 0, 0, nullptr, 0, nullptr, 1, &barrier);
        NEON_VK_TRACE("vt: barrier post\n");
    }

    void SubmitQueue(Frame& f, VkCommandBuffer cmd, bool waitAcquire = false) {
        NEON_VK_TRACE("bt: SubmitQueue\n");
        VkSubmitInfo si{};
        si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        si.commandBufferCount = 1;
        si.pCommandBuffers = &cmd;
        VkSemaphore waitSems[2] = {VK_NULL_HANDLE, VK_NULL_HANDLE};
        VkPipelineStageFlags waitStages[2] = {VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                                              VK_PIPELINE_STAGE_ALL_COMMANDS_BIT};
        uint32_t waitCount = 0;
        if (f.progressSignaled) waitSems[waitCount++] = f.progress;
        if (waitAcquire && inFrame_ && !f.acquiredWaited && f.imageAvailable) {
            waitSems[waitCount++] = f.imageAvailable;
            f.acquiredWaited = true;
        }
        if (waitCount > 0) {
            si.waitSemaphoreCount = waitCount;
            si.pWaitSemaphores = waitSems;
            si.pWaitDstStageMask = waitStages;
        }
        si.signalSemaphoreCount = 1;
        si.pSignalSemaphores = &f.progress;
        f.progressSignaled = true;
        const VkResult midRes = vkQueueSubmit(queue_, 1, &si, f.readFence);
        if (midRes != VK_SUCCESS) {
            NEON_LOG_CAT(neon::core::LogCategory::Gfx, neon::core::LogLevel::Error,
                         "Vulkan: mid-frame queue submit failed: %s (%d)",
                         VkResultName(midRes), static_cast<int>(midRes));
        }
        vkWaitForFences(device_, 1, &f.readFence, VK_TRUE, UINT64_MAX);
        vkResetFences(device_, 1, &f.readFence);
    }

    // Vulkan has no implicit lifetime tracking for handles referenced by a
    // recorded but unsubmitted command buffer (OpenGL does; the driver defers
    // deletion). Destroying a framebuffer/attachment (or a buffer/image still
    // reachable from pending commands) made the NVIDIA driver fault inside
    // vkCmdEndRenderPass. Flush first: an open render pass means attachments are
    // in flight, and SubmitQueue fully synchronises before the handle dies.
    void FlushPendingWorkBeforeDestroy(Target* destroyed) {
        if (!device_ || frames_.empty()) return;
        Frame& f = CurrentFrame();
        if (!f.cmdOpen || !rpActive_) {
            if (destroyed && target_ == destroyed) target_ = nullptr;
            return;
        }
        EndRenderPassIfActive(f);
        vkEndCommandBuffer(f.cmd);
        f.cmdOpen = false;
        SubmitQueue(f, f.cmd);
        if (destroyed && target_ == destroyed) target_ = nullptr;
    }
    // ------------------------------------------------------------------
    // Frame / target management
    // ------------------------------------------------------------------
    void BindTarget(Target* target) {
        NEON_VK_TRACE("bt: enter target=%p old=%p\n", (void*)target, (void*)target_);
        Frame& f = CurrentFrame();
        // Bound each submission to one target's render pass: the tested Intel
        // driver device-losts when the whole frame is in a single command
        // buffer. A semaphore chain keeps sampler reads memory-coherent across
        // the submissions.
        if (target_ != target && f.cmdOpen) {
            NEON_VK_TRACE("bt: flushing pending cmd\n");
            EndRenderPassIfActive(f);
            vkEndCommandBuffer(f.cmd);
            f.cmdOpen = false;
            SubmitQueue(f, f.cmd);
            NEON_VK_TRACE("bt: flush done\n");
        }
        EndRenderPassIfActive(f);
        EnsureFrameStarted();
        target_ = target;
        // Binding a target resets the viewport to its full size, matching the GL
        // backend. The shadow pass relies on this: it binds a shadowSize x
        // shadowSize cascade target without an explicit SetViewport, and a stale
        // window-sized viewport stretched the light frustum so the maps no
        // longer lined up with the lit shader sampling them.
        if (target) {
            viewportX_ = 0;
            viewportY_ = 0;
            viewportWidth_ = target->width;
            viewportHeight_ = target->height;
        }
    }

    void EnsureFrameStarted() {
        NEON_VK_TRACE("vt: EnsureFrameStarted ready=%d\n", (int)frameReady_);
        Frame& f = CurrentFrame();
        if (frameReady_) return;
        frameReady_ = true;
        inFrame_ = true;
        vkWaitForFences(device_, 1, &f.fence, VK_TRUE, UINT64_MAX);
        vkResetFences(device_, 1, &f.fence);
        vkResetCommandPool(device_, f.pool, 0);
        vkResetDescriptorPool(device_, f.descPool, 0);
        // A1: the sampler sets of THIS frame come from its own pool (fence
        // above covers them); drop the cache so the pool cannot be exhausted
        // by texture-id churn (dynamic sky / hot reloads used to grow it
        // unboundedly until every draw silently failed).
        vkResetDescriptorPool(device_, f.texPool, 0);
        texSetCache_.clear();
        f.uboCursor = 0;
        f.scratchCursor = 0;
        f.acquiredWaited = false;
        f.progressSignaled = false;
        f.uboSet = VK_NULL_HANDLE;
        AllocUboSet(f);
        rpActive_ = false;
        target_ = nullptr;
        // A frame starts with no pending clear: a target that was cleared but
        // never drawn into leaves no stale clear behind for its next pass.
        for (auto& entry : renderTargets_) entry.second.clearPending = false;
        if (swapTarget_) swapTarget_->clearPending = false;
        uniformsDirty_ = true;
        OpenCmd(f);
    }

    void AcquireIfNeeded() {
        if (acquired_ || !swapchain_) return;
        if (!AcquireSwapchainImage(CurrentFrame())) {
            NEON_LOG_CAT(neon::core::LogCategory::Gfx, neon::core::LogLevel::Error,
                         "Vulkan: swapchain image acquire failed");
        }
        acquired_ = true;
    }

    void EndRenderPassIfActive(Frame& f) {
        if (!rpActive_ || !f.cmd) return;
        vkCmdEndRenderPass(f.cmd);
        rpActive_ = false;
    }

    void OpenCmd(Frame& f) {
        if (f.cmdOpen) return;
        vkResetCommandBuffer(f.cmd, 0);
        VkCommandBufferBeginInfo bi{};
        bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        vkBeginCommandBuffer(f.cmd, &bi);
        f.cmdOpen = true;
    }

    VkImageLayout TrackedLayout(VkImage image) const {
        if (swapchain_ && imageIndex_ < swapImages_.size() &&
            image == swapImages_[imageIndex_]) {
            return swapImageLayout_;
        }
        for (const auto& [id, tex] : textures_) {
            if (tex.image == image) return tex.layout;
        }
        for (const auto& [id, rt] : renderTargets_) {
            if (rt.colorImage == image) return rt.layout;
            if (rt.depthImage == image) return rt.depthLayout;
        }
        return VK_IMAGE_LAYOUT_UNDEFINED;
    }

    void SetTrackedLayout(VkImage image, VkImageLayout layout) {
        if (swapchain_ && imageIndex_ < swapImages_.size() &&
            image == swapImages_[imageIndex_]) {
            swapImageLayout_ = layout;
        }
        for (auto& [id, tex] : textures_) {
            if (tex.image == image) tex.layout = layout;
        }
        for (auto& [id, rt] : renderTargets_) {
            if (rt.colorImage == image) rt.layout = layout;
            if (rt.depthImage == image) rt.depthLayout = layout;
        }
        if (image == swapDepthImage_) swapDepthLayout_ = layout;
    }

    void EnsureRenderPass(Frame& f) {
        NEON_VK_TRACE("vt: EnsureRenderPass target=%p swap=%d\n", (void*)target_, target_ ? (int)target_->swapchain : -1);
        if (rpActive_ || !target_) return;
        OpenCmd(f);
        if (target_->swapchain) AcquireIfNeeded();

        if (target_->depthOnly) {
            VkImageLayout cur = TrackedLayout(target_->depthImage);
            if (cur != VK_IMAGE_LAYOUT_GENERAL) {
                TransitionImage(f.cmd, target_->depthImage, cur, VK_IMAGE_LAYOUT_GENERAL,
                                VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                                VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT, 0,
                                VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
                                DepthAspect());
                SetTrackedLayout(target_->depthImage, VK_IMAGE_LAYOUT_GENERAL);
            }
        } else {
            VkImage colorImage =
                target_->swapchain ? swapImages_[imageIndex_] : target_->colorImage;
            if (colorImage) {
                VkImageLayout cur =
                    target_->swapchain ? swapImageLayout_ : TrackedLayout(colorImage);
                if (cur != VK_IMAGE_LAYOUT_GENERAL) {
                    TransitionImage(f.cmd, colorImage, cur, VK_IMAGE_LAYOUT_GENERAL,
                                    VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                                    VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                                    VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
                                    VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_IMAGE_ASPECT_COLOR_BIT);
                    SetTrackedLayout(colorImage, VK_IMAGE_LAYOUT_GENERAL);
                }
            }
            VkImage depthImage = target_->swapchain ? swapDepthImage_ : target_->depthImage;
            if (depthImage) {
                VkImageLayout cur =
                    target_->swapchain ? swapDepthLayout_ : TrackedLayout(depthImage);
                if (cur != VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL) {
                    TransitionImage(f.cmd, depthImage, cur,
                                    VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
                                    VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                                    VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT, 0,
                                    VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
                                    VK_IMAGE_ASPECT_DEPTH_BIT);
                    SetTrackedLayout(depthImage, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL);
                }
            }
        }

        const bool targetClear = target_->clearPending;
        VkRenderPass rp = targetClear ? target_->rpClear : target_->rpLoad;
        VkFramebuffer fb =
            target_->swapchain ? target_->swapFramebuffers[imageIndex_] : target_->framebuffer;
        if (!rp || !fb) return;

        VkRenderPassBeginInfo bi{};
        bi.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
        bi.renderPass = rp;
        bi.framebuffer = fb;
        bi.renderArea = {0, 0, static_cast<uint32_t>(target_->width),
                         static_cast<uint32_t>(target_->height)};
        VkClearValue clears[2] = {};
        if (target_->depthOnly) {
            clears[0].depthStencil = {target_->pendingClearDepth, 0};
        } else {
            clears[0].color = target_->pendingClearColor;
            clears[1].depthStencil = {target_->pendingClearDepth, 0};
        }
        bi.clearValueCount = target_->depthOnly ? 1 : 2;
        bi.pClearValues = clears;
        vkCmdBeginRenderPass(f.cmd, &bi, VK_SUBPASS_CONTENTS_INLINE);
        rpActive_ = true;
        target_->clearPending = false;
    }

    void DestroyTargetInternal(Target& rt) {
        if (rt.framebuffer) vkDestroyFramebuffer(device_, rt.framebuffer, nullptr);
        if (rt.colorView) vkDestroyImageView(device_, rt.colorView, nullptr);
        if (rt.depthView) vkDestroyImageView(device_, rt.depthView, nullptr);
        if (rt.colorImage) vkDestroyImage(device_, rt.colorImage, nullptr);
        if (rt.colorMem) vkFreeMemory(device_, rt.colorMem, nullptr);
        if (rt.depthImage) vkDestroyImage(device_, rt.depthImage, nullptr);
        if (rt.depthMem) vkFreeMemory(device_, rt.depthMem, nullptr);
        rt.framebuffer = VK_NULL_HANDLE;
        rt.colorView = VK_NULL_HANDLE;
        rt.depthView = VK_NULL_HANDLE;
        rt.colorImage = VK_NULL_HANDLE;
        rt.colorMem = VK_NULL_HANDLE;
        rt.depthImage = VK_NULL_HANDLE;
        rt.depthMem = VK_NULL_HANDLE;
    }

    void DestroyTextureInternal(Texture& tex) {
        if (tex.view) vkDestroyImageView(device_, tex.view, nullptr);
        if (tex.owned && tex.image) vkDestroyImage(device_, tex.image, nullptr);
        tex.view = VK_NULL_HANDLE;
        tex.image = VK_NULL_HANDLE;
        tex.owned = false;
    }

    // ------------------------------------------------------------------
    // Uniforms / descriptor binding
    // ------------------------------------------------------------------
    bool ScratchAlloc(Frame& f, uint64_t bytes, uint64_t align, uint64_t* offset) {
        const uint64_t aligned = (f.scratchCursor + align - 1) & ~(align - 1);
        if (aligned + bytes > kScratchBytes) {
            return false;
        }
        *offset = aligned;
        f.scratchCursor = aligned + bytes;
        return true;
    }

    uint64_t SnapshotUniforms(Frame& f) {
        const uint64_t stride = AlignUp(kUniformBlockSize, minUboAlignment_);
        const uint64_t offset = (f.uboCursor + minUboAlignment_ - 1) &
                                ~(static_cast<uint64_t>(minUboAlignment_) - 1);
        if (offset + kUniformBlockSize > kUboBytes) {
            return UINT64_MAX;
        }
        std::memcpy(f.uboPtr + offset, uniforms_, kUniformBlockSize);
        f.uboCursor = offset + stride;
        return offset;
    }

    uint64_t AlignUp(uint64_t v, uint64_t a) const { return (v + a - 1) & ~(a - 1); }

    VkDescriptorSet EnsureTextureSet(Frame& f) {
        Frame::TexKey key;
        for (uint32_t i = 0; i < kMaxSamplerSlots; ++i) key.ids[i] = boundTextures_[i];
        auto it = texSetCache_.find(key);
        if (it != texSetCache_.end()) return it->second;
        NEON_VK_TRACE("vt: texture set cache miss tex0=%u tex2=%u\n", boundTextures_[0], boundTextures_[2]);

        VkDescriptorSetAllocateInfo ai{};
        ai.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        ai.descriptorPool = f.texPool; // A1: per-frame pool (reset with its fence)
        ai.descriptorSetCount = 1;
        ai.pSetLayouts = &descLayoutSet1_;
        VkDescriptorSet set;
        if (vkAllocateDescriptorSets(device_, &ai, &set) != VK_SUCCESS) {
            return VK_NULL_HANDLE;
        }

        const Texture* white = GetTexture(white_);
        NEON_VK_TRACE("vt: ets alloc ok white=%p\n", (const void*)white);
        std::vector<VkDescriptorImageInfo> infos(kMaxSamplerSlots);
        std::vector<VkWriteDescriptorSet> writes;
        writes.reserve(kMaxSamplerSlots);
        for (uint32_t i = 0; i < kMaxSamplerSlots; ++i) {
            const Texture* tex = GetTexture({boundTextures_[i]});
            NEON_VK_TRACE("vt: ets i=%u id=%u tex=%p\\n", i, boundTextures_[i], (const void*)tex);
            if (!tex || !tex->view) tex = white;
            const VkImageLayout sampleLayout =
                tex->owned ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL : VK_IMAGE_LAYOUT_GENERAL;
            if (tex->image) {
                const VkImageAspectFlags aspect =
                    tex->format == depthFormat_ ? DepthAspect()
                                                : VK_IMAGE_ASPECT_COLOR_BIT;
                const VkImageLayout cur = TrackedLayout(tex->image);
                if (cur != sampleLayout && f.cmd) {
                    // A depth image written by a render pass needs the fragment-test
                    // stages in the source scope: naming only the colour stages makes
                    // the barrier claim nothing wrote the depth attachment.
                    const bool isDepth = tex->format == depthFormat_;
                    const VkPipelineStageFlags srcStage =
                        isDepth ? (VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT |
                                   VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT |
                                   VK_PIPELINE_STAGE_TRANSFER_BIT)
                                : (VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT |
                                   VK_PIPELINE_STAGE_TRANSFER_BIT);
                    const VkAccessFlags srcAccess =
                        isDepth ? (VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT |
                                   VK_ACCESS_TRANSFER_WRITE_BIT)
                                : (VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT |
                                   VK_ACCESS_TRANSFER_WRITE_BIT);
                    if (std::getenv("NEON_NO_TEX_TRANSITION")) {
                        SetTrackedLayout(tex->image, sampleLayout);
                    } else {
                        NEON_VK_TRACE("vt: ets transition i=%u img=%p cur=%d new=%d aspect=%u\n", i, (void*)tex->image, (int)cur, (int)sampleLayout, (unsigned)aspect);
                        TransitionImage(f.cmd, tex->image, cur, sampleLayout, srcStage,
                                        VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, srcAccess,
                                        VK_ACCESS_SHADER_READ_BIT, aspect);
                        SetTrackedLayout(tex->image, sampleLayout);
                    }
                }
            }
            infos[i].imageLayout = sampleLayout;
            infos[i].imageView = tex->view;
            infos[i].sampler = tex->sampler ? tex->sampler : samplerLinear_;
            VkWriteDescriptorSet w{};
            w.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            w.dstSet = set;
            w.dstBinding = i;
            w.descriptorCount = 1;
            w.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            w.pImageInfo = &infos[i];
            writes.push_back(w);
        }
        NEON_VK_TRACE("vt: ets updating %u writes\n", (unsigned)writes.size());
        vkUpdateDescriptorSets(device_, static_cast<uint32_t>(writes.size()), writes.data(), 0,
                               nullptr);
        NEON_VK_TRACE("vt: ets update ok\n");
        texSetCache_.emplace(key, set);
        return set;
    }

    const Texture* GetTexture(TextureHandle h) const {
        auto it = textures_.find(h.id);
        return it != textures_.end() ? &it->second : nullptr;
    }

    Program* GetProgram(uint32_t id) {
        auto it = programs_.find(id);
        return it != programs_.end() ? &it->second : nullptr;
    }

    bool PrepareDraw(Frame& f) {
        NEON_VK_TRACE("vt: PrepareDraw enter\n");
        const uint64_t uboOffset = SnapshotUniforms(f);
        NEON_VK_TRACE("vt: SnapshotUniforms -> %llu\n", (unsigned long long)uboOffset);
        if (uboOffset == UINT64_MAX) return false;
        if (!f.uboSet) AllocUboSet(f);
        lastUboOffset_ = uboOffset;
        NEON_VK_TRACE("vt: EnsureTextureSet start tex0=%u\n", boundTextures_[0]);
        lastTexSet_ = EnsureTextureSet(f);
        NEON_VK_TRACE("vt: EnsureTextureSet done set=%p\n", (void*)lastTexSet_);
        return lastTexSet_ != VK_NULL_HANDLE && f.uboSet != VK_NULL_HANDLE;
    }

    void BindPipeline(Frame& f, VkPipeline pipeline) {
        vkCmdBindPipeline(f.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
        VkViewport vp{};
        vp.x = static_cast<float>(viewportX_);
        vp.y = static_cast<float>(viewportY_);
        vp.width = static_cast<float>(viewportWidth_);
        vp.height = static_cast<float>(viewportHeight_);
        vp.minDepth = 0.0f;
        vp.maxDepth = 1.0f;
        vkCmdSetViewport(f.cmd, 0, 1, &vp);
        VkRect2D scissor;
        if (scissorEnabled_) {
            scissor.offset = {scissorX_, scissorY_};
            scissor.extent = {static_cast<uint32_t>(scissorW_), static_cast<uint32_t>(scissorH_)};
        } else {
            scissor.offset = {0, 0};
            scissor.extent = {static_cast<uint32_t>(viewportWidth_),
                              static_cast<uint32_t>(viewportHeight_)};
        }
        vkCmdSetScissor(f.cmd, 0, 1, &scissor);

        if (lastTexSet_ == VK_NULL_HANDLE || !f.uboSet) return;
        VkDescriptorSet sets[2] = {f.uboSet, lastTexSet_};
        const uint32_t dynOffset = static_cast<uint32_t>(lastUboOffset_);
        vkCmdBindDescriptorSets(f.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipelineLayout_, 0, 2,
                                sets, 1, &dynOffset);
    }

    void DrawIndexed(const Mesh& mesh, uint32_t instanceCount, uint64_t instanceOffset,
                     uint64_t colorOffset = UINT64_MAX) {
        Frame& f = CurrentFrame();
        if (!currentProgramId_) return;
        Program* prog = GetProgram(currentProgramId_);
        if (!prog) return;
        if (!PrepareDraw(f)) return;
        EnsureRenderPass(f);
        VkPipeline pipeline = GetPipeline(prog);
        if (!pipeline) return;
        BindPipeline(f, pipeline);

        const bool colored = prog->variant == VertexVariant::InstancedColored &&
                             colorOffset != UINT64_MAX;
        const bool instanced =
            prog->variant == VertexVariant::Instanced || colored;
        VkBuffer vbs[3] = {mesh.vbo, f.scratch, f.scratch};
        VkDeviceSize voffs[3] = {0, instanceOffset, colorOffset};
        vkCmdBindVertexBuffers(f.cmd, 0, instanced ? (colored ? 3u : 2u) : 1u, vbs, voffs);
        if (mesh.ibo) {
            vkCmdBindIndexBuffer(f.cmd, mesh.ibo, 0,
                                mesh.indexType ? VK_INDEX_TYPE_UINT32
                                                : VK_INDEX_TYPE_UINT16);
        if (VkTraceEnabled()) {
            const UniEntry* at = FindUniform("uAlphaTest");
            const float atv = at ? *reinterpret_cast<const float*>(uniforms_ + at->offset) : -1.0f;
            const UniEntry* ht = FindUniform("uHasTexture");
            const int htv = ht ? *reinterpret_cast<const int*>(uniforms_ + ht->offset) : -1;
            NEON_VK_TRACE("vt: draw prog=%s idx=%u inst=%u alphaTest=%.3f hasTex=%d tex0=%u\n",
                          prog->name.c_str(), mesh.indexCount, instanceCount, atv, htv,
                          boundTextures_[0]);
        }
            vkCmdDrawIndexed(f.cmd, mesh.indexCount, instanceCount, 0, 0, 0);
        }
    }

    // ------------------------------------------------------------------
    // Pipelines
    // ------------------------------------------------------------------
    VkPipeline GetPipeline(Program* prog) {
        if (!prog) return VK_NULL_HANDLE;
        const RpKind rp = target_ ? target_->kind : RpKind::Color1;
        PipelineKey key;
        key.programId = prog->id;
        key.rp = static_cast<uint8_t>(rp);
        key.blend = static_cast<uint8_t>(currentBlend_);
        key.depthTest = currentDepthTest_ ? 1 : 0;
        key.depthWrite = currentDepthWrite_ ? 1 : 0;
        key.cull = currentCull_;
        auto it = pipelines_.find(key);
        if (it != pipelines_.end()) return it->second;

        VkPipeline pipeline = CreatePipeline(prog, rp);
        if (!pipeline) return VK_NULL_HANDLE;
        pipelines_[key] = pipeline;
        return pipeline;
    }

    static VkFormat RpFormat(RpKind rp) {
        switch (rp) {
            // A2: float render passes carry SFLOAT attachments; pipelines and
            // render passes must agree on the actual target format.
            case RpKind::Float1:
            case RpKind::Float2:
            case RpKind::Float4: return VK_FORMAT_R16G16B16A16_SFLOAT;
            case RpKind::DepthOnly: return VK_FORMAT_UNDEFINED;
            default: return VK_FORMAT_R8G8B8A8_UNORM;
        }
    }

    VkPipeline CreatePipeline(Program* prog, RpKind rp) {
        const bool depthOnly = rp == RpKind::DepthOnly;

        VkPipelineShaderStageCreateInfo stages[2]{};
        stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
        stages[0].module = prog->vert;
        stages[0].pName = "main";
        stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
        stages[1].module = prog->frag;
        stages[1].pName = "main";

        std::vector<VkVertexInputBindingDescription> bindings;
        std::vector<VkVertexInputAttributeDescription> attributes;
        const bool coloredInst = prog->variant == VertexVariant::InstancedColored;
        if (prog->variant == VertexVariant::V3d || prog->variant == VertexVariant::Instanced ||
            coloredInst) {
            bindings.push_back({0, 80, VK_VERTEX_INPUT_RATE_VERTEX});
            attributes.push_back({0, 0, VK_FORMAT_R32G32B32_SFLOAT, 0});
            attributes.push_back({1, 0, VK_FORMAT_R32G32B32_SFLOAT, 12});
            attributes.push_back({2, 0, VK_FORMAT_R32G32_SFLOAT, 24});
            attributes.push_back({3, 0, VK_FORMAT_R32G32B32A32_SFLOAT, 32});
            attributes.push_back({4, 0, VK_FORMAT_R32G32B32A32_SFLOAT, 48});
            attributes.push_back({5, 0, VK_FORMAT_R32G32B32A32_SFLOAT, 64});
            if (prog->variant == VertexVariant::Instanced || coloredInst) {
                bindings.push_back({1, 64, VK_VERTEX_INPUT_RATE_INSTANCE});
                attributes[4] = {4, 1, VK_FORMAT_R32G32B32A32_SFLOAT, 0};
                attributes[5] = {5, 1, VK_FORMAT_R32G32B32A32_SFLOAT, 16};
                attributes.push_back({6, 1, VK_FORMAT_R32G32B32A32_SFLOAT, 32});
                attributes.push_back({7, 1, VK_FORMAT_R32G32B32A32_SFLOAT, 48});
            }
            if (coloredInst) {
                // Per-instance RGBA (GL attribute 8): binding 2, instance rate.
                bindings.push_back({2, 16, VK_VERTEX_INPUT_RATE_INSTANCE});
                attributes.push_back({8, 2, VK_FORMAT_R32G32B32A32_SFLOAT, 0});
            }
        } else if (prog->variant == VertexVariant::Ui) {
            bindings.push_back({0, 32, VK_VERTEX_INPUT_RATE_VERTEX});
            attributes.push_back({0, 0, VK_FORMAT_R32G32_SFLOAT, 0});
            attributes.push_back({1, 0, VK_FORMAT_R32G32_SFLOAT, 8});
            attributes.push_back({2, 0, VK_FORMAT_R32G32B32A32_SFLOAT, 16});
        } else {
            bindings.push_back({0, 28, VK_VERTEX_INPUT_RATE_VERTEX});
            attributes.push_back({0, 0, VK_FORMAT_R32G32B32_SFLOAT, 0});
            attributes.push_back({1, 0, VK_FORMAT_R32G32B32A32_SFLOAT, 12});
        }
        VkPipelineVertexInputStateCreateInfo vi{};
        vi.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
        vi.vertexBindingDescriptionCount = static_cast<uint32_t>(bindings.size());
        vi.pVertexBindingDescriptions = bindings.data();
        vi.vertexAttributeDescriptionCount = static_cast<uint32_t>(attributes.size());
        vi.pVertexAttributeDescriptions = attributes.data();

        VkPipelineInputAssemblyStateCreateInfo ia{};
        ia.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
        ia.topology = prog->variant == VertexVariant::Lines ? VK_PRIMITIVE_TOPOLOGY_LINE_LIST
                                                           : VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

        VkPipelineViewportStateCreateInfo vs{};
        vs.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
        vs.viewportCount = 1;
        vs.scissorCount = 1;

        VkPipelineRasterizationStateCreateInfo rs{};
        rs.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
        rs.polygonMode = VK_POLYGON_MODE_FILL;
        rs.cullMode = currentCull_ == 1 ? VK_CULL_MODE_BACK_BIT
                     : currentCull_ == 2 ? VK_CULL_MODE_FRONT_BIT
                                         : VK_CULL_MODE_NONE;
        rs.frontFace = prog->flipped ? VK_FRONT_FACE_CLOCKWISE : VK_FRONT_FACE_COUNTER_CLOCKWISE;
        rs.lineWidth = 1.0f;

        const uint32_t samples = rp == RpKind::Float4 ? 4 : (rp == RpKind::Float2 ? 2 : 1);
        VkPipelineMultisampleStateCreateInfo ms{};
        ms.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
        ms.rasterizationSamples = SamplesFlag(samples);

        VkPipelineDepthStencilStateCreateInfo ds{};
        ds.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
        ds.depthTestEnable = currentDepthTest_ ? VK_TRUE : VK_FALSE;
        ds.depthWriteEnable = currentDepthWrite_ ? VK_TRUE : VK_FALSE;
        ds.depthCompareOp = VK_COMPARE_OP_LESS;
        ds.back.compareOp = VK_COMPARE_OP_ALWAYS;
        ds.front.compareOp = VK_COMPARE_OP_ALWAYS;

        VkPipelineColorBlendAttachmentState cbAtt{};
        cbAtt.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                               VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
        switch (currentBlend_) {
            case BlendState::Alpha:
                cbAtt.blendEnable = VK_TRUE;
                cbAtt.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
                cbAtt.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
                cbAtt.colorBlendOp = VK_BLEND_OP_ADD;
                cbAtt.srcAlphaBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
                cbAtt.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
                cbAtt.alphaBlendOp = VK_BLEND_OP_ADD;
                break;
            case BlendState::Additive:
                cbAtt.blendEnable = VK_TRUE;
                cbAtt.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
                cbAtt.dstColorBlendFactor = VK_BLEND_FACTOR_ONE;
                cbAtt.colorBlendOp = VK_BLEND_OP_ADD;
                cbAtt.srcAlphaBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
                cbAtt.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
                cbAtt.alphaBlendOp = VK_BLEND_OP_ADD;
                break;
            case BlendState::Premultiplied:
                cbAtt.blendEnable = VK_TRUE;
                cbAtt.srcColorBlendFactor = VK_BLEND_FACTOR_ONE;
                cbAtt.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
                cbAtt.colorBlendOp = VK_BLEND_OP_ADD;
                cbAtt.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
                cbAtt.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
                cbAtt.alphaBlendOp = VK_BLEND_OP_ADD;
                break;
            case BlendState::Opaque:
            default:
                cbAtt.blendEnable = VK_FALSE;
                break;
        }
        VkPipelineColorBlendStateCreateInfo cb{};
        cb.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
        cb.attachmentCount = depthOnly ? 0 : 1;
        cb.pAttachments = depthOnly ? nullptr : &cbAtt;

        VkDynamicState dynamicStates[2] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
        VkPipelineDynamicStateCreateInfo dyn{};
        dyn.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
        dyn.dynamicStateCount = 2;
        dyn.pDynamicStates = dynamicStates;

        VkRenderPass renderPass = depthOnly ? depthOnlyPass_
                                            : GetRenderPasses(RpFormat(rp), samples).clear;

        VkGraphicsPipelineCreateInfo ci{};
        ci.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
        ci.stageCount = 2;
        ci.pStages = stages;
        ci.pVertexInputState = &vi;
        ci.pInputAssemblyState = &ia;
        ci.pViewportState = &vs;
        ci.pRasterizationState = &rs;
        ci.pMultisampleState = &ms;
        ci.pDepthStencilState = &ds;
        ci.pColorBlendState = &cb;
        ci.pDynamicState = &dyn;
        ci.layout = pipelineLayout_;
        ci.renderPass = renderPass;
        ci.subpass = 0;

        VkPipeline pipeline;
        if (vkCreateGraphicsPipelines(device_, pipelineCache_, 1, &ci, nullptr, &pipeline) !=
            VK_SUCCESS) {
            NEON_LOG_CAT(neon::core::LogCategory::Gfx, neon::core::LogLevel::Error,
                         "Vulkan: pipeline creation failed for '%s'", prog->name.c_str());
            return VK_NULL_HANDLE;
        }
        return pipeline;
    }

    // ------------------------------------------------------------------
    // Shader lookup
    // ------------------------------------------------------------------
    static const vk::VkShaderSpv* FindSpv(const char* name) {
        for (const auto& s : vk::kShaderTable) {
            if (std::strcmp(s.name, name) == 0) return &s;
        }
        return nullptr;
    }

    static bool IsFlippedProgram(const char* name) {
        static const char* kUnflipped[] = {
            "shadow",          "shadow_inst",   "shadow_skin",          "point_shadow",
            "point_shadow_inst", "point_shadow_skin", "bloom_bright",   "bloom_blur",
            "bloom_downsample", "bloom_upsample_add", "bloom_composite",
        };
        for (const char* u : kUnflipped) {
            if (std::strcmp(u, name) == 0) return false;
        }
        return true;
    }

    static VertexVariant VertexVariantFor(const char* name) {
        if (std::strcmp(name, "ui") == 0 || std::strcmp(name, "imgui") == 0)
            return VertexVariant::Ui;
        if (std::strcmp(name, "lines") == 0) return VertexVariant::Lines;
        if (std::strcmp(name, "unlit_instanced_colored") == 0)
            return VertexVariant::InstancedColored;
        if (std::strcmp(name, "lit_instanced") == 0 || std::strcmp(name, "unlit_instanced") == 0 ||
            std::strcmp(name, "shadow_inst") == 0 || std::strcmp(name, "point_shadow_inst") == 0)
            return VertexVariant::Instanced;
        return VertexVariant::V3d;
    }

    VkShaderModule CreateModule(const uint32_t* code, uint32_t wordCount) {
        VkShaderModuleCreateInfo ci{};
        ci.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
        ci.codeSize = wordCount * 4;
        ci.pCode = code;
        VkShaderModule m;
        if (vkCreateShaderModule(device_, &ci, nullptr, &m) != VK_SUCCESS) return VK_NULL_HANDLE;
        return m;
    }

    void ReadImage(VkImage image, VkImageLayout currentLayout, VkFormat format, int readW,
                   int readH, void* rgba, int x, int y, bool waitAcquire = false) {
        NEON_VK_TRACE("vt: ReadImage img=%p %dx%d at %d,%d\n", (void*)image, readW, readH, x, y);
        if (!rgba || readW <= 0 || readH <= 0) return;
        Frame& f = CurrentFrame();
        EndRenderPassIfActive(f);
        EnsureFrameStarted();
        OpenCmd(f);
        const bool halfFloat = format == VK_FORMAT_R16G16B16A16_SFLOAT;
        const size_t stagingStride = halfFloat ? 8 : 4; // A2: half4 is 8 B/px
        const size_t dataSize = static_cast<size_t>(readW) * readH * stagingStride;
        VkBuffer staging;
        VkDeviceMemory mem;
        if (!CreateHostBuffer(dataSize, VK_BUFFER_USAGE_TRANSFER_DST_BIT, &staging, &mem)) return;

        TransitionImage(f.cmd, image, currentLayout, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                        VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                        VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_SHADER_READ_BIT,
                        VK_ACCESS_TRANSFER_READ_BIT, VK_IMAGE_ASPECT_COLOR_BIT);
        VkBufferImageCopy region{};
        region.bufferOffset = 0;
        region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        region.imageOffset = {x, y, 0};
        region.imageExtent = {static_cast<uint32_t>(readW), static_cast<uint32_t>(readH), 1};
        vkCmdCopyImageToBuffer(f.cmd, image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, staging, 1,
                               &region);
        TransitionImage(f.cmd, image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                        VK_IMAGE_LAYOUT_GENERAL, VK_PIPELINE_STAGE_TRANSFER_BIT,
                        VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_ACCESS_TRANSFER_READ_BIT,
                        VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_SHADER_READ_BIT,
                        VK_IMAGE_ASPECT_COLOR_BIT);
        SetTrackedLayout(image, VK_IMAGE_LAYOUT_GENERAL);

        vkEndCommandBuffer(f.cmd);
        f.cmdOpen = false;
        SubmitQueue(f, f.cmd, waitAcquire);

        void* mapped = nullptr;
        vkMapMemory(device_, mem, 0, dataSize, 0, &mapped);
        if (format == VK_FORMAT_B8G8R8A8_UNORM || format == VK_FORMAT_B8G8R8A8_SRGB) {
            const uint8_t* src = static_cast<const uint8_t*>(mapped);
            uint8_t* dst = static_cast<uint8_t*>(rgba);
            const int count = readW * readH;
            for (int i = 0; i < count; ++i) {
                dst[i * 4 + 0] = src[i * 4 + 2];
                dst[i * 4 + 1] = src[i * 4 + 1];
                dst[i * 4 + 2] = src[i * 4 + 0];
                dst[i * 4 + 3] = src[i * 4 + 3];
            }
        } else if (halfFloat) {
            // A2: convert R16G16B16A16_SFLOAT (half precision) to RGBA8 so
            // HDR readbacks (self-test probes, screenshots) see real values
            // instead of raw half bytes.
            const uint16_t* src = static_cast<const uint16_t*>(mapped);
            uint8_t* dst = static_cast<uint8_t*>(rgba);
            const int count = readW * readH;
            for (int i = 0; i < count; ++i) {
                for (int c = 0; c < 4; ++c) {
                    const float v = HalfToFloat(src[i * 4 + c]);
                    float clamped = v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v);
                    dst[i * 4 + c] = static_cast<uint8_t>(clamped * 255.0f + 0.5f);
                }
            }
        } else {
            std::memcpy(rgba, mapped, dataSize);
        }
        vkUnmapMemory(device_, mem);
        vkDestroyBuffer(device_, staging, nullptr);
        vkFreeMemory(device_, mem, nullptr);
    }

    // ------------------------------------------------------------------
    // Members
    // ------------------------------------------------------------------
    platform::IWindow* window_ = nullptr;
    HWND hwnd_ = nullptr;
    VkInstance instance_ = VK_NULL_HANDLE;
    VkPhysicalDevice physicalDevice_ = VK_NULL_HANDLE;
    VkDevice device_ = VK_NULL_HANDLE;
    VkSurfaceKHR surface_ = VK_NULL_HANDLE;
    VkSwapchainKHR swapchain_ = VK_NULL_HANDLE;
    VkQueue queue_ = VK_NULL_HANDLE;
    uint32_t queueFamily_ = UINT32_MAX;
    VkFormat swapchainFormat_ = VK_FORMAT_B8G8R8A8_UNORM;
    VkFormat depthFormat_ = VK_FORMAT_D32_SFLOAT;
    uint32_t minUboAlignment_ = 256;
    VkSampleCountFlags maxSampleCounts_ = VK_SAMPLE_COUNT_1_BIT;
    uint32_t imageIndex_ = 0;
    uint32_t frameIndex_ = 0;
    bool inFrame_ = false;
    bool frameReady_ = false;
    bool acquired_ = false;

    std::vector<VkImage> swapImages_;
    std::unique_ptr<Target> swapTarget_;
    VkImage swapDepthImage_ = VK_NULL_HANDLE;
    VkDeviceMemory swapDepthMem_ = VK_NULL_HANDLE;
    VkImageView swapDepthView_ = VK_NULL_HANDLE;
    VkImageLayout swapImageLayout_ = VK_IMAGE_LAYOUT_UNDEFINED;
    VkImageLayout swapDepthLayout_ = VK_IMAGE_LAYOUT_UNDEFINED;

    std::vector<Frame> frames_;
    std::unordered_map<uint32_t, Program> programs_;
    std::unordered_map<PipelineKey, VkPipeline, PipelineKeyHash> pipelines_;
    std::unordered_map<RenderPassKey, RenderPassPair, RenderPassKeyHash> renderPasses_;
    std::unordered_map<uint32_t, Mesh> meshes_;
    std::unordered_map<uint32_t, Texture> textures_;
    std::unordered_map<uint32_t, Target> renderTargets_;
    std::unordered_map<Frame::TexKey, VkDescriptorSet, Frame::TexKeyHash> texSetCache_;
    uint32_t nextShaderId_ = 0;
    uint32_t nextMeshId_ = 0;
    uint32_t nextTextureId_ = 0;
    uint32_t nextTargetId_ = 0;

    VkDescriptorSetLayout descLayoutSet0_ = VK_NULL_HANDLE;
    VkDescriptorSetLayout descLayoutSet1_ = VK_NULL_HANDLE;
    VkPipelineLayout pipelineLayout_ = VK_NULL_HANDLE;
    VkPipelineCache pipelineCache_ = VK_NULL_HANDLE;
    VkRenderPass depthOnlyPass_ = VK_NULL_HANDLE;
    VkSampler samplerNearest_ = VK_NULL_HANDLE;
    VkSampler samplerLinear_ = VK_NULL_HANDLE;
    VkSampler samplerRepeatNearest_ = VK_NULL_HANDLE;
    VkSampler samplerRepeatLinear_ = VK_NULL_HANDLE;

    TextureHandle white_;
    ShaderHandle currentShader_;
    // Backend-internal depth-resolve pass (see ResolveDepth).
    ShaderHandle depthResolveShader_;
    MeshHandle resolveQuad_;
    uint32_t currentProgramId_ = 0;
    BlendState currentBlend_ = BlendState::Opaque;
    bool currentDepthTest_ = false;
    bool currentDepthWrite_ = true;
    uint8_t currentCull_ = 0;
    int viewportWidth_ = 1280;
    int viewportHeight_ = 720;
    int viewportX_ = 0;
    int viewportY_ = 0;
    bool scissorEnabled_ = false;
    int scissorX_ = 0;
    int scissorY_ = 0;
    int scissorW_ = 0;
    int scissorH_ = 0;

    uint8_t uniforms_[kUniformBlockSize] = {};
    bool uniformsDirty_ = false;
    uint32_t boundTextures_[kMaxSamplerSlots] = {};
    bool texSetsDirty_ = false;
    uint64_t lastUboOffset_ = 0;
    VkDescriptorSet lastTexSet_ = VK_NULL_HANDLE;
    Target* target_ = nullptr;
    bool rpActive_ = false;
    bool compressedTexSupported_ = false;
    bool depthUsable_ = true;
};

} // namespace

std::unique_ptr<IRenderBackend> CreateVulkanBackend() {
    return std::make_unique<VulkanBackend>();
}

} // namespace neon::gfx
