# 渲染管线后续待办（handoff）

> 目的：压缩上下文后据此继续。记录**已完成的渲染修复**、**待修的 bug**、**待补的功能**，
> 以及每条的做法、涉及文件、参考实现与坑。
> 日期：2026-10-03。上一阶段以 `24db079`（spot light）收尾。

---

## 0. 工程基本操作（务必遵守，省时间）

- 构建目录：`build-msvc`（VS2022，x64）。
  - 全量：`D:\cmake-3.23.2-windows-x86_64\bin\cmake.exe --build build-msvc --config Release --parallel`
  - **优先增量单 target**（快很多）：`--target neon_game` / `neon_editor` / `neon_tests` / `neon_vk_shaders`。
  - 若报 `ZERO_CHECK / VerifyGlobs` 错：CMakeCache 丢失，重配：
    `cmake -S . -B build-msvc -G "Visual Studio 17 2022" -A x64 -DNEON_ENABLE_VULKAN=ON`
- **编译前先强杀残留进程**，否则 `neon_editor.exe` 被锁：`Stop-Process -Name neon_editor,neon_game -Force -ErrorAction SilentlyContinue`
- 测试：`build-msvc\Release\neon_tests.exe`，基线 **842/842**。
- 截图/冒烟：
  - `neon_game.exe --scene <proj>/assets/scenes/x.json --scripts <proj> [--backend vulkan] --smoke-test N --screenshot out.png <frame>`
  - `neon_editor.exe --project <proj> --scene assets/scenes/x.json [--play] --frames N --screenshot out.png <frame>`
- 改了 VK shader 源必须重生成 SPIR-V：`--target neon_vk_shaders`（内部跑 `tools/gen_vk_shaders.ps1`，新增 pass 要登记到该脚本；新 shader 用 `post.vert`/`lit.vert` 等 + `.frag`）。
- 诊断开关：`NEON_VK_VALIDATION=1`（Vulkan 校验层）、`NEON_VK_TRACE=1`、`NEON_DUMP_POST=1`（导 depth/ao/ssr raw bin 到 CWD）、`NEON_SHADOW_DEBUG=1..4`、`NEON_NO_SHADOWS`、`NEON_GL_ERRORS`。
- **别用 PowerShell `Get-Content -Raw`/`Set-Content` 改含中文的文件**（GBK 会损坏；`-replace` 大小写不敏感）。用 edit 工具。
- 生成的 `.asset_db.json` 是缓存，不要提交；每次 `git checkout -- projects/default/.asset_db.json`。

---

## 1. 本阶段已完成（已提交，勿重复）

| commit | 内容 |
|---|---|
| `ff69756` | 直接光 Schlick-GGX 用 `k=(r+1)^2/8`（原 typo 用 roughness^4/2） |
| `ceb49e5` | **SSAO 空操作**改回真半球核（用抽样点自身深度比较），`kSsaoBiasWorld` 0.4→0.05 |
| `b87f018` | HDRI 等距天空极角用 `dir.y`（原用重映射 dy，只用到上半张） |
| `f981bd4` | bloom 被曝光乘两次 → bright pass 输出未曝光单位 |
| `67a6dd6` | IBL 漫反射能量用 DFG 的 (1-E)；SSR 按粗糙度淡出（当时代码用 HDR alpha 存粗糙度） |
| `1d37758` | TAA 改 variance clipping；自动曝光改全帧块平均 |
| `e0b00f7` | **G-buffer**（RGB=间接光、A=粗糙度）→ SSAO 只作用间接光(B1) + SSR 读真粗糙度(B5) |
| `e521c23` | GL 可选 uniform 缺失日志降噪 |
| `69719c5` | **VK**：三角形绕序/frontFace 反（单面网格不渲染）+ 深度 barrier aspect（D24S8 需 DEPTH\|STENCIL） |
| `dfc80f2` | **VK**：自动曝光偏黑（pass 内向图外持久目标第二绘制丢失）→ `SupportsTargetPersistence()`，VK 跳过时域平滑 |
| `24db079` | **spot light**（GL+VK），最多 4 个，含锥角/羽化 |
| (F2) | **启用点光阴影**（GL+VK）：目标带深度附件、按 texel 深度测试（非画家排序），`PointShadowRT` 真正分配（原来从未创建），`Renderer::SetPointShadowsEnabled` 运行时开关（low/medium 关，high/ultra 开），`NEON_NO_POINT_SHADOWS` 诊断 |
| (VK) | **修复 Vulkan 布局校验报错**：clear pass 的 `initialLayout` 用 UNDEFINED（原来谎称 GENERAL）；新 RT 的 tracked layout 从 UNDEFINED 起步（首次 bind 才真正 transition）；`EnsureTextureSet` 在过渡前补齐 `EndRenderPassIfActive` + 重开已结束的 cmd（换 target 会 flush/end 命令缓冲）。visual_acceptance 的 image-layout 错误从 110 → 0 |
| (F5) | **多 tonemapper**：Linear/Reinhard/Filmic/ACES（`TonemapMode`，GL+VK composite，CLI `--tonemap`），默认 ACES 不变 |
| (F5) | **AgX tonemapper**（`--tonemap agx`，Sobotka 近似，GL+VK） |
| (F5) | **FXAA**：composite→LDR 中间 RT→`post.fxaa`→backbuffer（默认 strength 0 精确拷贝）；`--fxaa` |
| (F5) | **DoF**：composite 内深度感知 gather；`--dof [--dof-focus d]` |
| (B3) | **可选 gamma-correct**（`Renderer::SetGammaCorrect`，默认关，CLI `--gamma-correct`）：lit 解码 sRGB albedo/emissive，composite `LinearToSrgb` 编码（tonemap 后、grade 前）；VK UBO slot 7636 |
| (F3) | **八面体 IBL（带方位）**：irradiance 32²→24²、prefiltered 改成八面体「粗糙度 atlas」（`kOctaPrefilterSize=48` × `48*5` 行，逐 tile GGX 预滤波），从**真实 equirect HDRI**（`SceneState::SetSkySource`，1024 宽降采样）或程序化渐变烘焙；lit shader 用八面体 encode + wrap-fold 双线性（消除接缝）按 `textureSize()` 推 tile 数并混合相邻粗糙度。观感较旧 Y-only 渐变明显改善；GL/VK 一致，烘焙 ~50–120ms |

架构要点（供后续参考）：
- 前向管线：lit pass（HDR+depth）→ 后处理图 `post_graph`（depth → gbuffer → ssao → vol → ssr → bloom → composite）。**无 MRT**；间接光/粗糙度靠 `post.gbuffer` 那个额外几何 pass 写 `gbuffer_`（RGBA16F）。
- `lit` shader 有一个 `#define GBUFFER` 变体（`gbuffer`/`gbuffer_skinned`/`gbuffer_instanced`），由 `Renderer::DrawGBufferCasters` 用再画一遍 caster 列表实现。VK 走预编译 SPIR-V 表的名字。
- VK UBO：`engine/src/gfx/vulkan/shaders/engine_ubo.glsl` + `vk_backend.cpp` 的 `kUniformOffsets` + `kUniformBlockSize`（当前 **7648**）三处必须同步。**std140 标量/向量数组步长都是 16**。
- SSR 粗糙度：读 `gbuffer_.a`；composite 用 `gbuffer_.rgb` 做 indirect-only AO。
- 自动曝光：`adaptDone_` = 平滑曝光；`SupportsTargetPersistence()` 为 false 的后端直接把 adaptation 设 1.0。

---

## 2. 待修的 bug

### B3. gamma-correct 管线（**已做成可选开关，默认关**）
- 引擎默认仍 **gamma-unaware**（albedo 当线性用、输出不编码），既有项目不受影响。
- 已实现（opt-in）：`Renderer::SetGammaCorrect(bool)`（默认 false）+ CLI `--gamma-correct`。
  - lit 端 `uGammaCorrect`（GL builtin + VK lit.frag；VK UBO slot `7636`）：为真时 albedo（含 tint/顶点色/草/地形）与 emissive 贴图 `pow(.,2.2)` 解码到线性。
  - composite 端 `CompositeParams::gammaCorrect`：tonemap 后、grade/vignette 前 `LinearToSrgb` 编码（两个分支都编码）。
  - 开启后观感明显变亮/发灰（visual_acceptance 验证），需按项目重调 exposure/光照。
- 若要彻底正确：把 albedo/emissive 贴图用 sRGB internal format 上传（硬件解码），并区分 color/data 贴图。改动更大。

### 其它已记录的低优先问题
- **SSAO** 仍用「深度重建法线」，Godot 用真法线缓冲 + horizon-based；有 gbuffer 后可考虑用 `gbuffer` 真法线提质。
- **SSR**：单 pass、无 HiZ、无时域去噪（只有空间 blur）；粗糙度只是淡出，未做 cone/mip 模糊。
- **点光阴影**已默认开启（见 F2）；高粗糙度地面在光源极近处仍可能有轻微 self-shadow（slope bias 封顶 0.03），如需可加 normal-offset。
- **VK 自动曝光**目前跳过时域平滑（`SupportsTargetPersistence()=false`）；彻底修需要让 VK 的「图外持久目标」可写（把历史改成图内资源并解决读写环，或用一次性 copy 写回）。
- `SetAmbientGroundColor` 无调用者（半球环境地面色恒为默认灰）。
- **VK 资源析构顺序**（校验层仅剩此类）：`vkDestroyBuffer-00922`/`vkDestroyPipeline-00765`/`vkDestroyImageView-01026` 在仍被 pending 使用时报错，`vkQueueSubmit-00065` semaphore 重用，以及 `vkDestroyDevice-05137` 设备内存泄漏（若干 DeviceMemory 未释放）。需要给资源加延迟销毁/帧围栏，并补全 DeviceMemory 释放路径。

---

## 3. 待补的功能（按建议顺序）

### F2. 启用点光阴影（**已完成**）
- 原因：`pointShadowRT_` 从未分配（`Init` 里被硬编码 `pointShadowsEnabled_=false`，`CreateCascadeTargets` 不建点光目标），且旧代码只有画家排序（无深度），大面积地面作接收者+投射者会产生 face 状光斑。
- 现在：`CreatePointShadowTargets()` 建 2 光 × 6 face 的 512² 目标，有深度缓冲时用 GL_LESS 逐 texel 深度测试（`CullMode::None` 画双面），否则退回画家排序。运行时开关 `Renderer::SetPointShadowsEnabled`；画质预设 low/medium 关、high/ultra 开；`NEON_NO_POINT_SHADOWS=1` 诊断关闭。GL/VK 截图一致，`neon_tests` 842/842。

### F3. 真实 IBL 预滤波（**已完成第一版；GPU 卸载待办**）
- 已完成：把旧 Y-only 渐变图换成**八面体（azimuth-aware）**，并从**真实 equirect HDRI** 烘焙（`SceneState::SetSkySource`，`draw_system.cpp` 在设置天空贴图时传入解码图像）。irradiance 用余弦卷积，prefiltered 用逐 tile GGX 卷积做成「粗糙度 atlas」（`kOctaRoughnessLevels` 个八面体 tile 纵向排列）；lit shader 用 `OctEncode` + wrap-fold 双线性（`OctBilinear`，消除八面体接缝）按 `textureSize()` 推 tile 数并混合相邻粗糙度。无 HDRI 场景回退程序化渐变。GL/VK 一致，`neon_tests` 845/845。
- 仍待办（当时选的完整 GPU 方案）：预滤波目前是 **CPU 烘焙**（HDRI 场景一次性；动画程序化天空每 ~20 帧重烘一次，~50–120ms）。搬到 GPU 需要：(1) 新增 equirect→八面体 / irradiance / GGX prefilter 三个 fragment shader 并登记 `tools/gen_vk_shaders.ps1`；(2) 新的「完整 mip 链上传」后端 API（GL `TexImage2D` 逐级 / VK 逐级 buffer-image copy）；(3) lit 改 `textureLod` + mip。当前用 atlas 代替 mip 以避免这套基础设施。
- 影响：HDRI 场景反射/漫反射已随照片天空变化（含水平方位）。

### F4. froxel 体积雾
- 现状：`volumetric.frag` 只是太阳光柱 raymarch（固定 38m slab，无高度衰减/阴影采样/逐 froxel 光注入）。
- 参考：Godot `environment/volumetric_fog.glsl`、`volumetric_fog_process.glsl`（froxel grid + light injection + temporal）。

### F5. 后期补缺（都属可选、低风险）
- **多 tonemapper（已做 Linear/Reinhard/Filmic/ACES）**：`bloom.hpp` 的 `TonemapMode` + CPU `TonemapColor`，GL `kCompositeFragmentShader` 与 VK `composite.frag` 加 `uTonemapMode`（VK UBO slot 7244），`PostParams::CompositeParams::tonemapMode`，`Renderer::SetTonemapMode`，CLI `--tonemap linear|reinhard|filmic|aces`。默认 ACES 保持既有观感不变。
- **AgX**：矩阵式，仍未做（可照 Godot `effects/tonemap.glsl`）。
- **DoF（已做）**：在 composite 内做深度感知 gather（读 `uFogDepth` 线性深度算 CoC，12 抽样碟形，按深度邻近度加权减少前景渗色）。`Renderer::SetDofEnabled/SetDofFocus/SetDofStrength`，CLI `--dof [--dof-focus d]`。VK UBO slot 7648/7664/7680/7696。DoF 会强制深度预 pass。
- 运动模糊：未做。
- 色彩校正 LUT（当前只有程序化 LGG）。
- **FXAA（已做）**：composite 改写到 LDR 中间 RT，末端新增 `post.fxaa` pass（GL `kFxaaFragmentShader` / VK `fxaa.frag`，strength 0 = 精确拷贝，故默认关闭也走同一图结构）。`Renderer::SetFxaaEnabled/Strength`，CLI `--fxaa`。VK UBO slot `7640 uFxaaStrength`（复用 `uTexelSize`）。
- SMAA：未做（Godot `effects/smaa_*.glsl`）。

### F6. 高级材质
- clearcoat / anisotropy / iridescence / sheen(cloth) / subsurface / 第二高光叶 / 金属能量补偿（split-sum 高粗糙度偏暗）。参考 Filament `surface_brdf.fs`。

### 其它大项（暂缓，规模大）
- Light clustering（当前点光最多 8、spot 4，逐片元循环）。
- SSIL、屏幕空间接触阴影、reflection probe、SDFGI/VoxelGI/Lightmap。

---

## 4. 参考仓库（在临时目录，可能被清，需要时重下）
- Volcano（自制引擎，PBR/SSAO/SSR/环境 cubemap 参考）：
  `git clone --depth 1 https://github.com/PoplarVolcano/Volcano <tmp>/ref/Volcano`
- Godot 4（clustered forward + effects/environment 全套 shader）：
  `git clone --depth 1 --filter=blob:none --sparse https://github.com/godotengine/godot <tmp>/ref/godot; git -C <tmp>/ref/godot sparse-checkout set servers/rendering/renderer_rd/shaders`
- Filament（PBR/IBL/阴影参考）：
  `git clone --depth 1 --filter=blob:none --sparse https://github.com/google/filament <tmp>/ref/filament; git -C <tmp>/ref/filament sparse-checkout set shaders/src`
- 关键参考文件：
  - Godot：`effects/ssao.glsl`, `effects/screen_space_reflection*.glsl`, `effects/taa_resolve.glsl`, `effects/tonemap.glsl`, `environment/sky.glsl`, `environment/volumetric_fog*.glsl`, `forward_clustered/scene_forward_clustered.glsl`。
  - Filament：`surface_brdf.fs`, `surface_light_directional.fs`, `surface_light_indirect.fs`, `surface_shadowing.fs`。

---

## 5. 快速验证清单（每次改渲染后）
- `neon_tests` = 842/842。
- GL 与 VK 都跑 `projects/default/assets/scenes/visual_acceptance.json`、`_mirror.json`、`projects/showcase/assets/scenes/04_lakeside.json`，肉眼/像素 diff 对比，确认无回归、两后端一致。
- 改 VK UBO 后跑一次 `NEON_VK_VALIDATION=1`，要求无 `pCode-08737`、无 `InvalidImageLayout`。剩余为**资源析构顺序**类（`vkDestroyBuffer-00922`/`vkDestroyPipeline-00765`/`vkDestroyImageView-01026`/`vkDestroyDevice-05137` 泄漏 + semaphore `-00065`），属已知的独立问题（见「待修」）。
