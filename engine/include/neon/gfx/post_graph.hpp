#pragma once
#include <cstddef>
#include <functional>

#include "neon/gfx/backend.hpp"
#include "neon/gfx/camera.hpp"
#include "neon/gfx/frame_graph.hpp"
#include "neon/math/math.hpp"

namespace neon::gfx {

// G1-5 SSAO / volumetric / SSR / scene-depth 链的 FrameGraph 封装（与
// BloomGraph 并列）：深度预 pass（直接绘制 casters）、SSAO 计算 + 模糊、
// 体积光 + 模糊、SSR + 模糊全部建模为声明式 pass。临时 RT（深度/AO/体积/SSR
// 及其 blur 缓冲）由 graph 的池管理——原手写的 ssaoDepthRT_/aoRT_/aoBlurA_/B_、
// volRT_/volBlurA_/B_、ssrRT_/ssrBlurA_/B_ 即对应图内资源。
//
// 外部输入：hdrScene（主场景 HDR 颜色 RT，图外资源，Execute 时注入 volumetric
// 与 ssr pass）。输出：sceneDepth（全尺寸，composite 的体积雾读它）、ao（原始
// AO，composite 直接采样）、volBlurB/ssrBlurB（模糊后的最终体积光/反射），
// 均为导出资源（ExportResource），在 Execute 与 ResetFrame 之间经对应访问器供
// composite 采样。
//
// 使用约定：Build() 在分辨率确定/变化时调用（重建前先 Destroy 释放旧 RT）。
// 每帧 Execute() 一次，帧末 ResetFrame() 归还临时 RT 池（renderer 在 BeginFrame
// 调用）。pass 之间的执行序由 FrameGraph 的版本化读取保证：depth 先于任何读
// sceneDepth 的 pass；ssao/vol/ssr 三条链互相独立。
class PostGraph {
public:
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
        math::Vec3 camPos; // volumetric 太阳投影
        math::Vec3 sunDir;
        math::Mat4 viewProj;
        Camera camera; // SSR 的 near/far
    };

    // 声明资源 + 注册 10 个 pass（shaders/mesh 来自 Renderer）。每次调用都
    // 重建一个全新图；调用前必须 Destroy() 释放旧图持有的 RT，避免 GPU 泄漏。
    // drawDepthCasters 是 depth pass 的"直接绘制"回调：由 Renderer 注入
    // `[this]{ DrawSsaoDepthCasters(viewProj_); }`，在 depth pass 绑定并清空
    // sceneDepth 后把场景几何画进当前 RT（不是全屏 quad）。
    void Build(ShaderHandle ssaoShader, ShaderHandle ssaoBlur, ShaderHandle volumetricShader,
               ShaderHandle ssrShader, MeshHandle postQuad, int w, int h,
               std::function<void()> drawDepthCasters);

    // 释放本图持有的全部 RT（分辨率重建 / renderer 销毁时）。
    void Destroy(IRenderBackend& backend);

    // 执行 post 链：把 hdrScene 注入，按 params 的开关启用各链并跑图。返回
    // true 表示图本身执行成功（任一链实际运行与否见各 SsaoRan/... 查询）。
    bool Execute(IRenderBackend& backend, const FrameParams& params);

    // 帧末把本帧仍存活的目标归还池（在 composite 采样完之后调用，renderer 在
    // BeginFrame 调用）。
    void ResetFrame() { graph_.ResetFrame(); }

    // composite 采样各链最终 RT 的颜色纹理（Execute 与 ResetFrame 之间有效；
    // 对应链未运行/不可用时返回无效句柄）。
    TextureHandle SceneDepthTexture(IRenderBackend& backend) const;
    TextureHandle AoTex(IRenderBackend& backend) const;
    TextureHandle VolTex(IRenderBackend& backend) const;
    TextureHandle SsrTex(IRenderBackend& backend) const;

    // 上一次 Execute 中实际运行（启用且图成功）的链。
    bool Ran() const { return ran_; }
    bool DepthRan() const { return depthRan_; }
    bool SsaoRan() const { return ssaoRan_; }
    bool VolumetricRan() const { return volRan_; }
    bool SsrRan() const { return ssrRan_; }

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
    ShaderHandle ssaoShader_;
    ShaderHandle ssaoBlur_;
    ShaderHandle volumetricShader_;
    ShaderHandle ssrShader_;
    MeshHandle postQuad_;
    std::function<void()> drawDepthCasters_;
    float halfTexelX_ = 0.0f;
    float halfTexelY_ = 0.0f;
    int hdrW_ = 0;
    int hdrH_ = 0;
    math::Vec2 sunUV_{0.5f, 0.5f}; // volumetric 太阳屏幕 UV（Execute 时重算）
    float nearPlane_ = 0.1f;       // SSR 的 near/far（Execute 时从 camera 复制）
    float farPlane_ = 800.0f;
    bool built_ = false;
    bool ran_ = false;
    bool depthRan_ = false;
    bool ssaoRan_ = false;
    bool volRan_ = false;
    bool ssrRan_ = false;
};

} // namespace neon::gfx
