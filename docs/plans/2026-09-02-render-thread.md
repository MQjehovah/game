# 渲染线程化设计（Render Thread Architecture）

日期：2026-09-02
状态：阶段 1 已完成（commit a5597d0），阶段 4 帧间重叠待实施
前置：共享 GL 上下文 + UploadThread 已完成（commit e796229）；平台线程原语 threading.hpp 已完成（7360c19）。IRenderBackend 立即式接口、主线程独占 GL。

## 0. 实施进度

- ✅ **阶段 1-3（commit a5597d0）**：`ThreadedBackend : IRenderBackend` 全方法命令化。
  ThreadedBackend 把每个 backend 调用录制为值语义闭包（数组/像素参数拷贝），由专用渲染
  线程持窗口 GL 上下文按序回放；无句柄方法 Enqueue 异步，句柄/查询/回读方法 Run 同步往返
  （reply 信号量），EndFrame frame-lock（swap 后返回）。因 ImGui / 编辑器 / GameRuntime 全部
  经 IRenderBackend，它们自动被命令化（阶段 3 无额外工作）。`--render-thread` 开关（默认关，
  现状不变）。
- ✅ **等价验证**：forest（780万顶点）+ Sponza（103 mesh）渲染线程 vs 主线程截图 SHA-256
  完全一致；play 模式正常；300 帧无 GL error；777 单测全绿；冒烟 fail-delta=0。
- ⏳ **阶段 4（帧间重叠）**：去掉 EndFrame frame-lock，让主线程不等 swap（PumpEvents/UI 不被
  vsync/渲染拖住）。卡点：PostGraph 的 RT 池跨帧复用（ResetFrame 在主线程逻辑、渲染线程可能
  还在用上一帧 RT）——需要 RT 生命周期按帧分代/双缓冲，且渲染慢于录制时需限帧背压。这是
  收益与边界都明确的独立下一步。

## 1. 目标

把「立即式主线程渲染」演进为业界标准的「主线程录制命令 → 专用渲染线程执行」（Godot 4 RenderingServer / Unreal Game→Render→RHI 的架构方向），使：
- vsync SwapBuffers 不再阻塞逻辑/UI 帧
- GL 驱动调用的 CPU 开销移出主线程
- 为未来：逻辑与渲染的帧间重叠（第 N+1 帧逻辑并行第 N 帧渲染）铺路

## 2. 现状（已核实）

### 2.1 主循环（engine/src/core/app.cpp）
```
PumpEvents → (固定步 OnUpdate×N) → OnRender → (循环)
```
- 渲染与更新同线程串行；Renderer::EndFrame → backend_->EndFrame → SwapBuffers（vsync 阻塞吃主线程）
- Renderer/SceneState/PostGraph/GameRuntime::Draw 是一体立即式状态机，无命令录制中间层

### 2.2 IRenderBackend（engine/include/neon/gfx/backend.hpp）
- **立即式**：每次调用直接做 GL 调用，大量方法带同步返回值
- 句柄类方法：CreateTexture/CreateShader/CreateMesh(U32)/CreateRenderTarget/CreateDepthTarget
- 无句柄方法：DrawMesh/DrawMeshInstanced/Set*State/Bind*/UseShader/SetUniform*/Clear/Begin|EndDepthPass
- 回读方法：ReadCurrentTargetPixel/CaptureFrame/GpuMemory/DepthAvailable（同步）
- 每帧方法：BeginFrame（空）/EndFrame（SwapBuffers）
- GL 函数表：进程级单例（gl_loader.cpp），WGL proc 地址上下文无关（跨线程安全）
- 线程亲和：上下文 current 绑定主线程（创建后不再显式 MakeCurrent）

### 2.3 已完成的地基
- `platform::Thread/Semaphore/SleepMs/CpuCount`（threading.hpp，win32/posix）
- `IWindow::CreateSharedContext/MakeSharedContextCurrent/MakeNoContextCurrent/DestroySharedContext`（Win32 WGL + X11 GLX）
- `gfx::UploadThread`：后台 worker 在共享上下文上执行 GL 工作

## 3. 核心难点

1. **句柄返回**：CreateX 返回句柄，调用方**当帧立即用**（材质/网格绑定）。命令队列化后句柄在渲染线程创建，主线程要"现在"拿句柄 → 需要同步往返或句柄预分配。
2. **状态竞争**：Renderer 持有可变状态（SceneState 相机/光照、材质 uniform、后处理 RT 绑定）。若渲染线程执行命令时主线程已进入下一帧 OnUpdate 改这些状态 → 竞争。需帧间双缓冲或命令里快照状态。
3. **ImGui 直连**：编辑器 ImGui 后端直接调 GL（imgui_neon），绕开 IRenderBackend → 编组不覆盖它，需单独路径或让它也走命令。
4. **回读**：截图（CaptureFrame）、拾取（ReadCurrentTargetPixel）、GpuMemory 是同步读回，编组下变阻塞点。
5. **共享上下文语义**：共享上下文共享对象，但并发访问同一对象需应用侧 fence（glFenceSync/glClientWaitSync）。

## 4. 设计

### 4.1 分层：Decorator 编组，不动 Renderer 上层（第一步）

最小侵入：写 `ThreadedBackendDecorator : IRenderBackend` 包住现有 backend。上层（Renderer/SceneState/DrawSystem）零改动编译期接入，运行时由 Decorator 接管。

```
现有上层 (Renderer/SceneState/ImGui?…)  →  ThreadedBackendDecorator  →  渲染线程 → 真正 backend (GL)
                                                     │
                              命令队列 (平台 Semaphore + 自旋锁 ring/deque)
```

**接入点**：`IRenderBackend::Init(IWindow*)` 是唯一工厂。GL 后端 Init 时若开启渲染线程：主线程创建共享上下文 → 渲染线程持**主上下文**（迁移），主线程用共享上下文只做命令录制？—— 否。GL 上下文 current 决定 GL 调用归属：**渲染线程必须持渲染用上下文**。

**两上下文方案（推荐）**：
- 渲染线程：持主渲染上下文（现在的 glrc_），执行全部绘制/上传命令
- 主线程：录制命令（纯 C++，无 GL 调用）；少数需要主线程 GL 的操作走共享上下文或同步往返
- 帧边界：主线程 EndFrame 提交命令队列 + 等渲染线程执行完（frame-lock，第一步不追求帧间重叠）

### 4.2 命令类型（第一步能覆盖的）

命令 = 一个小型字节/变体队列（避免每命令分配）：

```
enum Cmd : u8 { DrawMesh, DrawInstanced, BindRT, UseShader, SetUniformMat4, ... }
struct Command { Cmd kind; u8 args[64]; }  // 或 union 变体
```
每个无句柄方法 append 一条命令（参数已复制进队列，不引用主线程栈）。

### 4.3 句柄返回（CreateX）—— 同步往返或"主线程创建、渲染线程只见数据"

两个阶段：
- **阶段 A（本设计第一批）**：CreateX 保持同步——Decorator 用信号量做**主线程→渲染线程→主线程**往返（提交创建命令、等完成回执、拿句柄）。代价：资源创建在主线程等待（但创建本来就低频、非每帧），正确性零妥协。
- **阶段 B（后续）**：预分配句柄（backend 侧 handle 池），CreateX 立即返回，数据上传异步——配合共享上下文 + fence。

### 4.4 EndFrame 提交与同步

```
主线程 OnRender 末：decorator->EndFrame():
    提交本帧命令队列给渲染线程
    渲染线程：MakeCurrent(主上下文) → 执行全部命令 → SwapBuffers → 置 done
    主线程：等 done（第一步 frame-lock；后续用 fence 提前放行逻辑帧）
```

### 4.5 线程池关系

ECS ThreadPool / AsyncLoader 是**纯 CPU** worker，永不许碰 Renderer/GL（已有契约注释）。渲染线程是**唯一**碰 GL 的线程（除 UploadThread 的共享上下文上传，对象级隔离）。三者互不调用，避免锁序问题。

## 5. 分阶段实施清单

### 阶段 0（已完成）
- threading.hpp / Semaphore 信号量唤醒 ✓
- 共享 GL 上下文 + UploadThread ✓

### 阶段 1：Decorator 骨架 + 无句柄命令迁移
- [ ] `ThreadedBackendDecorator : IRenderBackend` 头/实现（命令队列 + 渲染线程循环 + 帧提交）
- [ ] 把「无句柄/低频」命令接入：Clear/SetViewport/SetScissor/SetCullMode/SetBlendMode/UseShader/SetUniform*/BindTexture/Begin|EndDepthPass/DrawMesh/DrawMeshInstanced（主场景绘制全路径）
- [ ] 渲染线程持主上下文；窗口层补 `MakeMainContextCurrent` 通用化（现在的 MakeGLContextCurrent 已够，渲染线程直接调它）
- [ ] EndFrame 提交 + frame-lock 同步
- [ ] 验证：默认项目 + Sponza + 森林场景 smoke/截图逐像素等价（渲染结果必须 bit-identical）
- [ ] 帧率验证：高负载场景主线程帧时（录制）明显小于帧时（执行）

### 阶段 2：句柄方法同步往返
- [ ] CreateTexture/CreateShader/CreateMesh(U32)/CreateRenderTarget/CreateDepthTarget → 同步往返命令
- [ ] Destroy* → 渲染线程执行（延迟到帧末销毁，防正在用的对象被删）
- [ ] UpdateMeshVertices / UpdateTextureRegion → 命令化

### 阶段 3：ImGui + 回读
- [ ] imgui_neon 后端走 Decorator 或渲染线程专用提交路径（ImGui 顶点在命令队列里）
- [ ] ReadCurrentTargetPixel / CaptureFrame（截图）：同步往返（阻塞可接受，低频）
- [ ] GpuMemory / DepthAvailable：线程安全快照

### 阶段 4（优化，可选）：帧间重叠 + fence
- [ ] Renderer/SceneState 状态双缓冲（相机/光照/材质 uniform 帧快照）
- [ ] 逻辑帧提前放行（不再每帧等渲染完成），用 fence 保证资源就绪
- [ ] glFenceSync 语义接入 UploadThread/渲染线程共享对象

## 6. 验证策略（每阶段必做）

- 渲染逐像素 SHA-256 等价（现有工具链：--no-msaa/--no-bloom/--no-tonemap diff）
- 默认项目、neon_realm、forest（高面扫描树）、pvz 冒烟
- 777 单测 + 编辑器冒烟 fail-delta=0
- 每阶段独立提交，可回退

## 7. 风险与回退

- **主线程安全风险最高的是状态竞争**（阶段 1 用 frame-lock 规避；不追求重叠前无竞争）
- 回退：RenderThread 开关（默认关），关闭即纯主线程立即式（现状）。逐步灰度
- ImGui 是最大回归面（编辑器日常用），阶段 3 前编辑器保持主线程立即式渲染（Decorator 只用于 play/独立 player），避免一次动编辑器
