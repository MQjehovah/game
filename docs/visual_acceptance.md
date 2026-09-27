# 视觉品质实机验收（A1-A5）

本文件是 A1-A5 视觉批的**实机验收流程**。所有 A/B 对比都通过 `neon_game --scene
<场景> --screenshot <png> <帧>` 逐帧截图（跑的是完整 GameRuntime，应用 RenderStack /
灯光 / 天空盒，与编辑器"编辑视图"不同）。编辑器 `--screenshot` 只截编辑视图，**不能**看到
A1/A5 的 composite 效果。

## 准备
- 已有验收场景：`projects/default/assets/scenes/visual_acceptance.json`（地面 + DamagedHelmet +
  相机 + 太阳光 + **renderstack 组件**，开启 SSAO/体积光/bloom/调色/自动曝光/暗角/天空盒）。
- 用 `neon_game` 以 loose scene 模式运行（需脚本基目录）：

```bat
build\Release\neon_game.exe --scene projects\default\assets\scenes\visual_acceptance.json --scripts projects\default
```

## 截图命令（A/B 对比）
每个特性都截两张做对比，用图像 diff 工具（或肉眼看差异）。

### A1 颜色分级
没开启 = 去掉 `renderstack` 里的 `grade*` 字段（或临时把 `"grade": false`）。
- ON：`--screenshot shots\a1_on.png 60`
- OFF：`--screenshot shots\a1_off.png 60`
- 预期：ON 版饱和度/色温/对比度明显更"电影感"；OFF 版与旧管线一致。

### A2 法线贴图
DamagedHelmet 自带动法线贴图（`Default_Normal.jpg`）。对比 `castShadow`/材质。
- 预期：法线贴图在光照下产生表面凹凸细节（头盔铆钉/划痕立体感），无它则偏平。
- 排查：若看不出差异，确认 `neon_game` 已用 `NEON_NO_NORMAL` 无关；法线永远随材质导入。

### A3 半球光 + 探针 GI
- 半球光：改 `light.ambientStrength` 或加 `renderstack` 相关；晴天阴影区应呈"暗而非黑"。
- 探针 GI：场景需一个 `.navgrid.json` 声明 + `BakeLightProbes`。默认关闭。

### A4 程序化天空盒
`light.skybox: true` + `useAtmosphere: true` → 太阳圆盘/月亮/云。
- 预期：天空随相机转动（不再是屏幕固定渐变）；可见太阳光晕 + 程序云。

### A5 自动曝光 + 暗角
`renderstack` 里 `autoExposure: true` + `vignette: true`。
- 自动曝光：面向亮处 vs 暗处（移动相机）画面应自适应明暗，不再死白/死黑。
- 暗角：画面四角径向变暗。

## 验收清单
| 特性 | 命令 | 预期 |
|------|------|------|
| A1 调色 | 上表 `--screenshot` | 电影感色调 |
| A2 法线 | DamagedHelmet 特写 | 表面凹凸 |
| A3 半球光 | 阴影面 | 暗而非黑 |
| A4 天空盒 | 旋转相机 | 太阳/云随视角 |
| A5 自动曝光/暗角 | 亮暗切换 | 自适应 + 四角暗化 |

> 提示：跑一次 `neon_game --scene <视觉验收场景> --screenshot out.png 60` 即可得到单帧，
> 反复用不同 `--screenshot` 帧号可做时间轴采样。A1/A5 的 `renderstack` 是场景数据驱动，
> 在编辑器"属性面板"可直接改（反射生成），改完保存再截图即 A/B。

## Demo 玩法升级（B1/B2/B3 应用到 NeonRealm）
- **B1 NavGrid 绕过障碍**：狼群用 `NavFindPath` 路点绕障追击/回巢（`level.navgrid` 声明）。
- **B2 数据驱动技能**：技能表从 `assets/data/skills.json` 经反射 `LoadDataTable` 加载。
- **B2 物品掉落**：狼击杀掉落物品（`assets/data/items.json`，经 `LoadDataTable("item")`），
  银币加金币、浆果回血，HUD 物品栏计数。
- **B3b BlendSpace1D locomotion**：英雄 `AnimBlend("Idle","Running_A",t)` 与狼
  `AnimBlend("02_walk","01_Run",t)` 连续混合走/跑（替代硬切），移动时角色平滑过渡。
- 验证：跑 `neon_game --scene <neon_realm 场景>`，拖动输入看角色走跑连续过渡，
  狼群追击时从走平滑到跑；击杀狼看物品飘字 + HUD 计数。

## Kenney 村庄资产就地重摆（带贴图真实模型，替代程序化纯色）

从 GitHub CC0 镜像 `shorepine/kenney` 拉取 **fantasy-town 村庄 kit（167 个带贴图 GLB +
共享 `colormap.png`）**，落地 `neon_realm/assets/kenney/fantasy-town/`。引擎加载器原生支持
这种"GLB 外部相对 image URI"（源码注释即提到 Kenney），已用 `KenneyColormapExternalImage`
测试证明材质 albedo 有效。

**预览验收**（看 Kenney 模型真实渲染效果 + 选型）。两个方式：
```bat
:: A) 编辑器打开(推荐, 自由 orbit 查看每个模型)
.\build\release\neon_editor.exe --project projects\neon_realm
::    在编辑器里加载 assets/scenes/kenney_preview.json, 右键旋转/滚轮缩放看清每个候选
:: B) 单帧截图(固定视角)
.\build\msvc\release\neon_game.exe --scene projects\neon_realm\assets\scenes\kenney_preview.json --scripts projects\neon_realm --screenshot preview.png 10
```
`kenney_preview.json` 横向摆 9 类候选(tree/tree-high/rock-large/rock-small/lantern/cart/stall/
windmill/wall/road/fountain-round)。截图/编辑后告诉我：树用哪个、石用哪个、路要多宽、
墙/马车/摊/风车/喷泉各留哪个——我据此批量重摆 `realm.json` 村庄。
注意: 松�逡单机场景模式必须先构建 MSVC 版 neon_game(build-msvc\Release), build\release\ 是旧 MinGW 车。


## 引擎画质升级（G1: 分级 / 动态分辨率 / TAA / 贴花 / CSM 软阴影）

本轮引擎层改动的验收方式都在 `neon_game` 命令行上，逐项 A/B：

```
build-msvc\Release\neon_game.exe --scene projects\moba\assets\scenes\moba.json --scripts projects\moba --moba-autostart --smoke-test 1550 --screenshot out.png 1450
```

### 1. 画质分级 + 渲染分辨率
- `--quality low|medium|high|ultra`：一次性设 MSAA / 阴影贴图 / 阴影距离 / 软阴影强度 /
  SSAO / 体积光 / SSR / bloom 宽度 / 渲染分辨率 / 粒子预算。
  | 档 | scale | MSAA | 阴影 | 粒子 | 备注 |
  |----|-------|------|------|------|------|
  | low | 0.75 | 关 | 512 | 8k | SSAO/体积/SSR 全关 |
  | medium | 0.90 | 关 | 1024 | 16k | SSAO 开 |
  | high | 1.00 | 4x | 2048 | 32k | TAA 开 |
  | ultra | 1.00 | 8x | 2048 | 64k | TAA+SSR+体积光 |
- `--render-scale <f>`（0.4–2.0）单独覆盖分辨率缩放，`--render-scale 2.0` 可作为
  超采样参考图（用于和 TAA 做画质对比）。
- `--dyn-res <fps>`：按帧时 EMA 在 [0.6, render-scale] 内自动升降渲染分辨率。
  注意 HUD/文字画在 window 分辨率上，缩放渲染分辨率不会糊字。

### 2. 时域抗锯齿 TAA（Step E）
- `--taa` / `--no-taa`；`--taa-sharpen <0-1.5>` 控制解析后的 unsharp 增益（默认 0.5）。
- 实现：Halton(2,3) 亚像素抖动 → 历史缓冲用 Catmull-Rom 双三次重采样（相机重投影 +
  3x3 邻域 clamp 抑制鬼影）→ 3x3 tent unsharp 补细节。**锐化只作用于显示输出，
  不回灌历史**（否则高频会逐帧正反馈放大）。
- 逐物体 motion vector（Step E2）：DrawSystem 每帧缓存实体的上一帧 local-to-world，变换
  发生变化的物体额外写一张 RGBA16F 速度图（alpha = 1 表示该像素属于移动物体），resolve
  优先用 `prevUV = UV + velocity`，静态几何回退到深度重投影。
  - 环境变量 `NEON_NO_VELOCITY=1` 可强制关闭速度图做 A/B（同 `NEON_NO_DECAL_PROJECT`）。
  - 骨骼动画只取**实体变换**的速度（用 rest-pose 几何），逐骨骼形变暂无 velocity；
    原地摆动的手脚仍靠邻域 clamp 抑制。
  - Billboard 特效不写速度（其朝向每帧随相机重建，正确速度就是纯相机重投影）。
  - `Renderer::Stats().velocityDraws` 给出本帧写入速度图的 draw 数。
- 验收：`--taa` vs `--no-taa` 同帧截图，边缘锯齿/贴图闪烁应明显收敛；HUD 因在
  composite 之后绘制，始终不受 TAA 影响。

### 3. 深度投影贴花（Step C）
- `SpawnDecal(tex, pos, size, alpha [, r,g,b,additive [, height]])`、
  `SetDecal(ent, size, alpha [, height])`；`height` 是投影体积高度，
  贴花会按场景深度贴合地形/台阶，而不是悬空平面。
- 调试开关：环境变量 `NEON_NO_DECAL_PROJECT=1` 可强制退回平面 quad 做 A/B。

### 4. CSM / PSSM 软阴影（Step G / G2 / G3）
- 级联按视锥分割（PSSM）+ 纹素对齐 + PCSS 变半影软阴影；`SetShadowSoftness` 控制
  半影宽度，`SetShadowDistance` 控制最远级联。
- **G2 修复（真 bug）**：`DrawSystem::Draw` 每帧会调用两次 `RefreshShadowPass()`，
  第二次发生在投影体记录之前。旧实现无条件重跑 `RunPass`，于是**每帧都用空投射体列表
  把 3 张级联图全部清成"远处"**，主 pass 采样到的永远是空白阴影图。现在
  `RefreshShadowPass` 在"本轮没有投射体 && 阴影图已初始化"时直接返回。
  读回统计：修复前 cascade 0/1/2 非清屏像素 = 0/0/0，修复后 = 53612/58551/2538（1024²）。
- **G3 深度缓冲级联**：级联 FBO 改为带 depth24 附件的目标
  （`IRenderBackend::CreateRenderTargetWithDepth`），阴影 pass 用 `SetDepthTest(true,true)`
  让最近表面按纹素胜出。画家排序每帧只有一个排序键，"整张地图导成一个 mesh"的关卡
  （地形 + 树 + 塔）内部互相穿透时无解；depth test 后不再依赖排序。
  只有后端 `DepthAvailable()` 为 false（Intel FBO 深度缺陷）时才回退画家排序。
- **G3 关闭背面剔除**：阴影 pass 用 `CullMode::None`。Summoner's Rift 的整张地图在
  light space 下大量三角形被判为背面，旧实现下**地面根本没进阴影图**，
  于是地面永远采样到"无遮挡"而看不出任何树影。关闭剔除后阴影图才包含完整地形。
- **G3 双面着色法线**：lit shader 在算光照前加 `if (dot(N, V) < 0.0) N = -N;`。
  该关卡地面网格的法线朝下（导出时未翻面），导致 (a) 阳光项 `ndl = 0`，地面只剩 IBL
  环境光（画面扁平、发灰蓝）；(b) 阴影接收偏移 `vWorldPos + N * texel * offset` 把
  接收点埋到自身深度以下，地面 `shadow` 恒为 0。修好法线后 MOBA 地面首次真正吃到
  平行光 + 树影/塔影/英雄影。
- 验收（`--quality high`，同帧 A/B，1280x720，步长 2 采样）：
  | 对比 | mean | max | px>12 |
  |------|------|-----|-------|
  | 阴影贡献（G2 修复前基线） | 0.62 | 152 | 3490 (1.5%) |
  | 阴影贡献（G3 全部落地后） | 8.26 | 161 | 30733 (13.3%) |
  | TAA 开/关 | 8.00 | 159 | 44722 (19.4%) |
  | 整帧（G3 前 vs 后） | 35.39 | 205 | 206507 (89.6%) |
- 调试开关：`NEON_SHADOW_DEBUG=1` 让 lit shader 直接输出级联阴影因子
  （白 = 晒到太阳，黑 = 全遮挡），是区分"阴影图坏"和"接收端坏"的最快手段。
  `NEON_NO_SHADOWS=1` 仍可整体关阴影做 A/B。
- 内容侧同步改动：`projects/moba/assets/scenes/moba.json` 的 renderstack `exposure`
  2.0 -> 0.8（原来的 2.0 是在"地面完全没有阳光"的前提下调出来的补偿曝光，
  法线修好后继续用 2.0 会整体过曝）。

## Vulkan 后端补齐（TAA / 速度图 / lit 特性）

GL 侧的画质改动此前未同步到 Vulkan；本轮把 `--backend vulkan`（RTX 2060, Vulkan 1.4）补齐到与 GL 同一水平，
并修掉两个只在 Vulkan 出现的真 bug。

### 1. TAA 开启后 3D 几何整体消失（真 bug，已修）
- 现象：`--taa --backend vulkan` 只剩天空 + HUD，地形/单位/树全部不见；`--no-taa` 正常。
- 根因：Vulkan 后端的"待清理"标记 `clearPending_` 是**全局单槽**。`BeginFrame` 对 HDR 目标
  `Clear()` 之后，速度图 pass 会 `BindRenderTarget(velocityRT_)` 再 `RebindMainTarget()`，
  `BindTarget` 每次都把该标记清掉 → 本帧 HDR 的颜色/深度**从未 clear**，第一帧起深度附件里是
  未定义内容（近平面），于是每个片元都过不了深度测试（只剩不吃深度测试的天空）。
- 修复：标记改为**按 target 记录**（`Target::clearPending/_Color/_Depth`），语义与 GL 一致
  —— `Clear()` 作用于调用时绑定的那个 target，无论它中间被换出去多少次。
- 判定依据：`NEON_NO_VELOCITY=1` 正常、TAA shader 换成 pass-through 仍复现 → 与 TAA/速度图
  shader 本身无关，是 target/clear 状态机的问题。

### 2. 速度图（motion vector）合批：每帧 1 个 pass
- 旧实现：`Renderer::SubmitVelocity` 对**每个运动物体**做一次
  `BindRenderTarget(velocityRT_)` + `RebindMainTarget()`。Vulkan 后端每次 `BindTarget` 切换目标
  都会 `vkEndCommandBuffer` + `vkQueueSubmit` + `vkWaitForFences`（一次完整 GPU 同步），
  即每个移动物体 2 次 GPU 停顿（也是上面 clear bug 的放大器）。
- 新实现：`SubmitVelocity` 只记录（`VelocityDraw{mesh, model, prevModel}`），
  由 `FlushVelocityPass()` 在 TAA resolve 之前**一次性**清空并绘制整批，
  目标 `velocityRT_` 每帧只绑一次（并真正 clear 颜色+深度）。
- 实测（Summoner's Rift, 1280x720, `--quality high --taa`）：13 FPS -> **97~145 FPS**。

### 3. 截图后 `VK_ERROR_DEVICE_LOST`（真 bug，已修）
- 现象：`--screenshot` 之后所有 `vkQueueSubmit` 返回 `VK_ERROR_DEVICE_LOST`，后续帧全部不提交
  （画面冻结在最后一帧，日志里刷 "queue submit failed"）。
- 根因：`ReadImage`（截图/回读）会把帧命令缓冲 `vkEndCommandBuffer` + 提交，
  而 `EndFrame` 里 `if (f.cmdOpen)` 为 false 就**跳过**了画面到 `PRESENT_SRC_KHR` 的布局转换，
  随后仍然 `vkQueuePresentKHR` —— 用 GENERAL 布局呈现 swapchain 图像是未定义行为。
- 修复：`EndFrame` 改为无条件 `OpenCmd(f)`（重新开始一个已提交过的 ONE_TIME 缓冲是合法的，
  提交前已 wait 过对应 fence）后再记录 present 转换；同时把 `vkQueueSubmit` 的返回值
  （`VkResultName`）打进日志，避免再次"静默丢帧"。
- 验证：`--smoke-test 400 --taa` + 截图 → 0 条 submit failed（修复前 ~2800 条）。

### 4. lit shader 与 GL 对齐（补 A2/A3/高光/接收阴影）
`lit.frag` 之前缺 GL 已有的这些项，Vulkan 画面因此偏平；现已按 GL 源码 1:1 移植：
- A2 法线贴图：`uNormalMap`(unit 23) + `uNormalScale` + `uHasNormalMap`，用 dFdx/dFdy 重建切线基。
- A3 半球环境光：`uAmbientGroundColor`（天光/地面反弹按法线 Y 过渡）。
- A3 探针 GI：`uLightProbeAtlas`(unit 24) + `uLightProbeMin/Extent/Res/InvMax/Enabled`，三线性采样。
- `uReceiveShadow`（材质级关闭接收阴影）、`uHighlightColor/uHighlightStrength` 菲涅尔描边高光。
- 顺带补上 GL 有而 VK 漏掉的：tint HDR 自发光（`max(uTint.rgb-1,0)`）、雾区间退化保护。
- UBO 从 6976 扩到 **7152** 字节（`engine_ubo.glsl` 与 `kUniformOffsets` 同步），
  采样器 unit 23/24 加入 set=1（未绑定时自动落到白色 fallback 纹理，未启用即无效）。

### 5. HDR 现状更正
- 之前的注释称"驱动采样 SFLOAT 返回黑、内部回退 RGBA8（伪 HDR）"——**已过时**。
  当前 `CreateRenderTarget(floatColor=true)` 就是 `VK_FORMAT_R16G16B16A16_SFLOAT`，
  本机 RTX 2060 的 HDR FBO 自检 + MSAA 4x 自检都 PASS（日志 `HDR float-target pipeline ACTIVE`），
  >1.0 的高光/bloom 是真实存在的；若某驱动过不了自检，渲染器会整体回退到非 HDR 路径。

### 验收命令
```bat
:: Vulkan（注意 --taa/--no-taa 放在 --backend 之前）
build-msvc\Release\neon_game.exe --scene projects\moba\assets\scenes\moba.json --moba-autostart ^
  --smoke-test 1550 --quality high --taa --backend vulkan --screenshot build-msvc\vk_p8.png 1450
:: GL 对照：去掉 --backend vulkan
```
- 期望：地形/森林/单位/阴影/HUD 齐全，`--taa` 与 `--no-taa` 画面一致（仅边缘更稳），日志无
  `queue submit failed`；本机 1450 帧截图为 vk_p8.png / gl_p8.png。
- `neon_tests`：836/841（5 个 PostGraph* 失败为既有基线，与本轮无关）。

### 仍未实现（如实记录）
- 点光源阴影：Vulkan 与 GL 一样仍关闭（`uPointShadowEnabled` 恒 0）。
- 资源上传仍是"每次上传一次提交 + fence 等待"（`CreateTexture`/`UploadBuffer`），
  大 glTF 分块流式加载时会有可见卡顿；建议后续做**每帧一次合批上传**。
- UI 文本像素差异：字体栅格化路径不同，Vulkan 与 GL 的字形边缘有细微差别。

## 召唤师峡谷：多套元素龙地形状态叠画（真 bug，已修）

### 症状
整张峡谷的地面把**多套地形状态画在了同一块坐标上**：同一个格子被
Base / Upgraded / Walled / Tunnel × (BaseLayer / ChemtechLayer / HextechLayer / CloudLayer /
OceanLayer / InfernalLayer / MountainLayer) 重复画一遍，另有一整套 `_earth_*` 重复拷贝。
结果就是地面出现 z-fighting 补丁、河道/中路一带的错位贴图；做单层过滤实验时还会看到
（去掉 `_default_*` 后）中心露出天空的大洞。

### 根因与证据（针对 `assets/models/sr/sr_map.glb`）
- 181 个 primitive，只有 136 个不同包围盒；**21 组 primitive 的 POSITION min/max 完全相同，
  合计 66 个 primitive 落在重合组里**。
- 最大那组 21 个成员 = 7 个元素层 × {Base, Upgraded, Walled}，每个都是 269 tri、包围盒
  一字不差，例：`_blend_master_Base_BaseLayer_Terrain_001_Baked`、
  `_blend_master_Upgraded_ChemtechLayer_Terrain_Baked`、
  `_earth_Walled_MountainLayer_Terrain_Baked`…
- 三套整体拷贝：`_default_*` 77 个（真正在用的地图）、`_blend_master_*` 60 个（状态层）、
  `_earth_*` 38 个（重复拷贝），另有 VertexDeform 植被 / Rubble / 塔基 6 个。

### 修复：直接从 mesh 资产里删掉状态层
`tools/lolimport/glb_strip_layers.py` 对已烘好的 .glb 做删除 + GC：丢掉状态层 primitive，
再把不再被引用的 material / texture / image / accessor / bufferView 回收，最后重打包二进制块
（保留的 primitive 数据一字不动）。默认规则就是运行时 `SceneMesh::materialExclude` 那份清单：

```
python tools/lolimport/glb_strip_layers.py --in <map>.glb --out <clean>.glb --dedupe-bbox
```

- 默认排除：ChemtechLayer / HextechLayer / CloudLayer / OceanLayer / InfernalLayer /
  MountainLayer / `_Tunnel_` / `_Upgraded_` / `_Walled_` / `_earth_`。
- `--dedupe-bbox` 再兜一层：POSITION 包围盒与已保留 primitive 完全相同的直接丢弃。
- `--keep` / `--exclude` 可覆盖规则，`--dry-run` 只出报告，默认写出到 `--out` 不动原文件。
- 运行时那套 `materialInclude/materialExclude` **保留**：地图以后要做"元素龙切换"仍然用它，
  资产层删干净之后它只是安全网（`projects/moba/assets/scenes/moba_layer_test.json` 留作 A/B）。

### 验收（`sr_map.glb` 148.8 MB -> 125.6 MB）
| 指标 | 修复前 | 修复后 |
|------|--------|--------|
| primitive | 181 | 119 |
| 不同包围盒 | 136 | **119** |
| 重合包围盒组 / 涉及 primitive | 21 / 66 | **0 / 0** |
| material / texture / image | 181 / 95 / 95 | 119 / 66 / 66 |
| accessor / bufferView | 724 / 819 | 476 / 542 |
| 二进制块 | 148.6 MB | 125.5 MB |

- 结构自检：所有 accessor/index/material/texture/image 引用可解析、index < 顶点数、
  bufferView 4 字节对齐且不越界 —— 全过。
- **保留的 119 个 primitive 与修复前逐字节一致**（POSITION sha1 + 三角形数 + min/max 全等）。
- **与运行时过滤等价**：`glb_strip_layers.py` 留下的材质名集合，与用
  `moba_layer_test.json` 的 `materialExclude` 跑 `MaterialLayerSelected` 得到的集合
  完全相同（119 == 119，集合相等）。
- 整图俯视 A/B（同一相机、同帧、无任何过滤）：`neon_pixel_diff` 只有
  1075/921600 像素（0.1166%）不同，且差异全部落在叠层互相打架的位置（中路/河道几处红块）。
- 游戏场景 `moba.json`（不带任何过滤）现在直接就是对的：`build-msvc/clean_a.png`。

### 贴图修正：VertexDeform 植被（风摆草丛）

**根因**：`mapgeo_fbx_to_gltf.py` 靠材质名 token 从贴图目录里打分"猜"贴图，`VertexDeform_inst1`
（风摆草丛，455k tri 的整层植被）落到 FORCE 表里被强行贴成地形烘焙图
`upgraded_baselayer_terrain_bake1_pbr_diffuse`（581 KB 地表 splat，带大片黑色空洞）
-- 于是草丛是泥泞的橄榄色。

**权威映射（不再猜）**：游戏自己的 `DATA__Maps__mapgeometry__map11__base_srx.materials.bin`
存着每个材质的 `DiffuseTexture`，直接读出来：

| GLB 内材质 | materials.bin 里的源贴图 | 本地文件 | 性质 |
|---|---|---|---|
| `.../LevelProp/Materials/VertexDeform_inst1` | `ASSETS/Maps/KitPieces/SRX/textures/SRU_Brush.dds` | `..._textures__sru_brush.dds` | 256^2，全不透明（alpha 恒 255） |
| `.../Materials/VertexDeform_WaterLily_B_inst1` | `ASSETS/Maps/KitPieces/SRX/textures/Ocean_WaterLily_A.tex` | `..._textures__Ocean_WaterLily_A.dds` | 256^2，带 alpha（42% 像素低于 cutout） |

**方案**：
- `VertexDeform_inst1` -> `SRU_Brush.dds`，`alphaMode` 保持 `OPAQUE`；该贴图本身没有有效 alpha，
  压成 JPEG 无损失。
- `VertexDeform_WaterLily_B_inst1` -> `Ocean_WaterLily_A.dds`，`alphaMode = mask`
  （`alphaCutoff 0.4`），让花朵/荷叶的镂空成片透出，而不是整块方片浮在水面。
- 替换在**已剥层**的 GLB 上原地做（不重跑 FBX 导入），由 `glb_strip_layers.py` 的
  `--set-texture` / `--alpha` 完成，被换掉的旧 diffuse 引用会一并 GC：

```
python tools/lolimport/glb_strip_layers.py --in <strip>.glb --out <fixed>.glb --dedupe-bbox ^
  --set-texture "VertexDeform_inst1=_textures__sru_brush.dds" ^
  --set-texture "VertexDeform_WaterLily_B_inst1=_textures__Ocean_WaterLily_A.dds" ^
  --alpha "VertexDeform_WaterLily_B_inst1=mask"
```

- `--set-texture` / `--alpha` 的 key 是**材质名子串**；`embed_texture()` 只重烘 baseColor
  （不透明走 JPEG、带 alpha 走 PNG），默认上限 1024^2。
- 导入侧同步修（以后重导出别再错）：`mapgeo_fbx_to_gltf.py` 的 `FORCE` 表加
  `("waterlily", ...)` / `("vertexdeform", ...)` 两条，且 `find_tex()` 改成匹配小写化**完整路径**，
  这样片段能锁定到具体一个文件，而不是靠短词撞运气。

**验收**（`NEON_GLTF_ONLY_MAT=VertexDeform` 只渲染该材质，同相机同帧）：
- `build-msvc/iso_old.png`（泥橄榄色）vs `build-msvc/iso_fixed.png`（鲜绿草丛 + 紫花）。
- 成品 GLB 实测：`VertexDeform_inst1` -> 13.3 KB JPEG，alpha 全 255（不透明）；
  `VertexDeform_WaterLily_B_inst1` -> 47.7 KB PNG，`alphaMode=mask` / cutoff 0.4，42.5% 像素被裁掉。
- 119 个保留 primitive 的几何与纯剥层结果**逐字节相同**（只动贴图引用），texture/image
  66 -> 67。当时产出 125,074,308 字节；下面"全量重贴"把它改写成了 118,368,528 字节。

### 全量重贴：119 个材质各自绑回自己的贴图（已落地）

上面只修了 VertexDeform 两处；同一套"猜贴图"逻辑其实错了一大片（118 / 119 个材质绑错）。
现在改成**从游戏数据里读**，不再打分猜：`base_srx.materials.bin` 里每个材质都有自己的
`DiffuseTexture`（少数是旧写法 `Diffuse_Texture`），材质路径和 .glb 里的 `name` 一字不差。

新工具 `tools/lolimport/glb_remap_textures.py`：

```
# 1) 从游戏数据抽出 材质 -> 贴图 表（119/119 全部解析成功）
python tools/lolimport/glb_remap_textures.py extract ^
  --materials-bin F:/assets/fantome/named/DATA__Maps__mapgeometry__map11__base_srx.materials.bin ^
  --glb projects/moba/assets/models/sr/sr_map.glb --out build-msvc/sr_tex_map.json

# 2) 按表重绑并重打包（几何一个字节都不动）
python tools/lolimport/glb_remap_textures.py apply ^
  --in projects/moba/assets/models/sr/sr_map.glb --map build-msvc/sr_tex_map.json ^
  --named-dir F:/assets/fantome/named --out build-msvc/sr_map_remap.glb
```

结果（输入 = 上一条修完的 125.1 MB 资产）：

| 指标 | 之前 | 之后 |
|------|------|------|
| 材质绑到正确贴图 | 1 / 119 | **119 / 119** |
| 无贴图的材质 | 3 | **0** |
| texture / image | 66 / 66 | 88 / 88 |
| 内嵌贴图体积 | 19.9 MB | 11.1 MB |
| .glb | 125.1 MB | **116.3 MB** |
| primitive 几何 | 119 | 119（**逐字节相同**） |

`_default_X` 和 `_blend_master_X` 这类成对材质引用同一张原图，`rebuild()` 现在按内容 sha1 复用，
所以 119 个材质只留 88 张图（去重前是 119 张、13.1 MB；去重后 88 张、11.1 MB）。

**两个坑（都踩过，工具现在会拦住）**
- 解包 dump 里的 `.png` 大都是**坏的**：同一张图既有 `.dds`（正常）又有 `.png`（解出来是彩色噪声）。
  第一版按 `.png` 重贴，地面直接变成白花花的噪点图（`build-msvc/remap_a.png` 就是那次翻车）。
  现在 `.dds` 优先，并且每个源图都过一道噪声检查（相邻像素差 > 60 直接拒绝写盘）。
  - 交叉验证：`.dds` 与旧资产里已经在用的贴图相关系数 1.000。
- `.tex` 的目录和 dump 暴露的目录对不上：`SRX/CustomMap/wallofgrass.tex` 实际是
  `Map11/textures/wallofgrass.dds`。所以解析顺序是 精确路径 `.dds` -> 同名 `.dds` -> 其它后缀。

**顺带修掉的两个 alphaMode 问题**
- 引擎（`engine/src/assets/asset_manager.cpp:1255`）原来是 `alpha == "MASK"` **逐字符比较**的：
  上一条给睡莲写的小写 `mask` 等于没生效，睡莲一直是**不透明**的。两处都修了：loader 先把
  `alphaMode` 折成大写再比（大小写都认），`glb_strip_layers.py` 写出时一律用大写 `MASK` +
  `alphaCutoff 0.4`（45 个材质）。验证：把全部 45 个材质改回小写 `mask` 重新出图，与大写版
  逐像素只差 32 / 921600（0.0035%，噪声级），即小写不再退化成不透明。
- 31 个材质用的是 `alphaCutoffFactor`（不是 glTF 字段，引擎当兼容写法认）。已统一成
  `alphaCutoff`；OPAQUE 材质上残留的 cutoff 一并清掉。

**分辨率选择**：地形烘焙图 UV 全是 0..1（单张铺满全图的 splat），游戏相机下地面被放大十几倍，
2048² 是浪费（2048²/q80 会让资产涨到 194 MB）。压到 1024² 后铺装细节仍然清晰，资产反而比
原来更小。要换回去用 `--max-size 2048`，质量用 `--quality`。

**验收**（都在 `build-msvc/`）
- `overview_before.png` vs `overview_after.png`（同一俯视相机、同一帧）：基地从一块发亮的荧光绿
  变成带砖缝、圆形铺装、青苔的石砌庭院。
- `ab_paving.png` / `ab_props.png` / `ab_mid.png`：同区域左右对比（左=之前，右=之后）。
- `remap_b.png`：游戏内整图（`moba.json` 默认相机）。
- 结构检查：119 primitive 的 POSITION / TEXCOORD_0 / NORMAL / indices 与输入**逐字节相同**，
  material 名集合相等，119 material / 88 texture / 88 image，`alphaModes = {MASK: 45, OPAQUE: 74}`；
  逐材质回验：119/119 的 baseColorTexture 与 `materials.bin` 指到的源图相关系数 > 0.98（0 个不符）。
- 引擎改动后重编，`neon_tests` 836/841（5 个 `PostGraph*` 为既有失败，与本次无关）。

**复核用的基线**：`build-msvc/sr_map_prefix.glb`（重贴前那份 125.1 MB 资产，125,074,308 字节）留着，方便重跑这组 A/B；
重贴前的原始资产是 `F:/assets/sr/sr_map.glb`。其余中间产物已清掉，`build-msvc/` 里只留证据图 + 这份基线。
全部证据图都在 `build-msvc/`：`remap_a.png`（踩坑那版）、`remap_b.png`、`overview_*.png`、`ab_*.png`。

## 召唤师峡谷：整层地面贴花被删（真 bug，已修）

### 症状
上面那次"全量重贴"之后，每个材质绑到的贴图都是对的了，但整张图**还是不对**：
地面看上去发平、发空。车道两侧该有的石板路、基地里的苔藓/草丛/花丛、防御塔和
兵营/水晶脚下的石砌基座、基地外圈的碎石缝隙，全都没有 —— 只剩一层单调的铺装。

### 根因
`tools/lolimport/mapgeo_fbx_to_gltf.py` 分组时无条件丢掉所有名字里带
`decal` / `seam` 的材质：

```python
if "decal" in mat_name.lower() or "seam" in mat_name.lower():
    continue
```

注释给的理由是"贴花是 alpha 混合叠层，不透明导入会变成硬边条带"。理由没错，
**错在把材质整个扔掉**：这 16 个是峡谷自己画在地面上的"地贴"，游戏每一帧都在画它们。
源数据一共 197 个 primitive = 181 个常规 + 16 个被这句话删掉的贴花。

被删掉的 16 个材质（左侧名字即 GLB 里的材质名，右侧是 `materials.bin` 里它自己的 DiffuseTexture）：

| 材质 | 源贴图 | 作用 |
|---|---|---|
| `NVRMaterial_new_stone_road_decalVersion3_no_shadow` | `new_stone_road.tex` | 车道石板路 |
| `NVRMaterial_order_tile_floor_border_decalVersion3_no_shadow` | `order_tile_floor_border.tex` | 基地铺装分块 |
| `NVRMaterial_order_ground_mix2_decalVersion3_no_shadow` | `order_ground_mix2.tex` | 草地/碎石混合 |
| `NVRMaterial_order_ground_moss_patch1_decalVersion3_no_shadow` | `order_ground_moss_patch1.tex` | 苔藓斑块 |
| `NVRMaterial_grasstuft_decalVersion3_no_shadow` | `order_base_decal_mid.tex` | 草丛 |
| `NVRMaterial_flowerb_decalVersion3_no_shadow` | `decal_blue_flower.tex` | 蓝花丛 |
| `NVRMaterial_flowerp_decalVersion3_no_shadow1` | `decal_purple_flower.tex` | 紫花丛 |
| `NVRMaterial_nexus_stoneBase_decalVersion3_no_shadow` | `nexus_stonebase.tex` | 水晶基座 |
| `NVRMaterial_lambert144_decalVersion3_no_shadow` | `inhibitor_stonebase.tex` | 兵营基座 |
| `NVRMaterial_lanetowerdcl_decalVersion3_no_shadow` | `bluetower_decal.tex` | 防御塔基座 |
| `NVRMaterial_base_chasm1_decalVersion3_no_shadow` | `base_chasm1.tex` | 基地外圈碎石 |
| `NVRMaterial_base_chasm2_decalVersion3_no_shadow` | `base_chasm2.tex` | 基地外圈碎石 |
| `NVRMaterial_order_tile_floor_mark1_decalVersion3_no_shadow` | `order_tile_floor_mark2.tex` | 地面印记 |
| `NVRMaterial_Order_seam_decalVersion3_no_shadow` | `order_seam.tex` | 区域接缝 |
| `NVRMaterial_firepit_ash_decalVersion3_no_shadow` | `sr_firepit_ash_decal_tx_dm.tex` | 篝火灰烬 |
| `NVRMaterial_v_decalVersion3_no_shadow` | `chaos_root_base_decal_mid.tex` | 混沌方基地地贴 |

证据：
- 从 `F:/assets/sr/base_srx.fbx` 重新导入得到 **197** 个 primitive，与
  `F:/assets/sr/sr_map.glb` 的对应 primitive 逐字节一致（POSITION / NORMAL /
  TEXCOORD_0 / indices 全等），即这 16 个就是被上面那句删掉的那一批。
- 16 张贴图全是 RGBA，**27% – 88% 的像素 alpha < 250** —— 是真 alpha 混合叠层，
  不是"没有 alpha 的硬边片"。

### 修复
1. **导入端**（`tools/lolimport/mapgeo_fbx_to_gltf.py`）：不再丢弃，保留几何并标
   `alphaMode = "BLEND"`（新增 `is_decal()`）。既进了场景，又由贴图自己的 alpha 做
   软混合，不会变成硬边条带。顺带的效果正好对上源数据：BLEND 材质在引擎里
   `material.transparent = true`，于是不进 CSM / SSAO 的 caster 列表
   （`engine/src/gfx/renderer.cpp:833`），与这些材质在源数据里的 `_no_shadow` 命名一致。
2. **重贴端**（`tools/lolimport/glb_remap_textures.py`）：MASK / OPAQUE 是**贴图**的性质
   （有没有 alpha），可以从图里推；**BLEND 不能推**，它是"怎么和底下混合"的**材质**属性，
   必须继承。原实现一律由贴图推导，会把 BLEND 重算成 MASK，把软边切成硬边。
3. **打包端**（`tools/lolimport/glb_strip_layers.py` 的 `rebuild()`）：换贴图时把
   `baseColorFactor` 复位成白。旧行为是"保留材质原有 tint"，但那个值是导入器
   `category_color()` 的**预览**调色板（不是游戏数据）；留着它等于给刚绑上的真实贴图
   乘一个 0.42 的灰棕 —— 16 个贴花当场变成一层灰蓝色"水膜"（第一版重导入就是这个翻车现场）。

### 完整流水线（可复跑）

```
# 1) FBX -> glb：197 个 primitive（181 常规 + 16 贴花）
#    --center --scale 0.007 复现既有资产的世界坐标（地图中心到原点）
python tools/lolimport/mapgeo_fbx_to_gltf.py --fbx F:/assets/sr/base_srx.fbx ^
  --out build-msvc/sr_base_197.glb --center --scale 0.007

# 2) 删掉元素龙状态层（62 个 primitive）-> 135
python tools/lolimport/glb_strip_layers.py --in build-msvc/sr_base_197.glb ^
  --out build-msvc/sr_135.glb --dedupe-bbox

# 3) 抽 材质->贴图 表并重贴，产物拷进工程
python tools/lolimport/glb_remap_textures.py extract ^
  --materials-bin F:/assets/fantome/named/DATA__Maps__mapgeometry__map11__base_srx.materials.bin ^
  --glb build-msvc/sr_135.glb --out build-msvc/sr_tex_map_135.json
python tools/lolimport/glb_remap_textures.py apply ^
  --in build-msvc/sr_135.glb --map build-msvc/sr_tex_map_135.json ^
  --named-dir F:/assets/fantome/named --out build-msvc/sr_map_decals_raw.glb

# 4) 修正地面贴花 UV：每个放置实例映射一次贴图（否则印章整图平铺，见下一节）
python tools/lolimport/glb_normalize_decals.py ^
  --in build-msvc/sr_map_decals_raw.glb --out build-msvc/sr_map_decals.glb
copy build-msvc\sr_map_decals.glb projects\moba\assets\models\sr\sr_map.glb
```

（上一节"全量重贴"里那次跑的临时产物叫 `sr_map_remap.glb`，已经清理；现在这条流水线
的产物叫 `sr_map_decals.glb`。16 个贴花都落在地图原有包围盒内，所以加上它们不会改变
`--center` 算出来的中心，世界坐标与旧资产完全一致。）

### 验收

| 指标 | 修复前 | 修复后 |
|---|---|---|
| primitive | 119 | **135** |
| material / texture / image | 119 / 88 / 88 | **135 / 104 / 104** |
| alphaMode | MASK 45 / OPAQUE 74 | **MASK 45 / OPAQUE 74 / BLEND 16** |
| `.glb` | 116,329,224 B | **124,019,072 B** |

- 原 119 个 primitive 逐个复核：POSITION / NORMAL / TEXCOORD_0 / indices、贴图字节、
  `alphaMode` / `alphaCutoff` / `doubleSided` **全部一致**（0 处不同）。
- 唯一有意的差异：`_default_NVRMaterial_shop_base_` / `_default_NVRMaterial_statue_warrior` /
  `_default_NVRMaterial_Chaos_stones_` 这 3 个材质在旧资产里残留着导入器的预览色
  （0.42/0.40/0.36），现在复位为白 —— 也就是说旧资产里它们一直被压暗约 60%。
- 贴花确实贴在地面上：每个贴花顶点到最近地面顶点的垂直差 dy 中位数只有
  **+0.003 ~ +0.053 世界单位**（125 单位宽的地图）。
- 同一相机同帧 A/B（`moba_overview.json`，frame 900，`build-msvc/`）：
  - `decals_skip.png`（`NEON_GLTF_SKIP_MAT=decalVersion3` 关掉贴花）
    vs `decals_overview.png`（开着）：**6.49% 像素不同**，差异集中在车道与基地。
  - 关掉贴花的新资产 vs 旧资产（`sr_map_119.glb`）：**0.04% 像素不同**（就是上面那 3 个
    材质），即"加贴花"本身没有其它副作用。
  - `decals_diff.png` 是差异热力图；`ab_decals_base/mid/left/right.png` 是分区左右对比
    （左 = 关贴花，右 = 开）。
- 渲染确定性：同配置连渲两次只有 39/921600（0.004%）像素不同，上面这些百分比都远高于噪声底。
- 可复跑：把上面三步流水线重跑一遍，产物与工程里那份 **SHA1 完全相同**
  （`F91B249FC389E028144E78153799FA3D4A8E2873`，124,019,072 字节）。

### 负结果：`_default_Terrain_All_1_Baked` 的 V 方向不是 bug（别再"修"它）

这一格的 `dv/dz` 是 **+0.04146**，而其余 24 格都是 **-0.04146**（`build-msvc/vsign.py`），
看上去很像是这一格的 UV 被镜像了。**其实不是**：这一格**贴图本身的 v 存储方向**与其它格相反，
UV 里的符号差正是补偿。把它"掰正"反而会撕开接缝：

| 相邻格对（共享边） | 原样（当前资产） | tile1 的 v 取反 |
|---|---|---|
| `_default_Terrain_All_1_Baked` ↔ `…_6_Baked` | **3.19** | 12.98 |
| `_default_Terrain_All_1_Baked` ↔ `…_2_Baked` | **2.41** | 20.18 |

（判据脚本 `build-msvc/seam_edge.py`：取两格共享边上的顶点，**读网格里存的实际 UV**，
按 GPU 的采样方式取色再算 MAD；其它相邻格对的基线在 2.8 ~ 4.9。）

按实际 UV 采样拼出的世界俯视图同样看得出来：`build-msvc/ground_uv_asis.png` 接缝连续，
`build-msvc/ground_uv_tile1_mirrored.png` 里 tile1 那一格出现硬边错位，
`build-msvc/ground_uv_tile1_ab.png` 是两版并排。

教训（重要）：`vsign.py` / `alltiles.py` 这种"图像行对图像行"的启发式**没有读真实 UV**，
于是把"这一格贴图是反的"误判成"这一格 UV 是反的"。判断地面拼合时，只有"按存下来的 UV
采样"的结果才算数。

### 复核用基线
- `build-msvc/sr_map_prefix.glb`（125,074,308 B）："全量重贴"之前的资产。
- `build-msvc/sr_map_119.glb`（116,329,224 B）：加贴花之前的资产（状态层已删、贴图已重贴）。

其余证据图都在 `build-msvc/`：`sheet_decals.png`（16 张贴花贴图铺在中灰上的样子）、
`sheet_ground.png` / `sheet_props.png`（贴图盘点）、`decal_height.py`（贴地高度量测）、
`mosaic_live.py`（按真实 UV 拼世界俯视图）。

## 召唤师峡谷：地面贴花 UV 平铺错位（真 bug，已修）

### 症状
上面那次把 16 个地面贴花加回来之后，每个材质绑到的贴图都对了，但整张图**又不对了**：
地面像一盘被打乱的拼花，**防御塔基座的圆环满地都是**，各种贴花图案（车道石片、花丛、
裂石、苔藓）在整张图上重复平铺、互相叠色。对比：关掉贴花（`NEON_GLTF_SKIP_MAT=decalVersion3`）
地面干净，开着就花。

### 根因
这 16 个 `*_decalVersion3_*` 贴图是**单枚印章**（`build-msvc/decal_layer_only.png`
单独看贴花层就很清楚）：`lanetowerdcl` 是一枚防御塔基座圆环，`flowerb` 是一丛蓝花，
`base_chasm1` 是一块裂石地面……贴图都带柔和的 alpha 边。但源 FBX 给它们的是**共享的
世界投影 UV 场**（`uvU/uvV` 范围远超 0..1），于是每个印章在它自己的 patch 上**平铺
2–8 次**：`lanetowerdcl` 的圆环就在整张图上密密麻麻。游戏本身用 decal shader 按
贴花数据重新投影，这份数据（`map11.bin.json` 是 VFX，不是地图贴花；`base_srx.materials.bin`
里也没有 UV 变换字段）在静态 .glb 里没有。

### 修复：按连通分量把每个贴花实例映射一次贴图
新工具 `tools/lolimport/glb_normalize_decals.py`：
- 把每个 decal primitive 的三角汤按**共享顶点**拆成连通分量（一个分量 = 一枚摆好的贴花，
  16 个材质合计 748 个实例）；
- 把每个分量的 `TEXCOORD_0` 包围盒归一化到 `[0,1]`（`--mode fit`，另有 `square` / `world`）；
- **几何一个字节都不动**：只重写 decal primitive 的 `TEXCOORD_0` accessor，输入输出同尺寸。
- `order_ground_mix2`（89 个四边形）和 `grasstuft` 本来就是按实例给了 0..1 的 UV，归一化
  对它们是恒等操作——正好印证"一个分量 = 一枚贴花"。

```
python tools/lolimport/glb_normalize_decals.py \
  --in projects/moba/assets/models/sr/sr_map.glb \
  --out build-msvc/sr_map_decals.glb        # 再 copy 回工程
```

### 验收
| 指标 | 修复前 | 修复后 |
|---|---|---|
| decal primitive / 实例 | 16 / 748 | 16 / 748 |
| TEXCOORD_0 被改的 decal primitive | — | **14**（另 2 个本就 0..1，不变） |
| POSITION / NORMAL / indices | — | **逐字节相同，0 处不同** |
| `lanetowerdcl` 的 UV 范围 | `U[-1.25,2.57] V[-1.43,2.97]` | **`U[0,1] V[0,1]`** |
| `.glb` | 124,019,072 B | **124,019,072 B**（同尺寸，SHA1 `33D95698FE6DB76670E8907E346A875A84F85E2C`） |

- 结构校验：135 primitive 中，非 decal 的 121 个 `TEXCOORD_0` 与几何全等；14 个 decal 的
  `TEXCOORD_0` 改变（正是那两个 0..1 的没变）；`lanetowerdcl` 归一后恰好铺满 0..1。
- 场景侧：`projects/moba/assets/scenes/moba.json` **不带任何 `materialExclude`** 就对了
  （贴图修对之后又回到了"无过滤"的基线，`materialExclude` 只留作元素龙切换的安全网）。
- 视觉：`build-msvc/decal_bug_game.png` / `decal_bug_ov.png`（修复前，花斑）
  vs `decal_fixed_game.png` / `decal_fixed_ov.png`（修复后，石砖车道 + 车道圆台/苔藓/花草）；
  `decal_layer_only.png` 是贴花层单独渲染。
- 注意：贴花仍是透明叠层，近景（`decal_fixed_game.png`）已经很 LoL；俯视看基地一带贴花较密，
  属主观观感，如需更"素"可只对个别材质收窄 `--tag`（工具按材质名子串选）。

### 复跑与基线
- 上游流水线第 4 步就是本工具（见上一节），复跑产物即 `build-msvc/sr_map_decals.glb`。
- 复核基线：`build-msvc/sr_map_119.glb`（无贴花）、`build-msvc/sr_map_prefix.glb`（重贴前）。

## 定向光阴影（CSM）：缺失 / 硬边切断 / 随相机抖动（真 bug，已修）

### 症状
- 地面**有的地方有阴影、有的地方没有**，阴影在一条直的边缘上被硬生生切断；
- 同一个静止物体，**只移动相机（晃动鼠标）**，阴影的形状/长度就变了。

### 根因
`engine/src/gfx/csm.cpp` 的 `ComputeCascadeLightViewProj` 把每个级联的正交包围盒
**与"投影物（caster）并集"的光空间 AABB 求了交集**（旧代码 55–80 行，注释写的
意图是"别让一个小场景被挤到地图角落"）。但那份 `sceneBounds` 只由**本帧可见、
且上一帧缓存的 caster** 组成（`shadow_system.cpp:302-321`），而地面一般是
`castShadow=false` 的纯接收体，落在该 AABB 之外：

- 正交盒实际只覆盖 `相机切片 ∩ caster 包围盒`，切片内落在 caster AABB 外的接收面
  全被裁掉，片元投影出盒后按"受光"处理（`lit.frag:369`）—— 于是有阴影/没阴影并存，
  并在 AABB 边缘留下一道硬切；
- caster 集合随相机平移变化 ⇒ 正交盒的大小/位置/纹素尺度每帧变 ⇒ 阴影随鼠标
  抖动/跳变。

### 修复
1. `csm.cpp`：**不再在 XY 上与 caster AABB 求交**。XY 恒等于相机切片（覆盖全部
   可见接收面）；只把光空间 **深度（z）** 范围扩展到 caster，让位于切片之外、沿
   光方向仍可能投影进画面的物体照样进阴影图。这样所有可见接收体都在盒内，且盒
   不再依赖"当前可见 caster 集合"。
2. 级联切换去硬缝：`lit.frag` 与 GL 版 `builtin_shaders.hpp` 都新增 `CascadeShadow()`，
   在每条分割线两侧各留 5% 的混合带（`smoothstep` 交叉淡入淡出），消除级联切换处
   的硬接缝。两段着色器保持一致；Vulkan 的 `engine/generated/vk_shaders.hpp` 在构建
   时由 `tools/gen_vk_shaders.ps1` 重新生成。

### 验收
用新增的简单场景 `projects/default/assets/scenes/shadow_test.json`
（平地面 + 高塔/矮塔/球 + 低角度太阳，静态、无脚本）：

| | 修复前 | 修复后 |
|---|---|---|
| 左塔阴影 | **完全没有** | 完整落地 |
| 中间高塔阴影 | 远端被切出尖角 | 完整 |
| 移动相机后同一物体的阴影 | 形状/长度明显变化 | 一致（只剩透视差异） |
| 级联接缝 | 硬缝 | 5% 混合带，平滑 |

- MOBA 实机：`build-msvc/shadow_moba_game.png`（塔/树/单位阴影连贯）。
- 对比图：`build-msvc/shadow_cam{A,B}.png`（修复前）vs `shadowfix_cam{A,B}.png` /
  `shadowblend_camA.png`（修复后）；`shadow_camC.png` / `shadowfix_camC.png`。
- `neon_tests` 836/841（5 个 `PostGraph*` 既有失败，与本改无关）。

### 复现命令
```
neon_game.exe --scene projects\default\assets\scenes\shadow_test.json ^
  --smoke-test 40 --screenshot out.png 30
set NEON_NO_SHADOWS=1     :: 关阴影做 A/B
set NEON_SHADOW_DEBUG=1   :: 直接输出级联阴影因子（白=受光，黑=被挡）
```

## 渲染管线全面体检：阴影 / 光照 / 后处理（一批真 bug，已修）

四路并行审计（CSM、光照着色器、Renderer/编辑器集成、后处理链）定位出以下问题，
全部修复并验证。GL 与 Vulkan 两条后端同步修改。

### 已修（按可见影响排序）
1. **SSAO 用的是未模糊的原始 AO**（`post_graph.cpp`）：composite 读的是 `post.ssao`
   直出纹理而不是模糊链产物 `aoBlurB_`，稀疏 6-tap 核直接上屏 → 表面黑色斑点
   （"破面"的主要来源之一）；两道模糊 pass 白跑。现读 `aoBlurB_`。
2. **SSAO 强度 >1 会外推出负值**（`post_graph.cpp`）：`mix(1.0, ao, i)` 对 i>1 是外推，
   i=2.5 时可到 -0.75 → ACES 后成黑斑；编辑器滑条允许 0..10。上传处 clamp 到 [0,1]。
3. **合成雾/体积光/SSR 把线性深度当 NDC 深度二次反推**（`bloom.hpp`、`composite.frag`、
   `volumetric.hpp/.frag`、`ssr.hpp/.frag`）：深度 RT 存的是**线性视距/uFar**（SSAO 深度
   编码器），消费端却用透视公式反推 → 所有距离坍缩到 ~2*near，**浓度雾从没生效过**、
   体积光步进区间为 0、SSR 按垃圾深度 marching。全部改为 `ndc * uFar`。
4. **阴影偏移量(bias)随场景大小失控**（`builtin_shaders.hpp` + `lit.frag`）：bias 固定
   0.0008..0.01 是**归一化深度**单位，级联深度范围被全场 caster 并集拉到数百单位后，
   最小 bias 相当于 0.5+ 世界单位 → 接触阴影脱离投影物（peter-panning）、薄墙漏光。
   改为按"整个阴影纹素"计量（`biasUnit = texelWorld/zRange`，zRange 从 `uLightVP[c][2].z`
   反推），世界空间偏移恒为 ~[0.2, 8] 纹素，与场景尺度无关。
5. **阴影接收端法线偏移用了法线贴图扰动后的法线**（两后端）：扰动法线的横向分量把采样
   沿表面滑动而不是抬离表面 → 轮廓漏光 + 弧面 acne 条带。改用几何法线（vNormal，
   同样做面向观察者翻转）。
6. **太阳阴影会把自发光/点光源/玩家灯/雾一起乘没**（两后端 lit shader）：
   `sunTerm = max(color-ambient,0)` 把 emissive、tint 自发光、点光、玩家灯全卷进太阳项，
   全影区里发光物直接熄灭；雾在阴影合成前混合，阴影还会把雾本身染暗。重构为
   extraLight 单独累加、阴影只作用于太阳直射项，雾移到合成之后。
7. **编辑器模型预览面板把预览模型记成了场景 caster**（`model_preview_panel.cpp`）：
   没像缩略图那样关 `SetShadowRecording`，面板开着时预览模型（画在世界原点）下一帧
   变成场景阴影 caster → 原点附近鬼影/闪烁。已包裹关闭/恢复。
8. **离屏工具渲染的 SetCamera 会清空级联**（`renderer.cpp`）：缩略图/预览的 SetCamera
   触发一次空 caster 列表的阴影 pass（消费并清掉场景待用 caster），主视口随后跳过
   pass → 整帧阴影消失（间歇性"表面忽明忽暗"）。SetCamera 加了与 RefreshShadowPass
   相同的空列表守卫。
9. **蒙皮/实例化路径记录 caster 不检查 castShadow 且在视锥剔除之后**（`renderer.cpp`）：
   屏幕外角色照样丢影子、castShadow=false 的材质照样写深度。两条路径都改为剔除前
   记录并检查 castShadow（与静态路径一致；实例化仍只记可见实例——植被批量太大）。
10. **正交相机的级联包围球按 FOV 计算**（`csm.cpp`）：编辑器顶视/前视、缩略图等正交
    相机的半高恒为 orthoSize，按 fovY 算导致级联盒与实际视锥无关、对缩放不敏感。
11. **级联 pass 重跑后场景 uniform 不重传**（`shadow_system.cpp`）：只有点光源 pass
    bump stamp，中途 RefreshShadowPass 后 uLightVP 可能与实际阴影图不一致（潜在）。
    级联 pass 末尾同样 bump。`SetAmbientLight` 也补上了缺失的 bump（`scene_state.cpp`）。
12. **编辑器无渲染栈场景泄漏上一场景的阴影参数**（`editor_viewport.cpp`）：else 分支
    不重置 shadowDistance/Softness/NormalOffset → 上一场景的设置粘住。现重置为默认。

### 验收
- 简单场景 `shadow_test.json`：接触阴影贴回塔脚、地面无 acne、各级联过渡平滑
  （`build-msvc/pipe_shadow.png`）。
- 编辑器 `_ed_repro.json`（绿地面+高柱+墙+低太阳）：两个对向 yaw 下阴影完整、
  边缘干净，此前墙影边缘的锯齿梳状走样消失（`build-msvc/pipe_ed_y0_7.png` /
  `pipe_ed_y4_0.png` vs 旧 `ed_y0_7.png`）。
- MOBA 实机（`build-msvc/pipe_moba.png`）：单位/塔/树阴影连贯，雾正常。
- `neon_tests` 836/841（5 个 PostGraph* 既有失败，无关）。

### 已知但未修（记录在案，按需再做）
- MASK 材质（草叶/卡片）投影是实心四边形——阴影 pass 无 alpha 测试变体。
- 合成 SSAO 乘的是整幅颜色而非仅环境光（阴影区会被再压暗一档）。
- 自动曝光的"平均"只采了 1/32 降采样图中心 2x2 texel；且 autoExposure 直接替换
  （不是乘）authored exposure；bloom 阈值在曝光前。
- 曝光适配状态切换场景不重置、按帧率变化。
- 无 MSAA 配置下深度预-pass 退化为按包围盒排序的重画（SSAO/SSR 深度可能错序），
  贴花失去深度投影退回平面四边形。
- 光照贴图 GI(probe) 与天空 IBL 同时开时环境光可能双计。
- 编辑器 demo 场景的 terrain/水面默认 castShadow=true（引擎文档建议大地面应为
  receiver-only）；MOBA 地图实体同样（其 135 个子节点会整体投影，靠 3 级联全画）。

### 调试钩子
- `NEON_ED_YAW / NEON_ED_PITCH / NEON_ED_DIST`（编辑器）：headless 指定自由相机
  角度/距离截图（本轮 A/B 用；`editor_viewport.cpp`）。
- `NEON_SHADOW_DEBUG=1`、`NEON_DUMP_SHADOW=1`、`NEON_NO_SHADOWS=1`（引擎，原有）。



## 渲染管线第二轮体检：自动曝光 / 粒子 / 贴花 / 光照链（一批真 bug，已修）

五路并行审计（bloom+曝光链、TAA+速度图、粒子+贴花+透明、点光+探针 GI+IBL、SSR+体积光+雾）
定位出以下问题，全部修复并验证。GL 与 Vulkan 双后端同步。

### 已修（按可见影响排序）

1. **P0 软粒子在"窗口深度"空间做比较**（`builtin_shaders.hpp` / `particle_soft.*`）：
   粒子把窗口深度（`0.5*z/w+0.5`，对距离是二次曲线）与场景深度直接相减，固定淡出带宽
   0.012 换算成世界距离 = `0.12*d²`——MOBA 相机距离 17 时约 **35 个世界单位**，贴地 5 单位
   高的粒子 alpha 只剩 ~11%，对着天空也只有 48%。"编辑器近处调好、游戏里隐形"的元凶。
   修复：VS 直接输出 `gl_Position.w`（= 视轴距离），FS 把原始深度线性化
   （`2nf/(f+n-(2d-1)(f-n))`）再比较；`uSoftFade` 改为**世界单位**（默认 0.75）。
2. **P0 VK 粒子顶点布局错配，实例缓冲根本没绑**（`vk_backend.cpp`）：`VertexVariantFor`
   没有 `"particle"`/`"particle_soft"` 分支 → 落到 V3d（非实例化）→ 只绑 mesh.vbo 却按
   instanceCount 绘制，quad 顶点数据被当 mat4 读 → VK 后端 GPU 粒子整体不可见/错乱；
   且 `DrawMeshInstancedColoredUv` VK 无实现（flipbook UV 静默丢弃）。修复：新增
   `InstancedColoredUv` 顶点变体（binding3/location9）+ VK 实现三缓冲实例上传。
3. **P0 自动曝光的"平均亮度"只测了画面中心 ~0.002%**（`bloom.hpp` / `autoexposure_avg.frag`）：
   lum pass 每 texel 单次双线性采样（1/32 图本身就不是均值），reduce 又只采中心 2x2 texel
   ——1080p 下 2000 个 texel 只读 9 个。准星指天空全帧压暗、指暗墙全帧过曝。
   修复：reduce 改为 **16x16 全图均匀网格（256 taps）**，并按 `uSceneVpRect` 剔除
   letterbox 黑边 tap（清屏黑 log=-9.2 会毒化均值）。
4. **P1 autoExposure 直接替换 authored exposure**（两后端 composite）：编辑器曝光滑杆在
   AE 开启时静默失效。改为 `exposure = authored * adapted`（作者意图保留，AE 只做自适应）。
5. **P1 bloom 阈值在曝光之前**（两后端 bright pass）：AE 拉亮暗场景时没有像素能过阈值，
   泛光恰好在画面最亮时消失。bright pass 现在乘同一份有效曝光（adapt 后 × authored）。
6. **P1 曝光适应按帧率变化 + 场景切换不重置**：adapt 改 `1-exp(-speed*60*dt)`（墙钟归一，
   renderer 逐帧采 dt）；新增 `Renderer::ResetAutoExposure()` / `PostGraph::ResetAutoExposure`，
   `GameRuntime::Start` 置标记、首次 `Draw` 时消费（上一场景的适应亮度不再漂进新场景）。
7. **P1 贴花 additive 分支预乘 alpha**（两后端）：引擎加法混合是 (SRC_ALPHA, ONE)，
   预乘后实际贡献 = `rgb·a²`——纹理 a=0.5 处只剩 25% 亮度，光晕半径缩小、边缘发硬。
   改为直色输出。
8. **P1 贴花深度 bias 是窗口深度常数**（两后端）：0.00035 对应世界距离 `0.0035·d²`——
   相机 17 米时约 1 个世界单位，技能圈把圈内单位的小腿都涂了色，与注释宣称的
   "never pokes through geometry" 直接矛盾。修复：FS 内线性化 → 世界单位偏移
   （0.05m）→ 重编码窗口深度。
9. **P1 VK bloom upsample 缺 tent 滤波，`bloomWidth` 完全无效**：VK 版只是单 bilinear tap，
   `uBloomWidth` 在 UBO 里不存在（静默 no-op）。移植 GL 的 9-tap tent + 宽裙边实现，
   UBO 尾部加 `uBloomWidth`（offset 7140，块大小不变），两后端泛光一致。
10. **P1 体积光/SSR 模糊链复用了单通道 AO 模糊**（`post_graph.cpp`）：`kSsaoBlurFragmentShader`
    只读 .r 并写回全部通道——两链各过 H+V 两次模糊后 G/B 被 R 覆盖，**光柱永远是灰色、
    SSR 反射全部去色**（形状对、颜色错，极难察觉）。vol/ssr 的 4 个 blur pass 换 vec4
    高斯 `blur_`（bloom 链已在用），AO 链保持单通道版。
11. **P1 探针 GI 解码被二次除以 maxIrr**（两后端 lit）：atlas 编码时已乘 `1/maxIrr`，
    解码应除回来，旧代码又乘了一次 → GI 暗 `maxIrr²` 倍（maxIrr≈7 时暗 ~49 倍，基本不可见）。
    改为 `/ uLightProbeInvMax`。
12. **P1 非 MSAA 回退深度 pre-pass 里，蒙皮网格用 shadow 编码器写深度**：shadow fragment 写
    窗口 NDC 深度，消费端按"线性视距/uFar"解码——10 米处解码出 ~990 米，开体积雾时
    **蒙皮角色整块变纯雾色剪影**。新增 `kSsaoDepthSkinnedVertexShader`（GL）+
    `ssao_depth_skinned.vert`（VK，gen_vk_shaders 注册），输出线性 `clip.w`。
13. **P2 光照边界**：点光 `radius<=0` 在 GPU 衰减公式里是 1+d/|r| → clamp 成 1 → 负半径
    全场常亮、r=0 且 d=0 产生 NaN——shader 循环加 `if (radius<=0) continue`；空光源槽
    默认值是"原点白灯+未初始化半径"（`SetPointLight(3)` 会点亮 3 个垃圾槽）→
    SceneState 构造时显式清黑/清零；IBL 资源失效后清 `uIblStrength=0`（不再采已销毁句柄）。
14. **P2 其它**：探针烘焙太阳半球平均系数 0.5→0.25（∫cos/4π=0.25，旧值偏大 2 倍）；
    alpha 粒子排序 `std::sort`→`std::stable_sort`（等距粒子逐帧抖动）；透明材质不再写
    速度图（velocity pass 深度写入会抢后面运动实体的运动向量）；诊断捕获
    （`--tonemap-compare` 等 chains=false 路径）不再推进 AE 适应状态；gamma 注释方向
    纠正（>1 是变亮，pow(c,1/gamma)）。

### 验证

- 新增确定性探针场景 `projects/moba/assets/scenes/fx_probe.json` +
  `assets/scripts/fx_probe.lua`（每帧 emit 贴地软粒子+加法火花+技能圈/盘贴花，固定相机）：
  GL 前后对比 `build-msvc/r2/probe_gl_{old,new}.png`——修复后火花/贴花/烟雾明显更亮更实
  （旧版全部被 alpha²/深度空间错误压暗）；VK 前后对比 `probe_vk_{old,new}.png`——
  旧版**所有**脚本特效（粒子+贴花）缺失，新版粒子正常。
- 回归：`shadow_test.json`（GL/VK）与旧二进制逐像素一致；`visual_acceptance.json` AE 场景
  正常（AE 乘 authored 后整体略暗，符合 authored=1 的预期）；MOBA GL/VK 整图无回归。
- `neon_tests` **836/841**（5 个 `PostGraph*` 为既有基线，与本轮无关；
  `BloomShaderSourceTokens` 的 token 断言已随 bright/reduce 着色器修改同步更新）。
  注意：测试必须在仓库根目录跑（server 测试用相对路径 `tests/data/...`）。

### 已知但未修（本轮新增记录，按需再做）

- **✅ 已修复（本轮）：VK 贴花不可见 —— 根因是 ResolveDepth 的深度写入从未发生**。
  复盘：`ResolveDepth`（vk_backend.cpp）用 `SetDepthTest(false, true)` 画解析 quad——
  Vulkan（与 GL 一致）在深度测试关闭时**深度写入也被跳过**，解析目标永远是清除值 1.0；
  贴花片元采样解析深度得到 d >= 0.99999 → `discard` 全部片元（软粒子淡出、TAA 深度重投影
  同受影响——它们读同一张解析深度）。修复 = `SetDepthTest(true, true)`（清除值 1.0 vs
  写入值 <1.0，LESS 恒通过，天空 texel 保持 1.0 清除值）。验证：车库网格贴花在 VK 首次
  渲染（`build-msvc/rv_vk_clean.png`），fx_probe 软粒子淡出恢复。
  排查过程证明有效的工具链：NEON_VK_TRACE 状态 trace + 着色器级二分（全屏 gl_VertexIndex
  quad / UBO 内容探针 / aPos.xz 探针 / 盒可视化 / UV 棋盘）。
  附带：新增 `NEON_VK_VALIDATION=1`（Khronos 验证层 + debug messenger，需 SDK 层文件，
  本机未装层时告警跳过）；`DrawIndexed` 增加 prog=decal 的 uMVP 字节 trace。
  残留小项：车库场景里网格贴花会"渗"到占位模块顶面（解析深度的垂直镜像嫌疑，
  仅影响装饰性网格，见下条）。
- **✅ 已修复（本轮）：VK decal 世界重建的 y 翻转是双重矫正**：场景顶点着色器的
  y 翻转已抵消 VK 帧缓冲行序差，decal.frag 的重建 `1 - scr.y*2` 再翻一次导致投影盒
  垂直镜像（车库网格渗到模块顶、fx_probe 盘画到镜像位置）。已改为与 GL 一致的
  `scr.y*2-1`。深度方向探针（d 编码成颜色）证实解析深度本身方向正确。
- **✅ 已修复（SDK 验证层接入后）**：两批系统性 VK 规范违规（验证层 5308→324 条）：
  1. **UBO std140 布局重叠**：`float uPointRadius[8]` 在 std140 下步长 16（占
     4832..4960），`uAmbientColor` 声明在 4864——直接别名到数组元素 [2]，CPU 两侧
     写入互相踩踏、SPIR-V 无效（73 个 shader module 全部违规）。修复 =
     uAmbientColor 及其后全部成员偏移 +96（kUniformBlockSize 7152→7248），
     C++/GLSL 两表 123 项同步平移（0 错配）。
  2. **渲染通道兼容性违规**（02684/00904，3622 条）：rpClear（0 依赖）与 rpLoad
     （1 依赖）不兼容而管线/帧缓冲跨用；swapchain（BGRA）与离屏（RGBA）共用
     RpKind::Color1 管线键。修复 = 两个通道携带相同依赖 + 管线键加入实际颜色格式。
- **残留（下轮）**：fx_probe 的盘贴花 (-2,1) 在 VK 上仍不渲染（位置依赖：环 (0,0)
  正常、盘在 (1.2,1.6) 时正常）。已排除：深度写入、重建镜像、UBO 重叠、通道兼容、
  纹理内容（回读 a=234 正确）、绑定（tex0=35 正确）、alpha 采样（灰度探针正常）、
  深度方向（编码探针无镜像）。剩余 324 条验证错误为帧图瞬时目标首帧 UNDEFINED 标记
  类（NVIDIA 下无害）。该问题需 RenderDoc 级别抓帧定位，暂不影响游戏内主要场景
  （MOBA 技能贴花待实机复核）。
- **TAA scene-rect 错位**：`DesignSpaceRect()` 宽度固定 16:9，非 16:9 窗口 + TAA 时
  velocity/TAA/后链三套屏幕空间映射错位（编辑器无 TAA 入口，休眠中）。
- 动态分辨率每步进全量重建 target 并作废 TAA 历史（活跃调整期 TAA 无法收敛）；
  TAA 无相机跳变检测（瞬移后 ~8 帧残影靠邻域 clamp 收敛）。
- 骨骼数上限 GL 128 / VK 64 不一致（>64 关节的资源 VK 丢权重）。
- 探针 GI 与天空 IBL 同时开时环境光双计（#11 修复后 GI 显形，此项更值得做）；
  编辑器烘焙输入仍是假灯（不带场景真实灯光）。
- 体积光无太阳阴影采样（光柱穿墙）；uDecay/uThreshold 死参数；SSR 命中语义/量纲混乱。
- lit 线性雾与 composite exp² 雾双重叠加（建议关 lit 雾只留 composite）。
- vignette 无宽高比校正（宽屏下是横椭圆；三处实现一致，属统一取舍）。

### 调试钩子
- `NEON_NO_TEX_TRANSITION=1`（VK）：跳过纹理布局转换（本轮排查用，兼作该路径的 A/B 开关）。
- `NEON_VK_TRACE=1`（VK）：后端 draw 级 trace（prog/idx/inst/tex0）。
