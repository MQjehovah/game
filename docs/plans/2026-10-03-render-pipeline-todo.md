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

架构要点（供后续参考）：
- 前向管线：lit pass（HDR+depth）→ 后处理图 `post_graph`（depth → gbuffer → ssao → vol → ssr → bloom → composite）。**无 MRT**；间接光/粗糙度靠 `post.gbuffer` 那个额外几何 pass 写 `gbuffer_`（RGBA16F）。
- `lit` shader 有一个 `#define GBUFFER` 变体（`gbuffer`/`gbuffer_skinned`/`gbuffer_instanced`），由 `Renderer::DrawGBufferCasters` 用再画一遍 caster 列表实现。VK 走预编译 SPIR-V 表的名字。
- VK UBO：`engine/src/gfx/vulkan/shaders/engine_ubo.glsl` + `vk_backend.cpp` 的 `kUniformOffsets` + `kUniformBlockSize`（当前 **7648**）三处必须同步。**std140 标量/向量数组步长都是 16**。
- SSR 粗糙度：读 `gbuffer_.a`；composite 用 `gbuffer_.rgb` 做 indirect-only AO。
- 自动曝光：`adaptDone_` = 平滑曝光；`SupportsTargetPersistence()` 为 false 的后端直接把 adaptation 设 1.0。

---

## 2. 待修的 bug

### B3. gamma-correct 管线（**已实现过、已回退**，需决策）
- 现状：引擎是 **gamma-unaware**（albedo 贴图当线性用、输出不编码，只有 grade 的 `uGamma` 默认 1.0 等于没做）。
- 试验过：lit 里对 albedo/tint/emissive/草贴图 `pow(.,2.2)` 解码 + composite 输出 `pow(.,1/2.2)` 编码。**结果：现有场景整体过曝发灰**（visual_acceptance mean 71→128），因为所有内容都按旧管线调过。
- 结论：**直接默认开启会破坏全部项目**。建议做成**可选开关**（默认关）：
  1. `Renderer::SetGammaCorrect(bool)` + 走场景 uniform（lit 端 `uniform int uGammaCorrect`；VK 加 UBO slot，注意还有空位：当前末尾 ~7640）。
  2. composite 端 `CompositeParams::gammaCorrect`，`if (gammaCorrect) graded = LinearToSrgb(graded);`（在 grade/vignette 之前）。
  3. 开启后需要按项目重调 exposure/光照。
- 若要彻底正确：把 albedo/emissive 贴图用 sRGB internal format 上传（硬件解码），并区分 color/data 贴图。改动更大。

### 其它已记录的低优先问题
- **SSAO** 仍用「深度重建法线」，Godot 用真法线缓冲 + horizon-based；有 gbuffer 后可考虑用 `gbuffer` 真法线提质。
- **SSR**：单 pass、无 HiZ、无时域去噪（只有空间 blur）；粗糙度只是淡出，未做 cone/mip 模糊。
- **点光阴影**默认关闭（见 F2）。
- **VK 自动曝光**目前跳过时域平滑（`SupportsTargetPersistence()=false`）；彻底修需要让 VK 的「图外持久目标」可写（把历史改成图内资源并解决读写环，或用一次性 copy 写回）。
- `SetAmbientGroundColor` 无调用者（半球环境地面色恒为默认灰）。

---

## 3. 待补的功能（按建议顺序）

### F2. 启用点光阴影（最省事，代码已存在）
- `ShadowSystem::kShadowPointLights=2`、`kPointShadowSize=512`，`pointShadowsEnabled_` 默认 false（`engine/src/gfx/shadow_system.cpp:182`）。lit shader 有 `PointShadowForLight` + 12 张 2D face map 采样（unit 8..19）。
- 做法：默认开（或按场景/画质开），跑 `NEON_DUMP_POST`/截图验证 GL+VK；注意 VK 的 face map 上传与 cube 约定要一致（`PointCubemapFaceUV` 对应 GL 规范表 8.19）。
- 风险：性能（2 光 × 6 face）+ 之前被关可能有原因，需回归。

### F3. 真实 cubemap IBL 预滤波（HDRI 光照准确）
- 现状（`engine/src/gfx/ibl.cpp`）：IBL 只按 `top`/`horizon` 两个颜色烘焙竖直渐变（irradiance 1×128、prefiltered 24×128、BRDF LUT 128×128），**即使场景用真 HDRI 天空也不是从 cubemap 预滤波**。
- 做法：把环境预滤波搬到 GPU（equirect→cube→irradiance convolution + GGX prefilter + BRDF LUT，参考 `ref/Volcano` 的 `EquirectangularToEnvCubeMap.glsl`/`EnvCubeMapToIrradianceMap.glsl`/`EnvCubeMapToPrefilterMap.glsl`、`ref/godot` 的 `environment/sky.glsl` 等）；lit shader 改成 `textureLod(uPrefiltered, R, rough*maxLod)`。
- 影响：改观感（HDRI 场景反射/漫反射变化）。

### F4. froxel 体积雾
- 现状：`volumetric.frag` 只是太阳光柱 raymarch（固定 38m slab，无高度衰减/阴影采样/逐 froxel 光注入）。
- 参考：Godot `environment/volumetric_fog.glsl`、`volumetric_fog_process.glsl`（froxel grid + light injection + temporal）。

### F5. 后期补缺（都属可选、低风险）
- DoF / bokeh（Godot `effects/bokeh_dof.glsl`）。
- 运动模糊。
- 多种 tonemapper：Linear/Reinhard/Filmic/ACES/AgX（Godot `effects/tonemap.glsl`，AgX 矩阵已抄在对话里）。当前只有 ACES(Narkowicz)，在 `bloom.hpp` kCompositeFragmentShader / VK `composite.frag`。
- 色彩校正 LUT（当前只有程序化 LGG）。
- 抗锯齿：FXAA / SMAA（Godot `effects/smaa_*.glsl`；当前 MSAA+TAA）。

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
- 改 VK UBO 后跑一次 `NEON_VK_VALIDATION=1`，要求只剩 `vkDestroyDevice` 泄漏告警（无 `pCode-08737`、无 layout 错误）。
