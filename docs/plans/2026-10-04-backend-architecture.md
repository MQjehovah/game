# 后端 / 渲染架构（参照 Godot RD 与 Unity SRP+RHI）

> 目的：把 NeonEngine 的后端从“逐项 set 状态 + 每 shader 一个成员 + 内联源码”
> 演进到接近成熟引擎的分层架构。本文记录目标设计、现状差距与分阶段迁移。
> 日期：2026-10-04。

## 1. 参考实现要点

### Godot 4 — RenderingDevice (RD)
- `RID`：所有 GPU 资源（shader/buffer/texture/pipeline/uniform set/framebuffer）都是
  不透明整数句柄；引擎上层绝不持有后端指针。
- `RDShaderFile` / `RDShaderSPIRV`：`.gdshader` 由 `ShaderCompiler` 编译成**各后端**
  形态（Vulkan SPIR-V / GLSL / Metal / D3D12）。同一份源码，多后端产物。
- `RDPipeline`：光栅化 / 深度模板 / 混合 / 顶点布局打包成**一个管线对象**（PSO），
  不再逐项 `set_*`。
- `RDUniform` + `UniformSet`：显式**绑定组**（≈ Vulkan descriptor set / D3D root sig）。
- `draw_list`（命令缓冲）+ RenderGraph：录制与执行分离，自动派生 barrier/依赖。
- RenderingServer / MaterialStorage：材质 = `Shader` + 属性表；uniform 由 shader 反射。

### Unity — SRP + RHI
- `CommandBuffer` + `ScriptableRenderContext`：命令录制 + render graph。
- Shader 变体/关键字：`#pragma multi_compile` → `LocalKeyword`，构建期展开成排列，
  按平台编译。（替代手写 `_skinned`/`_instanced`/`_gbuffer` 程序。）
- `Shader.PropertyToID`：整数 ID 代替字符串查 uniform（避免每帧字符串哈希）。
- `MaterialPropertyBlock` / `SetGlobalX`：属性按块绑定；SRP Batcher 按变体归组。

## 2. NeonEngine 现状 → 目标

| 维度 | 现状 | 目标 |
|---|---|---|
| 资源句柄 | `TextureHandle/ShaderHandle/MeshHandle/RenderTargetHandle`（≈ RID） | 保持，补齐 RT/mesh/pipeline 句柄 |
| Shader 管理 | `Renderer` 每 shader 一个成员 + 手写创建 | **`ShaderLibrary` 名字表 + 反射布局** |
| Shader 源码 | GL 内联字符串；VK 独立文件 | **单源 + 构建期按后端编译**（进行中） |
| 管线状态 | 逐项 `SetCullMode/SetDepthTest/SetBlendMode/SetPolygonOffset` | **`PipelineState` 打包 + 缓存** |
| 绑定 | 固定全局 UBO + 按名字 `SetUniform*` | **`UniformSet`/绑定组**（渐进） |
| 命令 | 直接调后端 draw；FrameGraph 仅后处理 | FrameGraph/命令缓冲覆盖全场景 |
| 变体 | `#define` 手插 + 手写程序名 | **关键字/变体系统** |

## 3. 分阶段迁移

- **P0（本次做）ShaderLibrary + 单源双编译**
  - `Renderer` 去掉 `litShader_/waterShader_/terrainShader_/skinnedLitShader_/...` 成员，
    改为 `std::unordered_map<std::string, ShaderHandle>` + `Shader(name)`。
  - 创建集中在 `InitShaders()` 的一张表里（名字 → 顶点/片元源码）。
  - shader 源码：`engine/src/gfx/vulkan/shaders/*.frag` 为**单源**；
    `tools/gen_vk_shaders.ps1` → SPIR-V，`tools/gen_gl_shaders.ps1` → GL(loose uniform)。
- **P1 PipelineState**：`struct PipelineState { CullMode, bool depthTest, bool depthWrite,
  BlendMode, polygonOffset, ... }` + 后端按 hash 缓存 PSO；替换逐项 set。
- **P2 UniformSet**：把 `engine_ubo` 的 slot 与采样器槽位建模为显式绑定布局，
  GL 侧用 UBO（GL 4.5 uniform block）/ VK 侧 descriptor；`ApplyMaterial`/`ApplySceneUniforms`
  改为填绑定组。
- **P3 命令 + RenderGraph 全场景**：主 pass / 透明 / 水 / 后处理统一进图。
- **P4 变体系统**：shader 内声明关键字，构建期生成排列；运行期按材质选变体。

## 4. P0 落地细节（ShaderLibrary）

```cpp
// renderer.hpp
std::unordered_map<std::string, ShaderHandle> shaders_;
ShaderHandle Shader(std::string_view name) const; // 未注册返回 {}（后端无关）
```
- `InitShaders()`：一张 `{name, vertSrc, fragSrc}` 表驱动创建；GL 用源码编译，
  VK 忽略源码按 name 取预编译 SPIR-V（`IRenderBackend::CreateShader` 已是该语义）。
- 所有 `UseShader(litShader_)` → `UseShader(Shader("lit"))`。
- 新 shader（water）只加表项 + 源码文件，不再加成员。

## 5. water（真实水体）— 在 P0 之上
- 单源 `engine/src/gfx/vulkan/shaders/water.frag`（`eng.` UBO + 八面体 IBL + 动态波法线 +
  Fresnel body/反射 + 太阳高光）。
- `Material::water` + 参数（`waterWaveScale/Speed/Strength`、`waterSpecular/SpecPower`、
  `waterDeep/Shallow`）；scene JSON 可选字段。
- `draw_system` 在 `meshKey == "water"` 时置 `material.water = true`。
- `ShaderLibrary` 里注册 `water`。
