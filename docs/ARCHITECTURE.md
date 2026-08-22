# NeonEngine 架构说明

## 1. 设计目标

引擎面向**大型 3D 多人网络游戏**（类《魔兽世界》）。这决定了几个核心取舍：

- **大世界**：数千实体、分区加载、流式资源 → 需要 ECS 与可扩展的资源管线，而不是单例管理器堆砌。
- **多人在线**：客户端/服务器同构的代码组织，确定性、可序列化的状态 → 需要清晰的接口边界与数据驱动设计。
- **跨平台**：Windows / macOS / Linux 一套代码 → 平台层与渲染层必须可插拔。
- **工程化**：模块边界、单元测试、CI、文档、代码规范优先于堆功能。

## 2. 分层总览

```
┌────────────────────────────────────────────────────────┐
│ game        NeonRealm demo（玩法系统、场景、HUD、存档）    │
├────────────────────────────────────────────────────────┤
│ engine::core   平台无关核心                              │
│   应用循环/时间/日志/配置/RNG  ECS  SceneManager         │
├────────────────────────────────────────────────────────┤
│ engine::interface   纯抽象接口（无平台头文件）             │
│   IWindow  IInput  IRenderBackend  IAudioBackend        │
├──────────────┬──────────────┬──────────────┬────────────┤
│ platform     │ gfx          │ audio        │ assets     │
│ Win32 ✅     │ OpenGL ✅    │ miniaudio ✅ │ stb 系     │
│ X11   ⃝      │ Vulkan ⃝     │ WinMM  ✅    │ OBJ 解析器  │
│ Cocoa  ⃝     │              │ Null   ✅    │ 程序化生成  │
└──────────────┴──────────────┴──────────────┴────────────┘
```

✅ = 本机（Windows + Intel GL 4.6）实测；⃝ = 代码就绪，CI 编译验证，未实机运行。

**依赖规则**：

1. `game` 只能依赖 `engine::` 公共头文件，禁止直接包含平台/GL 头文件。
2. `engine::core` 不依赖任何平台实现与第三方库。
3. 后端（`platform::*`、`gfx::gl`、`gfx::vulkan`、`audio::miniaudio`/`audio::winmm`）实现接口，且只在对应平台编译。
4. 高层（Renderer/Material/Mesh）只通过 `IRenderBackend` 触达 GPU。

## 3. 模块说明

### 3.1 math（`neon::math`）

全部 header-only、零依赖。统一使用**行主序** `Mat4`（`m[row*4+col]`），包含：

- `Vec2/Vec3/Vec4`、`Mat4`（正交/透视/旋转/平移/缩放）、`Quat`、`Transform`
- `Rect2`、`AABB`、`Ray` + 相交测试（射线-AABB/球）

> 约定说明：矩阵在内存中按行主序存储；传给 OpenGL uniform 时后端以 `transpose=GL_TRUE` 提交。所有构造器、乘法、变换函数都必须遵守同一约定（本仓库曾在这一点上混用过列主序，测试 `Mat4Ortho/Perspective` 防回归）。

### 3.2 core（`neon::core`）

- `Application`：拥有窗口/输入，运行固定步长 60Hz 循环（累加器模式，防螺旋死亡），支持冒烟测试帧数。
- `Time`、`Log`（分级、带时间戳）、`Config`（key=value 存档）、`Rng`（xorshift64*，确定性）。

### 3.3 ecs（`neon::ecs`）

SparseSet 风格的 ECS：

- `Entity` = 32 位 id + 32 位 generation（句柄，防悬垂引用）。
- `World::Pool<T>`：dense 数组 + sparse 索引，swap-erase 删除；`View<T>` 顺序遍历。
- **批量迭代（T5.5）**：`View<T>::ForEach` / `View<T,U>::ForEach`（两组件视图，只访问同时持有 T+U 的实体）提供缓存友好的批量遍历；`ParallelForEach` 把 dense 区间切成固定连续 chunk 交给 worker 线程，对“只碰自己那条目”的独立工作负载，结果与串行路径**逐位一致**且跨运行一致。
- **确定性并行 job（`neon::ecs::parallel`）**：`parallel::ParallelFor(count, fn)` 固定切分 + 持久线程池（本工具链无 `std::thread`，用 Win32 `CreateThread` / POSIX pthread，同 async_loader 模式）；无 worker 时自动回退串行。`parallel::Reducer<T>` 提供按 chunk 槽位的归约助手。
- **并行契约**：`ParallelForEach` 期间禁止 `Create/Destroy/Add/Remove`（debug 构建 assert 触发）；需要改世界的系统必须先收集变更、并行阶段结束后再应用。demo 系统保持串行，并行 API 供未来系统与无头服务器（T6）使用。
- `System`：`Update(dt, World&)` 接口；demo 的玩法逻辑按系统组织。

当前实现适合数千实体；大规模 MMO 需要的 archetype 存储（跨组件缓存友好布局）留作后续重构——批量迭代 API（`ForEach`/`ParallelForEach`/`View<T,U>`）已就位，届时只需替换存储后端。

### 3.4 platform（`neon::platform`）

- `IWindow`：创建/事件泵/交换缓冲/GL 上下文/鼠标捕获。每平台一个实现：
  - **Win32**：`CreateWindowEx` + WGL。上下文创建带降级链：请求版本 → 3.3 core → 2.1 legacy。DPIAware 动态加载。
  - **X11**：`XCreateWindow` + GLX，`WM_DELETE_WINDOW` 关窗，捕获时 `XWarpPointer`。
  - **Cocoa**：`NSWindow` + `NSOpenGLView`（ObjC++），事件经 `nextEventMatchingMask` 泵出。
- `IInput`：引擎侧状态机（当前帧/上一帧按键、鼠标、滚轮、相对位移），一套实现服务所有平台。平台只负责把原生事件翻译成 `InputEvent`。

### 3.5 gfx

#### IRenderBackend（底层）

资源：`ShaderHandle/TextureHandle/MeshHandle`（VAO）；状态：混合/深度/剔除/视口/清屏；绘制：`DrawMesh` + 立即模式 `DrawPrimitives`；uniform 按名称惰性缓存位置。**渲染器与游戏完全不感知具体 API。**

#### OpenGL 后端

- **自研 GL 加载器**（`gl_loader`）：函数指针表 + 平台加载路径（Windows：`wglGetProcAddress` → `opengl32.dll` 导出回退；Linux：`glXGetProcAddressARB`；macOS：符号直链）。不依赖 GLEW/glad。
- 纹理走 `glTexStorage2D + glTexSubImage2D`（避免旧版 `glTexImage2D` 桩函数在核心上下文上的驱动兼容问题，实测 Intel 驱动）。
- **上下文深度校验**：部分驱动（含本次实测的 Intel 驱动）会对 attrib 创建的上下文报告无深度缓冲，或深度缓冲清屏后恒为 0。启动自检检测 `GL_DEPTH_BITS` 与“清除→回读”一致性；不可用时渲染器自动关闭深度测试，游戏场景按画家算法（远→近排序）绘制。
- 统一批量 2D 顶点缓冲（UI/文字/粒子公告板共用一条 draw call 路径）。
- 网格实例化（`DrawMeshInstanced`，每实例模型矩阵经 CPU 视锥剔除后上传）、顶点色管线（OBJ 的 MTL Kd 颜色直接进入顶点色）。
- **PBR 材质**：Cook-Torrance BRDF（GGX 分布 + Schlick-GGX 几何 + Schlick 菲涅尔），支持金属度/粗糙度（可来自金属-粗糙度贴图，G=roughness、B=metallic）、AO、自发光；点光/方向光共用 BRDF。
- **投影阴影**：CPU 端把物体顶点沿光方向投影到地面平面（y=0），用立即模式三角形批量半透明绘制（接触阴影/AO）。不依赖深度缓冲与 FBO，兼容深度损坏的驱动。
- **glTF 2.0 导入器**：自研 JSON DOM 解析器（`core::Json`，递归下降、UTF-8、转义/代理对）；bufferView/accessor 读取（支持 byteStride 与 16/32 位索引）；PBR 材质映射；节点变换（TRS/矩阵）层级。

#### Renderer（高层）

- 相机（透视 lookAt）、方向光 + 8 点光 + 玩家手电光、距离雾、天空渐变。
- 内置 Shader：lit（Blinn-Phong）、unlit、UI、line。
- **HDR + Bloom 后处理（T3.6）**：主场景渲染进窗口尺寸的 RGBA16F 离屏目标（`CreateRenderTarget(w,h,true)` 新增半浮点变体），`EndFrame` 先跑明亮度阈值（`max(color-1.0,0)`）→ 1/2、1/4 两级 5 tap 可分离高斯模糊金字塔 → 逐级上采样累加（渐进 bloom）→ 合成到后备缓冲，最后再绘制 2D/HUD 覆盖层（UI 不被 bloom）。截图路径（`CaptureFrame`）先触发合成再 `glReadPixels`，读到的就是最终合成图像；`--bloom-compare <off> <on> <frame>` 在同一帧同一 HDR 目标上分别输出关/开 bloom 两张图用于验证。浮点目标能力启动自检（FBO 写入 + 字节回读），失败自动回退到原直绘后备缓冲流程；`--no-bloom` / `NEON_NO_BLOOM` 只关 bloom 项（HDR 路径保留）。
- **色调映射 + MSAA（T3.7）**：合成 Shader 用 ACES 拟合曲线（Narkowicz）`clamp((x*(2.51x+0.03))/(x*(2.43x+0.59)+0.14),0,1)` 替换 T3.6 的 `min(c,1)` 钳制，作用在 `hdr + bloom*strength` 上并乘以曝光 `uExposure`（默认 1.0，`SetExposure`/`--exposure` 可调）；`--no-tonemap` 保留旧钳制分支，`--tonemap-compare <clamped> <aces> <frame>` 同帧输出两张图做差异验证。MSAA：`CreateRenderTarget(w,h,floatColor,samples)` 新增采样数参数，samples>0 时颜色+深度变为多重采样 renderbuffer（无采样纹理），`ResolveRenderTarget(src,dst)` 用 `glBlitFramebuffer` 解析到单采样目标；主场景渲染进 4x（失败回退 2x/单采样）MSAA HDR 目标，`CompositeSceneToBackbuffer` 先解析到单采样 HDR 目标再跑 bloom/合成。MSAA 能力自检（多重采样 FBO 完整性 + blit 回读）启动时执行，失败自动回退并记录日志；`--no-msaa` 强制单采样用于差异对比。阴影 FBO（CSM/点光）保持 RGBA8 单采样颜色编码深度不变，bloom 金字塔与后备缓冲 UI 均为单采样。
- 2D 覆盖层使用设计分辨率 1280×720 等比缩放居中，输入坐标通过 `ScreenToUI` 换算。
- 调试线框（`DrawBox/DrawSphere/DrawLines`）、截图（`glReadPixels`）。
- 渲染统计（绘制调用/三角形/实例数），HUD 实时显示。

### 3.6 audio

`IAudioBackend` 接口；首选用 miniaudio（T5.1，vendored 单头，MIT-0/公有领域）——WASAPI/DirectSound/CoreAudio/ALSA，开设备失败（无音频设备的 CI 等）自动回退 WinMM 软件混音器（Windows，后台线程 + 4 缓冲 + 临界区）或 Null 后端（静音可用）。三平台统一，音效/音乐由 demo 程序化合成（PCM），零音频资产。混音器抽成纯函数（`neon/audio/mixer.hpp`）可无头测试。`NEON_NO_MINIAUDIO` 环境变量可强制回退（测试钩子）。

### 3.7 physics

内置轻量物理：动态球体 vs 静态 AABB、重力、地面、球-球分离、碰撞事件对、射线检测。接口（`physics::World`）与实体解耦（owner 用 64 位实体编码），后续可替换 Jolt/Bullet 而不影响游戏层。

### 3.8 assets / ui / scene

- `assets::AssetManager`：按路径缓存的贴图（stb_image）、OBJ 网格、字体加载；程序化资产不经过磁盘。
- OBJ 解析支持 MTL `Kd` 材质色分组与平坦法线回退；系统 CJK 字体加载（TTC 集合支持）。
- `ui`：两套 API。
  - **立即模式**（`ui/ui.hpp`）：`DrawLabel/DrawPanel/DrawButton/DrawBar`，输入直接查询 `IInput`，适合 HUD 与快速原型。
  - **控件树**（`ui/system.hpp`）：`UIManager` + `Element` 层级。每个控件有 `rect`（**相对父节点**）、`Measure/Layout/Draw` 三阶段，`AbsolutePos()` 累加祖先矩形得到屏幕坐标。内置 `Panel/Label/Button/TextField/Slider/CheckBox/VBox/HBox/Window/ScrollArea/List`，支持命中测试（逐层坐标变换）、焦点、拖拽、滚轮冒泡、按键与 UTF-8 文本输入。
  - 布局约定：容器（`VBox/HBox/Window`）以**相对坐标**给子节点分配矩形；显式设置了 `rect` 的节点保持自己的位置，未设置（w/h=0）的节点继承父级区域。拖动窗口时子控件跟随移动。
  - 程序化访问：`UIManager::Find(name)` 按名称查找控件（编辑器工具栏按钮以文本命名），`HitTestAt(pos)` 做设计坐标命中测试；UI 冒烟测试依赖这两个接口。
- `scene::SceneManager`：场景切换（进入/退出/更新/绘制/事件），切换延迟到帧边界生效。

### 3.9 editor（场景编辑器）

`editor` 是引擎第一个工具型应用（`neon_editor.exe`）。**工具 UI 使用 Dear ImGui（docking 分支）**（行业惯例：编辑器/调试工具用 ImGui，游戏内 HUD 用自研控件树），内置"引擎 UI 演示"窗口对照验证自研控件树：

- **停靠布局**：主 DockSpace + `DockBuilder` 默认布局（首次运行，ini 恢复用户布局）。五个可停靠面板 + 视口：
  - **场景**：实体列表（名称/类型）、添加/复制/删除/上移/下移，选中同步到属性与视口包围盒。
  - **资产**：跨平台目录浏览（Win32 `FindFirstFileW` / POSIX `opendir`，UTF-8 路径），双击目录进入、双击模型导入场景、双击图片预览（`ImGui::Image` 注册引擎纹理）、面包屑/快捷目录。
  - **资源**：AssetManager 缓存统计（纹理数量/内存、网格/三角形、字体），按纹理/网格/字体分页签列出已加载资源。
  - **属性**：名称/类型/位置/旋转/缩放/颜色 + 材质金属度/粗糙度 + 网格信息（三角形/包围盒）。
  - **日志**：引擎日志环形缓冲（`core::GetRecentLogs`），分级过滤（全部/INFO+/WARN+/ERROR）、自动滚动、清空。
  - **视口**：NoInputs 停靠窗口，不拦截 3D 输入；右键旋转、中键平移、滚轮缩放、射线-AABB 拾取；`WantCaptureMouse` 为真时让出输入。
- **资产导入**：任意 `.obj`/`.gltf` 可作为实体导入（`meshKey = "obj:<path>" / "gltf:<path>"`），随场景 JSON 持久化并按路径重解析。
- **场景数据**：`core::Json` + `JsonWriter` 保存/加载 `editor_scene.json`（含位置/缩放/颜色/金属度/粗糙度）。
- **验证**：`--smoke-test <帧>` 自动运行 UI 冒烟测试（ImGui 上下文/图集/绘制数据、自研控件命中/点击/选择、日志缓冲、资产目录、资源统计、glTF 导入），任一检查失败退出码为 1。

#### Dear ImGui 集成（`neon_imgui` 模块）

- 静态库：imgui 核心 4 文件 + `imgui_demo.cpp` + 自研后端 `gfx/imgui_neon.cpp`；仅 `neon_editor` 链接，运行时游戏不依赖。
- **渲染后端**：ImDrawData → `IRenderBackend::DrawPrimitives`（顶点格式 32B：pos2/uv2/rgba4 与引擎 UI 一致）；逐命令裁剪（SetScissor）、绑纹理、正交 y-down 投影。
- **输入**：引擎 `IInput` 状态机 → `io.AddMouse*Event`/`AddKeyEvent`/`AddInputCharactersUTF8`；`WantCaptureMouse/Keyboard` 与自研 UI、3D 相机路由。
- **字体**：系统 CJK 字体（Windows msyh.ttc / macOS PingFang / Linux Noto），legacy 图集路径一次性烘焙全量字形（约 2048×4096，启动 1-2 秒），避免 1.92+ 动态图集（WantUpdates）在自研后端上的兼容问题。
- **混合模式**：自研 UI 着色器输出非预乘颜色，ImGui 绘制必须用 `BlendMode::Alpha`（SRC_ALPHA）而非官方后端的预乘混合——否则字形四边形的 RGB 全强度叠加，中文会变成实心白块（已踩坑记录）。
- **停靠支持**：使用官方 **docking 分支**（master 分支不含停靠 API）；`io.ConfigFlags |= DockingEnable`，DockBuilder 仅在新 ini 时设置默认布局，用户调整结果写入 `neon_editor_imgui.ini`。

## 4. 数据流（一帧）

```
PumpEvents → 平台事件 → IInput::HandleEvent
                    ↓
固定步长累加器：OnUpdate(dt) 60 次/秒
  ├─ PlayerSystem（输入→移动/冲刺/攻击）
  ├─ EnemySystem（AI/追击/近战）
  ├─ PickupSystem / WaveSystem
  └─ physics::World::Step
OnRender：
  ├─ Renderer::BeginFrame（清屏）
  ├─ DrawSky（渐变）→ 全部 3D 绘制进入 HDR 浮点离屏目标
  ├─ Scene::Draw → SetCamera → DrawMesh（lit/unlit）
  ├─ 粒子公告板 / 调试线框
  ├─ HUD（2D 批量 + 字体图集，先入批量缓冲）
  └─ Renderer::EndFrame
      ├─ Bloom：bright→模糊金字塔→上采样累加（HDR 目标上）
      ├─ 合成 min(hdr+bloom, 1) → 后备缓冲
      ├─ 刷新 HUD 批量到后备缓冲
      └─ SwapBuffers
```

## 5. 关键设计决策记录

| 决策 | 理由 |
| --- | --- |
| 自研 GL 加载器 | 无第三方依赖、可审计、跨平台加载路径统一；项目规模可控 |
| 行主序矩阵 + transpose=GL_TRUE | 引擎数学与 shader 提交之间单一约定，测试防回归 |
| ECS 而非 GameObject 树 | 面向大量实体的缓存友好迭代与后续并行化；MMO 规模必须 |
| 固定步长 + 累加器 | 物理与逻辑确定性；渲染帧率独立 |
| 程序化资产（贴图/音效/字体内嵌） | 仓库零二进制资产、三平台表现一致、演示资源管线扩展点 |
| 深度不可用降级为画家算法 | 实测 Intel 驱动深度缓冲损坏；引擎自适应，普通机器仍用深度测试 |
| 字体灰度打包后展开 RGBA | stb 过采样预滤波要求 stride==宽度，RGBA 直写会越界 |
| UI 控件树使用相对坐标 + AbsolutePos 累加 | 子节点布局与父节点（窗口拖拽/移动）解耦；命中测试逐层换算 |
| CJK 字形由 cjkSamples 显式收集 | 系统字体图集按码点烘焙，UI 新增中文必须同步加入采样串（demo/editor 各自维护） |
| 投影阴影而非阴影贴图 | 实测该 Intel 驱动 FBO 的 VAO 渲染损坏（Clear 可写、DrawElements 不写）；CPU 投影零依赖、跨平台稳定 |
| HDR 离屏 RGBA16F + 渐进 bloom | 主场景保留超 1.0 的亮部，后处理金字塔模糊后加回；浮点目标能力自检失败自动回退直绘后备缓冲 |
| 截图先合成再回读 | `CaptureFrame` 在 HDR 目标上触发 bloom+合成到后备缓冲后 `glReadPixels`，截图即最终画面；`--bloom-compare` 同帧同 HDR 目标输出关/开对比 |
| glTF/JSON 自研 | 控制解析细节（byteStride、代理对、UTF-8），无第三方 JSON 依赖 |
| 平台后端按 OS 编译 | X11/Cocoa 无法在 Windows 验证，CI 矩阵分别编译 |
| miniaudio 音频 | vendored 单头（v0.11.25），三平台统一；Windows 回退 WinMM、其余回退 Null；`MA_NO_NULL` 让无设备时如实报不可用 |

## 6. 扩展点

- **Vulkan 后端**：实现 `IRenderBackend` 即可，接口已冻结；步骤见 `docs/VULKAN_ROADMAP.md`。
- **物理引擎替换**：保留 `physics::World` 外观，内部接 Jolt/Bullet。
- **音频后端**：miniaudio 已 vendored 且三平台编译，Windows 已启用；Linux/macOS 可逐步把默认选择切到 miniaudio（当前保留 Null 兜底，CI 无音频设备也能跑）。
- **资源流式加载**：`AssetManager` 增加异步/分块读取，供大世界分区使用。
- **服务器复用**：玩法系统只依赖 `ecs::World` 与输入抽象，可编译为无渲染的服务器目标。
