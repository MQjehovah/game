# NeonEngine 开发文档（架构 & 模块设计）

> 项目介绍 / 快速上手见 [`README.md`](./README.md)。本文件是**唯一主开发文档**：
> 架构分层、模块设计、数据流、关键决策，以及各子系统（插件、网络、内容创作运行时）
> 的完整设计。

## 目录

- [1. 项目概览 &amp; 设计目标](#1-项目概览--设计目标)
- [2. 架构分层](#2-架构分层)
- [3. 核心模块](#3-核心模块)
- [4. 数据流（一帧）](#4-数据流一帧)
- [5. 工具链层（编辑器 / 插件 / 网络）](#5-工具链层编辑器--插件--网络)
- [6. 数据驱动游戏示例](#6-数据驱动游戏示例)
- [7. 关键设计决策记录](#7-关键设计决策记录)
- [8. 代码规范（附录）](#8-代码规范附录)
- [9. 路线图 &amp; 与 Godot 的差距](#9-路线图--与-godot-的差距)
- [10. 缺陷与差距清单（A/B/C/D/G）](#10-缺陷与差距清单abcdg)
- [11. 关联文档](#11-关联文档)

---

## 1. 项目概览 & 设计目标

引擎面向**大型 3D 多人网络游戏**（类《魔兽世界》）。核心取舍：

- **大型世界**：数千实体、分区加载、流式资源 → **ECS** 与可扩展资源管线。
- **多人在线**：客户端/服务器同构、确定性、可序列化状态 → 清晰接口边界 + 数据驱动。
- **跨平台**：Windows / macOS / Linux 一套代码 → 平台层、渲染层可插拔。
- **工程化**：模块边界、单元测试、CI、文档、代码规范优先于堆功能。

**差异化赛道**：确定性权威服务器 + 数据驱动工具链（编辑器 → 打包 → 通用播放器），
而非通用编辑器优先。对比外部引擎按"MMO 目标是否受益"取舍，不逐项对齐。

**交付形态**：`neon_editor`（编辑器）/ `neon_game`（通用播放器）/ `neon_server`（无头服务器）。

---

## 2. 架构分层

```
┌─────────────────────────────────────────────────────────────────┐
│ 应用层    neon_rush(demo) / neon_game(播放器) / neon_server / neon_editor │
├─────────────────────────────────────────────────────────────────┤
│ neon_scene  (L3)  assets / scene(GameRuntime) / script / bt / anim │
│                   physics / ui / audio / plugin(runtime) / modules │
├─────────────────────────────────────────────────────────────────┤
│ neon_gfx    (L2)  渲染（IRenderBackend：GL / Vulkan）              │
├─────────────────────────────────────────────────────────────────┤
│ neon_core   (L0/L1) math / core / io / ecs / net / nav / kernel    │
│                     plugin(core) / platform(IWindow/IInput)        │
└─────────────────────────────────────────────────────────────────┘
neon_engine = INTERFACE 门面，聚合 core/gfx/scene（现有消费者链接不变）
```

**依赖规则（已由 CMake `target_link_libraries` 白名单强制，非靠自觉）**：

1. `neon_core ← neon_gfx ← neon_scene`：只能向下依赖，越层 include/link 直接编译失败。
2. `game`/`editor`/`server`/`plugins` 只链接公开库（`neon_engine` 门面或具体层）。
3. 后端（platform/gfx/audio）实现接口，只在对应平台编译。
4. 高层只通过 `IRenderBackend` 触达 GPU。

**微内核（microkernel）**：`neon_core` 内的 `kernel::`（`IModule` + `ServiceRegistry` +
`ModuleRegistry` + `Kernel`）是统一模块系统。可替换子系统（渲染/物理/音频/脚本/平台/UI）各实现
`IModule`，通过 `ServiceRegistry` 拿依赖、发布服务——替换模块 = 换注册表一项。已落地：
`neon/modules/subsystem_modules.hpp`（Gfx/Physics/Audio/Script 模块包装）+
`GameRuntimeConfig.services`（运行时从注册表注入物理世界/脚本宿主，回退自建）。

> **状态（2026-08-31 架构重构后）**：
>
> - 三层库拆分已完成（原单一 `neon_engine` 拆为 core/gfx/scene，依赖方向由构建强制）。
> - 两处循环已破：`scene↔script`（bindings 依赖注入 3 个 hook）、`platform↔gfx`（窗口内联 WGL/GLX 加载）。
> - 微内核 P-A/P-B/P-C/P-E 已落地并验证（698 项测试含 9 项微内核专项）。
> - 剩余：GameRuntime 上帝类按子系统组合服务化（C1，机制已就绪）、Renderer 上帝类（C4）、
>   server/game 未库化（C15）。详见 §10 与
>   [`plans/2026-08-31-architecture-review.md`](./plans/2026-08-31-architecture-review.md)、
>   [`plans/2026-08-31-microkernel-design.md`](./plans/2026-08-31-microkernel-design.md)。

**模块规模（粗略）**：core ~2.1k、gfx ~5.5k、assets ~1.6k、scene ~4.3k、script ~2.8k、
bt ~1.3k、anim ~0.8k、physics ~1.2k(+Jolt)、net ~1.6k、ui ~1.6k、audio ~0.4k、
editor(含 ImGui) ~12k。

---

## 3. 核心模块

按模块组织，每个为一小节；`assets` 模块内包含 VFS 路径体系子节。

### 3.1 数学（`neon::math`）

header-only、零依赖、**行主序** `Mat4`（`m[row*4+col]`）。
`Vec2/3/4`、`Mat4`（正交/透视/旋转/平移/缩放）、`Quat`、`Transform`、`Rect2`、`AABB`、
`Ray` + 相交。

> 约定：行主序，GL uniform 提交 `transpose=GL_TRUE`；角度弧度；Y 轴向上；相机 -Z。
> 曾混用列主序，测试防回归。

### 3.2 核心（`neon::core`）

`Application`（固定步长 60Hz 累加器 + 冒烟帧数）、`Time`、`Log`（分级时间戳）、
`Config`（key=value）、`Rng`（xorshift64* 确定性）、`Serializer`（版本化二进制）、
`Json`（自研 DOM，递归下降 + UTF-8）。

### 3.3 ECS（`neon::ecs`）

SparseSet。`Entity` = 32 位 id + 32 位 generation；`World::Pool<T>`（dense + sparse，
swap-erase）、`View<T>` 顺序遍历。

- **批量迭代**：`View<T>::ForEach` / `View<T,U>`、`ParallelForEach`（dense 切连续 chunk，
  与串行逐位一致）。
- **确定性并行 job**（`neon::ecs::parallel`）：`ParallelFor(count, fn)` + 持久线程池
  （Win32 CreateThread / pthread；无 worker 回退串行）。
- **并行契约**：`ParallelForEach` 期间禁 `Create/Destroy/Add/Remove`（优雅拒绝 + 日志）；
  需改世界的系统先收集、并行后应用。
- 当前适合数千实体；MMO archetype 存储留作后续（批量迭代 API 已就位，届时换存储后端）。

### 3.4 平台（`neon::platform`）

`IWindow`（Win32 `CreateWindowEx`+WGL 降级链 / X11 GLX / Cocoa `NSOpenGLView`）、
`IInput`（引擎侧状态机，一套实现服务所有平台）。

### 3.5 渲染（`neon::gfx`）

**IRenderBackend（底层）**：资源（Shader/Texture/Mesh Handle）、状态（混合/深度/剔除/视口/清屏）、
绘制（`DrawMesh` + 立即模式 `DrawPrimitives`）、uniform 按名惰性缓存。
**渲染器与游戏完全不感知具体 API。**

**OpenGL 后端**：

- 自研 GL 加载器（`wglGetProcAddress` → `opengl32.dll` / `glXGetProcAddressARB` / 符号直链）。
- 纹理 `glTexStorage2D + glTexSubImage2D`（避开旧桩在 core context 的驱动问题）。
- **深度校验自检**（`GL_DEPTH_BITS` + 清除→回读）：不可用则关深度、画家算法排序。
- 2D 统一批量缓冲、网格实例化、顶点色（OBJ MTL Kd）、PBR（Cook-Torrance/GGX）、
  投影阴影（CPU 投影 y=0，兼容深度损坏驱动）、glTF 2.0 导入（自研 JSON DOM）。

**Vulkan 后端**（`--backend vulkan` 与 GL 逐像素一致；GL 仍默认）：

- 实例与设备（surface 扩展、选离散 GPU、graphics+present）、表面与交换链（SRGB/FIFO/2~3 图像）、
  渲染管线（GLSL → SPIR-V）、资源（`VkBuffer`/`VkDeviceMemory`、`vkCreateImage`+传输队列）、
  帧同步（acquire→submit→present，semaphore/fence 环形）、验证（validation layers + GL 对比）。
- 历史灰度问题排查：① 材质/贴图描述符（set 1 / `lit.frag` set/binding）→ 有光照无贴图；
  ② 颜色 writeMask/混合；③ 光照 UBO 偏移（`engine_ubo.glsl` vs `kVkUniformOffsets`）→ 无 PBR；
  ④ 深度 load/clear 顺序；⑤ 截图路径；⑥ RenderDoc 抓帧对照。

**Renderer（高层）**：相机（透视 lookAt）、方向光 + 8 点光 + 玩家手电、距离雾、天空渐变；
内置 Shader lit/unlit/UI/line。**HDR + Bloom + ACES + MSAA**：场景渲染进 RGBA16F 离屏目标 →
明亮度阈值 → 1/2、1/4 高斯金字塔 → 上采样累加 → ACES 色调映射（曝光可调）→ 后备缓冲；
MSAA 多重采样 + `glBlitFramebuffer` 解析。能力自检失败自动回退到直绘后备缓冲。

### 3.6 音频（`neon::audio`）

`IAudioBackend`；miniaudio（WASAPI/DirectSound/CoreAudio/ALSA），无设备自动回退 WinMM/Null；
三平台统一。总线（Master/Sfx/Music）、3D 空间音效、WAV 加载、Lua 绑定；demo 程序化合成 PCM。

### 3.7 物理（`neon::physics`）

接入 **Jolt v5.0.0**（Godot 4 同款 vendored）：刚体/碰撞层掩码/角色控制器（`CharacterVirtual`）/
射线/接触查询。保留 `physics::World` 接口抽象（后续可换引擎）。
旧自研轻量物理（球 vs AABB）保留作确定性回退。**注意**：Jolt 同构建产物确定，跨架构不保证逐位一致。

### 3.8 资产（`neon::assets`）

`AssetManager` 按路径缓存贴图（stb_image）/OBJ 网格/字体；程序化资产不过磁盘。
**引用计数 + 延迟回收**（资源生命周期），chunk 卸载释放、ObjectPool。

#### 3.8.1 资产路径体系 & VFS 统一读取（`@assets/`）

所有文件读取收敛到一处（VFS），外部统一用虚拟路径 `@assets/`，兼容相对/绝对。

**目标**：过去路径散落、反复转换——① 形式不统一（项目相对 `assets/x` / CWD 相对
`projects/wc3/assets/x` / 绝对 `E:\game\...`）；② 相对提取散落（glTF 外部 `.bin`/图片 URI
需相对 `.gltf`）。目的：单一路径模型（`@assets/`）、一处解析（VFS）、兼容迁移
（VFS 优先 + 直读回退）。

**三层解析**：

```
① NormalizeAssetPath  scheme 归一化（@assets / assets: / 历史去重）
② IoRead / ReadAllBytes VFS 优先（虚拟路径 → 项目根）→ 失败回退直读
③ 磁盘文件
```

**各层职责**：

- **`NormalizeAssetPath`**（`asset_path.hpp`）：`@assets/a.png`→`assets/a.png`；
  `assets/assets/a.png`（历史重复）→`assets/a.png`；已项目相对/绝对/外部 → 原样。
  `HasAssetScheme` 判定是否带显式 scheme。**不做**项目根/CWD 拼接。
- **`AssetManager::IoRead / IoMTime`**：归一化 → `fs_->ReadFile` → 失败回退
  `ReadAllBytes(nullptr, path)`（CWD 直读）。所有 `LoadTexture/LoadGLTF/LoadMeshOBJ/…`
  内部读取收敛到这两者。
- **`ReadAllBytes(fs, path)`**：最底层唯一文件读取入口（可被 `ParseGltfContainer` 用）：
  VFS 命中则读，否则 CWD 直读回退。**VFS 拒绝（绝对/越界）时回退直读**——保证挂 VFS 后
  旧绝对引用仍可读，glTF 外部 bin/图片自动回退直读。
- **`GameRuntime::FullAssetPath`**：收敛为纯归一化（不拼 project-dir 绝对），返回项目相对
  虚拟路径，交给 IoRead 解析。刻意不拼项目根绝对（与 VFS 根重复 + 破坏 glTF 相对 bin）。

**关键设计决策**：

| 决策                               | 理由                                                                                                      |
| ---------------------------------- | --------------------------------------------------------------------------------------------------------- |
| `projectDir_` 一律绝对化         | 路径解析不依赖 CWD——重启后启动目录不同，相对 projectDir 拼接会静默失效                                  |
| 不拼项目根绝对路径                 | VFS 已挂载项目根；再拼绝对会双前缀且拒绝 glTF 相对 bin                                                    |
| glTF 外部 bin/图片相对提取         | `out.dir + uri`；传项目相对虚拟路径 VFS 直接解析，绝对靠回退直读                                        |
| 编辑器存储输出`@assets/`         | `NormalizeEntityAssetPaths` 只在路径确实位于项目根下才转换；不 CWD 拼 bare 相对；rel 去开头 `assets/` |
| 编辑器加载用`NormalizeAssetPath` | 解析实体材质（`mesh.material.*` 嵌套 material 子对象）时归一化引用                                      |

**调用点约定**：场景/预制体/材质球/贴图槽**存** `@assets/...`；读取方**传**存储路径给
`LoadTexture/LoadGLTF/LoadSkinnedModel`，由 IoRead 统一解析。**不再**在调用点做
`FullAssetPath(拼绝对)`/`NormalizeAssetPath`/`ResolveMeshAssetPath` 重复转换。

**关键文件**：

| 文件                                                           | 作用                                                    |
| -------------------------------------------------------------- | ------------------------------------------------------- |
| `engine/include/neon/assets/asset_path.hpp`                  | `NormalizeAssetPath` / `HasAssetScheme`             |
| `engine/include/neon/io/vfs.hpp` / `engine/src/io/vfs.cpp` | `IFileSystem` / `DiskFileSystem`                    |
| `engine/src/assets/asset_manager.cpp`                        | `IoRead` / `IoMTime` / `ReadAllBytes`             |
| `engine/src/scene/game_runtime_content.cpp`                  | `FullAssetPath`（归一化入口）                         |
| `engine/src/scene/skinned_model.cpp`                         | `LoadSkinnedModel`（经 `IoRead` 读 gltf JSON text） |
| `editor/src/editor_scene.cpp`                                | `NormalizeEntityAssetPaths`、实体材质解析             |
| `editor/src/editor.hpp`                                      | `MountAssetVfs`（挂载项目根 VFS）                     |

### 3.9 UI（`neon::ui`）

立即模式（`DrawLabel/...`）+ 控件树（`UIManager` + `Element`，相对坐标 +
`Measure/Layout/Draw` + `AbsolutePos` 累加）。游戏内 HUD 用控件树；编辑器工具 UI 用 ImGui。

### 3.10 场景 & 内容创作运行时（`neon::scene` / `neon::script` / `neon::bt` / `neon::anim`）

数据驱动工具链核心：编辑器编辑的内容被 `neon_game`（通用播放器）与 `neon_server`
（无头服务器）无差异运行。

- **脚本**（`neon::script`）：`IScriptHost` + Lua 5.4 / QuickJS 双后端；`Value/Table`、
  原生函数注册、引擎绑定（ECS/物理/音频/GameVars/JSON）。**确定性沙箱**：xorshift RNG
  替换 `math.random`、模拟时钟注入、`io/os/package/require` 关闭、表转换深度/循环保护。
  Lua 编辑器调试器（断点/单步/变量/调用栈）。
- **行为树**（`neon::bt`）：`Node::Tick(Context&)`（状态在调用方 Context）、JSON 加载/序列化、
  节点类型注册表、编辑器可视化 + 活动路径高亮。
- **骨骼动画**（`neon::anim`）：glTF 蒙皮导入（JOINTS/WEIGHTS/IBM）、clip（LINEAR/STEP/
  CUBICSPLINE）、状态机 + BlendSpace1D/2D + 两骨骼 IK + Lua Tween + `.anim.json` 时间线编辑器、
  GPU 蒙皮（`uBoneMatrices[64]`）。
- **场景**（`neon::scene`）：组件化 JSON（transform/mesh/rigidbody/animator/behaviorTree/
  script/health…）、预制体深合并 + 实例覆盖、场景继承（`extends`）+ 组（groups）、`game.json` 清单。
- **`GameRuntime`**：解析 → 实例化 → 挂脚本/行为树 → tick → 绘制；headless 供服务器复用。
  含 `WorldChunk`/`ChunkStreamer`、LOD 资产链。`SceneManager` 处理场景切换（帧边界延迟生效）。

### 3.11 微内核（`neon::kernel` / `neon::modules`）

统一模块系统，实现"每个模块可重写/替换"：

- **`IModule`**（`neon/kernel/module.hpp`）：统一模块契约——`Info()`（id/version/依赖）、
  `Init(ServiceRegistry&)`（注册服务 + 拿依赖）、`Shutdown()`。模块间不直接 include/link。
- **`ServiceRegistry`**：类型擦除的服务表（接口类型 → 实现指针），`Register<T>`/`Get<T>`；
  替换模块 = 覆盖注册一项。
- **`ModuleRegistry`**：按 `requires` 拓扑序初始化、逆序关闭、环/缺依赖检测。
- **`Kernel`**：微内核本体（`ModuleRegistry` + `ServiceRegistry` 打包），应用层驱动的
  `core::Application` 替代——加载模块 + 主循环生命周期。
- **子系统模块**（`neon/modules/subsystem_modules.hpp`）：`GfxModule`/`PhysicsModule`/
  `AudioModule`/`ScriptModule` 包装已有接口（`IRenderBackend`/`physics::World`/
  `IAudioBackend`/`IScriptHost`）。
- **运行时接线**：`GameRuntimeConfig.services`（`ServiceRegistry*`）——运行时从注册表注入
  物理世界/脚本宿主（非拥有，回退自建，`injectedScriptHost_` 守卫生命周期）。

可替换的 I/O 服务（渲染/物理/音频/脚本/平台/UI）均已接口化并接成模块；BT/动画/资产是
引擎内置的确定性内容运行时（纯逻辑），非可替换后端。详见
[`plans/2026-08-31-microkernel-design.md`](./plans/2026-08-31-microkernel-design.md)。

**三个数据驱动入口均已采用 Kernel**（P-E 生产落地）：

| 入口 | 接线 |
| --- | --- |
| `neon_game`（播放器） | `PhysicsModule(Jolt)` + `ScriptModule(Lua)` |
| `neon_editor`（F5 试玩） | `PhysicsModule(Jolt)` + `ScriptModule(Lua)`（每试玩会话新建 Kernel，`StopPlay` 回收） |
| `neon_server`（服务器） | `PhysicsModule(custom/Jolt 按 `--physics`)` + `ScriptModule(Lua)`；`plugin:<name>` 原生后端保留字符串回退 |

`neon_rush` 是硬编码 demo（不走 `GameRuntime`），不在"数据驱动可替换"范畴内。

---

## 4. 数据流（一帧）

```
PumpEvents → 平台事件 → IInput::HandleEvent
                    ↓
固定步长累加器：OnUpdate(dt) 60Hz
  ├─ 玩法系统（输入→移动/攻击；wc3 单位 AI/波次）
  ├─ GameRuntime::Tick（脚本/行为树/动画/物理）
  └─ physics::World::Step
OnRender：
  ├─ Renderer::BeginFrame（清屏）
  ├─ 场景 3D 绘制进 HDR 浮点离屏目标（SetCamera + DrawMesh）
  ├─ 粒子公告板 / 调试线框 / 2D HUD 批量
  └─ Renderer::EndFrame：Bloom → ACES 色调映射 → 后备缓冲 → SwapBuffers
```

要点：

- **固定步长 60Hz 累加器**（`Application`）驱动逻辑，渲染帧率独立——物理/逻辑确定性前提。
- **场景先 HDR 后合成**：3D 画进 RGBA16F 离屏目标，`EndFrame` 跑 Bloom + ACES 到后备缓冲；
  2D/HUD 覆盖层最后画（不受 bloom 影响）。
- 服务器（headless）只跑 `OnUpdate`，无渲染，复用同一 `GameRuntime`。

---

## 5. 工具链层（编辑器 / 插件 / 网络）

引擎之上的工具链与应用层：编辑器、插件系统、网络。

### 5.1 编辑器（`neon_editor`）

`neon_editor.exe`，**工具 UI 用 Dear ImGui（docking 分支）**，游戏内 HUD 用自研控件树。

- **停靠布局**：主 DockSpace + DockBuilder 默认布局（ini 恢复用户布局）。面板：场景/资产/
  资源/属性/日志 + 材质/行为树/脚本/打包/性能/导航/本地化/输入映射/插件。
- **场景面板**：实体列表（树形层级，父分组）+ 增删复制；右键**保存为预置体**（弹输入框）；
  资产面板 `prefabs/` 目录 `.json` **拖拽到场景/视口生成实例**。
- **预制体模型**（Godot 式）：模板 = `assets/prefabs/<name>.json`；实例引用模板名，只存字段
  覆盖（diff）；属性面板"预制体实例"折叠栏 + **重置为预制体**还原默认。
- **资产面板**：目录浏览、双击导入/预览、拖拽（模型→场景、贴图→材质槽、脚本→实体、
  预置体→场景）、缩略图、材质球 `materials/*.mat.json`、导入 watch。
- **属性面板**：变换/网格/精灵/贴花/脚本/生命等多组件；网格 = meshKey + PBR 材质槽
  （**漫反射 albedoTex**/金属度粗糙度/环境光遮蔽/自发光）+ 颜色；**贴花**组件
  （贴图+尺寸+透明度，铺地面）；相机/光源节点特化字段；Schema 驱动组件编辑。
- **视口**：3D（右键旋转/中键平移/滚轮缩放/射线拾取）+ 2D 画布（格子编辑）+ 多相机 +
  gizmo（ImGuizmo）+ 相机"跟随选中"视野预览 + 视锥/视野框。
- **编辑器进阶**：撤销/重做、多选/批量、层级拖拽、地形笔刷实时预览、tilemap 调色板、
  UI 可视化编辑器、行为树可视化、脚本编辑器 + Lua 调试器、shader 热重载、性能面板。
- **热重载**：脚本（play 重启）/资产（mtime）/自定义 fragment shader（`--hot`）。

#### 5.1.1 渲染视口 & 坐标体系

把"渲染窗口"分两层（很多人踩坑处）：

- **渲染目标（render target / HDR 目标）= 整窗大小**（画布）。
- **渲染视口（rasterization viewport）= 3D 场景真正写入像素的区域** = dock/panel 矩形
  （`viewportScreenRect_`）。`renderer_.SetSceneViewport(x,y,w,h)` 交给后端；GL 后端
  `SetViewport` 做 y 翻转（左上→左下）。
- **两套坐标**：① 屏幕像素（`viewportScreenRect_`，左上原点，GL 翻转成左下）；
  ② 设计单位 `viewportRect_`（1280×720，2D 游戏输入，经 `ScreenToUI` 换算）。
- **gizmo 对齐**：覆盖层（DrawDebugOverlay）必须在 `ResetSceneViewport()` **之前**画
  （panel 视口仍生效），否则差一条菜单栏 ≈80px。`SetSceneViewport`+y 翻转+`ScreenToUI`
  **必须保留**，别删。

### 5.2 插件系统

插件是引擎的平台化扩展：**编辑器插件**（面板/工具/资产源/组件检查器）、**运行时插件**
（跨游戏玩法模块）。共用 `plugin.json` 与双语言加载器（Lua/QuickJS），共享确定性沙箱。

- **清单**：`id/name/version/type(editor|runtime|native)/backend(lua|js)/entry/ minEngineVersion/requires/permissions`。
- **运行时插件 API**（`Plugin`）：`Info/Log/On(event,fn)/OnCommand/GetVar/SetVar/ Export/Call/RegisterComponent`；引擎绑定全量可用。加载：`GameRuntime::Start` 扫
  `<scriptBaseDir>/plugins` 按依赖排序 → `on_load/on_start`；`Tick` 派发 `tick`；
  `Stop` 派发 `stop`；服务器 `player_join` 转发。
- **编辑器插件 API**（`NeonEditor`）：`panel/tool/assetSource/registerComponent/buildMesh/ spawn/selected/entities/importAsset/listDir/log/ui.*`。
- **关键实现**：`engine/src/plugin/plugin.cpp` / `runtime_plugin.cpp` /
  `editor/src/editor_plugin.cpp`。无工厂组件存 `SceneData`，脚本经
  `EntityComponent(entity, name)` 读取；`RegisterField` 支持点号路径。
- **示例**：`tree_gen`（editor/lua 程序化树）、`asset_vault`（editor/lua 资产源）、
  `inventory`（runtime/js 背包）。

### 5.3 网络层

客户端/服务器**同构**：复用同一 `scene::GameRuntime` + 确定性 Lua 沙箱。

```
  neon_game(client)  client::ClientSync（快照缓冲+插值+预测回滚）
        │ UDP
  ReliableChannel（滑动窗口 ACK/重传/乱序重排/断线判定）
        │
  neon_server(host)  GameServer（固定 60Hz 权威模拟 → 广播快照）
        │             AoiGrid（九宫格兴趣集裁剪）
```

- **传输层**：`UdpSocket` + `ReliableChannel`（滑动窗口 ACK + 位图）、`MessageCodec`
  （magic/version/CRC + 字段边界检查）。
- **协议**（`neon/net/protocol.hpp`）：版本化消息集（v3），客户端/服务器共用编解码对；
  加入 RPC（`MsgRpc` + `RpcDispatcher`）、服务器房间、反作弊（输入限速/封禁/
  admin.kick/ban/world.hash 校验）。
- **权威服务器**（`server::GameServer`）：headless、固定 60Hz 累加器步进、`Step`；AOI 九宫格
  裁剪快照 + spawn/despawn 增量；多玩家输入模型（`on_player_join` + `BindPlayerToClient`）。
- **客户端同步**（`client::ClientSync`）：快照环缓冲 + 相邻插值（yaw 最短弧）、预测回滚
  （v1 分歧即纠正）。
- **确定性验收**（`tests/test_determinism.cpp`）：同一脚本输入流在服务器/客户端逐位一致 +
  状态哈希相等。LAN demo：一 `neon_server` + 两 `neon_game --connect`（首个登录 = 输入控制器，
  其余观察者）；需**相同 `--scene`/`--seed`**。

---

## 6. 数据驱动游戏示例

- **2D**（`projects/pvz`，植物大战僵尸）：玩法+绘制全在 Lua（向日葵/豌豆/僵尸/波次表），
  精灵贴图在 `assets/sprites/`；植物/僵尸是 `assets/scenes/pvz.json` 里带 `plant`/`zombie`
  组件的实体，编辑器 2D 画布按格子编辑写回场景，运行时 `pvz.lua` 读取；F5/▶ 编辑内试玩。
- **3D**（`projects/neon_realm`）：112 实体场景 JSON + `realm.lua`；程序化 mesh key
  （terrain/tree/house/hero/wolf/npc:r,g,b/rock/water/road…）；脚本：相机相对移动、近战/火球/
  治疗、狼群 AI 与波次、NPC 对话、HUD/小地图、存档。
- **RTS 复刻**（`projects/wc3`，3D 模式）：`wc3.lua` 驱动——WC3 式相机（方向键/边缘平移 +
  滚轮缩放）、屏幕空间拾取（单位/建筑选择、射线-地面求交移动落点）、框选
  （`InputMouseReleased`）、右击移动/**采矿**/**攻击**、小地图、经济（金矿采集）、
  建筑放置/生产队列、敌军 AI 波次、胜负。`Game` 实体为脚本宿主（script 挂 wc3.lua，无 mesh）。

**一键试玩 & 打包**：编辑器单一试玩入口（工具栏/F5），`StartPlaytest` 按模式分支（3D 序列化
当前编辑场景 / 2D 加载项目场景文件）；数据驱动游戏有 `on_render` 则自己画 HUD。打包器收集
glTF 依赖（buffers/images URI 一并入包）、程序化 mesh key 校验放行。

---

## 7. 关键设计决策记录

| 决策                                | 理由                                                                          |
| ----------------------------------- | ----------------------------------------------------------------------------- |
| 自研 GL 加载器                      | 无第三方依赖、可审计、跨平台路径统一                                          |
| 行主序矩阵 + transpose=GL_TRUE      | 单一约定，测试防回归                                                          |
| ECS 而非 GameObject 树              | 大实体缓存友好迭代 + 并行化                                                   |
| 固定步长 + 累加器                   | 物理/逻辑确定性；渲染帧率独立                                                 |
| 深度不可用降级画家算法              | Intel 驱动深度缓冲损坏；自适应                                                |
| 投影阴影而非阴影贴图                | Intel 驱动 FBO VAO 渲染损坏；CPU 投影零依赖稳定                               |
| HDR 离屏 + 渐进 bloom + ACES        | 保留亮部、可调曝光；能力自检失败自动回退                                      |
| 客户端/服务器同构 + 确定性沙箱      | 同一输入流逐位一致，是快照/预测/回滚前提                                      |
| AOI 按客户端裁剪快照                | 九宫格兴趣集 + 增量，避免全量快照超帧上限                                     |
| 资产路径统一`@assets/` + VFS      | 一处解析、可读可区分、绝对/相对兼容回退                                       |
| 接入 Jolt 物理                      | Godot 4 同库，MMO 所需刚体/角色/碰撞层；保留接口抽象可换                      |
| 编辑器用 ImGui                      | 工具 UI 惯例（游戏内 HUD 用自研控件树，分开）                                 |
| 三层库拆分 core/gfx/scene           | 依赖方向由 CMake 强制（只能向下），越层引用编译失败                           |
| 微内核（IModule + ServiceRegistry） | 每个可替换子系统一个模块，替换 = 换注册表一项；C++ 接口为主、原生插件包 C ABI |
| 运行时依赖注入（`cfg.services`）  | 物理/脚本从注册表拿（非拥有 + 回退自建），模块与运行时解耦                    |

---

## 8. 代码规范（附录）

- **语言/构建**：C++17（禁 C++20 特性）；CMake ≥3.15；第三方依赖必须 vendored、附许可证。
- **命名**：命名空间 `neon::()`；类型/函数 `PascalCase`；变量 `camelCase`；成员 `camelCase_`
  尾下划线；常量 `k` 前缀（`kMaxPointLights`）；宏 `NEON_` 前缀；`.hpp`/`.cpp`/`.mm`。
- **头文件**：自包含 + `#pragma once`；接口头禁平台/GL 头；优先 `const T&`，接口类纯虚 +
  工厂返回 `unique_ptr`。
- **错误处理**：返回值/日志优先；`core::Result<T>` 显式可恢复结果；日志分级
  Debug/Info/Warn/Error，禁静默吞错；资源创建失败返回无效句柄 + `NEON_LOG_ERROR`。
- **数学约定**：`Mat4` 行主序，GL 提交 `transpose=GL_TRUE`；角度弧度；Y 上、相机 -Z；
  矩阵改动必须补单测。
- **分层纪律**：游戏层禁 `windows.h`/GL/X11/Cocoa 头；平台后端按 OS 编译；新增跨平台先加接口。
- **提交前**：`cmake --build build -j` 无警告（`-Wall -Wextra`）；`neon_tests` 全绿；
  `neon_rush --smoke-test 240` 退出码 0。

---

## 9. 路线图 & 与 Godot 的差距

> 目标 = 类《魔兽世界》MMO。按"MMO 收益/成本"取舍，**不逐项对齐 Godot**
> （确定性权威服务器 + 数据驱动工具链是本引擎差异化赛道）。

### 9.1 当前状态（已交付）

分层引擎、GL/Vulkan 后端、内容创作运行时（Lua 沙箱/行为树/脚本）、数据驱动工具链闭环
（编辑器 → 打包 game.pack → 播放器）、编辑器深化（gizmo/撤销/材质/行为树/缩略图/多相机/
热重载/性能）、渲染（PBR/CSM/HDR+Bloom+ACES/MSAA/IBL/GPU 蒙皮/LOD/BC1）、平台/性能
（miniaudio/ECS 并行/世界分区流式）、网络层（UDP 可靠/权威服务器/快照插值/预测回滚/AOI/
确定性验收）、测试 600+ 项与冒烟。

### 9.2 里程碑（M1~M5）

- **M1 渲染质量**：Vulkan 后端、CSM+点光阴影、PBR、实例化+视锥剔除、IBL、HDR+Bloom+ACES+MSAA
  ✅；地形分块 LOD 与纹理 splatting ⃝。
- **M2 角色与战斗**：骨骼动画管线、技能/状态效果框架 ✅；命中检测/胶囊体/旋转刚体/击退 ⃝；
  摄像机碰撞与智能跟随 ⃝。
- **M3 大世界与流式**：chunk 分区、glTF 资产管线、BC1+LOD+异步解码、对象池 ✅；archetype
  存储与确定性快照为未来后端 ⃝。
- **M4 网络化**：UDP 可靠、无头权威服务器、快照插值/预测回滚、AOI、v0 登录/角色、
  多玩家输入模型 ✅；分区分服（world/instance server）⏳。
- **M5 工具与运营**：场景/编辑器基础 + 进阶 ✅；地图编辑器进阶、性能时间线、崩溃上报、
  日志汇聚 ⃝。

### 9.3 与 Godot 的关键差距（按 MMO 收益排序）

1. **渲染管线深度**（收益最高）：SSAO → 体积雾 → 光照贴图/GI。需先补"深度贴图 + 后处理链"
   基建（当前 HDR 目标深度是 RBO，shader 无法采样）；GPU 粒子、任意表面贴花次之。
   - **已完成**：SSAO/SSR/体积雾代码本就存在但无启用入口；已接入编辑器「后处理效果」
     面板开关 + 强度，并同步到 play（`GameRuntime::SetPostFx`）。SSAO 深度 shader 修复：
     `DrawSsaoDepthCasters` 原错用 CSM 阴影 shader（非线性 `gl_FragCoord.z`），已改为线性
     相机深度 shader（`ssaoDepthShader_` 实例 / `ssaoDepthMeshShader_` 单 mesh，
     `vViewDepth/uFar` 编码）。
   - **遗留（待跟进）**：编辑器视口 `drawCalls=1`（validMesh=17 但只 1 个真正收集），
     致 `ssaoCasters_` 覆盖极少 → SSAO 深度目标大部分为远码 → AO 效果不可见。需查
     编辑器渲染实体为何多数被 `frustum culled` / mesh 未入收集，以及静态网格合批
     （DrawMeshInstanced 合并 caster）对覆盖的影响。
2. **编辑器打磨**：动画多轨时间线、UI 锚点/容器布局、检查器曲线编辑等深度工具。
3. **脚本体验**：JS 调试器、Lua 完整调用栈、悬停文档/补全、远程调试。
4. **音频/2D 纵深**：效果器（低通/混响）、流式背景音乐；2D 光照与 2D 物理工具。
5. **平台扩张**：WASM/WebGPU、macOS/Linux 实机验证、TLS/WebSocket 传输。
6. **场景级环境/后处理（待跟进，参照 Godot 范式）**：当前天空/雾/IBL/曝光是引擎硬编码，
   场景无法差异化氛围。计划按 Godot 做法做——**Environment 作为独立资源**
   （`environments/*.env.json`，含 sky/fog/ibl/tonemap 参数，像材质球可复用），
   场景用 **`WorldEnvironment` 组件实体**引用它，渲染器读组件 → 加载环境资源 → 应用。
   **注意**：切勿自创"场景根内联 environment block"方案（曾尝试后否决）；用 Godot 式
   资源 + 节点，才能可复用、可多环境、为将来体积混合（PostProcessVolume）铺路。

### 9.4 建议顺序

1. 渲染后处理链基建 → SSAO → 体积雾 → GI；2. 编辑器体验迭代；3. 脚本工具链；
2. 音频/2D 纵深；5. 平台扩张。**先 P0-1（Jolt）+ P0-3（资源生命周期）**为地基；
   P1-1 编辑器（产能杠杆）→ P1-2 调试器 + P1-3 动画并行。

---

## 10. 缺陷与差距清单（A/B/C/D/G）

> 唯一待办事实源（速查见 [`TODO.md`](./TODO.md)）。状态 `[ ]` 待办 · `[~]` 部分完成 ·
> `[x]` 已完成。引用按编号（如 A3、B1、G3-4）。
> 系列：**A = 正确性缺陷**（崩溃/泄漏/UB/精度，最优先）、**B = 每帧性能热点**、
> **C = 结构性重构**（可维护性）、**D = 安全与工程化**。

### 10.1 A 系列 · 正确性缺陷（P0，已全部完成）

A1 Vulkan descriptor set 只增不泄；A2 Vulkan 伪 HDR + 动态纹理更新空操作；A3 core::Json UB
（GetString 悬垂/无深度限制/尾残留/非法数字）；A4 ParallelFor 异常安全；A5 Lua 宿主失控
（指令预算/内存上限）；A6 per-entity vars 共享全局；A7 Jolt Collisions 无限增长；
A8 SetPosition 不写回物理 / Raycast 丢结果；A9 资源引用 key 不匹配 + 热重载 UAF；
A10 客户端不发 Ping；A11 world.hash 精度；A12 JsonWriter `%g` 精度；A13 OBJ/glTF 导入损坏。

### 10.2 B 系列 · 每帧性能热点（P1）

B1 uniform 无分层；B2 GL SetUniform 带 glGetError + string 分配；B3 无绘制队列/排序/合批；
B4 主场景深度不可采样 → SSAO 画两遍；B5 VK 目标切换 submit + 等 fence；B6 BuildDrawList O(N×M)；
B7 蒙皮矩阵每帧多遍 + HUD 逐顶点蒙皮；B8 Lua DebugHook 常驻税；B9 Lua-C++ 边界全量转换；
B10 每帧堆分配热点；B11 编辑器撤销触发全场景 JSON 往返；B12 UI 布局每帧全树重算 ×2；
B13 网络快照全量 + 48 实体上限 + 全走可靠通道；B14 物理同步无脏标记；B15 日志热路径开销。
→ B1/B2/B6/B8/B10/B11/B12/B13/B14/B15 [x]；B3/B4/B5/B7/B9 [~]。

### 10.3 C 系列 · 结构性重构（P1/P2）

C1 GameRuntime 上帝类拆分（已拆 content+combat+draw 三簇）；C2 动画状态存 DrawItem → headless 空转；
C3 EditorApp 巨类 / panels.cpp 4100 行；C4 Renderer 上帝类 + 无 render graph；C5 字符串 key
贯穿全栈（无 intern/GUID）；C6 组件序列化三份手写镜像；C7 脚本绑定手写 95 个 ×3 处；
C8 线程基建 3 份复制；C9 UI 四轨并存；C10 着色器系统原始（全内嵌字符串）；C11 ECS Pool 防护；
C12 CMake 单文件 655 行；C13 scene↔script 循环；C14 玩法混进引擎核心；C15 editor/server/game 未库化。
→ C6/C7/C11/C13 [x]；C1/C3/C12/C14/C15 [~]；C2/C4/C5/C8/C9/C10 [ ]。

### 10.4 D 系列 · 安全与工程化

D1 编辑器插件无版本门 + 任意路径访问；D2 插件 permissions 不强制；D3 网络无认证/加密/防重放；
D4 场景 JSON 不可 diff（已加缩进）；D5 CI 与构建工程化；D6 编辑器功能洞（撤销/静默失败）；
D7 服务器时基漂移。
→ D4 [x]；D1/D5/D6/D7 [~]；D2/D3 [ ]。

### 10.5 G 系列遗留（编号保留）

G1-1 渲染后端覆盖（D3D12/Metal 空白）；G2-1 反射系统（仅标量）；G2-2 ECS archetype；
G2-4 动态 GI（probe 场已落地，shader/DDGI 未做）；G2-5 Vulkan 自定义 shader 热重载；
G3-1 LLM 集成（远期）；G3-2 PCG 节点图；G3-3 视频编解码；G3-4 网络生产化（分区分服/
多玩家输入收尾/delta 编码/认证/断线重连/负载压测）；G3-5 UI（图标/链接/内嵌图片）；
G4-1 原生插件（ABI+加载器已落地，渲染/物理运行时替换未做）；G5-1 运行时切换渲染后端；
G5-2 任务图调度（写读区域自动分析/lock-free 队列未做）；G5-3 确定性（跨平台 bit 一致 CI）；
G6-1 资产变体表（显存预算 API 未做）；G6-2 异步 obj/gltf（LOD 异步未做）；G6-3 堆监控
（relocating allocator 未做）；G7-2 shader IL 翻译层；G7-3 输入时序（触屏未做）；
G8-2 C++ 实时代码热替换；G8-4 增量打包（分布式农场未做）；G-收尾 GameRuntime 分解。
**已完成存档**：G1-2 动态 BVH、G1-3 场景树/变换缓存、G1-4 资源依赖图、G1-5 SSAO+体积光+
GPU 粒子、G2-3 地形 Layer Blend+chunked LOD+植被 Impostor、G3-4 服务器 lag comp、G3-5 UI
九宫格+富文本、G4-1 原生 DLL/SO 插件、G5-2 任务图调度器、G7-1 VFS 全链路（pack 直读 +
Mod 挂载 + assets:/ scheme）、G7-3 输入时序绑定、G8-1 Profiler+崩溃报告、G8-3 调试覆盖层、
G8-4 增量打包、G6-1/6-2/6-3 平台/异步/堆监控、BC1 离线烘焙/检查器 schema/GameRuntime 接调度器。

### 10.6 优先级建议

1. A1–A13 正确性批量修复（地基）；2. B1–B4 渲染 CPU 管线治理；3. C1/C2 GameRuntime 拆分；
2. B8/B9/C7 脚本边界提速；5. B13 + D3 网络规模化与安全；6. B5–B15 → C3–C12 → D 系列 → G 系列远期。

---

## 11. 关联文档

- [`README.md`](./README.md) — 项目介绍 / 快速上手（仓库根）
- [`TODO.md`](./TODO.md) — 缺陷与差距清单（`[ ]/[~]/[x]` 速查更新基线；详细项见 §10）
- [`plans/2026-08-31-architecture-review.md`](./plans/2026-08-31-architecture-review.md) —
  架构评审（分层 / 依赖方向 / 内聚耦合分析 + 分阶段改进计划）
- [`plans/2026-08-31-microkernel-design.md`](./plans/2026-08-31-microkernel-design.md) —
  微内核模块化设计（IModule / ServiceRegistry / 子系统模块 / 增量路径）

> `docs/` 归档为唯一主文档；各主题（渲染/Vulkan/插件/网络/路线图/代码规范）均已按章节并入，
> 不再保留独立副本。
