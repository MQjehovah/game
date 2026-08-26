# NeonEngine 缺陷与差距清单（TODO）

> 记录日期：2026-08-24（第一轮：三阶段路线图；第二轮：架构/资源/跨平台/开发调试四类 13 项）
> 来源：对照"三阶段路线图"与"进阶特性 13 项"逐项盘点（代码核对 + [`ROADMAP.md`](./ROADMAP.md) + [`godot-gap-analysis.md`](./godot-gap-analysis.md)）。
> 约定：`[ ]` = 待办；`[~]` = 部分完成/进行中；`[x]` = 已完成（存档对照，不在本清单跟踪）。
> 本清单只记录"差距/缺陷"，讨论时可直接按编号引用（如 G1-2）。

## 一、核心基础（第一阶段）

### G1-1 渲染后端覆盖不足

- [~] 现状：`IRenderBackend` 接口完整；OpenGL 后端成熟（Windows 实测）；Vulkan 后端已修复（`--backend vulkan` 与 GL 逐像素一致，见 godot-gap-analysis P0-2）；**无 DirectX 12 / Metal 后端**。
- 差距：跨平台"一次编写到处编译"只覆盖 GL/VK，D3D12/Metal 为空白。
- 建议：暂不实现 D3D12/Metal，但预留后端注册/选择机制，并统一文档状态（见 G4-2）。

### G1-2 空间索引缺失（优先级最高）

- [x] 现状：**动态 BVH 已落地**（`neon/math/bvh.hpp`，Box2D 风格动态树：插入/更新/移除 + AABB/视锥/射线/球查询）。已接入 `GameRuntime::Draw` 的场景实例化裁剪（帧内 BVH 预裁剪 + `DrawMeshInstanced(frustumCull=false)`）；万级（10k）实体视锥查询基准：暴力 243.55ms → BVH 19.33ms（约 12.6 倍，Debug 200 次查询）。单元测试 `tests/test_bvh.cpp` 与暴力法逐项比对（AABB/视锥/射线/移除/移动）。2026-08-25。
- 差距：大世界 / 万级实体时 CPU 裁剪与查询成为瓶颈。
- 建议：定义统一空间索引接口（插入/更新/移除/查询），2D 用四叉树、3D 用 BVH；接入渲染裁剪、游戏查询、服务器 AOI 三处。
- 验收：万级动态实体视锥裁剪帧耗时显著下降；查询接口有单元测试。
- 后续：BVH 接口可进一步复用于游戏侧查询（投射物/AOI）与 2D 四叉树。

### G1-3 场景树接口与两套实体表示

- [x] 现状：运行时父子层级存在（`SceneParentLink` 组件）；**场景树接口与变换缓存已落地**：`GameRuntime::GetChildren/GetDescendants`（O(n) 遍历，供工具/查询/测试），`RebuildWorldTransforms()`（父先子后的迭代 DFS，任意深度，解除原 8 层上限）+ `CachedLocalToWorld()`（Draw 每帧重建后使用）。单元测试 `tests/test_scene_tree.cpp`（遍历 / 13 层世界变换合成 / 变更后重建）。顺带修复引擎既有 bug：`game_runtime` 从 `m[12..14]` 读世界平移（列向量约定下应为 `m[3,7,11]`，原读取恒为 0，导致 LOD 距离与贴花放置错误）。编辑器 ↔ 运行时转换层（序列化桥）语义对齐留作后续。2026-08-25。
- 差距：无全局场景树遍历接口（GetChildren/GetDescendants、变换脏标记与缓存）；编辑器与运行时是两套实体表示。
- 建议：运行时补场景树接口与变换缓存；明确编辑器 ↔ 运行时的转换层（序列化已是桥，但 API 语义需对齐）。
- [x] 2026-08-25 增补：**编辑器父级已迁移到稳定实体 id**——`SceneEntity.id/parentId`（场景树、拖拽重排、撤销重做按 id；序列化写 id/parentId 并保留旧名字字段兼容）；运行时场景格式新增实体 `id` 与 transform `parentId`（按 id 精确解析，名字回退），Instantiate 拒绝自父/环；编辑器拖拽带 id 防环（不能拖成自己/后代的子级）；属性栏移除父级字段（场景树已可视化层级）。**同名实体不再歧义、改名不破坏父子、环被三层拦截**（编辑器拖拽 / 运行时解析 / 遍历 visited 守卫）。测试：`test_scene_tree.cpp`（重名 + parentId 精确解析、环/自父拒绝）。
- 后续：变换脏标记（增量失效，当前为逐帧全量重建）——建议只做轻量版（全局变换版本号，未变更时跳过重建）；逐实体脏子树在当前规模非瓶颈且正确性税高。
- [x] **编辑器↔运行时场景桥无损化（G2-2 场景同构第一步）**——`SceneFile::MakeSpriteEntity`（规范 2D 精灵构建器，镜像 `MakeEntity`）：统一输出 transform/sprite/health/parent/id；编辑器 `BuildPlaySceneJson` 的 sprite 分支（曾手写 JSON 且丢 health）改用它，编辑器导出与运行时解析走同一代码路径，消除漂移源。单元测试 `SceneSpriteMakeEntityRoundTrip`（sprite/health/transform 经 Parse+Instantiate 无损往返；无 health 时不生成组件）——即会捕获"编辑器导出丢血量"那类 bug 的回归网。2026-08-26。
- [x] **编辑器 ECS 化第一阶段（持有 live ecs::World）**——`EditorApp` 新增 `sceneWorld_`（`ecs::World`）+ `sceneCompReg_`：`LoadScene` 末尾经 `RefreshSceneWorld` 用运行时 `Instantiate` 装载当前场景（与播放器完全相同的组件路径），编辑器持有场景的规范运行时表示；`entities_` 仍为 UI 读写模型（后续阶段迁移面板/视口/历史直读/写 World 组件）。冒烟检查：编辑器 ecs world 镜像 sprite 场景 + 变换（`editor ecs world mirrors the sprite scene`）。单元测试 `PvzSceneHostsInEcsWorld`：pvz 场景经 Instantiate 无损承载（transform 全覆盖 / sprite / health / plant+zombie 经 SceneData 存活）。2026-08-26。
- 剩余（全量 ECS 化后续）：面板/视口/撤销历史直读 `sceneWorld_` 组件（替换 `entities_` 扁平字段），删除双模型——大项，分阶段迁移。

### G1-4 资源系统小缺口

- [x] 现状：路径缓存、引用计数 + 延迟回收、异步 worker 线程、chunk 流式加载/卸载、BC1 异步解码、`core::ObjectPool` 均已完成；**资产依赖图已落地**：`AssetManager` 在 glTF/OBJ 加载时记录依赖边（纹理/缓冲/MTL）与反向边，`DependenciesOf/DependentsOf` 查询；`MissingDependencies(path)` 递归遍历返回缺失叶子（精确错误传播，替代静默白回退）；`LoadDependenciesAsync(path, cb)` 递归异步加载图片叶子依赖并回报首个失败路径。单元测试 `tests/test_asset_deps.cpp`（真实 glTF 依赖记录与反向边 / 缺失纹理精确定位 / 异步成功与失败）。2026-08-25。
- 差距：**无统一资源依赖图**——（已落地，见上）；资源句柄无弱引用/自动卸载策略（目前手动 Acquire/Release）。
- 建议：资源句柄 + 依赖图加载器，统一错误传播与回滚。

### G1-5 渲染表现缺口（P2-1 遗留）

- [x] SSAO（环境光遮蔽）——已落地：`neon/gfx/ssao.hpp`（AO kernel 数学可单测）+ 渲染器 `RunSsaoPass`：颜色编码场景深度 pass（复用 shadow 深度 shader，规避 FBO 深度纹理在 Intel 上的问题）→ AO（半帧）→ 分离模糊 → 合成乘 AO。默认 `--no-ssao` 可关（关时合成输出不变）；`--ssao` 开启。单元测试 `tests/test_ssao.cpp`（深度编码往返 / 平坦零遮挡 / 遮挡）。`neon_rush --smoke-test 60 --ssao` GL 冒烟通过。
- [x] 体积光 / 体积图——已落地：`neon/gfx/volumetric.hpp`（god-ray 累积数学可单测）+ 渲染器 `RunVolumetricPass`：屏幕空间光柱（crepuscular rays），朝太阳方向径向采样 HDR 场景色 + 分离模糊，合成时叠加。默认关，`--volumetric` 开启。单元测试 `tests/test_volumetric.cpp`。`neon_rush --smoke-test 60 --ssao --volumetric` GL 冒烟通过。2026-08-25。
- [x] GPU 粒子实例化渲染（当前粒子渲染模型需改造）——已落地：`Renderer::DrawBillboards` + `DrawMeshInstancedColored`（GL 每实例 RGBA 属性 location 8 + 新 instanced-colored shader），粒子从逐粒子屏幕空间 billboard 改为**单次 3D 实例化相机朝向广告牌**（depth-aware，additive/alpha 分两组各一次 draw）。`neon_rush --smoke-test 60` GL 冒烟通过。2026-08-25。
- 现状：PBR / IBL / HDR / Bloom / ACES / MSAA / CSM / 点光源 cubemap 阴影 / 实例化 / GPU 蒙皮 / 贴花均已交付。

## 二、进阶扩展（第二阶段）

### G2-1 反射系统自动化

- [~] 现状：`ComponentSchema` 手写元数据驱动编辑器（Godot `@export` 风格）；脚本绑定手写 `RegisterField`；**轻量反射已落地**（`neon/scene/component_reflect.hpp`，模板+成员指针，无需宏迭代）：`Field("key","标签",FieldType,T Owner::*,def,min,max,step)` 单条声明字段，`FieldList<Owner>` 一次定义同时派生 `ComponentSchema`（编辑器）与 `ToJson/FromJson`（运行时/脚本层）——schema 与数据结构共用同一字段表，改成员名即编译期断裂，不会漂移。字段类型支持 int/float/double/bool/string（MSVC 用 initializer-list 展开规避 std::apply C1001）。**示范迁移**：`SceneAudioSource` 的 schema（原手写 6 行）与 scene_file 解析工厂（原 ~25 行手写 JSON 读取）改为反射生成/读取，行为与严格性（未知字段拒绝）不变。单元测试 `ReflectedAudioSourceSchemaAndJson`（schema 字段/round-trip/错类型拒绝）。2026-08-26。
- [ ] 差距：C++ 编译期反射覆盖有限（仅标量字段，无 Vec3/Color/Enum/嵌套）；脚本绑定仍未从字段表自动生成。
- 建议：模板 + 宏做轻量自动注册（字段名/类型/默认值），生成 schema 与脚本绑定，避免引入重量级反射库——模板层已落地并示范，扩展 Vec3/Enum 与脚本绑定生成为后续。

### G2-2 ECS archetype 存储与系统调度

- [~] 现状：SparseSet ECS（实体代际句柄、dense+sparse、双组件视图）、确定性 `ParallelForEach` + 持久线程池已交付；demo 系统保持串行。
- [x] 系统级调度器已落地（`neon/ecs/system_scheduler.hpp`）：系统按注册顺序声明组件读/写访问，调度器对写-写 / 写-读冲突推导依赖边（先注册者先执行），无冲突系统经 TaskGraph（G5-2）并行执行；`Run(false)` 提供串行确定性参考路径。单元测试 `tests/test_system_scheduler.cpp`（写写排序 / 写读依赖 / 读读并行确定性 / 独立写 / 空图 / 清空）。2026-08-25。
- [ ] 跨组件 SoA（archetype）存储未做（保持现有 API 不变替换存储后端，接口已预留）。
- 建议：先补系统调度器（已完成），archetype 存储作为下一步替换 World 的 Pool 后端。

### G2-3 地形与植被

- [x] 地形 Layer Blend / splatting（按高度/坡度混合岩石、草地、泥土）——`neon/gfx/terrain.hpp` `TerrainLayerColor`（grass→dirt→rock，按高度 + 坡度=1-法线Y混合），已接入 `Mesh::CreateTerrain` 与 `MakeTerrainMesh`（编辑器/运行时默认地形即分层配色）。
- [x] 地形 chunked LOD（四叉树）——`BuildTerrainLODChunks`/`BuildTerrainChunk` 将高度场划分为 gridDiv×gridDiv 分块，每块带独立 `LodChain`（近密远疏）；运行时以每块一个 DrawItem 绘制（`SceneTerrain.chunkGridDiv` 开启），边带 skirt 隐藏裂缝。
- [x] 植被放置与 Impostor（远处树木转 2D 面片）——`ScatterVegetation` 按高度/坡度确定性撒点，`MakeImpostorQuad` 生成朝向相机的 Y-偏航公告板；运行时（`GameRuntime::DrawVegetation`）近处实例化网格、远处自动换 Impostor（`SceneTerrain.vegMeshKey/vegCount` 开启）。编辑器地形面板新增 LOD/植被控制。
- 现状：高度图程序化生成 + 顶点色 + 笔刷雕刻 + 运行时重建网格已完成；新增 Layer Blend + chunked LOD + 植被 Impostor 落地。单元测试 `tests/test_terrain.cpp`（TerrainLayer* / chunked LOD / scatter / impostor / runtime chunked+veg）。2026-08-25。

### G2-4 动态全局光照

- [~] 现状：仅静态 IBL（预计算天空）。**light-probe 场已落地**（`neon/gfx/light_probe.hpp`：`BuildProbeField`/`SampleProbeField`，按场景 AABB 生成 irradiance 探针网格，太阳/天空/点光衰减采样；供 G8-3 探针可视化 + 未来动态 GI）。单元测试 `tests/test_light_probe.cpp`。
- [ ] 动态 GI shader 集成（探针采样接入 lit 着色器 / DDGI / Voxel Cone Tracing）——待做；移动端优先探针式（不依赖光追硬件）。
- 建议：排在地形之后；移动端优先探针式 GI（不依赖光追硬件）。

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
- [x] 服务器端 lag compensation（历史姿态回溯命中判定）——已落地：GameRuntime 每固定 tick 记录权威姿态环形缓冲（64 tick ≈ 1s）；`MeleeAttackLagComp` / `AttackBoxLagComp` / `LagCompPosition` 按回滚 tick 用历史姿态判定、伤害落在当前实体；`SetAutoLagComp` 自动回滚模式让普通攻击与 CastSkill 一并生效。GameServer 由 MsgPing 测量客户端 RTT，每 tick 按最活跃客户端半 RTT 折算回滚 tick（`AutoLagCompTicks` 可观测）。单元测试 `tests/test_lagcomp.cpp`（历史命中 / box 回滚 / 自动模式 / 技能 / 容量与新生实体回退 / 服务器 RTT 集成）。2026-08-25。
- [ ] 分区分服（world server / instance server）
- [ ] 多玩家输入模型收尾（`on_player_join` + `BindPlayerToClient` 已有，接入正式玩法；per-attacker lag-comp 回滚随攻击上下文细化）

### G3-5 UI 控件能力

- [x] **九宫格切片（9-slice）**——`ui::ComputeNineSlice(rect, slice, texW, texH, out[9])`（纯数学，可单测）：固定四角、单轴拉伸四边、中心填充剩余；UV 按纹理像素换算。`UiNode` 新增 `slice` 字段（设计 px）并随 `.ui.json` round-trip；`UiDocument::Draw` 新增可选 `UiTextureLoader`（加载 `sprite` 纹理）：Panel/Image/Button 带 sprite 时画纹理四边形，`slice>0` 时拆 9 块画（角不变形）；GameRuntime `DrawUI` 与编辑器 UI 预览已接线（asset manager 作 loader）。单元测试 `UiNineSliceLayout`（几何+UV+过小拒绝）+ `UiNineSliceJsonRoundTrip`。2026-08-26。
- [ ] 富文本标签（颜色/图标/链接/内嵌图片）——未做。
- [x] **富文本标签（颜色分段）**——`ui::ParseRichText(text, baseColor)`（纯解析，可单测）把 `[color:#rrggbb]...[/color]`（也接受 `[color=#rrggbb]`）拆成 `RichSpan` 段，无标记段继承基础色，畸形/未闭合标签按字面渲染；`ui::DrawRichLabel` 逐段测宽、按各自颜色在同一基线上左到右绘制（centerX 整体居中）；`UiDocument` Label 节点走富文本（纯文本行为不变，UI 编辑器的文本字段直接输入标记即可）。单元测试 `UiRichTextParse`（纯文本单段/混合分段/color= 形式/畸形字面）。2026-08-26。
- 现状：自研 canvas 控件树已有（Panel/Label/Button/TextField/Slider/VBox/HBox/Window/ScrollArea/List/TreeView/ComboBox/TabBar/DockLayout，Measure/Layout/Draw 三阶段）；ImGui 仅用于编辑器，游戏运行时 UI 不依赖 ImGui。

## 四、核心建议落实

### G4-1 微内核二进制插件

- [~] 现状：接口化分层（编译期"微内核"）完成，core 不依赖平台实现；所有模块静态链接；插件为 Lua/JS 脚本级（编辑器插件 + 运行时插件）。
- [x] **原生 DLL/SO 插件 ABI + 加载器已落地**——`neon/plugin/native.hpp`（`neon::plugin` 命名空间内扩展，复用预留的 `PluginType::Native`，不另起炉灶）：`NativePluginInfo`（版本号 + size 校验 + `create/destroy` 生命周期回调，纯 C ABI、POD + extern "C"，无 C++ 运行时对象跨边界）+ `NativePlugin`（跨平台加载：Windows `LoadLibrary`/`GetProcAddress`、POSIX `dlopen`/`dlsym`；ABI 校验；`Symbol()` 解析模块特定函数；`Reload()` 销毁实例+释放库+重载=热插拔）+ `LoadNativePlugins`（复用 `DiscoverPlugins`，按 `PluginType::Native` 清单加载）。`plugin.json` 的 `backend` 现接受 `"native"`，`entry` 为共享库文件名。**示例物理插件** `plugins/physics_plugin`（独立 DLL，自包含微物理：重力下落/地面碰撞，导出 `NeonPlugin_GetInfo` + 模块特定 `NeonPhysics_GetApi` 函数表）。单元测试 `tests/test_plugin_native.cpp`（加载+驱动物理 / 缺失库错误路径 / 热重载 / 清单发现）。2026-08-26。
- [ ] 差距（剩余）：渲染/物理后端仍静态链接，未真正做到运行时替换（插件系统已就绪，需把 Jolt/音频适配为插件形态）；编辑器原生插件加载（编辑器 `plugins` 面板接入 native 类型）未做。
- 建议：以物理（Jolt 已按接口隔离）或音频做原生插件示范，定义 ABI、生命周期与版本兼容策略（ABI+加载器+物理示范已落地；接入真实后端与编辑器为后续）。

### G4-2 文档同步

- [x] [`ROADMAP.md`](./ROADMAP.md) M1/当前状态的 Vulkan 灰度描述、M5 shader 热重载、对象池、横向"资源生命周期"与单元测试数量（564）已全部更新（2026-08-24）。
- [x] [`ARCHITECTURE.md`](./ARCHITECTURE.md) 扩展点的 Vulkan 灰度描述已更新为"已修复、与 GL 逐像素一致"（2026-08-24）。
- [x] [`VULKAN_ROADMAP.md`](./VULKAN_ROADMAP.md) 开头"占位实现（Init 返回 false）"已更新为"已实现并修复灰度问题"，下文保留为实现记录（2026-08-24）。
- [x] [`godot-gap-analysis.md`](./godot-gap-analysis.md) P0-2 现状已标注"已修复"，排查清单保留为历史记录（2026-08-24）。

## 五、架构设计级特性（第二轮 1–3）

### G5-1 原生插件与运行时热插拔（第 1 项）

- [~] 现状：core/interface 分层，编辑器/播放器/服务器共享同一套核心（`neon_engine`）；插件为 Lua/JS 脚本级（编辑器 + 运行时）+ 原生 DLL/SO（[G4-1](#g4-1-微内核二进制插件)，已落地 ABI 与加载器）。
- [x] **第三方中间件"即插即用"示范**——`neon/plugin/backend.hpp/cpp`：kind-scoped 后端提供者宿主，`PhysicsWorldApi`（纯 C 函数表）+ `LoadNativePhysicsBackend(name, baseDir)`（按插件名/库名发现 `NeonPhysics_GetWorldApi` 提供者）。示例物理插件 `plugins/physics_plugin` 现把引擎**真实确定性求解器**（`engine/src/physics/physics.cpp`）编译进 DLL，成为自包含物理中间件；创建/销毁都在模块内（跨 CRT 安全）。`GameRuntime` 的 `physicsBackend` 支持 `"plugin:<name>"`（运行时 DLL 后端，无需重链接，失败回退 custom），宿主经 `pluginBaseDir` 配置（编辑器 play / neon_game 已接）。单元测试 `NativeBackendLoadsPhysicsProvider`/`NativeBackendByName` + 端到端 `GameRuntimeNativePhysicsBackendPlugin`（运行时真用 DLL 后端让球落到地面）。2026-08-26。
- [x] **编辑器加载原生插件**——编辑器"插件"面板新增原生 (DLL/SO) 分节：`LoadNativePlugins(<project>/plugins)` 按 manifest 发现并加载，列出 ABI 信息与库路径；与脚本插件面板并列。2026-08-26。
- [x] **音频模块插件化（第二个中间件示范）**——`backend.hpp/cpp` 的 kind-scoped 发现通用化为共享模板（`FindProvider<ApiT>`），新增 `AudioApi`/`AudioBackend`/`LoadNativeAudioBackend`。`plugins/audio_plugin` 把引擎真实 WinMM waveOut 软件混音器（`winmm_audio.cpp` + 其依赖的 `log.cpp`）编入 DLL，成为自包含音频中间件（Windows-only，非 Windows 跳过目标）。编辑器启动优先从 `./plugins` 加载音频插件后端（staged 时生效），否则回退平台 miniaudio→WinMM→null；已验证两条路径（staged → "audio backend from native plugin 'winmm'"；未 staged → miniaudio）。单元测试 `tests/test_plugin_audio.cpp`（加载/驱动/名称匹配；Init 失败时接口安全）。2026-08-26。
- [ ] 运行时切换渲染后端（Vulkan → D3D12，无需重启）——后端在启动时固定，无运行时重建渲染器路径（仍为最大项，需 D3D12 后端 + 渲染器重建路径）。
- 关联 [G4-1](#g4-1-微内核二进制插件)，建议合并实施：先定 ABI，再以物理/音频做示范插件（ABI + 物理/音频示范 + 运行时接入已达成；渲染接入与真实"运行时热切换"为后续）。

### G5-2 依赖图任务调度器（第 2 项）

- [x] 现状：ECS 有确定性 `ParallelForEach` + 持久线程池（spinlock 队列，**非 lock-free**）；**任务依赖图调度器已落地**（`neon/ecs/task_graph.hpp`）：任务声明依赖边（支持前向引用），Kahn 拓扑分层 + 环检测，逐层经 `ParallelFor` 并行执行同层独立任务，`Run(false)` 串行路径供确定性校验。单元测试 `tests/test_task_graph.cpp`（依赖顺序 / 并行=串行 / 两次并行确定性 / 环检测 / 越界依赖 / 清空复用 / 渲染-物理-逻辑示例图）。2026-08-25。
- [x] **动态工作分发（work-stealing 方向）**——`ThreadPool::ParallelFor` 改为共享原子计数 `next`：每个 worker + 调用线程运行时拉取下一块（`grab()`），空闲 worker 不再空转，加载平衡更好；块边界仍是原静态划分（每索引恰好访问一次 → 逐位确定性不变）。修掉一个 use-after-free 陷阱：chunk 与 grab-job 解耦后 `remaining==0` 不代表 worker 的 grab-job 已退出，故加入 `doneJobs` 计数，join 同时等待全部块完成 + 全部 worker job 退出。单元测试全绿（`TaskGraphParallelMatchesSerial`/`DeterministicAcrossRuns`/`ECSParallel*`/`ParallelForReducerMisSizedDetected`，多轮无 flaky）。2026-08-26。
- [ ] 目标：任务声明读写内存区域，调度器自动并行无冲突任务；避免死锁；适配未来数百核。
- 建议：先实现任务图 + 依赖分析（读/写/独占分区），再考虑 lock-free 队列；`ParallelForEach` 保留为"数据并行"特例接入调度器。
- 验收：渲染/物理/逻辑三系统的依赖图示例 + 并行正确性测试（复用现有确定性校验）——示例与并行正确性测试已在 `test_task_graph.cpp` 落地；读写内存区自动分析（无冲突推断）与每线程 lock-free work-stealing 双端队列留作后续。

### G5-3 确定性模拟（第 3 项）——已完成

- [x] 现状：确定性 RNG（xorshift64*）、固定 60Hz tick、串/并行逐位一致、服务器权威模拟 + 客户端预测回滚、`tests/test_determinism.cpp`。
- [ ] 备注：未强制"禁止自动向量化"（跨编译器/跨平台 bit 一致性依赖工具链行为）；Jolt 物理的跨平台 bit 一致性未验证（轻量物理引擎是确定的）。
- 建议：补一个 CI 跨平台 determinism 测试（同输入序列在 Windows/Ubuntu/macOS 输出逐位一致）。

## 六、资源与数据流特性（第二轮 4–6）

### G6-1 多级资产烘焙与显存自适应（第 4 项）

- [~] 现状：数据驱动 LOD 链（`PickLod` 按距离选择，`game_runtime.cpp:137`）✅；BC1 纹理压缩 + 异步解码 ✅。
- [x] **平台/LOD 资产变体表**——`neon/assets/asset_variants.hpp`：`AssetVariantTable`（逻辑路径 → 具体文件，未列入的路径回落自身，JSON 读写 + `LoadVariant` 从 `variants.json` 选命名变体）。`<project>/variants.json`（`{"mobile": {"models/wolf.obj": "models/wolf_low.obj", ...}}`）随包打入（packer 收集）；`neon_game --variant <name>` 加载变体表，`GameRuntime::FullAssetPath` 在加载前解析（`GameRuntimeConfig.variantTable`），mesh/纹理/模型全部走变体。单元测试 `AssetVariantTableResolvesAndFallsBack`（解析/回落/round-trip/缺失变体/坏形状）+ `GameRuntimeVariantAssetResolution`（端到端：场景引用逻辑路径，资产缓存落具体变体文件）。真实打包 e2e：`--package` → `--pack --variant mobile` 日志 "asset variant 'mobile' active (1 overrides)" 退出 0。2026-08-26。
- [ ] 运行时按显存余量动态切换资产版本——无 GPU 显存预算查询/切换机制。
- 建议：资产清单（manifest）增加平台/LOD 变体表；渲染器加显存预算 API，流式加载按预算选版本——变体表已落地（清单=项目 variants.json），显存预算 API 为后续。

### G6-2 资源依赖图与按需加载（第 5 项）

- [~] 与 [G1-4](#g1-4-资源系统小缺口) 合并：**依赖图已落地**（G1-4：依赖边/反向边、MissingDependencies、LoadDependenciesAsync）；**异步 OBJ 网格加载已落地**（G6-2）：`AssetManager::LoadMeshOBJAsync(path, cb)`——文件读+OBJ 解析（纯 CPU）在 worker 线程，GPU 上传/缓存/回调在主线程 `PumpAsync` 内完成；已缓存或无线程池时内联完成；同路径并发请求合并为一次加载（`meshInFlight_`/`meshPendingCallbacks_`）；OBJ 解析抽取为 `ParseObjMesh`（同步 `LoadMeshOBJ` 与异步共用，行为不变，MTL 依赖边由主线程记录）。单元测试 `AssetDepsAsyncMeshLoad`（成功/并发合并/失败路径 + 缓存命中）。2026-08-26。
- [x] **绘制管线异步接入**——`GameRuntimeConfig.asyncMeshLoad`（默认关，行为不变）：`ResolveDrawItem` 对 `obj:`/`gltf:` 文件网格先探缓存（`HasMesh`/`HasGLTF`），未命中则踢 `LoadMeshOBJAsync`/`LoadGLTFAsync` 并置 `DrawItem.asyncPending`；Draw 两个渲染 pass 经 `ResolveOrSkip` 每帧探缓存，就绪则解析、否则跳过该帧（无逐帧卡顿）。`BuildDrawList` 保留已解析项使 asyncPending 跨帧保持。端到端测试 `GameRuntimeAsyncMeshStreaming`（首帧不阻塞、Pump 后缓存就绪、下一帧解析；对照同步路径）。2026-08-26。
- [ ] 差距（剩余）：异步仅覆盖 obj/gltf 网格；LOD 链等级仍同步；场景/编辑器侧未默认开启（性能敏感 3D 场景可 `asyncMeshLoad=true`）。
- 建议：导入期构建依赖图（节点 + 边），运行时按需解析；chunk 流式加载（3×3 窗口）已具备基础。

### G6-3 移动式分配器（第 6 项）

- [~] 现状：`core::ObjectPool` 固定容量池、ECS dense 数组、粒子稳定 arena（`particles.cpp:11`）——同类对象物理相邻，缓存友好 ✅。
- [x] **全局堆监控（碎片统计前置）**——`core::MemStats` + 全局 `operator new/delete` 覆写（`mem_stats.cpp`，编入 neon_engine）：纯计数转发 malloc/free，不改分配行为；普通（malloc 支撑）分配按块可用大小精确对称（`_msize`/`malloc_usable_size`），`liveBytes` 不漂移；C++17 对齐分配走 CRT 默认不统计（避免对 `_aligned_malloc` 块取大小）。指标：分配总次数/总字节、存活字节/次数、峰值存活（monotonic 高水位）。编辑器"性能"面板新增"堆: 存活/峰值/分配次数"行。单元测试 `tests/test_mem_stats.cpp`（真实容器分配/释放反映在计数、峰值单调、直接钩子）。为 G6-3 的 compact 工作提供确证数据。2026-08-26。
- [ ] 通用 relocating allocator（大块连续内存 + 后台 compact 整理）——无；堆碎片仅靠对象池局部规避。
- 建议：先加碎片统计/监控确证瓶颈，再实现 compact；或按类型分池扩展（粒子/实体/特效各自独立池）——全局堆监控已落地，compact 分配器为后续。

## 七、跨平台与兼容性特性（第二轮 7–9）

### G7-1 虚拟文件系统 VFS（第 7 项）

- [~] 现状：`game.pack` 打包容器 + 播放器加载 ✅；编辑器资产目录 watch 自动刷新 ✅。
- [x] 统一虚拟路径与多层挂载——已落地 `neon/io/vfs.hpp`：`IFileSystem` 抽象 + `DiskFileSystem`（根目录、防 `..` 逃逸）+ `PackFileSystem`（pack 容器直读，免解包）+ `MountStack`（后挂载优先覆盖）。`AssetManager::SetFileSystem` 让纹理/字体/OBJ/MTL/glTF+bin 全部走 VFS（null 时保持磁盘直读）。单元测试 `tests/test_vfs.cpp`（路径规范化/逃逸拒绝/磁盘/pack/挂载覆盖/List 合并/AssetManager 经 MountStack 读纹理+OBJ）。2026-08-25。
- [x] 多层挂载（主包 + Mod 覆盖层）——`neon_game --pack X --mod DIR`（可重复，后挂载优先）：Mod 经挂载栈覆盖资产，并叠加到解包目录覆盖脚本/prefab/本地化；端到端冒烟通过（pvz 打包 + mod 覆盖 + 60 帧启动退出 0）。DLC 多包即再 `Mount` 一层。2026-08-25。
- 建议：定义 `IFileSystem` 抽象（路径规范化 + 只读挂载栈），pack 容器作为一层挂载；Mod 覆盖层复用同一挂载栈。这是"资源市场/Mod 生态"的前置。
- [x] **GameRuntime 脚本读取直通 VFS**——`GameRuntimeConfig.fileSystem`（`IFileSystem*`）：`ReadScript` 在 VFS 安装时剥离 scriptBaseDir 前缀、以虚拟路径经挂载栈读取（pack 优先、后挂载 Mod 覆盖），VFS 未命中回退 readScript/磁盘。播放器接入：`cfg_.vfs` 存在时 `rcfg.fileSystem = cfg_.vfs.get()`，脚本/行为树/预制体/本地化/input.json 全部经包直读，Mod 覆盖不再依赖解包目录拷贝。单元测试 `GameRuntimeScriptsViaVfs`（DiskFileSystem 供脚本）+ `GameRuntimeModOverridesScriptViaVfs`（后挂载覆盖，Mod 行为生效）；真实打包 e2e：`neon_editor --package` → `neon_game --pack` 与 `--mod` 均退出 0。2026-08-26。
- [x] **播放器免解包直读 + assets:/ scheme**——`BootPack` 改为先建 VFS（PackFileSystem + Mod 挂载），game.json/scenes/UI 全部经 VFS 读取，**跳过 `core::Unpack` 与临时目录**（`GameRuntime::ShowUI` 与场景读取同步改为 ReadScript/VFS，运行时磁盘直读清空）；`neon_game --pack`/`--mod` e2e：日志 "served via VFS (no unpack)" + 启动 + 退出 0。`neon/assets/asset_path.hpp` `NormalizeAssetPath` 剥离 `assets:/`/`asset:/` scheme（`FullAssetPath` 统一处理，场景可写 `obj:assets:/...`）。单元测试 `AssetPathSchemeNormalization` + `GameRuntimeAssetSchemeMeshKey`。2026-08-26。
- 后续（剩余）：无（G7-1 项已收口）。

### G7-2 着色器字节码翻译层（第 8 项）

- [ ] 现状：GLSL 源码（内嵌字符串）+ Vulkan 离线 SPIR-V（构建脚本生成）；**无中间语言（IL）层**。
- [ ] 目标：引擎内只维护 IL（HLSL 或自研 IR），打包时按平台生成字节码；运行时按图形设置重编译（阴影质量高→低即时生效）。
- 关联 [G2-5](#g2-5-自定义-shader-热重载仅限-gl)（自定义 shader 热重载 GL-only）。
- 建议：短期先做"shader 源码资产化 + 平台编译产物缓存"（给内置 shader 也开热重载）；IL/JIT 列为远期。

### G7-3 输入动作系统（第 9 项）——大部分已完成

- [x] 现状：Godot 风格 `InputMap`——动作名→多键绑定、positive/negative 轴、`input.json` 数据驱动、编辑器可视化面板改键（`SetPrimaryKey`）、脚本按动作名查询（`ActionDown/Axis`）✅。
- [ ] 和弦/组合键/长按/双击——无时序类绑定。
- [x] **输入动作时序绑定（和弦/双击/长按）**——`InputAction` 新增 `modifiers`（和弦：全部按住才触发，作用于所有查询）+ `doubleTapMs`（双击窗口，第 2 次按下触发，第 1 次消耗）+ `longPressMs`（长按阈值，达到一次性触发，释放上报）；`InputMap::Update(dt, in)` 逐帧推进时序时钟（每键独立 press 历史，多动作共享键保持一致），`Reset()` 清理跨场景状态；`input.json` 扩展并 round-trip；`GameRuntime::Tick` 每帧在脚本查询前调 `Update`。编辑器输入映射面板新增"时序规则"节（滑块 + 修饰键文本）。未配时序的动作走原直接读取路径，行为逐位不变。单元测试 `tests/test_input_map.cpp`（和弦门控 / 双击窗口 / 长按单次触发+释放 / round-trip+Reset）。2026-08-26。
- [ ] 触屏滑动映射——未确认（平台输入以键盘/鼠标为主）。
- 建议：在 `InputAction` 上增加修饰键与时序规则（chord、double-tap），面板可视化编辑；触屏映射随平台输入层扩展。

## 八、开发与调试特性（第二轮 10–13）

### G8-1 Live Profiler 环形缓冲 + 追溯式崩溃报告（第 10 项）

- [~] 现状：性能面板（帧时/实体/物理/BT/内存统计）✅；`--bench` 基准日志 ✅；日志环形缓冲（`log.hpp:79`）✅。
- [x] 每帧函数耗时/内存分配/DrawCall 的持续环形缓冲记录——已落地 `neon/core/profiler.hpp`：固定 512 帧环形缓冲 + 命名作用域计时（`ScopedTimer`），`Application::Run` 自动逐帧采样，`GameRuntime::Tick/Draw` 已埋点（runtime.tick/scripts/physics/draw）；`Report()` 输出最近 N 帧。单元测试 `tests/test_profiler.cpp`（假时钟确定性 + 环形回绕 + 崩溃报告落盘）。2026-08-25。
- [x] 崩溃时自动导出崩溃前 5 秒性能数据报告——已落地 `neon/core/crash.hpp`：Windows SEH / POSIX 信号钩子写 `crash_report.txt`（日志环形缓冲 + profiler 最近 512 帧 ≈ 8.5s@60Hz），已接入 neon_rush / neon_game / neon_editor / neon_server 入口。2026-08-25。
- 建议：轻量采样 profiler（固定容量 ring，低开销），崩溃处理钩子（Windows MiniDump / POSIX 信号）导出报告。

### G8-2 实时代码热替换（第 11 项）

- [ ] 现状：Lua/JS/资产/自定义 shader 热重载 ✅；**C++ 无运行时重载**。
- [ ] 目标：LLVM/Clang 增量编译 + 重定位表替换函数指针——大工程，远期。
- 建议：短期以脚本热重载为正式工作流（已具备）；C++ 侧先做"模块级重载"（原生插件 DLL 卸载/重载，关联 [G4-1](#g4-1-微内核二进制插件) / [G5-1](#g5-1-原生插件与运行时热插拔第-1-项)）作为过渡。

### G8-3 可视化场景调试覆盖层（第 12 项）

- [~] 现状：`DrawBox/DrawSphere/DrawLines` 调试绘制 API ✅；物理碰撞体线框（playtest，动态青/静态灰）✅；导航网格在导航面板内可视化（绿/红格 + 路径）✅；相机边框/选中包围盒/地形笔刷预览 ✅。
- [x] 视口内 NavMesh 可行走区域覆盖层（绿/红半透明）——`DrawDebugOverlay` 在视口绘制 walkable/blocked 半透明格。
- [ ] 音频源 3D 衰减球体（半透明蓝）——暂无音频源数据结构（面板提供音频开关占位）。
- [x] **音频源组件 + 3D 衰减球可视化**——新增运行时组件 `SceneAudioSource`（sound/volume/radius，`scene_file.cpp` 注册解析 + 编辑器 schema `component_schema.cpp`，可在 inspector 编辑/添加）；`GameRuntime::Start` 经 `playSfx3D` 钩子在实体位置播放一次（环境循环需 loop-3D 钩子，后续）；编辑器 F3「调试覆盖层」勾选"音频源"即在视口绘制半透明蓝衰减球（radius = 衰减距离）。单元测试 `SceneAudioSourceParse`（解析/round-trip/未知字段拒绝）+ `GameRuntimePlaysAudioSources`（播放钩子按位置触发）。2026-08-26。
- [x] 光照探针分布显示——配合 G2-4 的 `BuildProbeField`，视口按 irradiance 着色绘制探针标记。
- [x] F3 统一调试面板（图层开关：碰撞/导航/音频/光照）——F3 开关「调试覆盖层」面板，含碰撞线框/导航/光照探针/音频复选框。
- 建议：定义 DebugOverlay 图层注册表，F3 统一开关；先补导航覆盖层（收益最直接）。

### G8-4 增量打包与内容哈希（第 13 项）

- [~] 现状：打包产出 `game.pack` + FNV-1a 校验（`packager.cpp:784`）+ `update.json` 更新管线 ✅。
- [x] 增量打包——已落地 `pack_manifest.json` 每文件内容哈希清单（FNV-1a，size/hash 以字符串存储规避 JsonWriter 的 %g 精度问题）；`PackProject` 对比新旧哈希，内容/版本/updateUrl 未变时跳过 game.pack 重建（`report.unchanged`，`PackConfig.force` 可强制重打）。单容器格式暂不支持文件级增量写回（需要格式升级），当前为"未变更零重打、变更才全量重建"。单元测试 `tests/test_packager.cpp`（未变更跳过 / 内容变更重建 / 版本变更重建 / force 强制）。2026-08-25。
- [ ] 分布式构建农场（多机并行打包）——无。
- 建议：资产清单记录每文件内容哈希，打包器对比新旧哈希只处理变更项；农场可先复用 CI 分发（Windows/Ubuntu/macOS 已有）。

## 优先级建议（供讨论）

1. **G1-2 空间索引**——唯一影响架构根基的缺口，直接决定大世界/万级实体可行性。
2. **G5-2 依赖图任务调度器**——与 G1-2 一起做（任务图 + 数据并行），是"数百核"与 ECS 演进的地基。
3. **G2-3 地形进阶**（Layer Blend + chunked LOD）——工作量可控，编辑器体验与场景表现提升明显。
4. **G7-1 VFS + Mod 挂载**——平台化/资源市场/Mod 生态的前置，优先级高于原生插件。
5. **G4-1/G5-1 原生二进制插件**——把"微内核"从编译期落到运行时，先以物理/音频做示范。
6. **G8 系列**（Profiler 环形缓冲、可视化覆盖层、增量打包）——见效快、风险低，可与主线并行。
7. **G3 系列**（LLM / PCG / 视频）——待上述稳定后再碰；LLM 需先设计外部服务边界。
