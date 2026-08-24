# NeonEngine 缺陷与差距清单（TODO）

> 记录日期：2026-08-24
> 来源：对照"三阶段路线图"逐项盘点（代码核对 + [`ROADMAP.md`](./ROADMAP.md) + [`godot-gap-analysis.md`](./godot-gap-analysis.md)）。
> 约定：`[ ]` = 待办；`[~]` = 部分完成/进行中；`[x]` = 已完成（存档对照，不在本清单跟踪）。
> 本清单只记录"差距/缺陷"，讨论时可直接按编号引用（如 G1-2）。

## 一、核心基础（第一阶段）

### G1-1 渲染后端覆盖不足

- [~] 现状：`IRenderBackend` 接口完整；OpenGL 后端成熟（Windows 实测）；Vulkan 后端已修复（`--backend vulkan` 与 GL 逐像素一致，见 godot-gap-analysis P0-2）；**无 DirectX 12 / Metal 后端**。
- 差距：跨平台"一次编写到处编译"只覆盖 GL/VK，D3D12/Metal 为空白。
- 建议：暂不实现 D3D12/Metal，但预留后端注册/选择机制，并统一文档状态（见 G4-2）。

### G1-2 空间索引缺失（优先级最高）

- [ ] 现状：**无四叉树 / 八叉树 / BVH**。渲染裁剪是逐实例暴力 `TransformAABB + Frustum::Intersects`（`renderer.cpp` 1376/1396/1431）；游戏侧空间查询无加速；碰撞加速依赖 Jolt 内部 broadphase；网络 AOI 九宫格是唯一已落地的空间索引。
- 差距：大世界 / 万级实体时 CPU 裁剪与查询成为瓶颈。
- 建议：定义统一空间索引接口（插入/更新/移除/查询），2D 用四叉树、3D 用 BVH；接入渲染裁剪、游戏查询、服务器 AOI 三处。
- 验收：万级动态实体视锥裁剪帧耗时显著下降；查询接口有单元测试。

### G1-3 场景树接口与两套实体表示

- [~] 现状：运行时父子层级存在（`SceneParentLink` 组件 + 世界变换合成，`game_runtime.cpp:1662`）；编辑器侧 `SceneEntity.parent` 是名字字符串。
- 差距：无全局场景树遍历接口（GetChildren/GetDescendants、变换脏标记与缓存）；编辑器与运行时是两套实体表示。
- 建议：运行时补场景树接口与变换缓存；明确编辑器 ↔ 运行时的转换层（序列化已是桥，但 API 语义需对齐）。

### G1-4 资源系统小缺口

- [~] 现状：路径缓存、引用计数 + 延迟回收、异步 worker 线程、chunk 流式加载/卸载、BC1 异步解码、`core::ObjectPool` 均已完成。
- 差距：**无统一资源依赖图**——glTF 的材质/纹理依赖在解析器内部同步 `LoadTexture`，未形成通用的递归异步依赖加载与失败回滚；资源句柄无弱引用/自动卸载策略（目前手动 Acquire/Release）。
- 建议：资源句柄 + 依赖图加载器，统一错误传播与回滚。

### G1-5 渲染表现缺口（P2-1 遗留）

- [ ] SSAO（环境光遮蔽）
- [ ] 体积光 / 体积图
- [ ] GPU 粒子实例化渲染（当前粒子渲染模型需改造）
- 现状：PBR / IBL / HDR / Bloom / ACES / MSAA / CSM / 点光源 cubemap 阴影 / 实例化 / GPU 蒙皮 / 贴花均已交付。

## 二、进阶扩展（第二阶段）

### G2-1 反射系统自动化

- [~] 现状：`ComponentSchema` 手写元数据驱动编辑器（Godot `@export` 风格）；脚本绑定手写 `RegisterField`；**无 C++ 编译期反射 / 自动暴露**。
- 差距：新增 C++ 组件需手写 schema + 绑定，未达到"结构体自动暴露给 Lua/JS"。
- 建议：模板 + 宏做轻量自动注册（字段名/类型/默认值），生成 schema 与脚本绑定，避免引入重量级反射库。

### G2-2 ECS archetype 存储与系统调度

- [~] 现状：SparseSet ECS（实体代际句柄、dense+sparse、双组件视图）、确定性 `ParallelForEach` + 持久线程池已交付；demo 系统保持串行。
- 差距：跨组件 SoA（archetype）存储未做（接口已预留）；系统级调度器（依赖图 + 并行执行）未做。
- 建议：保持现有 API 不变，替换存储后端；再补系统调度器。

### G2-3 地形与植被

- [ ] 地形 Layer Blend / splatting（按高度/坡度混合岩石、草地、泥土）
- [ ] 地形 chunked LOD（四叉树）
- [ ] 植被放置与 Impostor（远处树木转 2D 面片）
- 现状：高度图程序化生成 + 顶点色 + 笔刷雕刻 + 运行时重建网格已完成。

### G2-4 动态全局光照

- [ ] DDGI 或 Voxel Cone Tracing；现状仅静态 IBL（预计算天空）。
- 建议：排在地形之后；移动端优先 DDGI（不依赖光追硬件）。

### G2-5 自定义 shader 热重载仅限 GL

- [ ] 现状：自定义 fragment shader 编译 + 热重载面板已完成，但 **GL-only**（`editor.cpp:1301`，Vulkan 文档化不支持）。
- 差距：Vulkan 后端下无法使用自定义 shader 热重载。
- 建议：补齐 Vulkan 的运行时 shader 编译（或明确以离线 SPIR-V 管线支持）。

## 三、前沿扩展（第三阶段）

### G3-1 LLM 集成

- [ ] NPC 对话/行为实时生成（本地 7B-13B 模型）
- [ ] 编辑器 AI 助手（如"生成追随摄像机脚本"并挂载到选中实体）
- 注意：与引擎的确定性沙盒设计有冲突，需先设计外部服务边界、异步调用与失败回退策略。

### G3-2 PCG 节点图

- [ ] 图节点编辑器（美术连线定义生成规则，如"道路两侧 5 米内随机间距生成路灯"）
- [ ] 分形噪声 + 侵蚀模拟（地形/河流）
- 现状：仅程序化网格/程序化地形/程序化音频。

### G3-3 视频编解码

- [ ] Bink/VP9 过场视频解码，独立于渲染线程（当前无任何视频播放能力）。

### G3-4 网络栈补缺

- [~] 现状：UDP 可靠通道、快照插值、预测回滚、AOI 九宫格、确定性权威服务器、房间/防作弊（RPC、输入限速、kick/ban、world.hash）、客户端脚本 Rpc 绑定均已交付。
- [ ] 服务器端 lag compensation（历史姿态回溯命中判定）
- [ ] 分区分服（world server / instance server）
- [ ] 多玩家输入模型收尾（`on_player_join` + `BindPlayerToClient` 已有，接入正式玩法）

### G3-5 UI 控件能力

- [ ] 九宫格切片（9-slice）
- [ ] 富文本标签（颜色/图标/链接/内嵌图片）
- 现状：自研 canvas 控件树已有（Panel/Label/Button/TextField/Slider/VBox/HBox/Window/ScrollArea/List/TreeView/ComboBox/TabBar/DockLayout，Measure/Layout/Draw 三阶段）；ImGui 仅用于编辑器，游戏运行时 UI 不依赖 ImGui。

## 四、核心建议落实

### G4-1 微内核二进制插件

- [~] 现状：接口化分层（编译期"微内核"）完成，core 不依赖平台实现；所有模块静态链接；插件为 Lua/JS 脚本级（编辑器插件 + 运行时插件）。
- 差距：**无运行时原生模块**（DLL/SO + 稳定 ABI），渲染器/物理/音频无法独立替换或热插拔。
- 建议：先以物理（Jolt 已按接口隔离）或音频做原生插件示范，定义 ABI、生命周期与版本兼容策略。

### G4-2 文档同步

- [ ] [`ROADMAP.md`](./ROADMAP.md) M1 的 Vulkan 状态仍标"灰度"，但 godot-gap-analysis P0-2 已记录修复完成，需更新。
- [ ] ROADMAP M5 的 "shader 热重载未做" 已过时（代码与 gap-analysis 均已完成，仅限 GL），需更新。
- [ ] 单元测试数量（ROADMAP 记录 561，当前实际 564）与横向"渲染资源生命周期管理"条目同步。

## 优先级建议（供讨论）

1. **G1-2 空间索引**——唯一影响架构根基的缺口，直接决定大世界/万级实体可行性。
2. **G2-3 地形进阶**（Layer Blend + chunked LOD）——工作量可控，编辑器体验与场景表现提升明显。
3. **G4-1 原生二进制插件**——把"微内核"从编译期落到运行时，先以物理/音频做示范。
4. **G3 系列**（LLM / PCG / 视频）——待前三项稳定后再碰；LLM 需先设计外部服务边界。
