#pragma once
#include "neon/gfx/backend.hpp"
#include "neon/gfx/frame_graph.hpp"

namespace neon::gfx {

// bloom 链的 FrameGraph 封装：bright → blur(h/v) → downsample → blur(h/v) →
// upsample-add，全部建模为声明式 pass，临时 RT 由 graph 的池管理（原手写链的
// bloomHalfA_/B_/QuarterA_/B_ 即对应 graph 内资源）。
//
// 外部输入：hdrScene（主场景 HDR 颜色 RT，图外资源，Execute 时注入 bright
// pass）。输出：bloomAcc（半分辨率累加 bloom，导出资源，Execute 与 ResetFrame
// 之间经 BloomColorTexture 供 composite 采样）。
//
// 使用约定：Build() 在分辨率确定/变化时调用（重建图；重建前先 Destroy 释放旧
// RT）。每帧 Execute() 一次，帧末 ResetFrame() 归还临时 RT 池（renderer 在
// BeginFrame 调用）。
class BloomGraph {
public:
    // 声明资源 + 注册 7 个 pass（shaders/mesh 来自 Renderer，hdrW/hdrH 用于声明
    // 半/四分之一分辨率资源）。每次调用都重建一个全新图；调用前必须 Destroy()
    // 释放旧图持有的 RT，避免 GPU 泄漏。
    void Build(ShaderHandle brightPass, ShaderHandle blur, ShaderHandle downsample,
               ShaderHandle upsampleAdd, MeshHandle postQuad, int hdrW, int hdrH);

    // 释放本图持有的全部 RT（分辨率重建 / renderer 销毁时）。
    void Destroy(IRenderBackend& backend);

    // 执行 bloom 链：把 hdrScene RT 的颜色纹理注入 bright pass，结果留在
    // bloomAcc。hdrW/hdrH 用于计算 texel 尺寸（须与 Build 时的分辨率一致）。
    // enabled=false 跳过（返回 false，bloom 未运行）。返回 true 表示 bloom 链
    // 本帧实际执行过（composite 据此决定是否加 bloom 项）。
    bool Execute(IRenderBackend& backend, RenderTargetHandle hdrScene, int hdrW, int hdrH,
                 bool enabled);

    // 帧末把本帧仍存活的目标归还池（在 composite 采样完之后调用，renderer 在
    // BeginFrame 调用）。
    void ResetFrame() { graph_.ResetFrame(); }

    // composite 采样 bloom 结果：返回 bloomAcc 的颜色纹理（Execute 与
    // ResetFrame 之间有效；未运行/不可用时返回无效句柄）。
    TextureHandle BloomColorTexture(IRenderBackend& backend) const;

    // 上一次 Execute 的 pass 执行序 + 各 pass 输入/输出 RT（测试验证用）。
    const std::vector<FrameGraphPassTrace>& LastTrace() const { return graph_.LastTrace(); }
    bool Ran() const { return ran_; }
    size_t PassCount() const { return graph_.PassCount(); }

private:
    void Fullscreen(IRenderBackend& backend, ShaderHandle shader);

    FrameGraph graph_;
    ResourceId hdrScene_ = kInvalidResource;  // 外部输入（主场景 HDR）
    ResourceId halfA_ = kInvalidResource;     // 1/2 分辨率：bright → blurV → blurH 采样
    ResourceId halfB_ = kInvalidResource;     // 1/2 分辨率：blurH 输出 → upsample-add 输出
    ResourceId quarterA_ = kInvalidResource;  // 1/4 分辨率：downsample → blurV
    ResourceId quarterB_ = kInvalidResource;  // 1/4 分辨率：blurH 输出
    ResourceId bloomAcc_ = kInvalidResource;  // == halfB_（upsample-add 输出，导出）
    ShaderHandle bright_;
    ShaderHandle blur_;
    ShaderHandle downsample_;
    ShaderHandle upsampleAdd_;
    MeshHandle postQuad_;
    float halfTexelX_ = 0.0f;
    float halfTexelY_ = 0.0f;
    float quarterTexelX_ = 0.0f;
    float quarterTexelY_ = 0.0f;
    bool built_ = false;
    bool ran_ = false;
};

} // namespace neon::gfx
