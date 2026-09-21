# NeonEngine 待办与优化方向（TODO）

> 唯一待办事实源。状态：`[ ]` 待办 · `[~]` 部分完成。
> **已完成项不再逐条保留**（一行式存档见文末；完整历史在 git log）。
> 修复/落地时请带上验证方式（单测名 / 冒烟命令 / 截图）。

---

## 一、接下来的优化方向（2026-09-21 重新评估）

### 0. 先决条件：把 GPU 画面验证做成可回归
近两次渲染改动（V1 上屏雾遮罩、B4 深度复用、编辑器 play 世界构建）都因**只能靠肉眼截图**而反复卡住/无法判定。
优先补一个最小「GPU 像素验证基线」，否则下面所有渲染项都难以收口：

- 新增 `--capture <png> <frame>`（已有截图）+ `tools/pixel_diff`（同场景两次运行/黄金图逐像素比对，容差 + 差异占比）。
- 冒烟脚本把「截图 + 阈值」纳入 CI；关键 pass（雾/SSAO/环境/后处理）各留一张黄金图。
- 收益：B3/B4/C4/C10/G2-4 全部可自动验收。**建议第一件做这个。**

### P0 · 渲染管线深化（画面收益最高，做完 0 后不再阻塞）
1. **B4 收尾：SSAO 复用主深度**。`IRenderBackend::ResolveDepth` 已落地；剩下 MSAA 深度 resolve→
   SSAO 采样深度、去掉全量 caster 重画（几何 pass 减半）。带驱动自检回退旧的颜色编码路径。
2. **C4 Renderer 拆分 + render graph**：`renderer.cpp` ~2800 行、17 个 RT 手工生命周期、三份近似 caster
   提交（CSM/SSAO 深度/点光）。先抽 caster 提交 helper + pass 描述 + RT 自动生命周期。
3. **C10 shader 资产化**：全部内嵌 C++ 字符串 → 源文件 + 产物缓存 + 变体；VK 端 `CreateShader`
   按 debugName 查表导致**自定义 shader 在 VK 静默无效**（同一接口两种语义）。
4. **B3 透明/不透明排序 + 静态合批**：opaque 前到后（early-z）、透明按距离；同材质/网格合并 caster。
5. **G2-4 GI/DDGI**：探针场 + atlas 已有；接 shader 间接光 / 动态更新（并入 C4 的 pass 体系）。
6. **B7 收尾**：pose 缓存已做（主+HUD）；阴影/SSAO 两遍仍各自 `BindPose→Sample→ComputeBoneMatrices`，
   让各 pass 复用同一缓存。

### P1 · 编辑器产能（内容创作杠杆）
- **D6 undo 覆盖洞**：灯光/相机参数、地形笔刷、UI 文档不入撤销栈（`editor_scene.cpp`、`panels.cpp`、
  `editor_viewport.cpp`）。
- **动画多轨时间线**（动画/事件/混合的可视化编辑）。
- **profiler 时间线 + GPU 捕获**（当前仅环形计时）。
- **C3 续拆**：gizmo/BT/UI-editor 面板状态（PanelDef 已铺路）。

### P1 · 脚本工具链
- **C7 注册表驱动绑定**：90+ 手写 native ×3 处（名字/参数 schema/错误上报）→ 单表驱动；键名表单一来源。
- **JS 调试器**、Lua 完整调用栈、悬停文档/补全、远程调试（§9.3）。
- Python 宿主运行时绑定（CPython C-API + 沙箱，`NEON_ENABLE_PYTHON` 门控）。

### P1/P2 · 网络规模化与安全（本引擎差异化赛道，长期投入）
- **B13 后半**：快照 delta/量化；突破 **48 实体/帧**上限（分片已做，量化未做）。
- **D3**：session token + 管理命令鉴权 + HMAC 防重放；`PumpNetwork` 每帧收包上限。
- **G3-4**：分区分服（world/instance）、断线重连/会话恢复、服务器负载压测基线。

### P2 · ECS / 并行
- **G2-2** archetype 存储（调度器已落地，接口已预留）。
- **G5-2** job 读写区域自动分析 / lock-free 队列。
- **C8** 线程基建三份复制（parallel/async_loader/log）收口到任务图。

### P2 · 动画 / UI / 音频 / 2D
- **C2** 动画状态从 DrawItem 迁到 ECS 组件（服务器 `draws_` 空导致动画空转、客户端/服务器分叉）。
- 动画重定向、>64 骨骼上限、BlendSpace 相位漂移（`B3b` 已落地 BlendSpace1D）。
- **C9** UI 四轨并存收敛（控件树/文档 UI/立即模式/ImGui）；TextField 光标/选择（IME 远期）；主题/文本测量单一来源。
- 音频：流式 BGM、效果器（低通/混响）、3D 追踪；2D 光照/2D 物理工具。
- **G3-5** UI 图标/链接/内嵌图片。

### P2 · 平台 / 工程化
- 平台：macOS/Linux **实机**验证、WASM/WebGPU、TLS/WebSocket。
- **D5** CI：Vulkan 进 CI、MinGW/sanitizer 跨平台、server/game 联机与打包 e2e。
- **C12** 按模块 `add_subdirectory` + PCH；**C1/C14/C15** GameRuntime 余下簇拆分、server/game 库化。
- **C5** 字符串 key 句柄化（热路径先行：GameVars/冷却表；资源 GUID 贯通列大项）。
- **C6**/反射：字段级 codec（条件省略/校验交给 `kFields`）。

### 明确搁置（低优先，除非需求变化）
- G1-1 D3D12/Metal 后端（保留注册机制即可）、G5-1 运行时切后端（依赖 D3D12）。
- G3-1 LLM 集成、G3-2 PCG 节点图、G3-3 视频编解码、G8-2 C++ 热替换（原生插件重载足够）、
  G8-4 分布式打包农场。
- D2 插件 permissions 强制（安全基线，非当前痛点）。

---

## 二、与主流引擎差距速查（2026-09-21 更新）

| 领域 | 差距 | 对标要点 | 对应项 |
|---|---|---|---|
| 渲染架构 | 大→中 | RenderStack 数据驱动 + **Environment 资源** + 深度 resolve 已落地；缺 FrameGraph/shader 资产化/合批/GI | B3/B4/C4/C10, G2-4 |
| Vulkan 成熟度 | 中→大 | descriptor/内存子分配/串行提交/伪 HDR/自定义 shader 语义 | A1/A2/B5/C10 |
| 物理 | 中 | **触发器/角色/形状查询/旋转/CCD/关节/可配置池+多线程**已补；缺胶囊/凸包、约束限位/马达、去隐式地面 | Jolt 封装扩展 |
| 动画 | 中 | ASM + BlendSpace1D + 事件已做；缺重定向、>64 骨骼、相位漂移、状态组件化 | C2, B3b 收尾 |
| 网络 | 强项 + 硬上限 | 确定性+lag comp+AOI+分片强；缺 delta/量化、48 实体上限、认证/加密、重连、分区分服 | B13/D3/G3-4 |
| ECS/并行 | 中 | 调度器+任务图；缺 archetype、job 依赖分析 | G2-2/G5-2/C8 |
| 资产管线 | 中 | GUID 库/VFS/Mod/变体/异步/LOD 已做；缺 USD、GUID 贯通场景引用、显存预算 | C5/G6-1/G6-2 |
| UI | 中 | 三轨+9-slice+富文本；缺锚点/容器深化、TextField 可用 | C9/G3-5 |
| 脚本工具链 | 中→小 | 反射字段访问/多宿主/Lua 预算已做；缺注册表驱动、JS 调试器、补全 | C7/E |
| 音频 | 中 | 一次性播放；缺流式、效果器、3D 追踪 | 音频项 |
| 编辑器 | 中 | 面板拆分/UI 编辑器/打包已做；缺 undo 覆盖、时间线、profiler 时间线/GPU 捕获 | C3/D6 |
| 平台 | 大 | Windows 实机；mac/X11 未实机、无 Web/移动 | 平台项 |

---

## 三、已完成存档（一行式；详情见 git log）

- **正确性 A1–A13**：Vulkan descriptor 泄漏/真 HDR+采样自检/动态纹理、Json UB+精度、ParallelFor 异常、
  Lua 预算、脚本 vars 隔离、Jolt 碰撞快照、SetPosition 物理回写 + Raycast 结果、资源变体释放/热重载 retire、
  客户端 Ping、world.hash 字符串化、OBJ 索引上限、glTF 多 buffer。
- **每帧性能 B**：B1 uniform 分层、B2 SetUniform 无分配、B6 BuildDrawList 增量、B8 Lua 调试钩子按需、
  B9 Lua 边界 fast path + native 指针缓存、B10 每帧堆分配、B11 撤销命令、B12 UI 布局 dirty、
  B13 快照分片、B14 物理脏标记、B15 日志热路径。
- **结构 C**：C1 拆 content/combat/draw、C6 序列化收敛、C7 反射字段访问、C11 ECS Pool 防护、C13 scene↔script 解耦。
- **工程 D**：D4 JSON 缩进、D7 服务器时基。
- **G 系列历史**：G1-2 动态 BVH、G1-3 场景树、G1-4 资源依赖图、G1-5 SSAO+体积光+GPU 粒子、
  G2-1 反射体系、G2-3 地形 splat+chunked LOD+植被、G3-4 lag comp、G3-5 9-slice+富文本、
  G4-1 原生插件、G5-2 任务图、G7-1 VFS、G7-3 输入时序、G8-1 profiler+崩溃落盘、G8-3 调试覆盖层、
  G8-4 增量打包、G6-1/2/3 变体表/异步网格/堆监控、BC1 离线烘焙。
- **视觉批**：颜色分级、法线贴图（导数 TBN）、半球光 + 光探针场 GI、程序化天空盒（太阳/月亮/FBM 云）、
  自动曝光 + 暗角、RenderStack 运行时接线。
- **玩法/工具批**：NavGrid 寻路、DataTable、动画事件、BlendSpace1D 运行时。
- **2026-09-20/21 引擎优化批**：
  - 战斗空间索引 BVH（重叠查询 ~22×）、静态 glTF 节点空间切块（三角形 −56%）、多节点视锥剔除。
  - 脚本可用 GPU 地面贴花（`SpawnDecal/SetDecal`）、`DrawTrail` 拖尾、蒙皮姿势缓存。
  - 雾：网格+LOS 顶点 alpha 柔化、`BuildMask`、绘制 scratch 复用。
  - **物理封装扩展**：触发器（含 `CharacterVirtual`）、`SphereCast`/`OverlapSphere`/`OverlapBox`、
    旋转 get/set、CCD、固定/铰链/距离关节、可配置池 + 可选多线程、Lua 绑定、
    `projects/physics_sandbox` 沙盒 demo+回归场景。
  - **Environment 资源**（`environments/*.env.json`）+ 场景引用（单一读取器消除镜像漂移）。
  - `IRenderBackend::ResolveDepth`（B4 前置）。

---

## 四、编号索引（保持跨文档引用有效）

> 其他文档（DEVELOPMENT.md / reflection.md / plans/*）按下述编号引用；此处给一行式状态，
> 详细正文已随完成删除（见 git 历史）。`✔` 完成 · `~` 部分完成 · `·` 待办 · `-` 搁置。

**A 正确性**：A1 ✔ VK descriptor 泄漏 · A2 ✔ VK 伪 HDR + 动态纹理更新 · A3 ✔ Json UB/健壮性 ·
A4 ✔ ParallelFor 异常安全 · A5 ✔ Lua 失控保护 · A6 ✔ per-entity vars 命名空间 · A7 ✔ Jolt Collisions 有界 ·
A8 ✔ SetPosition 回写物理 + Raycast 结果 · A9 ✔ 资源引用计数 key/热重载 UAF · A10 ✔ 客户端 Ping ·
A11 ✔ world.hash 精度 · A12 ✔ JsonWriter 精度 · A13 ✔ OBJ/glTF 导入损坏。

**B 每帧性能**：B1 ✔ uniform 分层 · B2 ✔ SetUniform 无分配 · B3 ~ 绘制队列/排序/合批 ·
B4 ~ 主深度可采样（`ResolveDepth` 已落地，SSAO 接线待做）· B5 ~ VK 提交合并 ·
B6 ✔ BuildDrawList 增量 · B7 ~ 蒙皮多遍重算（pose 缓存已做；阴影/SSAO 复用待做）· B8 ✔ Lua 调试钩子按需 ·
B9 ✔ Lua 边界 fast path + native 指针缓存 · B10 ✔ 每帧堆分配 · B11 ✔ 编辑器撤销 JSON 往返 ·
B12 ✔ UI 布局 dirty · B13 ~ 网络快照 delta/量化 + 48 实体上限（分片已做）· B14 ✔ 物理脏标记 · B15 ✔ 日志热路径。

**C 结构**：C1 ~ GameRuntime 拆分（content/combat/draw 已拆）· C2 · 动画状态出 DrawItem ·
C3 ~ EditorApp/panels 拆分 · C4 · Renderer 拆分 + render graph · C5 · 字符串 key 句柄化 ·
C6 ~ 序列化收敛（字段级 codec 待做）· C7 ~ 脚本绑定注册表驱动（反射字段访问已做）· C8 · 线程基建收口 ·
C9 · UI 四轨收敛 · C10 · shader 资产化/变体 · C11 ✔ ECS Pool 防护 · C12 ~ CMake 按模块 ·
C13 ✔ scene↔script 解耦 · C14 ~ 玩法下沉（并入 C1）· C15 ~ server/game 库化（editor 已库化）。

**D 安全/工程化**：D1 ~ 插件版本门/路径 · D2 · 插件 permissions 强制 · D3 · 网络认证/加密/防重放 ·
D4 ✔ 场景 JSON 缩进 · D5 ~ CI（Vulkan/MinGW/sanitizer/server e2e）· D6 ~ 编辑器 undo/静默失败 · D7 ✔ 服务器时基。

**G 遗留**：G1-1 - D3D12/Metal · G2-1 ✔ 反射体系 · G2-2 · ECS archetype · G2-2 ~ 组件类型注册 ·
G2-4 ~ 动态 GI · G2-5 · VK 自定义 shader 热重载（并入 C10）· G3-1 - LLM · G3-2 - PCG · G3-3 - 视频 ·
G3-4 ~ 网络栈（分区分服/重连/压测）· G3-5 ~ UI 控件 · G4-1 ~ 原生插件 · G5-1 - 运行时切后端 ·
G5-2 ~ 任务图 · G5-3 ~ 确定性（跨平台 bit 一致 CI）· G6-1 ~ 变体表/显存预算 · G6-2 ~ 异步加载/LOD 默认开 ·
G6-3 ~ 堆监控/relocating 分配器 · G7-2 · shader IL 层（并入 C10）· G7-3 ~ 输入时序（触屏）·
G8-2 - C++ 热替换 · G8-4 ~ 增量打包（分布式搁置）· G-收尾 ~ GameRuntime 分解里程碑。
