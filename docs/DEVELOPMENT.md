# NeonEngine 开发文档（架构 & 模块设计）

> 项目介绍 / 快速上手见 [`README.md`](./README.md)。本文件是**唯一主开发文档**：
> 架构分层、模块关系、各模块功能与接口、数据流、关键决策，以及各子系统
> （内容创作运行时、插件、网络）的完整设计。
>
> 基线：与 2026-08-31 源码对齐（三层库拆分、微内核 P-A~P-E、战斗玩法下沉 Lua /
> Gameplay 基础库、VFS 全链路、原生插件、快照分片等均已落地）。待办状态以
> [`TODO.md`](./TODO.md) 为准。

## 目录

- [1. 项目概览 &amp; 设计目标](#1-项目概览--设计目标)
- [2. 架构分层：构建目标、依赖规则与模块地图](#2-架构分层构建目标依赖规则与模块地图)
- [3. 核心模块（功能 &amp; 接口）](#3-核心模块功能--接口)
- [4. 数据流（一帧 / 一个固定步）](#4-数据流一帧--一个固定步)
- [5. 工具链层（编辑器 / 插件 / 网络）](#5-工具链层编辑器--插件--网络)
- [6. 数据驱动游戏示例](#6-数据驱动游戏示例)
- [7. 关键设计决策记录](#7-关键设计决策记录)
- [8. 代码规范（附录）](#8-代码规范附录)
- [9. 路线图 &amp; 与 Godot 的差距](#9-路线图--与-godot-的差距)
- [10. 缺陷与差距清单（A/B/C/D/G）](#10-缺陷与差距清单abcdg)
- [11. 关联文档](#11-关联文档)

---

## 1. 项目概览 & 设计目标

引擎面向**大型 3D 多人网络游戏**（类《魔兽世界》）设计，C++17 编写。核心取舍：

- **大型世界**：数千实体、分区加载、流式资源 → **ECS** 与可扩展资源管线。
- **多人在线**：客户端/服务器同构、确定性、可序列化状态 → 清晰接口边界 + 数据驱动。
- **跨平台**：Windows / macOS / Linux 一套代码 → 平台层、渲染层可插拔。
- **工程化**：模块边界（CMake 分层强制）、单元测试、CI、文档、代码规范优先于堆功能。

**差异化赛道**：确定性权威服务器 + 数据驱动工具链（编辑器 → 打包 → 通用播放器），
而非通用编辑器优先。对比外部引擎按"MMO 目标是否受益"取舍，不逐项对齐 Godot。

**交付形态**：

- `neon_editor`：场景/资产/资源/属性/日志等多面板编辑器（含 2D 画布与 3D 视口）。
- `neon_game`：通用播放器——读取 `game.json` + 场景 JSON + Lua/JS 脚本，无硬编码玩法。
- `neon_server`：无头权威服务器，headless 复用同一 `GameRuntime`。

---

## 2. 架构分层：构建目标、依赖规则与模块地图

### 2.1 分层图

```
┌──────────────────────────────────────────────────────────────────────────┐
│ 应用层     neon_game(通用播放器) / neon_server(无头服务器) /                │
│            neon_editor(= neon_editor_lib + 薄壳 main) / neon_tests        │
├──────────────────────────────────────────────────────────────────────────┤
│ L3  neon_scene   assets / scene(GameRuntime) / script / bt / anim         │
│                  physics / ui / audio / plugin(runtime+backend)          │
├──────────────────────────────────────────────────────────────────────────┤
│ L2  neon_gfx     渲染：IRenderBackend(GL/Vulkan) + Renderer 高层 + 后处理   │
├──────────────────────────────────────────────────────────────────────────┤
│ L0/L1 neon_core  math / core / io / ecs / net / nav / platform /          │
│                  plugin(core) / kernel(微内核)                            │
└──────────────────────────────────────────────────────────────────────────┘
neon_engine = INTERFACE 门面，聚合 core/gfx/scene（旧消费者链接不变）
第三方静态库：neon_lua / neon_quickjs / neon_miniaudio / neon_imgui /
              neon_volk / neon_ufbx / Jolt
```

**模块归属与依赖方向由 CMake `target_link_libraries` 白名单强制，非靠自觉：**

1. `neon_core ← neon_gfx ← neon_scene`：只能向下依赖，越层 include/link 直接编译失败。
2. `game`/`editor`/`server`/`plugins`/`tests` 只链接公开库（`neon_engine` 门面或具体层）。
3. 后端（platform/gfx/audio/physics）实现接口，平台相关文件只在对应平台编译。
4. 高层只通过接口触达 GPU/音频/物理：`IRenderBackend`、`IAudioBackend`、`physics::World`。

### 2.2 构建目标（CMake）

| 目标                                            | 内容                                                                         |
| ----------------------------------------------- | ---------------------------------------------------------------------------- |
| `neon_core`                                   | L0/L1 基础层：math/core/io/ecs/net/nav/platform/plugin-core/kernel，平台无关 |
| `neon_gfx`                                    | L2 渲染层：GL + Vulkan 后端、Renderer、网格/材质/纹理/字体/粒子/后处理       |
| `neon_scene`                                  | L3 内容/运行时层：assets/scene/script/bt/anim/physics/ui/audio/plugin 运行时 |
| `neon_engine`                                 | INTERFACE 门面，聚合三层，历史消费者不变                                     |
| `neon_lua` / `neon_quickjs`                 | Lua 5.4 / QuickJS(ES2020) 脚本后端（JS 在 MSVC 默认 OFF）                    |
| `neon_miniaudio` / `neon_imgui`             | 跨平台音频 / Dear ImGui 工具 UI                                              |
| `neon_volk`                                   | Vulkan 加载器（`NEON_ENABLE_VULKAN=ON` 时编译）                            |
| `neon_ufbx`                                   | FBX 导入库（vendored 单文件 C）                                              |
| `Jolt`                                        | Jolt 物理（`NEON_ENABLE_JOLT=ON`，默认开）                                 |
| `neon_game` / `neon_server`                 | 通用播放器 / 无头服务器                                                      |
| `neon_editor_common` + `neon_editor_lib`    | 编辑器公共库（history/packager）+ 编辑器主体库                               |
| `neon_editor`                                 | 薄壳`main()`，链接 `neon_editor_lib`                                     |
| `neon_plugin_physics` / `neon_plugin_audio` | 原生后端示例插件（C ABI DLL/SO）                                             |
| `neon_tests`                                  | 单元测试（当前源码 731 个用例，`TEST(name)` 自研框架）                     |

### 2.3 微内核（microkernel）

`neon_core` 内的 `kernel::`（`IModule` + `ServiceRegistry` + `ModuleRegistry` + `Kernel`）
是统一模块系统。可替换子系统（渲染/物理/音频/脚本/平台/UI）各实现 `IModule`，通过
`ServiceRegistry` 拿依赖、发布服务——**替换模块 = 换注册表一项**。已落地：

- `neon/modules/subsystem_modules.hpp`：`GfxModule`/`PhysicsModule`/`AudioModule`/
  `ScriptModule` 包装已有接口，模块拥有实例并在 `Init` 注册接口。
- `GameRuntimeConfig.services`：运行时从注册表注入物理世界/脚本宿主（非拥有，回退自建）。
- 三个数据驱动入口均走 Kernel：`neon_game`、`neon_editor` F5 试玩（每会话新建 Kernel，
  `StopPlay` 回收）、`neon_server`。

### 2.4 当前状态（2026-08-31）

- 三层库拆分已完成（原单一 `neon_engine` 拆为 core/gfx/scene，依赖方向由构建强制）。
- 两处循环已破：`scene↔script`（`ScriptContext` 依赖注入 hook，script 零 scene 依赖）、
  `platform↔gfx`（窗口内联 WGL/GLX 加载）。
- 微内核 P-A/P-B/P-C/P-E 已落地并验证。
- **战斗玩法下沉 Lua**：C++ 玩法 API（`MeleeAttack`/`AttackBox`/`CastSkill`/`SkillTable`）
  已删除，`scene/skills.hpp` 移除；引擎只保留通用原语（`OverlapSphere`/`OverlapBox`/
  `SpawnProjectile`/状态容器/lag-comp），规则逻辑在**内嵌 Gameplay Lua 基础库**
  （`engine/generated/gameplay_lib.hpp`）。
- `GameRuntime` 已拆成**编排器 + 16 个组合服务**（`scene/systems/`：projectile/hud/particle/
  tween/lagcomp/status/prefab/plugin/sceneTree/scriptCanvas/ui/animation/scriptRuntime/btRuntime/
  physicsBridge/drawSystem）；`panels.cpp` 4143 行拆为主文件 ~390 行 +
  9 个 `.inc`；编辑器已库化（`neon_editor_common`/`neon_editor_lib`）。
- 剩余：Renderer 上帝类（C4）、server/game 未库化（C15）。详见 §10 与
  [`plans/2026-08-31-architecture-review.md`](./plans/2026-08-31-architecture-review.md)、
  [`plans/2026-08-31-microkernel-design.md`](./plans/2026-08-31-microkernel-design.md)。

### 2.5 模块规模（粗略，含头文件与实现行数）

| 模块     | 行数（约） | 模块              | 行数（约） |
| -------- | ---------- | ----------------- | ---------- |
| gfx      | 11.4k      | plugin            | 1.2k       |
| scene    | 7.9k       | physics           | 1.2k       |
| script   | 4.3k       | bt                | 1.1k       |
| assets   | 3.7k       | anim              | 1.0k       |
| core     | 2.5k       | ecs               | 1.0k       |
| ui       | 2.2k       | math              | 0.9k       |
| platform | 1.3k       | audio             | 0.7k       |
| net      | 1.3k       | io / nav / kernel | 0.2~0.3k   |

应用层：editor/src ~17.6k、tests ~21.7k、server+game ~3.2k。

---

## 3. 核心模块（功能 & 接口）

按模块组织，每个为一小节；每节给出**功能**、**关键接口**与**关键文件**。

### 3.1 数学（`neon::math`）

header-only、零依赖、**行主序** `Mat4`（`m[row*4+col]`）。

**功能**：`Vec2/3/4`、`Mat4`（正交/透视/旋转/平移/缩放）、`Quat`、`Transform`、
`Rect2`、`AABB`、`Ray`、`Plane`、`Frustum`（从 view-projection 提取，用于视锥剔除）、
相交检测（Ray-AABB / Ray-Sphere）、`TransformAABB`；通用数学工具（`Clamp`/`Lerp`/
`Approach`/`SmoothStep`/`WrapAngle`）；`spatial.hpp` 提供小地图/XZ 散布等纯函数；
`bvh.hpp` 提供动态 BVH（场景绘制前的视锥预剔除，G1-2）。

**关键接口**：

- `math::Mat4`（行主序；`Ortho`/`Perspective`/`Translation`/`Rotation*`/`Scale`）
- `math::Quat`（`FromEuler`/`Slerp`/`ToMat4`）
- `math::Frustum`（`FromViewProjection`/`Intersects(AABB)`）
- `math::Bvh`（插入/移除/视锥查询）

**关键文件**：`neon/math/math.hpp`、`mat4.hpp`、`quat.hpp`、`bvh.hpp`、`spatial.hpp`。

> 约定：行主序，GL uniform 提交 `transpose=GL_TRUE`；角度弧度；Y 轴向上；相机 -Z。
> 曾混用列主序，测试防回归。

### 3.2 核心（`neon::core`）

**功能**：

- `Application`：固定步长 60Hz 累加器 + 可变渲染帧 + 冒烟帧数（`SetSmokeTestFrames`）。
- `Time`、`Log`：分级（Debug/Info/Warn/Error）+ 分类（core/gfx/audio/physics/scene/ecs/
  script/bt/net/editor/game）+ 时间戳/帧号 + 环形缓冲 + 订阅 sink + 文件日志。
- `Config`（key=value）、`Rng`（xorshift64* 确定性）。
- `Serializer`/`Deserializer`：大端、magic+CRC32+version 的版本化二进制（网络与磁盘共用）。
- `Json`/`JsonWriter`：自研 DOM，递归下降 + UTF-8 + 深度限制 + 严格数字文法；
  输出支持 `%.17g` 无损往返与 pretty 缩进（场景 JSON 可 diff）。
- `Result<T>`/`Status`：显式可恢复错误。
- `PackWriter`/`PackReader`/`Unpack`：`game.pack` 单文件容器（按需读 + 每项 CRC），
  `IsUnsafeRelPath` 做路径穿越防御。
- `Profiler`/`ScopedTimer`：环形帧样本（约 8.5s 历史），供性能面板与崩溃报告。
- `Crash`：进程级崩溃处理（SEH/信号），落盘 `crash_report.txt`（日志环 + profiler）。
- `MemStats`：全局堆统计（`operator new/delete` 覆盖）。
- `ObjectPool<T,N>`：定长槽位复用，热路径免堆分配。
- `Localization`：多语言字符串表（激活 → 默认 → key 回退链）。

**关键文件**：`neon/core/app.hpp`、`log.hpp`、`serialize.hpp`、`json.hpp`、`pack.hpp`、
`profiler.hpp`、`crash.hpp`、`mem_stats.hpp`、`object_pool.hpp`、`localization.hpp`。

### 3.3 ECS（`neon::ecs`）

**功能**：SparseSet 实体-组件-系统。`Entity` = 32 位 id + 32 位 generation；
`World::Pool<T>`（dense + sparse，swap-erase）；`View<T>`/`View<T,U>` 顺序遍历。

- **批量迭代**：`View<T>::ForEach`（串行）、`ParallelForEach`（dense 切连续 chunk，
  与串行逐位一致）。
- **确定性并行 job**（`neon::ecs::parallel`）：`ParallelFor(count, fn)` 固定分块 + 持久
  线程池（Win32 CreateThread / pthread；无 worker 回退串行）；`Reducer<T>` 做逐 chunk
  归约（槽位校验，结果稳定）。
- **任务图**（`TaskGraph`）：显式依赖边、拓扑序（Kahn），同层独立任务并行、串行路径一致。
- **系统调度器**（`SystemScheduler`）：系统声明读/写组件类型 → 冲突边（写-写/写-读）
  保持注册序串行，无冲突系统并行；`Run(false)` 为确定性参考路径。
- **并行契约**：`ParallelForEach` 期间禁 `Create/Destroy/Add/Remove`（优雅拒绝 + 日志），
  视图在并行前预创建所有池；需改世界的系统先收集、并行后应用。
- 当前适合数千实体；MMO archetype 存储留作后续（批量迭代 API 已就位）。

**关键接口**：`World::Create/Destroy/Add/Get/Has/Remove`、`View<T,U>::ForEach/ ParallelForEach`、`parallel::ParallelFor/Reducer/ThreadPool`、`TaskGraph::Add/Run`、
`SystemScheduler::Add(name, sys, reads, writes)/Run`。

**关键文件**：`neon/ecs/world.hpp`、`parallel.hpp`、`task_graph.hpp`、`system_scheduler.hpp`。

### 3.4 平台（`neon::platform`）

**功能**：原生窗口 + 输入抽象，一套引擎侧状态机服务所有平台。

**关键接口**：

- `IWindow`：`Create(WindowConfig)`（GL 3.3 上下文）、`PumpEvents`、`SwapBuffers`、
  `MakeGLContextCurrent`、`ShouldClose`、`NativeHandle`（Vulkan surface）、
  `SetCaptureMouse`（相对视角）、`SetImeEnabled`（编辑器中文输入切换）。
- `IInput`：`HandleEvent(InputEvent)`（KeyDown/Up、Mouse*、Wheel、Resize、TextInput）；
  `IsDown/Pressed/Released`（边缘语义）；`MouseDown/Pressed/Released`、`MousePos/MouseDelta/ WheelDelta`；`ConsumeMouseDelta/Wheel`（独占输入，如轨道相机）；`EndTick`（固定步边缘
  推进，防止 0-tick 帧吞点击）；`EndFrame`（清逐帧累加器）。

**后端**：`platform/win32`（`CreateWindowEx`+WGL 降级链）、`platform/x11`（GLX）、
`platform/cocoa`（`NSOpenGLView`），按 OS 编译。

**关键文件**：`neon/platform/window.hpp`、`input.hpp`；`engine/src/platform/*`。

### 3.5 渲染（`neon::gfx`）

**IRenderBackend（底层接口，游戏层完全不感知具体 API）**：

- 资源：`ShaderHandle`/`TextureHandle`/`MeshHandle`/`RenderTargetHandle`；
  `CreateRenderTarget`（RGBA8 / RGBA16F 浮点 / MSAA 多重采样）、`CreateDepthTarget`（阴影
  深度纹理）、`CreateTexture`/`CreateTextureCompressed`（BC1/DXT1，驱动拒绝自动回退）、
  `CreateMesh`/`CreateMeshU32`（>65535 顶点）、`UpdateTextureRegion`（动态字形图集）、
  `UpdateMeshVertices`（蒙皮骨骼数据后置上传）。
- 状态：混合（Opaque/Alpha/Additive/Premultiplied）、深度、剔除、视口/裁剪（dock 子矩形）、
  清屏；uniform 按名惰性缓存，`SetUniformMat4Array` 上传骨骼矩阵。
- 绘制：`DrawMesh`、`DrawMeshInstanced`/`DrawMeshInstancedColored`（GPU 实例化）、
  `DrawPrimitives`（立即模式）；`BeginFrame/EndFrame`、`CaptureFrame`、`DepthAvailable`
  能力自检、`GpuMemory` 统计。
- 后端：**OpenGL**（自研加载器，`wglGetProcAddress`→`opengl32.dll`/GLX/符号直链；
  `glTexStorage2D+glTexSubImage2D` 避开旧桩驱动问题）；**Vulkan**（`NEON_ENABLE_VULKAN=ON`
  经 volk，构建期 glslang 预编译 SPIR-V；descriptor/伪 HDR 等历史灰度问题已修，GL 仍默认）；
  **NullBackend**（测试/工具 headless 上传）。

**Renderer（高层，`gfx::Renderer`）**：

- 相机（透视 lookAt / 正交）、天空渐变、距离雾 + **体积雾**、方向光 + 8 点光 + 玩家手电、
  环境光 + **IBL**（天空梯度 → CPU 预计算 irradiance/prefiltered/BRDF LUT，懒重建）。
- 阴影：**CSM**（3 级联，颜色编码深度 + 画家排序，兼容深度损坏驱动）+ **点光 cubemap
  阴影**（2 灯 × 6 面 2D map）；能力自检失败自动回退 CPU 投影接触阴影。
- 后处理：**HDR RGBA16F 离屏 → Bloom 金字塔 → ACES 色调映射（曝光可调）→ MSAA**
  （多重采样 + `glBlitFramebuffer` 解析，4x→2x 自检降级）；**SSAO**（颜色编码线性深度
  预 pass + AO + 模糊）、**体积光柱**、**SSR**，编辑器「后处理效果」面板可开关。
- 绘制：实例化 + 视锥剔除（`ViewFrustum`）、蒙皮（`uBoneMatrices[64]`）、地形 splatmap
  变体、粒子公告板、2D 立即模式覆盖层。
- **2D 设计空间**：固定 1280×720 design 单位（`kDesignWidth/Height`），`Set2DViewport`
  （fit+居中+缩放平移）、`Set2DViewportPixels`（1:1）、`SetSceneViewport`（3D 场景画进
  dock 子矩形）、`DesignSpaceRect`/`SceneViewport`/`ScreenToUI`/`ToScreen`——2D HUD 与
  3D 场景共用同一取景，避免锚点漂移。

**关键文件**：`neon/gfx/backend.hpp`、`renderer.hpp`、`camera.hpp`、`csm.hpp`、
`point_shadow.hpp`、`ibl.hpp`、`ssao.hpp`、`ssr.hpp`、`volumetric.hpp`、`bloom.hpp`、
`terrain.hpp`、`particles.hpp`、`light_probe.hpp`、`scene_props.hpp`、`material.hpp`、
`mesh.hpp`、`shader.hpp`、`texture.hpp`、`font.hpp`、`imgui_neon.hpp`。

### 3.6 音频（`neon::audio`）

**功能**：`IAudioBackend` 三平台统一；miniaudio 后端（WASAPI/DirectSound/CoreAudio/ALSA），
Windows 无设备回退 WinMM；纯软件混音器（单声道/立体声）；总线（Master/Sfx/Music）；
3D 空间音效（距离衰减 + 水平声像）；WAV 与 miniaudio 支持格式加载；Lua 绑定；
编辑器/项目程序化合成 PCM 音效。

**关键接口**：`IAudioBackend::Play/PlayMusic/Play3D/SetBusVolume/StopAll/Available`；
`LoadWav/LoadSoundFx`；`MixVoices/MixVoicesStereo`（header-only，headless 可单测）。

**关键文件**：`neon/audio/audio.hpp`、`mixer.hpp`；`engine/src/audio/miniaudio/`、
`winmm/`。

### 3.7 物理（`neon::physics`）

**功能**：可替换物理抽象，三种后端：

- **custom**：自研确定性轻量求解器（动态球/AABB vs 静态 + y=0 地面，冲量法，线速度，
  无旋转），固定步逐位确定，是跨平台 bit-exact 回退。
- **Jolt v5.0.0**（Godot 4 同款 vendored，`NEON_ENABLE_JOLT=ON`）：刚体/碰撞层掩码
  （layer 0..255）/角色控制器（`CharacterVirtual` 胶囊）/射线/接触查询。注意：同构建
  产物确定，跨架构不保证逐位一致。
- **`plugin:<name>` 原生插件后端**：经 `plugin::LoadNativePhysicsBackend` 从 DLL/SO 加载
  C ABI 工厂（`NeonPhysics_GetWorldApi`），不重链接换物理引擎。

**关键接口**（`physics::World`）：`AddSphere/AddBox/AddCharacter`、`SetCharacterMove`、
`SetPosition/SetVelocity/SetMass/...`、`Step(dt, gravity)`、`Collisions()`（owner 碰撞对）、
`Raycast`、`DebugBodies`（编辑器线框）、`RigidBodyDesc`（mass/restitution/friction/
damping/gravityScale/layer/mask）。

**关键文件**：`neon/physics/physics.hpp`、`jolt_world.hpp`；`engine/src/physics/*`。

### 3.8 资产（`neon::assets`）

**功能**：

- `AssetManager`：按路径缓存贴图（stb_image）/OBJ/glTF/GLB/FBX/字体；`IoRead/IoMTime`
  统一读取入口；**引用计数 + 延迟回收**（`Acquire/Release`，`PumpAsync` 每帧回收退休 GPU
  资源）；**资源依赖图**（glTF 外部 bin/贴图/MTL，缺失定位 + 递归异步预载）；**热重载**
  （mtime 对比 + 安全重建）；负缓存（缺失文件不反复开）。
- **异步管线**（`AsyncLoader`）：2 线程工作池，解码/解析在 worker、GPU 上传与回调在主线程
  `PumpAsync`；同路径请求合并（coalescing）；`asyncMeshLoad` 支持 OBJ/glTF 流式加载。
- **导入格式**：OBJ+MTL、glTF 2.0/GLB（多 buffer、PBR 材质、蒙皮）、**FBX**（vendored
  ufbx，多 mesh part，32 位索引）、BC1 压缩（不透明贴图 1/8 显存，驱动拒绝回退 RGBA8）。
- `AssetDatabase`：`.asset_db.json` GUID 库（path→guid/hash/size/mtime），文件移动识别 +
  场景引用重写。
- `AssetVariantTable`：`variants.json` 平台/LOD 变体（`--variant mobile`），纯数据稀疏覆盖。
- `AssetImporter`：离线 BC1 烘焙（`.neon/imported/*.nbc1`），运行时直接上传。

**关键文件**：`neon/assets/asset_manager.hpp`、`asset_db.hpp`、`asset_importer.hpp`、
`asset_variants.hpp`、`async_loader.hpp`、`bc1.hpp`、`mesh_format.hpp`、`image_decode.hpp`。

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

**VFS 实现（`neon::io`）**：`IFileSystem` 只读接口（`Exists/ReadFile/FileMTime/ListFiles`）；
`DiskFileSystem`（目录根，防穿越）、`PackFileSystem`（`game.pack` 直读不解包）、
`MountStack`（多层覆盖，后挂载的 Mod 目录优先）；`NormalizeVirtualPath` 做统一归一化。

**关键设计决策**：

| 决策                               | 理由                                                                                                      |
| ---------------------------------- | --------------------------------------------------------------------------------------------------------- |
| `projectDir_` 一律绝对化         | 路径解析不依赖 CWD——重启后启动目录不同，相对 projectDir 拼接会静默失效                                  |
| 不拼项目根绝对路径                 | VFS 已挂载项目根；再拼绝对会双前缀且破坏 glTF 相对 bin                                                    |
| glTF 外部 bin/图片相对提取         | `out.dir + uri`；传项目相对虚拟路径 VFS 直接解析，绝对靠回退直读                                        |
| 编辑器存储输出`@assets/`         | `NormalizeEntityAssetPaths` 只在路径确实位于项目根下才转换；不 CWD 拼 bare 相对；rel 去开头 `assets/` |
| 编辑器加载用`NormalizeAssetPath` | 解析实体材质（`mesh.material.*` 嵌套 material 子对象）时归一化引用                                      |
| pack 直读 + Mod 挂载               | 打包游戏读 pack 虚拟路径；Mod 层覆盖基础包，运行时无需解包                                                |

**调用点约定**：场景/预制体/材质球/贴图槽**存** `@assets/...`；读取方**传**存储路径给
`LoadTexture/LoadGLTF/LoadSkinnedModel`，由 IoRead 统一解析。**不再**在调用点做
`FullAssetPath(拼绝对)`/`NormalizeAssetPath`/`ResolveMeshAssetPath` 重复转换。

**关键文件**：

| 文件                                                           | 作用                                                                       |
| -------------------------------------------------------------- | -------------------------------------------------------------------------- |
| `engine/include/neon/assets/asset_path.hpp`                  | `NormalizeAssetPath` / `HasAssetScheme`                                |
| `engine/include/neon/io/vfs.hpp` / `engine/src/io/vfs.cpp` | `IFileSystem` / `DiskFileSystem` / `PackFileSystem` / `MountStack` |
| `engine/src/assets/asset_manager.cpp`                        | `IoRead` / `IoMTime` / `ReadAllBytes`                                |
| `engine/src/scene/game_runtime_content.cpp`                  | `FullAssetPath`（归一化入口）                                            |
| `engine/src/scene/skinned_model.cpp`                         | `LoadSkinnedModel`（经 `IoRead` 读 gltf JSON text）                    |
| `editor/src/editor_scene.cpp`                                | `NormalizeEntityAssetPaths`、实体材质解析                                |
| `editor/src/editor.hpp`                                      | `MountAssetVfs`（挂载项目根 VFS）                                        |

### 3.9 UI（`neon::ui`）

**四条轨道**（各司其职）：

- **立即模式 UI**（`ui.hpp`）：`DrawLabel/...`，HUD 小元素。
- **控件树**（`system.hpp`）：`UIManager` + `Element`（相对坐标 + `Measure/Layout/Draw` +
  `AbsolutePos` 累加），Widget 含 Panel/Label/Button/TextField/Slider/CheckBox/VBox/HBox/
  ScrollArea/Window/List/TreeView/ComboBox/TabBar/DockLayout——游戏内 HUD 用。
- **数据驱动文档 UI**（`document.hpp` + `layout_solver.hpp`）：`ui/*.ui.json` 节点树
  （Panel/Row/Column/Label/Button/Bar/Image），两种布局：legacy 绝对 `rect` 与
  **boxflex**（px/%/auto/center + Row/Column flex），可插拔 `ILayoutSolver` 注册表 +
  `ITextMeasure`；9-slice 边框、富文本颜色；布局结果按 (dirty, viewport) 记忆化（B12）。
- **编辑器工具 UI**：Dear ImGui（`neon_imgui`，docking 分支，CJK 中文）。

**可替换缝（IUiSystem）**：`GameRuntime` 与脚本只依赖 `ui::IUiSystem`
（`Show/Hide/Update/Clicked/SetText/SetFill/SetVisible/SetColor/Draw`），经
`GameRuntimeConfig.uiSystem` 注入；默认实现为 `CreateDocumentUiSystem`（文档 + 布局求解器）。
换整套 UI 栈无需动脚本/场景/编辑器。点击为边缘触发（`ConsumeClicks` 在 tick 末清理，
保证 0-tick 渲染帧不吞点击）。

**关键文件**：`neon/ui/ui.hpp`、`system.hpp`、`document.hpp`、`layout_solver.hpp`、
`ui_system.hpp`。

### 3.10 场景 & 内容创作运行时（`neon::scene` / `neon::script` / `neon::bt` / `neon::anim`）

数据驱动工具链核心：编辑器编辑的内容被 `neon_game`（通用播放器）与 `neon_server`
（无头服务器）无差异运行。

#### 场景文件（`neon::scene`）

组件化 JSON（`assets/scenes/*.json`）：实体带 `id/name/parentId` + 组件列表。内置组件：
`transform`、`mesh`（meshKey + LOD 链 + PBR 材质槽 + UV 平铺）、`sprite`（序列帧/spritesheet/
billboard）、`health`、`script`/`scripts`（多脚本）、`behaviorTree`、`rigidbody`、
`character`、`audio`、`terrain`（高度图 + chunked LOD + 植被 Impostor）、`tilemap`、
`decal`、`camera`（fov/正交/宽高比）、`light`（方向/点/环境）、`groups`、`nodeType`、
`animOverride`、`sortOrder`、`zombie` 等。

- **预制体**：`assets/prefabs/*.json` 模板，实例只存字段覆盖（`ComputePrefabOverrides`/
  `MergePrefabOverrides` 往返）。
- **场景继承**：`extends` 父场景先载入，子场景同名实体覆盖。
- **序列化闭环**：`SceneFile::Parse/ToJson/MakeEntity/MakeSpriteEntity/FromWorld/ EntityToJson`——`Parse(FromWorld(w))` 可还原等价 World（编辑器 live World 驱动检查器）。
- **组件工厂**：`ComponentRegistry` + `ComponentFactory`；无工厂组件存 `SceneData` 供脚本/
  插件读取。**反射**（`component_reflect.hpp`）：字段列表单一声明 → 编辑器 Schema + JSON
  序列化/反序列化（C6 单一事实源）。
- **Instantiate**：按字母序确定性应用组件，prefab 展开 + 实例覆盖，失败整体回滚。

#### 世界分区流式（`WorldChunk` / `ChunkStreamer`）

`(2*radius+1)²` 窗口（默认 3×3，chunk 64 单位）围绕焦点异步加载/卸载：文件读取在
`AsyncLoader` worker，解析 + Instantiate + 回调在主线程 `PumpAsync`；`Clear()` 用 epoch
取消在途加载；`AcquireChunkAssets/ReleaseChunkAssets` 配合资产引用计数卸载释放。

#### `GameRuntime`（可复用运行时）

生命周期：`Start(sceneJson, cfg)` → `Tick(dt)` / `Draw(renderer, camera)` / `DrawUI` →
`Stop()`。headless 模式（`cfg.headless`）跳过绘制列表，服务器/测试复用同一类。

**配置**（`GameRuntimeConfig`）：`assets`（null = 纯模拟）、`services`（微内核注入）、
`scriptBaseDir/assetBaseDir`、`readScript`（pack 覆盖）、音频 hooks、`input`、
`font2d`、`uiSystem`、`rngSeed`（固定可复现）、`physicsBackend`（custom/jolt/plugin:*）、
`pluginBaseDir`、`fileSystem`（VFS）、`variantTable`、`asyncMeshLoad`、`parallelSystems`。

**Tick 子系统**（固定 60Hz 步内，串行或经 `SystemScheduler` 并行且逐位一致）：
脚本（捕获的 chunk handler，逐实体 `on_start/on_update`）→ 行为树 → tween → 状态效果
（tick 规则经 Lua `OnStatusTick`）→ 投射物 → 动画（固定步）→ 物理 `Step` + 变换回写 →
运行时插件 `tick` → `ChangeScene` 帧边界延迟生效。

**Draw 子系统**：meshKey 解析（`obj:`/`gltf:`/程序化图元）、LOD 链选择、实例化合批 +
每帧 BVH 视锥预剔除、蒙皮模型（每实体 clip 覆盖）、精灵/序列帧/spritesheet/billboard/
decal、地形 chunk + 植被 Impostor、粒子、`WorldToScreen` 锚点（血条/名牌）、
`FloatTexts`、脚本 2D 画布（`on_render` → `FlushCanvas`）。

**战斗原语（引擎保留，规则在 Lua）**：`SpawnProjectile(pos, dir, speed, damage, life, caster, range, hitRadius, statuses)`（fixed-step 移动 + 命中 + 粒子）、
`OverlapSphere/OverlapBox`（`rewindTicks` 滞后补偿回滚查询）、状态容器
（`ApplyStatus/HasStatus/StatusMagnitude/RemoveStatus`，id：burning/poison/regen/slow）、
lag-comp 姿态环形历史（服务器按 RTT 自动回滚命中）。

**内嵌 Gameplay 基础库**：`engine/generated/gameplay_lib.hpp` 的 `kGameplayLibLua` 在
`GameRuntime::Start` 于项目脚本前注入全局 `Gameplay` 表：

| 模块     | 函数                                                                                                      |
| -------- | --------------------------------------------------------------------------------------------------------- |
| 属性     | `Gameplay.Stats.Get/Set/Add`（基于 GameVar）                                                            |
| 冷却     | `Gameplay.Cooldowns.new/set/left/ready/tick`                                                            |
| 命中/AoE | `Gameplay.MeleeArc/AoE/BoxAttack`（基于 Overlap 原语）                                                  |
| 投射物   | `Gameplay.Projectile(...)`（SpawnProjectile 薄包装）                                                    |
| 状态     | `Gameplay.RegisterStatus(name, interval, tick)` + 内置 burning/poison/regen/slow；默认 `OnStatusTick` |
| 技能     | `Gameplay.SkillTable.fromJson/cast`（冷却/mana/kind 分发）                                              |
| 背包     | `Gameplay.Inventory`（堆叠/上限/货币/use 回调/存档）                                                    |
| 控制器   | `Gameplay.FirstPerson/ThirdPerson`（鼠标视角 + 相对移动 + 相机 GameVar 约定）                           |

> 历史：C++ 玩法 API（近战/攻击盒/技能表）已删除，`scene/skills.hpp` 移除；
> 玩法 = 原语 + 数据 + Lua，详见
> [`plans/2026-08-31-combat-lua-design.md`](./plans/2026-08-31-combat-lua-design.md)。

#### 脚本（`neon::script`）

- `IScriptHost`：`Init/Shutdown/Load/Run/Call/CallCaptured/CaptureFunction/Register/ RegisterField/SetRngSeed/SetSimClock/CheckSyntax` + 调试器（断点/单步/局部变量/调用栈）
  + 失控保护（指令预算/内存上限）。
- 双后端：Lua 5.4（默认）/ QuickJS ES2020（MSVC 默认 OFF）；同一 `IScriptHost` 语义，
  场景脚本按 `backend` 字段选。
- **确定性沙箱**：xorshift RNG 替换 `math.random`、模拟时钟注入、`io/os/package/require`
  关闭、表转换深度/循环保护、同 seed 逐位一致。
- **绑定**（`bindings.cpp`，经 `ScriptContext` hooks 注入，脚本层零 scene 依赖）：
  ECS（Spawn/GetPosition/...）、物理（Raycast/Physics*）、音频、GameVars、Json、输入
  （InputAxis/Key/Mouse + `input.json` Action 映射）、UI（UIShow/UIClicked/...）、
  2D 画布（DrawRect/DrawText/DrawSprite）、OverlapSphere/Box、SpawnProjectile、状态、
  动画（PlayAnimation/状态机）、信号（SignalConnect/Emit）、Tween、ChangeScene、
  实体组件读写、地形高度、组查询、存档读写、相机 GameVar 约定（`cameraFocus` 等）。
- **隔离**：每实体 vars 实例快照注入（A6 修复）、每个 chunk 捕获自己的
  `on_start/on_update` 句柄（跨 chunk 不互相遮蔽）。
- `GameVars`（脚本与行为树共享全局键值）、`Blackboard`（实体级）、`InputMap`
  （Godot 式动作→按键 JSON + `ActionDown/ActionPressed/ActionAxis`）。

**关键文件**：`neon/script/script.hpp`、`bindings.hpp`、`gamevars.hpp`、`blackboard.hpp`、
`input_map.hpp`、`lua_host.hpp`、`js_host.hpp`。

#### 行为树（`neon::bt`）

`Node::Tick(Context&)`（状态在调用方 Context）：组合（Sequence/Selector/RandomSelector/
Parallel）、装饰（Invert/Cooldown/Repeat/UntilFail）、行为（MoveTo/Attack/Dialogue/Spawn/
Wait/PlaySfx/RunScript）、条件（InRange/HasTarget/QuestState/HealthBelow/BlackboardCmp/
GameVarCmp/ScriptBool）；JSON 加载/序列化（`{"root": {...}}` 往返）；节点类型注册表
（编辑器校验子节点数/参数）；每实体计时器驻留在 `Context::timers`，一棵树可服务多实体；
`activePath` 供编辑器高亮。

#### 骨骼动画（`neon::anim`）

glTF 蒙皮导入（JOINTS/WEIGHTS/IBM，含 `FixSkinBind` 处理非标准导出）、clip
（LINEAR/STEP/CUBICSPLINE）、`Skeleton/Pose`、`Animator`、**参数驱动状态机**
（`AnimationStateMachine`，过渡 + 交叉淡化，`.asm.json` 数据驱动资产）、
`BlendSpace1D/2D`、两骨骼 IK、`SkinnedModel` 每实体 clip 覆盖（多实体共享模型各播各的）、
`.anim.json` clip 编辑器、GPU 蒙皮（`uBoneMatrices[64]`）。

### 3.11 导航（`neon::nav`）

`NavGrid`：2D 可行走网格（世界空间 cell + walkable 位图）+ A*（八方向、绕墙、少转弯）、
`.navgrid.json` 资产往返；编辑器导航面板（格子编辑/起点终点/路径预览）；运行时
`WorldToCell/CellToWorld/FindPath` 查询。

**关键文件**：`neon/nav/nav_grid.hpp`。

### 3.12 网络（`neon::net`，引擎层）

**功能**：

- `UdpSocket`：非阻塞 UDP（Winsock/BSD），单 peer 模型，`Bind/BindLoopback/SetPeer/ Send/Recv/RecvFrom`，Winsock 引用计数启停，零线程。
- `ReliableChannel`：滑动窗口可靠通道（u16 序号，累积 + 选择性 ACK 位图、重传、乱序
  重排、断线超时），MTU 上限 1200B；由应用时钟驱动 `Tick(nowMs)`。
- `MessageCodec`：magic/version/CRC + 字段边界校验（防恶意输入）；协议 v5，消息集：
  Join/Welcome/Input/Snapshot（**分片** B13）/Spawn/Despawn/Ping/Pong/Ack/Login/LoginOk/
  CharList/Rpc。
- `RpcDispatcher`：按名注册/派发 JSON 参数 RPC（`MsgRpc`），服务器内置
  `room.create/join/leave/list/broadcast`。

**关键文件**：`neon/net/socket.hpp`、`reliable.hpp`、`protocol.hpp`、`rpc.hpp`。

### 3.13 插件（`neon::plugin`）

**清单**：`plugin.json` = `id/name/version/type(editor|runtime|native)/backend(lua|js| native)/entry/minEngineVersion/requires/permissions`；`DiscoverPlugins` 扫描
`<base>/plugins/*/plugin.json`。

**运行时插件**（`RuntimePluginManager`，Lua/JS，共享确定性沙箱）：`Plugin.Info/Log/ On(event,fn)/OnCommand/GetVar/SetVar/Export/Call/RegisterComponent`；变量按插件 id 前缀
隔离；事件 `tick/stop/player_join` 等；加载顺序按 `requires`。

**原生插件**（G4-1）：稳定 C ABI（`NeonPlugin_GetInfo` + 模块专属 getter），
`NativePlugin::Load/Symbol/Reload`；**后端提供者**（G5-1）：`NeonPhysics_GetWorldApi` /
`NeonAudio_GetApi` 工厂表 → `LoadNativePhysicsBackend/LoadNativeAudioBackend`，宿主经
`GameRuntimeConfig.physicsBackend="plugin:<name>"` 运行时换后端；创建/销毁在同一模块
（不跨 CRT）。

**关键文件**：`neon/plugin/plugin.hpp`、`runtime_plugin.hpp`、`native.hpp`、`backend.hpp`。

### 3.14 微内核（`neon::kernel`）

统一模块系统，实现"每个模块可重写/替换"：

- **`IModule`**（`neon/kernel/module.hpp`）：`Info()`（id/version/requires）、
  `Init(ServiceRegistry&)`（注册服务 + 拿依赖）、`Shutdown()`；模块间不直接 include/link。
- **`ServiceRegistry`**：类型擦除服务表（接口类型 → 实现指针），`Register<T>`/`Get<T>`；
  替换模块 = 覆盖注册一项。
- **`ModuleRegistry`**：按 `requires` 拓扑序初始化、逆序关闭、环/缺依赖检测。
- **`Kernel`**：微内核本体（`ModuleRegistry` + `ServiceRegistry` 打包），加载模块 + 主循环
  生命周期。
- **子系统模块**（`neon/modules/subsystem_modules.hpp`）：`GfxModule`/`PhysicsModule`/
  `AudioModule`/`ScriptModule` 包装已有接口。
- **运行时接线**：`GameRuntimeConfig.services`——运行时从注册表注入物理世界/脚本宿主
  （非拥有，回退自建，`injectedScriptHost_` 守卫生命周期）。

可替换的 I/O 服务（渲染/物理/音频/脚本/平台/UI）均已接口化并接成模块；BT/动画/资产是
引擎内置的确定性内容运行时（纯逻辑），非可替换后端。详见
[`plans/2026-08-31-microkernel-design.md`](./plans/2026-08-31-microkernel-design.md)。

**可替换边界（重要，避免"半成品"误读）**：微内核的**运行时替换**只对「被 GameRuntime
消费的服务」有意义——物理世界、脚本宿主（它们经 `cfg.services` 非拥有注入）。**渲染/音频/
平台/UI** 是**源码级或构造时替换**（`IRenderBackend` 的 GL/Vulkan 走编译开关、`IAudioBackend`
的 miniaudio/winmm/null 走构造、`IWindow`/`IInput`、`IUiSystem` 走注入），它们的生命周期与
窗口/Draw 循环深度绑定，运行时替换需求不强，故生产入口**刻意只接 `PhysicsModule` +
`ScriptModule`**；`GfxModule`/`AudioModule` 保留为接口包装（`test_modules.cpp` 验证），供
未来需要运行时热替换时启用。BT/动画/资产是确定性内容运行时（纯逻辑），非可替换后端。

---

## 4. 数据流（一帧 / 一个固定步）

### 4.1 渲染帧（客户端/编辑器）

```
PumpEvents → 平台事件 → IInput::HandleEvent
                    ↓
固定步长累加器：OnUpdate(dt) 60Hz（0..n 个固定步）
  ├─ 每步 GameRuntime::Tick（脚本/BT/tween/状态/投射物/动画/物理/插件）
  │    └─ physics::World::Step（固定 dt）
  └─ assets.PumpAsync / chunkStreamer.PumpAsync（异步资产与 chunk 完成）
OnRender：
  ├─ Renderer::BeginFrame（清屏）
  ├─ SetCamera → CSM/点光阴影 pass
  ├─ 场景 3D 绘制进 RGBA16F HDR 离屏目标（实例化合批 + BVH 剔除 + 蒙皮 + 地形/植被）
  ├─ Renderer::EndScene：Bloom → ACES 色调映射 → 后备缓冲（之后 2D 不被 tone-map）
  ├─ 脚本 2D 画布（on_render）→ FlushCanvas + HUD 锚点/飘字
  ├─ DrawUI（数据驱动 ui/*.ui.json，在 EndScene 之后保持作者颜色）
  ├─ 编辑器：ImGui 工具 UI / 调试覆盖层（在 ResetSceneViewport 之前画 gizmo）
  └─ Renderer::EndFrame：交换缓冲
```

要点：

- **固定步长 60Hz 累加器**（`Application`/`GameServer`）驱动逻辑，渲染帧率独立——
  物理/逻辑确定性前提。`IInput::EndTick` 在固定步末推进边缘基线（0-tick 渲染帧不吞点击）。
- **场景先 HDR 后合成**：3D 画进 RGBA16F 离屏目标，`EndScene` 跑 Bloom + ACES 到后备缓冲；
  2D/HUD/UI 最后画（不受 bloom 影响）。
- 服务器（headless）只跑 `OnUpdate`，无渲染，复用同一 `GameRuntime`。

### 4.2 服务器固定步

```
GameServer::Step(nowMs)（单调时钟，最多推进 1 个 60Hz 固定步）
  ├─ PumpNetwork：收 datagram → ReliableChannel.OnDatagram → 消息分派
  │    （Join/Welcome/Login/Input/Ping/RPC/...）
  ├─ 应用输入（每客户端 NetInput → 对应实体脚本 / v1 控制器回退）
  ├─ runtime_.Tick（确定性沙箱）
  ├─ 姿态历史入环形缓冲（lag-comp）→ 按客户端 RTT 回滚命中
  └─ BroadcastSnapshot：AOI 九宫格兴趣集 → 增量 Spawn/Despawn → MsgSnapshot（可分片）
```

---

## 5. 工具链层（编辑器 / 插件 / 网络）

引擎之上的工具链与应用层：编辑器、插件系统、网络。

### 5.1 编辑器（`neon_editor`）

`neon_editor.exe`（= `neon_editor_lib` + 薄壳 `main()`），**工具 UI 用 Dear ImGui
（docking 分支）**，游戏内 HUD 用自研控件树。源码已按职责拆分：
`editor_viewport.cpp`（相机/渲染/拾取/gizmo）、`editor_play.cpp`（F5 试玩，每会话新建
Kernel）、`editor_scene.cpp`（场景树/序列化/预制体）、`editor_assets.cpp`（导入/缩略图/
材质球）、`editor_ui.cpp`、`editor_util.cpp`（共享纯工具单一来源）、`bt_editor.cpp`、
`editor_plugin*.cpp`、`packager.cpp`、`history.cpp`；`panels.cpp` 主文件 ~390 行 + 9 个 `.inc`
（按面板拆分），面板由 `PanelDef` 注册表统一驱动（视图菜单 + ini 持久化）。

**面板**：场景（树形层级 + 增删复制 + 拖拽排父子）、资产、资源（已加载资源统计）、属性
（Schema 驱动）、日志、视口、模型预览、插件、导航（NavGrid 编辑）、调试覆盖层（F3）、
UI 编辑器（`ui/*.ui.json` 节点编辑 + 1:1 预览 + 拖动/缩放）、本地化、性能（Profiler）、
输入映射、脚本编辑器（语法检查/外部编辑器）、行为树可视化、打包、地形/瓦片等。

**编辑器功能要点**：

- **项目选择器**：扫描 `projects/*/game.json`（`editor.mode` 2d/3d），场景选择器；
  上次项目从 `neon_editor_config.json` 恢复。
- **预制体工作流**：`assets/prefabs/*.json` 模板 + 实例字段覆盖（diff）+ "重置为预制体"。
- **材质球**：`materials/*.mat.json`（颜色/金属度/粗糙度/AO/自发光/四张贴图），
  另存为/拖拽赋值，场景导出带引用并展开。
- **视口**：3D（右键旋转/中键平移/滚轮缩放/射线拾取 + ImGuizmo + 多相机）+ 2D 画布
  （格子编辑，如 PvZ 草坪）；`DockViewportScope` 统一把 `SetSceneViewport` 设为 design 矩形。
- **热重载**：脚本（play 重启）/资产（mtime）/自定义 fragment shader（`--hot`）。
- **一键试玩 & 打包**：工具栏/F5 统一入口（3D 序列化当前场景 / 2D 加载项目场景）；
  `--package <project> <out>` → `game.pack`（打包器收集 glTF 依赖、程序化 mesh key 校验、
  内容哈希增量）；`neon_game --pack <game.pack>` 运行。
- **CLI**：`--smoke-test <n>`（UI 交互冒烟）、`--screenshot out.png <frame>`、
  `--bench`、`--project`、`--2d`、`--ui-editor`、`--hot`、`--backend gl|vulkan` 等。

#### 5.1.1 渲染视口 & 坐标体系

把"渲染窗口"分两层（很多人踩坑处）：

- **渲染目标（render target / HDR 目标）= 整窗大小**（画布）。
- **渲染视口（rasterization viewport）= 3D 场景真正写入像素的区域** = dock/panel 矩形
  （`viewportScreenRect_` / `Renderer::SetSceneViewport`；GL 后端 y 翻转 左上→左下）。
- **两套坐标**：① 屏幕像素（`viewportScreenRect_`，左上原点，GL 翻转成左下）；
  ② 设计单位 `viewportRect_`（1280×720，2D 游戏输入，经 `ScreenToUI` 换算）。
- **统一取景**：3D 场景视口 = design 空间映射矩形（16:9，dock 内居中留黑边），相机宽高比
  固定 16:9；`renderer.SceneViewport()` 应等于 `renderer.DesignSpaceRect()`——否则世界锚定
  2D 元素系统性偏移。
- **gizmo 对齐**：覆盖层（DrawDebugOverlay）必须在 `ResetSceneViewport()` **之前**画
  （panel 视口仍生效），否则差一条菜单栏 ≈80px。`SetSceneViewport`+y 翻转+`ScreenToUI`
  **必须保留**，别删。

### 5.2 插件系统

插件是引擎的平台化扩展：**编辑器插件**（面板/工具/资产源/组件检查器）、**运行时插件**
（跨游戏玩法模块）、**原生插件**（DLL/SO 后端提供者）。共用 `plugin.json` 与双语言加载器
（Lua/QuickJS），共享确定性沙箱。

- **运行时插件 API**（`Plugin`）：`Info/Log/On(event,fn)/OnCommand/GetVar/SetVar/Export/ Call/RegisterComponent`；引擎绑定全量可用。加载：`GameRuntime::Start` 扫
  `<scriptBaseDir>/plugins` 按依赖排序 → `on_load/on_start`；`Tick` 派发 `tick`；
  `Stop` 派发 `stop`；服务器 `player_join` 转发。
- **编辑器插件 API**（`NeonEditor`）：`panel/tool/assetSource/registerComponent/buildMesh/ spawn/selected/entities/importAsset/listDir/log/ui.*`。
- **原生插件**：稳定 C ABI + 生命周期（创建/销毁同模块），`NeonPhysics_GetWorldApi` /
  `NeonAudio_GetApi` 后端工厂；宿主 `plugin:<name>` 运行时替换物理后端。
- **示例**：`tree_gen`（editor/lua 程序化树）、`asset_vault`（editor/lua 资产源）、
  `inventory`（runtime/js 背包）、`physics_plugin` / `audio_plugin`（native 后端）。

**关键实现**：`engine/src/plugin/plugin.cpp` / `runtime_plugin.cpp` / `native.cpp` /
`backend.cpp`、`editor/src/editor_plugin.cpp`。

### 5.3 网络层（服务器 / 客户端）

客户端/服务器**同构**：复用同一 `scene::GameRuntime` + 确定性 Lua 沙箱。

```
  neon_game(client)  client::ClientSync（快照缓冲+插值+预测回滚）
        │ UDP
  ReliableChannel（滑动窗口 ACK/重传/乱序重排/断线判定）
        │
  neon_server(host)  GameServer（固定 60Hz 权威模拟 → 广播快照）
        │             AoiGrid（九宫格兴趣集裁剪 + Spawn/Despawn 增量）
```

- **权威服务器**（`server::GameServer`）：headless、`Step(nowMs)` 每调用至多 1 个固定步；
  确定性沙箱（固定 seed）；每客户端 AOI 快照（`(2r+1)²` 格，默认 r=1、cell 32 单位）；
  快照**分片**（B13，突破 ~48 实体/1200B 帧上限）；多玩家输入模型
  （`on_player_join` + `BindPlayerToClient` → 每实体输入路由）；v0 匿名登录 + 角色列表；
  **RPC + 房间**（`room.create/join/leave/list/broadcast`）；**防作弊**（输入限速、
  kick/ban 按 clientId/name、admin 命令）；**lag comp**（Ping RTT → 回滚 tick 数）；
  超时断线；`SetScriptedInputs` 确定性验收注入。
- **客户端同步**（`client::ClientSync`）：快照环形缓冲 + 相邻插值（yaw 最短弧，渲染时钟
  落后 6 tick ≈100ms）+ 本地预测（同场景同 seed 跑本地 `GameRuntime`）+
  **快照对齐回滚**（分歧超阈值即纠正）；1Hz Ping 心跳。
- **确定性验收**（`tests/test_determinism.cpp`）：同一脚本输入流在服务器/客户端逐位一致 +
  状态哈希相等。LAN demo：一 `neon_server` + 两 `neon_game --connect`（首个登录 = 输入
  控制器，其余观察者）；需**相同 `--scene`/`--seed`**。

---

## 6. 数据驱动游戏示例

- **2D**（`projects/pvz`，植物大战僵尸）：玩法+绘制全在 Lua（向日葵/豌豆/僵尸/波次表），
  精灵贴图在 `assets/sprites/`；植物/僵尸是 `assets/scenes/pvz.json` 里带 `plant`/`zombie`
  组件的实体，spritesheet 图集序列帧；编辑器 2D 画布按格子编辑写回场景，运行时读取；
  F5/▶ 编辑内试玩。
- **3D**（`projects/neon_realm`）：112 实体场景 JSON + `realm.lua`；程序化 mesh key
  （terrain/tree/house/hero/wolf/npc:r,g,b/rock/water/road…）；脚本：`Gameplay`
  第一/第三人称控制器、近战弧线/火球投射物/状态效果（OnStatusTick）、狼群 AI 与波次、
  NPC 对话、HUD/小地图、存档。
- **RTS 复刻**（`projects/wc3`，3D 模式）：`wc3.lua` 驱动——WC3 式相机、屏幕空间拾取、
  框选、右击移动/采矿/攻击、小地图、经济、建筑放置/生产队列、敌军 AI 波次、胜负；
  `Game` 实体为脚本宿主（script 挂 wc3.lua，无 mesh）。
- **经典小游戏**（`projects/snake`）：纯 Lua 贪吃蛇，零贴图（`DrawRect/DrawText`），
  `vars.demo=1` 自动演示模式；UI 用数据驱动文档（`UIShow`/`UIClicked`）。
- **物理 demo**（`projects/physics_demo`）：Jolt 刚体质量/弹性/摩擦/碰撞演示。
- **默认项目**（`projects/default`）：编辑器默认 3D 场景（hero.lua + Kenney/glTF 示例资产）。

**一键试玩 & 打包**：编辑器单一试玩入口（工具栏/F5），`StartPlaytest` 按模式分支（3D 序列化
当前编辑场景 / 2D 加载项目场景文件）；数据驱动游戏有 `on_render` 则自己画 HUD。打包器收集
glTF 依赖（buffers/images URI 一并入包）、程序化 mesh key 校验放行、内容哈希增量打包；
`neon_game --pack` 经 VFS 直读 pack（Mod 目录可叠加覆盖）。

---

## 7. 关键设计决策记录

| 决策                                | 理由                                                                          |
| ----------------------------------- | ----------------------------------------------------------------------------- |
| 自研 GL 加载器                      | 无第三方依赖、可审计、跨平台路径统一                                          |
| 行主序矩阵 + transpose=GL_TRUE      | 单一约定，测试防回归                                                          |
| ECS 而非 GameObject 树              | 大实体缓存友好迭代 + 并行化                                                   |
| 固定步长 + 累加器                   | 物理/逻辑确定性；渲染帧率独立                                                 |
| 深度不可用降级画家算法              | 深度能力自检；不可用时自适应降级                                          |
| 颜色编码深度（阴影/SSAO）           | 颜色编码 + 画家排序；深度纹理路径能力自检后启用                              |
| 投影阴影 → CSM/点光阴影            | 能力自检通过后启用 CSM；失败回退 CPU 投影                                     |
| HDR 离屏 + 渐进 bloom + ACES + MSAA | 保留亮部、可调曝光；能力自检失败自动回退                                      |
| 客户端/服务器同构 + 确定性沙箱      | 同一输入流逐位一致，是快照/预测/回滚前提                                      |
| AOI 按客户端裁剪快照                | 九宫格兴趣集 + 增量，避免全量快照超帧上限                                     |
| 快照分片（B13）                     | 突破 MTU/实体上限；等时效快照可走不可靠通道演进                               |
| 资产路径统一`@assets/` + VFS      | 一处解析、pack 直读、Mod 覆盖、绝对/相对兼容回退                              |
| 资产引用计数 + 延迟回收             | 流式卸载不 UAF，缓存随焦点移动收敛                                            |
| 异步资产管线                        | 解码/解析离线、上传主线程，无每帧卡顿；同路径合并                             |
| 接入 Jolt 物理                      | Godot 4 同库，MMO 所需刚体/角色/碰撞层；保留接口抽象可换                      |
| 战斗玩法下沉 Lua（Gameplay 库）     | 引擎只留通用原语，玩法 = 数据 + 脚本，跨游戏复用                              |
| 编辑器用 ImGui                      | 工具 UI 惯例（游戏内 HUD 用自研控件树，分开）                                 |
| UI 数据驱动文档 + IUiSystem 缝      | 脚本可组 UI；换整套 UI 栈不动脚本/场景                                        |
| 三层库拆分 core/gfx/scene           | 依赖方向由 CMake 强制（只能向下），越层引用编译失败                           |
| 微内核（IModule + ServiceRegistry） | 每个可替换子系统一个模块，替换 = 换注册表一项；C++ 接口为主、原生插件包 C ABI |
| 运行时依赖注入（`cfg.services`）  | 物理/脚本从注册表拿（非拥有 + 回退自建），模块与运行时解耦                    |
| 原生插件 C ABI 后端                 | DLL/SO 独立编译/热替换，创建销毁同模块不跨 CRT                                |
| 确定性 ECS 并行（调度器/任务图）    | 冲突边串行、独立并行，结果与串行逐位一致                                      |

---

## 8. 代码规范（附录）

- **语言/构建**：C++17（禁 C++20 特性）；CMake ≥3.15；第三方依赖必须 vendored、附许可证。
- **命名**：命名空间 `neon::()`；类型/函数 `PascalCase`；变量 `camelCase`；成员 `camelCase_`
  尾下划线；常量 `k` 前缀（`kMaxPointLights`）；宏 `NEON_` 前缀；`.hpp`/`.cpp`/`.mm`。
- **头文件**：自包含 + `#pragma once`；接口头禁平台/GL 头；优先 `const T&`，接口类纯虚 +
  工厂返回 `unique_ptr`。
- **错误处理**：返回值/日志优先；`core::Result<T>` 显式可恢复结果；日志分级
  Debug/Info/Warn/Error + 分类，禁静默吞错；资源创建失败返回无效句柄 + `NEON_LOG_ERROR`。
- **数学约定**：`Mat4` 行主序，GL 提交 `transpose=GL_TRUE`；角度弧度；Y 上、相机 -Z；
  矩阵改动必须补单测。
- **分层纪律**：游戏层禁 `windows.h`/GL/X11/Cocoa 头；平台后端按 OS 编译；新增跨平台先加接口；
  新增模块依赖只允许向下。
- **提交前**：`cmake --build build -j` 无警告（`-Wall -Wextra`）；`neon_tests` 全绿；
  `neon_editor --smoke-test 240` 退出码 0。

---

## 9. 路线图 & 与 Godot 的差距

> 目标 = 类《魔兽世界》MMO。按"MMO 收益/成本"取舍，**不逐项对齐 Godot**
> （确定性权威服务器 + 数据驱动工具链是本引擎差异化赛道）。

### 9.1 当前状态（已交付）

分层引擎（core/gfx/scene + 微内核）、GL/Vulkan 后端、内容创作运行时（Lua 沙箱/行为树/
动画状态机/数据驱动 UI）、数据驱动工具链闭环（编辑器 → 打包 game.pack → 通用播放器，
VFS 直读 + Mod 覆盖）、编辑器深化（gizmo/撤销/材质球/行为树可视化/脚本面板/缩略图/
多相机/热重载/性能面板/UI 编辑器/导航/本地化/输入映射）、渲染（PBR/CSM+点光阴影/
HDR+Bloom+ACES/MSAA/IBL/SSAO/体积光/SSR/GPU 蒙皮/LOD/BC1/地形 splat+chunked LOD+植被）、
资产管线（异步加载/引用计数/依赖图/变体表/GUID 库/BC1 烘焙/FBX）、平台/性能
（miniaudio/ECS 并行调度/世界分区流式/原生插件后端）、战斗下沉 Lua（Gameplay 基础库：
技能/背包/控制器/状态）、网络层（UDP 可靠/权威服务器/AOI/快照插值/预测回滚/分片/RPC/
房间/防作弊/确定性验收）、测试 700+ 项（源码 731 个用例）与冒烟。

### 9.2 里程碑（M1~M5）

- **M1 渲染质量**：Vulkan 后端、CSM+点光阴影、PBR、实例化+视锥剔除、IBL、HDR+Bloom+ACES+MSAA
  ✅；地形分块 LOD 与纹理 splatting ✅（chunked LOD + Layer Blend 已落地）。
- **M2 角色与战斗**：骨骼动画管线、技能/状态效果框架 ✅（已下沉 Lua Gameplay 库）；
  命中检测/胶囊体/旋转刚体/击退 ⃝；摄像机碰撞与智能跟随 ⃝。
- **M3 大世界与流式**：chunk 分区、glTF 资产管线、BC1+LOD+异步解码、对象池 ✅；archetype
  存储与确定性快照为未来后端 ⃝。
- **M4 网络化**：UDP 可靠、无头权威服务器、快照插值/预测回滚、AOI、v0 登录/角色、
  多玩家输入模型、快照分片、RPC/房间/防作弊/lag comp ✅；分区分服（world/instance server）⏳。
- **M5 工具与运营**：场景/编辑器基础 + 进阶 ✅（面板拆分/UI 编辑器/导航/本地化/输入映射/
  性能面板）；地图编辑器进阶、性能时间线、崩溃上报（✅ 已落盘）、日志汇聚 ⃝。

### 9.3 与 Godot 的关键差距（按 MMO 收益排序）

1. **渲染管线深度**（收益最高）：SSAO/SSR/体积雾已接入编辑器「后处理效果」面板并同步
   play；遗留：编辑器视口 `drawCalls=1`（多数 mesh 被视锥剔除/未入收集）→ SSAO 深度覆盖
   少、AO 效果不明显；静态网格合批（DrawMeshInstanced 合并 caster）待做。深度贴图 +
   后处理链基建（当前 HDR 目标深度是 RBO，shader 无法采样）是 GI/光照贴图的前置。
2. **编辑器打磨**：动画多轨时间线、UI 锚点/容器布局（boxflex 已落地，仍可深化）、
   检查器曲线编辑等深度工具。
3. **脚本体验**：JS 调试器、Lua 完整调用栈、悬停文档/补全、远程调试。
4. **音频/2D 纵深**：效果器（低通/混响）、流式背景音乐；2D 光照与 2D 物理工具。
5. **平台扩张**：WASM/WebGPU、macOS/Linux 实机验证、TLS/WebSocket 传输。
6. **场景级环境/后处理**：当前天空/雾/IBL/曝光是引擎硬编码；计划按 Godot 范式做
   **Environment 独立资源**（`environments/*.env.json`）+ **`WorldEnvironment` 组件实体**
   引用，渲染器读组件 → 加载环境资源 → 应用。**切勿自创**"场景根内联 environment block"
   方案；用资源 + 节点才能可复用、可多环境、为体积混合（PostProcessVolume）铺路。

### 9.4 建议顺序

1. 渲染后处理链基建 → SSAO 覆盖修复 → 体积雾 → GI；2. 编辑器体验迭代；3. 脚本工具链；
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
→ 全部 `[x]`。

### 10.2 B 系列 · 每帧性能热点（P1）

B1 uniform 无分层；B2 GL SetUniform 带 glGetError + string 分配；B3 无绘制队列/排序/合批；
B4 主场景深度不可采样 → SSAO 画两遍；B5 VK 目标切换 submit + 等 fence；B6 BuildDrawList O(N×M)；
B7 蒙皮矩阵每帧多遍 + HUD 逐顶点蒙皮；B8 Lua DebugHook 常驻税；B9 Lua-C++ 边界全量转换；
B10 每帧堆分配热点；B11 编辑器撤销触发全场景 JSON 往返；B12 UI 布局每帧全树重算 ×2；
B13 网络快照全量 + 48 实体上限 + 全走可靠通道；B14 物理同步无脏标记；B15 日志热路径开销。
→ B1/B2/B6/B8/B10/B11/B12/B13（快照分片）/B14/B15 `[x]`；B3/B4/B5/B7/B9 `[~]`。

### 10.3 C 系列 · 结构性重构（P1/P2）

C1 GameRuntime 上帝类拆分（已拆成编排器 + 16 个组合服务：projectile/hud/particle/tween/
lagcomp/status/prefab/plugin/sceneTree/scriptCanvas/ui/animation/scriptRuntime/btRuntime/
physicsBridge/drawSystem，各系统独立可测，`game_runtime.cpp` 2395→1178 行）；
C2 动画状态存 DrawItem → headless 空转（已解：动画状态移入 AnimationSystem 独立表，DrawItem
只留渲染引用）；C3 EditorApp 巨类（已面板插件化：**21 个面板**拆为独立 `IPanel` 类 +
`PanelRegistry` 注册表，`editor/src/panels/*`，可独立加载/卸载（Register/Unregister），
含视口与行为树编辑器（原「核心面板」，后继续拆出）；`editor.hpp` 1020→890 行，
剩余为门面职责——共享状态、相机/gizmo/undo/播放/插件转发、资产 VFS。插件动态注册的
面板（BuildPluginPanels，EditorPluginManager::Panels()）是独立机制，保留）；C4 Renderer 上帝类 + 无 render graph（已解：后处理全链改造为 Frostbite 式 FrameGraph
（18-pass，`neon/gfx/frame_graph.hpp` + `post_graph.hpp`，临时 RT 自动池 + 版本化读取），
Renderer 门面化拆出 ShadowSystem/SceneState/DrawBatch2D，`renderer.cpp` 2828→1174 行，
渲染逐像素等价）；
C5 字符串 key 贯穿全栈（无 intern/GUID）；C6 组件序列化三份手写镜像（反射系统 G2-1 收敛中）；
C7 脚本绑定手写 95 个 ×3 处（hook 化后脚本层零 scene 依赖）；C8 线程基建 3 份复制；
C9 UI 四轨并存；C10 着色器系统原始（全内嵌字符串）；C11 ECS Pool 防护；C12 CMake 单文件
（三层库拆分完成，未按模块 add_subdirectory）；C13 scene↔script 循环；C14 玩法混进引擎核心
（战斗/技能/状态已全部下沉 `Gameplay` 基础库 + Lua，引擎只留通用原语）；C15 editor/server/game 未库化（editor 已库化，server/game 未）。
→ C1/C2/C3/C4/C6/C7/C11/C13/C14 `[x]`；C12/C15 `[~]`；C5/C8/C9/C10 `[ ]`。

### 10.4 D 系列 · 安全与工程化

D1 编辑器插件无版本门 + 任意路径访问（版本门已做）；D2 插件 permissions 不强制；D3 网络
无认证/加密/防重放；D4 场景 JSON 不可 diff（已加缩进 + `%.17g`）；D5 CI 与构建工程化；
D6 编辑器功能洞（撤销/静默失败）；D7 服务器时基漂移。
→ D4 `[x]`；D1/D5/D6/D7 `[~]`；D2/D3 `[ ]`。

### 10.5 G 系列遗留（编号保留）

G1-1 渲染后端覆盖（D3D12/Metal 空白）；G2-1 反射系统（仅标量字段收敛中）；G2-2 ECS archetype；
G2-4 动态 GI（probe 场已落地，shader/DDGI 未做）；G2-5 Vulkan 自定义 shader 热重载；
G3-1 LLM 集成（远期）；G3-2 PCG 节点图；G3-3 视频编解码；G3-4 网络生产化（分区分服/
delta 编码/认证加密/断线重连/负载压测）；G3-5 UI（图标/链接/内嵌图片）；G4-1 原生插件
（ABI+加载器+物理/音频示范已落地，渲染运行时替换未做）；G5-1 运行时切换渲染后端；
G5-2 任务图调度（读写区域自动分析/lock-free 队列未做）；G5-3 确定性（跨平台 bit 一致 CI）；
G6-1 资产变体表（显存预算 API 未做）；G6-2 异步 obj/gltf（LOD 等级异步未做）；
G6-3 堆监控（relocating allocator 未做）；G7-2 shader IL 翻译层；G7-3 输入时序（触屏未做）；
G8-2 C++ 实时代码热替换；G8-4 增量打包（分布式农场未做）；G-收尾 GameRuntime 分解。
**已完成存档**：G1-2 动态 BVH、G1-3 场景树/变换缓存、G1-4 资源依赖图、G1-5 SSAO+体积光+
GPU 粒子、G2-3 地形 Layer Blend+chunked LOD+植被 Impostor、G3-4 服务器 lag comp、
G3-5 UI 九宫格+富文本、G4-1 原生 DLL/SO 插件、G5-2 任务图调度器、G7-1 VFS 全链路
（pack 直读 + Mod 挂载 + assets:/ scheme）、G7-3 输入时序绑定、G8-1 Profiler+崩溃报告、
G8-3 调试覆盖层、G8-4 增量打包、G6-1/6-2/6-3 平台/异步/堆监控、BC1 离线烘焙/检查器
schema/GameRuntime 接调度器。

### 10.6 优先级建议

1. A1–A13 正确性批量修复（地基，已完成）；2. B1–B4 渲染 CPU 管线治理；3. C1/C2 GameRuntime 拆分；
2. B8/B9/C7 脚本边界提速；5. B13 + D3 网络规模化与安全；6. B5–B15 → C3–C12 → D 系列 → G 系列远期。

---

## 11. 关联文档

- [`README.md`](./README.md) — 项目介绍 / 快速上手 / 构建（仓库根）
- [`TODO.md`](./TODO.md) — 缺陷与差距清单（`[ ]/[~]/[x]` 速查更新基线；详细项见 §10）
- [`plans/2026-08-31-architecture-review.md`](./plans/2026-08-31-architecture-review.md) —
  架构评审（分层 / 依赖方向 / 内聚耦合分析 + 分阶段改进计划）
- [`plans/2026-08-31-microkernel-design.md`](./plans/2026-08-31-microkernel-design.md) —
  微内核模块化设计（IModule / ServiceRegistry / 子系统模块 / 增量路径）
- [`plans/2026-08-31-combat-lua-design.md`](./plans/2026-08-31-combat-lua-design.md) /
  [`plans/2026-08-31-combat-lua.md`](./plans/2026-08-31-combat-lua.md) —
  战斗玩法下沉 Lua + Gameplay 基础库（设计 / 实现计划）

> `docs/` 归档为唯一主文档；各主题（渲染/Vulkan/插件/网络/路线图/代码规范）均已按章节并入，
> 不再保留独立副本。
