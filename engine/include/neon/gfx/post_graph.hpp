#pragma once
#include <cstddef>
#include <functional>

#include "neon/gfx/backend.hpp"
#include "neon/gfx/bloom.hpp"
#include "neon/gfx/camera.hpp"
#include "neon/gfx/frame_graph.hpp"
#include "neon/math/math.hpp"

namespace neon::gfx {

// 统一后处理 FrameGraph（Task 3 的 depth/ssao/vol/ssr 链 + Task 2 的 bloom 链 +
// Task 4 的 composite）：深度预 pass（直接绘制 casters）、SSAO 计算 + 模糊、
// 体积光 + 模糊、SSR + 模糊、bloom（bright → blur h/v → downsample → blur h/v
// → upsample-add）以及最终 composite 全部建模为声明式 pass。临时 RT（深度/AO/
// 体积/SSR/bloom 金字塔及各自的 blur 缓冲）由 graph 的池管理——原手写的
// ssaoDepthRT_/aoRT_/aoBlurA_/B_、volRT_/volBlurA_/B_、ssrRT_/ssrBlurA_/B_、
// bloomHalfA_/B_/QuarterA_/B_ 即对应图内资源。
//
// 外部输入：hdrScene（主场景 HDR 颜色 RT，图外资源，Execute 时注入 volumetric /
// ssr / bright / composite 采样）。输出：backbuffer（图外目标）——composite pass
// 在 execute 里 BindDefaultTarget() 直接画到默认目标，因此各链最终 RT（sceneDepth、
// 原始 ao、volBlurB、ssrBlurB、bloomAcc）由 composite 作为图内输入消费，无需导出。
//
// 使用约定：Build() 在分辨率确定/变化时调用（重建前先 Destroy 释放旧 RT）。
// 每帧 Execute() 一次（一次执行整条链，composite 是最后一 pass），帧末
// ResetFrame() 归还临时 RT 池（renderer 在 BeginFrame 调用）。pass 之间的执行序由
// FrameGraph 的版本化读取保证：depth 先于任何读 sceneDepth 的 pass；各链互相独立，
// composite 依赖所有链的 final，故排最后。
class PostGraph {
public:
    // Build() 所需的 shader/纹理句柄集（均来自 Renderer 的 InitBuiltinResources）。
    struct Shaders {
        ShaderHandle ssaoShader;      // AO 计算
        ShaderHandle ssaoBlur;        // AO/体积/SSR 共享的分离式高斯模糊
        ShaderHandle volumetricShader;
        ShaderHandle ssrShader;
        ShaderHandle brightPass;      // bloom bright
        ShaderHandle blur;            // bloom 分离式模糊
        ShaderHandle downsample;
        ShaderHandle upsampleAdd;
        ShaderHandle compositeShader; // 最终 composite
        TextureHandle white;          // 未启用链的占位纹理（composite 绑定）
    };

    // composite pass 的每帧状态（Execute 时快照；CaptureBloom/TonemapComparison
    // 在同一帧内改 exposure/tonemap 开关后再次 Execute，故必须逐次传入）。
    struct CompositeParams {
        float ssaoIntensity = 1.0f;
        float volStrength = 1.0f;
        float ssrStrength = 1.0f;
        bool volumetricFog = false; // 体积雾（composite 读 sceneDepth）
        math::Vec3 fogColor{};
        float fogDensity = 0.02f;
        float exposure = 1.0f;
        bool tonemapEnabled = true;
        // Data-driven bloom (A/RenderStack): bright threshold + add-back strength,
        // defaulting to the original constants so existing scenes are unchanged.
        float bloomThreshold = kBloomThreshold;
        float bloomStrength = kBloomStrength;
        TextureHandle white; // 与 Shaders::white 同源（Execute 逐帧重给，防句柄过期）
    };

    // 每帧的状态输入：各链是否启用 + 链需要的场景状态。depthPass 表示需要
    // 深度预 pass（ssao/ssr 或体积雾任一启用）；ssaoPass 额外要求场景有 caster
    // （无 caster 时 AO 结果无意义，与原 RunSsaoPass 的空 caster 短路一致）。
    struct FrameParams {
        RenderTargetHandle hdrScene; // 主场景 HDR 颜色（图外）
        int hdrW = 0;
        int hdrH = 0;
        bool depthPass = false;
        bool ssaoPass = false;
        bool volumetricPass = false;
        bool ssrPass = false;
        bool bloomPass = false;
        math::Vec3 camPos; // volumetric 太阳投影
        math::Vec3 sunDir;
        math::Vec3 sunColor; // volumetric light-shaft colour (sun * intensity)
        math::Mat4 viewProj;
        Camera camera; // SSR / composite 的 near/far
        CompositeParams composite;
    };

    // 声明资源 + 注册 18 个 pass（shaders/mesh 来自 Renderer）：10 个 post 链
    // pass + 7 个 bloom 链 pass + 1 个 composite pass。每次调用都重建一个全新图；
    // 调用前必须 Destroy() 释放旧图持有的 RT，避免 GPU 泄漏。
    // drawDepthCasters 是 depth pass 的"直接绘制"回调：由 Renderer 注入
    // `[this]{ DrawSsaoDepthCasters(viewProj_); }`，在 depth pass 绑定并清空
    // sceneDepth 后把场景几何画进当前 RT（不是全屏 quad）。
    void Build(const Shaders& shaders, MeshHandle postQuad, int w, int h,
               std::function<void()> drawDepthCasters);

    // 释放本图持有的全部 RT（分辨率重建 / renderer 销毁时）。
    void Destroy(IRenderBackend& backend);

    // 执行整条后处理链：把 hdrScene 注入，按 params 的开关启用各链并跑图，
    // composite 在最后一 pass 把结果画到默认目标（backbuffer）。返回 true 表示
    // 图本身执行成功（任一链实际运行与否见各 SsaoRan/... 查询）。
    bool Execute(IRenderBackend& backend, const FrameParams& params);

    // 帧末把本帧仍存活的目标归还池（renderer 在 BeginFrame 调用；同时清除
    // CompositeRan() 的"本帧已 composite"闩存）。
    void ResetFrame() {
        graph_.ResetFrame();
        compositeRan_ = false;
    }

    // 上一次 Execute 中实际运行（启用且图成功）的链。
    bool Ran() const { return ran_; }
    bool DepthRan() const { return depthRan_; }
    bool SsaoRan() const { return ssaoRan_; }
    bool VolumetricRan() const { return volRan_; }
    bool SsrRan() const { return ssrRan_; }
    bool BloomRan() const { return bloomRan_; }
    // True 表示上一次 Execute 成功结束（composite pass 已把结果画到 backbuffer）。
    // Renderer 用它在 EndScene/CompositeFrame 里作"本帧已 composite"闩存
    // （替代旧的 compositedThisFrame_），避免帧内二次 composite。
    bool CompositeRan() const { return compositeRan_; }

    // 上一次 Execute 的 pass 执行序 + 各 pass 输入/输出 RT（测试验证用）。
    const std::vector<FrameGraphPassTrace>& LastTrace() const { return graph_.LastTrace(); }
    size_t PassCount() const { return graph_.PassCount(); }

private:
    void Fullscreen(IRenderBackend& backend, ShaderHandle shader);

    FrameGraph graph_;
    ResourceId hdrScene_ = kInvalidResource; // 外部输入（主场景 HDR）
    ResourceId sceneDepth_ = kInvalidResource; // 全尺寸，depth pass 写
    ResourceId ao_ = kInvalidResource;         // 半尺寸，ssao 写
    ResourceId aoBlurA_ = kInvalidResource;
    ResourceId aoBlurB_ = kInvalidResource;
    ResourceId vol_ = kInvalidResource; // 半尺寸，volumetric 写
    ResourceId volBlurA_ = kInvalidResource;
    ResourceId volBlurB_ = kInvalidResource;
    ResourceId ssr_ = kInvalidResource; // 半尺寸，ssr 写
    ResourceId ssrBlurA_ = kInvalidResource;
    ResourceId ssrBlurB_ = kInvalidResource;
    ResourceId bloomHalfA_ = kInvalidResource;    // 半尺寸，bright → blurV → blurH 采样
    ResourceId bloomHalfB_ = kInvalidResource;    // == bloomAcc_（upsample-add 输出）
    ResourceId bloomQuarterA_ = kInvalidResource; // 四分之一尺寸
    ResourceId bloomQuarterB_ = kInvalidResource;
    size_t depthPassIndex_ = 0;
    size_t ssaoPassIndex_ = 0;
    size_t ssaoBlurHIndex_ = 0;
    size_t ssaoBlurVIndex_ = 0;
    size_t volPassIndex_ = 0;
    size_t volBlurHIndex_ = 0;
    size_t volBlurVIndex_ = 0;
    size_t ssrPassIndex_ = 0;
    size_t ssrBlurHIndex_ = 0;
    size_t ssrBlurVIndex_ = 0;
    size_t brightPassIndex_ = 0;
    size_t blurHalfHIndex_ = 0;
    size_t blurHalfVIndex_ = 0;
    size_t downsamplePassIndex_ = 0;
    size_t blurQuarterHIndex_ = 0;
    size_t blurQuarterVIndex_ = 0;
    size_t upsampleAddIndex_ = 0;
    size_t compositePassIndex_ = 0;
    ShaderHandle ssaoShader_;
    ShaderHandle ssaoBlur_;
    ShaderHandle volumetricShader_;
    ShaderHandle ssrShader_;
    ShaderHandle bright_;
    ShaderHandle blur_;
    ShaderHandle downsample_;
    ShaderHandle upsampleAdd_;
    ShaderHandle compositeShader_;
    MeshHandle postQuad_;
    std::function<void()> drawDepthCasters_;
    float halfTexelX_ = 0.0f;
    float halfTexelY_ = 0.0f;
    float quarterTexelX_ = 0.0f;
    float quarterTexelY_ = 0.0f;
    int hdrW_ = 0;
    int hdrH_ = 0;
    math::Vec2 sunUV_{0.5f, 0.5f}; // volumetric 太阳屏幕 UV（Execute 时重算）
    // Volume god-ray ray-march state (Execute snapshot from FrameParams).
    math::Mat4 viewProj_;  // camera view-proj (invert in shader for view ray)
    math::Vec3 camPos_{};
    math::Vec3 sunDir_{0.0f, -1.0f, 0.0f};
    math::Vec3 sunColor_{1.0f, 1.0f, 1.0f};
    float nearPlane_ = 0.1f;       // SSR / composite 的 near/far（Execute 时从 camera 复制）
    float farPlane_ = 800.0f;
    CompositeParams comp_; // 上一次 Execute 的 composite 快照
    bool built_ = false;
    bool ran_ = false;
    bool depthRan_ = false;
    bool ssaoRan_ = false;
    bool volRan_ = false;
    bool ssrRan_ = false;
    bool bloomRan_ = false;
    bool compositeRan_ = false;
};

} // namespace neon::gfx
