# Renderer FrameGraph 实施计划

> **For Claude:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task.

**Goal:** 引入 Frostbite 式 FrameGraph（声明式渲染 pass 图，自动管理临时 RT 生命周期），把后处理链改造为 graph；Renderer 拆成门面 + 组合服务。

**Architecture:** `FrameGraph` 类（`AddResource`/`AddPass`/`Execute` + 临时 RT 池），后处理链（depth/ssao/volumetric/ssr/bloom/composite）建模为 pass DAG；Renderer 门面化 + `ShadowSystem`/`SceneState`/`DrawBatch2D` 组合服务。

**Tech Stack:** C++17、CMake + MSVC、`neon_tests`（CHECK 宏 + NullBackend）、`neon_editor --smoke-test`。

**关键上下文（已核实）：**
- `IRenderBackend` 有 `CreateRenderTarget(w,h,...)`/`DestroyRenderTarget`/`BindRenderTarget`/`ResolveRenderTarget`/`RenderTargetColorTexture`（backend.hpp:86-98）。
- 后处理链在 renderer.cpp：`EnsurePostTargets`(2478)/`RunBloom`(2649)/`CompositeToBackbuffer`(2735)/`RunSsaoPass`(1346)/`RunVolumetricPass`(1406)/`RunSsrPass`(1475)/`RunSceneDepthPass`(1333)。
- 执行入口：`EndScene`(2819)/`CompositeFrame`(2834)/`CompositeSceneToBackbuffer`(2802)。`compositedThisFrame_` 防重跑。
- `RunBloom` 用 `bloomHalfA_/B_/QuarterA_/B_`（RGBA16F 金字塔）。
- 现有测试后端：`tests/test_backend.hpp` 的 NullBackend。

**构建/测试命令（全程）：**
- `cmake --build build-msvc --config Release`
- `& "build-msvc\Release\neon_tests.exe"`（基线 743 全绿）
- 渲染等价验证：`neon_editor --smoke-test 240` + `CaptureBloomComparison`（editor 后处理对比截图，若有 CLI 入口）

---

## 阶段 1：FrameGraph 核心 + bloom 链验证（验证框架可行）

### Task 1: `FrameGraph` 核心

**Files:**
- Create: `engine/include/neon/gfx/frame_graph.hpp`
- Create: `engine/src/gfx/frame_graph.cpp`
- Modify: `CMakeLists.txt`（neon_gfx 加 frame_graph.cpp）

**Step 1:** 头文件：

```cpp
#pragma once
#include <functional>
#include <string>
#include <unordered_map>
#include <vector>
#include "neon/gfx/backend.hpp"

namespace neon::gfx {

using ResourceId = uint32_t;

struct FrameGraphResourceDesc {
    uint32_t width = 0, height = 0;
    uint32_t format = 0;      // 对齐 IRenderBackend 的格式约定（先 0 = RGBA16F）
    uint32_t samples = 1;
};

// 执行上下文：pass 内通过它拿输入/输出 RT 并绘制。
class FrameGraphContext {
public:
    FrameGraphContext(IRenderBackend& backend) : backend_(backend) {}
    IRenderBackend& Backend() { return backend_; }
    void SetOutput(ResourceId id, RenderTargetHandle rt) { outputs_[id] = rt; }
    RenderTargetHandle GetInput(ResourceId id) const { return inputs_.at(id); }
    RenderTargetHandle GetOutput(ResourceId id) const { return outputs_.at(id); }
    void SetInput(ResourceId id, RenderTargetHandle rt) { inputs_[id] = rt; }
private:
    IRenderBackend& backend_;
    std::unordered_map<ResourceId, RenderTargetHandle> inputs_, outputs_;
};

// 一个渲染 pass：声明输入/输出资源 + 执行体。
struct FramePass {
    std::string name;
    std::vector<ResourceId> reads;
    std::vector<ResourceId> writes;
    bool enabled = true;
    std::function<void(FrameGraphContext&)> execute;
};

// Frostbite 式渲染资源图：声明式 pass + 临时 RT 自动生命周期。
class FrameGraph {
public:
    ResourceId AddResource(const FrameGraphResourceDesc& desc);
    bool AddPass(FramePass pass);  // 环/缺资源返回 false
    bool Execute(IRenderBackend& backend); // 拓扑排序 + 临时 RT 池 + 按序执行
    void ResetFrame();  // 帧末释放临时资源（或池复用）
    size_t PassCount() const { return passes_.size(); }
    size_t ResourceCount() const { return resources_.size(); }

private:
    std::vector<FramePass> passes_;
    std::vector<FrameGraphResourceDesc> resources_;
    // 资源 -> 最后使用 pass 索引（引用计数用）。
    std::vector<int> lastUse_;
    // 临时 RT 池：按 (w,h,format,samples) 键复用。
    struct PoolKey { uint32_t w, h, format, samples; bool operator==(const PoolKey&) const = default; };
    std::unordered_map<uint64_t, std::vector<RenderTargetHandle>> rtPool_;
    std::vector<RenderTargetHandle> liveRTs_;
};

} // namespace neon::gfx
```

**Step 2-4:** 实现 `AddResource`（分配 id + 记录 desc）、`AddPass`（存 pass + 更新 lastUse_）、`Execute`（Kahn 拓扑排序 → 为每个 pass 绑定输入/输出 RT → 调 execute → 引用计数释放到池）、`ResetFrame`（liveRTs_ 回池）。临时 RT 池：`PoolKey → hash`，创建时从池取/新建，释放时回池。**拓扑排序复用 microkernel `ModuleRegistry::InitAll` 的 Kahn 模式（registry.cpp）**。`lastUse_` 按 pass 的 reads/writes 更新：资源被最后一个引用它的 pass 用完后回池。

**Step 5:** 单测（`tests/test_frame_graph.cpp`）：AddResource/AddPass 正常、`Execute` 拓扑序（用记录执行序的 dummy pass）、环检测返回 false、临时 RT 复用（同一格式创建一次）。用 NullBackend。

**Step 6:** `cmake --build build-msvc --config Release` + `neon_tests.exe` 全绿（743 + 新测试）。
**Step 7:** Commit: `feat: FrameGraph 核心（AddResource/AddPass/Execute + 临时 RT 池）`

---

### Task 2: bloom 链改造为 graph pass（验证框架）

**Files:**
- Create: `engine/include/neon/gfx/bloom_graph.hpp` + `engine/src/gfx/bloom_graph.cpp`（bloom 链 pass 封装）
- Modify: `engine/src/gfx/renderer.cpp`（`RunBloom` 改用 bloom graph）
- Modify: `engine/include/neon/gfx/renderer.hpp`（bloom 相关 RT/shader 移入或保留）

**Step 1-6（TDD）：** 先读 `renderer.cpp` 的 `RunBloom`(2649)/`EnsurePostTargets`(2478) 完整实现（bright pass → downsample 1/2 → 1/4 → blur ping-pong → upsample-add）。把 bloom 的 5 个 sub-pass（bright/downsample/blurA/blurB/upsample-add）建模成 FrameGraph pass，资源（`bloomHalfA_`/`bloomHalfB_`/`bloomQuarterA_`/`bloomQuarterB_`）声明为 graph 资源。**关键**：执行顺序/绑定必须与手写链逐 pass 一致。用一个 debug 单测验证 pass 执行序（记录序 + 资源读写序）。渲染等价验证：`CaptureBloomComparison`（bloomOff vs bloomOn 对比）经 editor 冒烟或现有后处理测试确认无回归。

**Step 7:** Commit: `feat: bloom 链改造为 FrameGraph pass`

---

### Task 3: SSAO / volumetric / SSR / depth pass 入图

**Files:** Modify `renderer.cpp`/`renderer.hpp` + `frame_graph` 扩展（若需）

**Step 1-6：** 把 `RunSceneDepthPass`/`RunSsaoPass`/`RunVolumetricPass`/`RunSsrPass` 逐个改造成 FrameGraph pass（每 pass 声明 reads/writes 资源：`sceneDepth`/`aoRT_`/`volRT_`/`ssrRT_` + blur scratch）。`*RanThisFrame_` 语义由 pass 的 `enabled` 开关替代。每改造一个 pass 跑一次全绿 + editor 冒烟。
**Step 7:** Commit: `feat: SSAO/volumetric/SSR/depth pass 入图`

---

## 阶段 3：composite + Renderer 门面化

### Task 4: composite 入图 + 手工 RT 管理清理

**Step 1-6：** `CompositeToBackbuffer` 入图（`hdrScene`/`ao`/`vol`/`ssr`/`bloomAcc` → backbuffer）。删除 `Ensure*Targets`/`*RanThisFrame_` 手工管理。`EndScene`/`CompositeFrame` 改 `postGraph_.Execute(backend)`。**渲染等价验证**：`CaptureBloomComparison`/`CaptureTonemapComparison` 逐像素一致（或现有后处理测试）。
**Step 7:** Commit: `feat: composite 入图，后处理全链由 FrameGraph 驱动`

### Task 5: Renderer 门面化（ShadowSystem / SceneState / DrawBatch2D）

**Step 1-6：** 把 `RunShadowPass`/`RunPointShadowPass`/`shadowRT_`/`shadowCasters_` → `ShadowSystem`；`camera_`/`sunDir_`/`sky`/`fog`/`ibl*` → `SceneState`；`Flush2D`/`PushQuad*`/billboard 批 → `DrawBatch2D`。Renderer 保留公开 API 转发（`DrawMesh`/`SetCamera`/`SetShadowsEnabled` 等），调用方零改动。renderer.cpp 2891 → <1500 行。
**Step 7:** Commit: `refactor: Renderer 门面化（ShadowSystem/SceneState/DrawBatch2D）`

---

## 验收（全部完成后）

1. `neon_tests` 全绿（743 基线）+ `neon_editor --smoke-test 240` 通过。
2. 后处理链全部由 FrameGraph 驱动，无 `Ensure*Targets`/`*RanThisFrame_` 手工管理残留。
3. `renderer.cpp` <1500 行。
4. `CaptureBloomComparison`/`CaptureTonemapComparison` 逐像素一致（渲染等价）。
5. `grep -rE "bloomHalfA_|aoRT_|volRT_|ssrRT_"` 无残留（RT 全在 graph 资源声明里）。

## 风险与回滚

- **渲染等价性是最大风险**：每改造一个 pass 立即跑冒烟 + 截图对比，发现偏差回滚到上一 commit。
- 临时 RT 池复用策略（同尺寸/格式）需谨慎，避免跨 pass 污染。
- 每任务独立 commit，可单独回滚。
