# Renderer 拆分 + Render Graph 设计

日期：2026-09-01
状态：待批准
前置：GameRuntime 组合服务化已完成（C1/C2），Renderer 是最后一个上帝类（632 头 + 2891 实现）

## 1. 目标

把 `gfx::Renderer` 拆成「门面 + 组合服务」，并引入 **FrameGraph**（Frostbite 式渲染资源 DAG）：
后处理链从手写 if-else（`EnsurePostTargets` → `RunSsaoPass` → `RunVolumetricPass` →
`RunSsrPass` → `RunBloom` → `Composite`）改为**声明式 pass 图**，自动管理临时 RT 生命周期。

## 2. 现状（已核实）

- `renderer.hpp` 668 行、`renderer.cpp` 2891 行。
- 后处理链 pass：`RunSceneDepthPass`/`RunSsaoPass`/`RunVolumetricPass`/`RunSsrPass`/
  `RunBloom`/`CompositeToBackbuffer`，手写顺序 + 手动管理 RT（`hdrRT_`/`bloomHalfA_`/
  `aoRT_`/`volRT_`/`ssrRT_` 等）+ `*RanThisFrame_` 标志防重跑。
- 阴影（CSM/点阴影）、主场景绘制（DrawMesh/Instanced/Billboards）是另一性质（直接绘制，非全屏后处理）。

## 3. 设计

### 3.1 架构：Renderer 门面 + 组合服务

```
gfx::Renderer（门面，公开 API 全兼容，调用方零改动）
├─ PostProcessGraph（FrameGraph 实例，持有全部后处理 RT + pass）
├─ ShadowSystem      CSM + 点阴影（shadowRT_/shadowCasters_/RunShadowPass）
├─ SceneState        相机/光照/天空/雾/IBL
└─ DrawBatch2D       quad/billboard 批 + Flush2D
```

Renderer 保留 `DrawMesh`/`BeginFrame`/`EndFrame`/`EndScene` 等公开 API（转发），
`GameRuntime::DrawSystem`/`editor` 零改动。

### 3.2 FrameGraph 核心（`neon::gfx::FrameGraph`）

```cpp
namespace neon::gfx {

struct TextureDesc { uint32_t w, h; // + format/samples 等（对齐 IRenderBackend::CreateTexture） };

// 一个渲染 pass 的声明：输入/输出资源 + 执行体。
struct Pass {
    std::string name;
    std::vector<ResourceId> reads;   // 输入资源
    std::vector<ResourceId> writes;  // 输出资源（含 overdraw）
    std::function<void(FrameGraphContext&)> execute;
};

class FrameGraph {
public:
    // 声明一个资源（返回 handle，跨 pass 引用）。
    ResourceId AddResource(const TextureDesc& desc);
    // 注册一个 pass（DAG 边来自 reads/writes）。返回 false 表示环/缺资源。
    bool AddPass(Pass pass);
    // 拓扑排序 + 分配临时资源 + 按序执行。每帧调用。
    bool Execute(IRenderBackend& backend);
    // 临时资源池：按 (w,h,format) 复用 RT，pass 间自动过渡，图外释放。
    void ResetFrame();

private:
    std::vector<Pass> passes_;
    std::vector<TextureDesc> resources_;
    // 资源生命周期：引用计数（被 pass 读写），执行时惰性创建、结束释放/复用。
    std::vector<RenderTargetHandle> transientRTs_;
};
} // namespace neon::gfx
```

**执行模型**：
- `AddPass` 记录 `reads`/`writes`，`Execute` 做拓扑排序（Kahn，复用 microkernel `ModuleRegistry` 的模式）。
- 每个资源按"最后使用 pass"引用计数：首次使用创建、最后一次使用后回收到临时池。
- `FrameGraphContext` 给 pass 提供 `GetInput(id)`/`GetOutput(id)`（返回 `RenderTargetHandle`）+ `backend`。

### 3.3 后处理链改造（PostProcessGraph）

把现有后处理链改造成 graph 的 pass：

| Pass | 输入 | 输出 |
|---|---|---|
| `depth`（SSAO 前置） | — | `sceneDepth` |
| `ssao` | `sceneDepth`, `hdrScene` | `ao` |
| `volumetric` | `hdrScene`, `shadow` | `vol` |
| `ssr` | `hdrScene`, `sceneDepth` | `ssr` |
| `bloom` | `hdrScene` | `bloomAcc` |
| `composite` | `hdrScene`, `ao`, `vol`, `ssr`, `bloomAcc` | backbuffer |

`*RanThisFrame_` 标志语义由 graph 的"pass 只执行一次/可禁用"替代（pass 带 `enabled` 开关）。

**渲染流程（EndScene/CompositeFrame）**：
```
ResolveMainTarget → postGraph_.Execute(backend) → 2D HUD 直绘 backbuffer
```

### 3.4 阴影系统（ShadowSystem）

CSM + 点阴影从 Renderer 抽出：`RunShadowPass`/`RunPointShadowPass`/`shadowRT_`/
`shadowCasters_`/`lightViewProj_` 迁入，Renderer 转发 `SetShadowsEnabled` 等。阴影是
"直接绘制"非"全屏 pass"，不入 FrameGraph（YAGNI）。

### 3.5 场景状态（SceneState）

相机/光照/天空/雾/IBL（`camera_`/`sunDir_`/`sky`/`fog`/`ibl*`）迁入。`SetCamera`/
`SetSky`/`SetDirectionalLight`/`SetIblStrength` 等转发。IBL 重算逻辑（`RecomputeIbl`）一并迁入。

### 3.6 DrawBatch2D

`Flush2D`/`PushQuad*`/billboard 批 + 2D 顶点缓冲迁入。`DrawBillboards` 转发。

## 4. 分阶段实施

- **阶段 1**：`FrameGraph` 核心（`AddResource`/`AddPass`/`Execute` + 临时 RT 池）+ 把
  **bloom 链**（bright→downsample→blur→upsample-add）改造成 graph 验证框架 + 单测。
- **阶段 2**：SSAO/SSR/volumetric/depth pass 改造成 graph。
- **阶段 3**：composite 进 graph；`*RanThisFrame_` 语义清理；Renderer 拆出
  `ShadowSystem`/`SceneState`/`DrawBatch2D` 组合服务（门面化）。
- 每阶段独立 commit，`neon_tests` 全绿；`neon_editor --smoke-test 240` + `CaptureBloomComparison`
  截图对比验证渲染等价。

## 5. 验收

1. `neon_tests` 全绿 + `neon_editor --smoke-test 240` 通过。
2. 后处理链由 FrameGraph 驱动，`Ensure*Targets`/`*RanThisFrame_` 手工管理消除。
3. `renderer.cpp` 2891 → <1500 行（后处理迁入 graph 系统）。
4. `CaptureBloomComparison`/`CaptureTonemapComparison`（editor 后处理对比截图）逐像素一致。
5. 渲染行为等价（无可见回归）。

## 6. 风险

- **渲染等价性**是最大风险：graph 的资源复用/时序必须与原手写链一致（尤其
  `ResolveMainTarget`/`compositedThisFrame_` 的 MSAA resolve 时序）。靠截图对比 + 冒烟兜底。
- 临时 RT 池的复用策略（尺寸/格式匹配）需谨慎，避免跨帧污染。
- FrameGraph 是重架构，阶段 1 先用 bloom 链验证框架可行，再铺开。
