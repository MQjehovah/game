# NeonEngine 与 Godot 差距分析及改进路线

> 目标：以"类《魔兽世界》大型 3D 网络游戏"为产品目标，对照 Godot 4.x 的成熟能力，
> 找出 NeonEngine 的真实差距，并给出可执行的改进方案与验收标准。

## 0. 实施状态（截至 2026-08-24）

| 项目 | 状态 | 说明 |
| --- | --- | --- |
| P0-2 Vulkan 灰度 | 完成 | 采样器 set/binding 与管线布局对齐；移除旧 exposure hack；`--backend vulkan` 与 GL 逐像素一致 |
| P0-3 资源生命周期 | 完成 | AssetManager 引用计数 + 延迟回收；Chunk 卸载释放；ObjectPool |
| P0-1 Jolt 物理 | 完成 | vendored Jolt v5.0.0；JoltWorld（刚体/碰撞层/射线/角色控制器）；需 GCC>=9（已升级 MinGW 16.2） |
| P1-3 动画 | 完成 | BlendSpace1D/2D、两骨骼 IK、Lua Tween、.anim.json clip 时间线编辑器 |
| P1-1 组 | 完成 | `groups` 组件 + `GetEntitiesInGroup` + 编辑器 schema |
| P1-1 场景继承 | 完成 | `extends` 父场景 + 同名实体覆盖合并 + 编辑器"另存为子场景" |
| P1-1 节点类型表 | 完成 | nodeType 类型表 + 相机组件 + 以选中相机为视图 |
| P1-1 动画时间线 | 完成 | 数据驱动 clip JSON + 关键帧/播放头编辑面板 |
| P1-1 地形编辑 | 完成 | 高度图组件 + 笔刷雕刻 + 运行时按高度建网格 |
| P1-1 2D tilemap | 完成 | tilemap 组件 + 网格涂色面板 + 运行时逐格渲染 |
| P1-1 导入管线 | 完成 | 资产目录 watch 自动刷新 + 场景引用资产 mtime 自动重导入 |
| P1-1 shader/材质 | 完成 | 材质球已有；新增自定义片元 shader 文件 + 热重载面板（GL；Vulkan 文档化不支持） |
| P1-2 Lua 调试器 | 完成 | 断点/单步/局部变量/调用栈（顶层帧）+ 引擎绑定参考与补全 |
| P2-2 音频 | 完成 | 立体声混音、Master/Sfx/Music 总线、3D 空间音效、WAV 加载、Lua 绑定 |
| P2-3 2D 路径 | 完成 | 精灵 z 排序、场景相机实体（Camera3D）、tilemap |
| P2-6 工程质量 | 完成 | `--bench` 基准日志、shader 热重载、资产 watch |
| P2-1 渲染表现 | 部分 | 贴花（地面投影）完成；SSAO、体积雾、GI 需新渲染通道（深度贴图/后处理链），GPU 粒子需改实例化渲染模型——留待下一阶段按序推进 |
| P2-4 网络生产化 | 完成 | MsgRpc + RpcDispatcher + 服务器房间 + 反作弊（输入限速/封禁/admin.kick/ban/world.hash 校验和）+ 客户端/脚本 Rpc 绑定 |
| P2-5 平台 | 完成（可落地部分） | Windows 图标 + 版本资源、打包发布件（update.json/install.bat/update.bat）、CI 覆盖 Windows/Ubuntu/macOS + Sanitizer；WASM/WebGPU 与 macOS/Linux 实机验证需外部工具链/硬件，留待后续 |

构建注意：Jolt 需要新工具链，工程现用 MSYS2 MinGW-w64 GCC 16.2（`C:\msys64\mingw64\bin`，
已加入用户 PATH）。旧 MinGW 8.1 无法编译 Jolt。

## 1. 现状快照

NeonEngine 是一个 C++17 自研引擎，核心约 1.9 万行、编辑器约 1.1 万行、测试 500+ 项。
与 Godot 相比，它选择了**不同的赛道**：确定性权威服务器 + 数据驱动工具链，而不是
通用编辑器优先。因此对比时应按"MMO 目标是否受益"来取舍，而非逐项对齐。

### 1.1 已有能力（相对 Godot 的强项）

| 能力 | 说明 |
| --- | --- |
| 确定性模拟 | 服务器权威模拟 ≡ 客户端本地预测逐位一致（`tests/test_determinism.cpp`），Godot 不具备 |
| 网络层 | UDP 可靠通道、快照插值、预测回滚、AOI 九宫格、多玩家输入路由，MMO 方向比 Godot 内置更贴合 |
| 数据驱动工具链 | 编辑器 → 打包 `game.pack` → 通用播放器闭环，Godot 的导出/运行链路之外自成一套 |
| 固定 60Hz tick | 服务端友好 |
| 脚本/行为树 | Lua 5.4 确定性沙箱 + 行为树引擎 + 按实例捕获 |
| CJK | 系统字体动态字形，中文开箱即用 |
| 体积 | 自研代码量小、自包含，易于改造（Godot 数百万行，改内核门槛高）；Jolt 为 vendored 第三方 |
| 物理 | Jolt v5.0.0（Godot 4 同款）——刚体/碰撞层/角色控制器开箱即用 |
| 工程闭环 | 545 项测试 + 多平台 CI + 打包发布（game.pack + 安装/更新脚本） |

### 1.2 模块规模（粗略行数）

| 模块 | 行数 | 模块 | 行数 |
| --- | --- | --- | --- |
| gfx（渲染） | ~5.5k | script | ~2.8k |
| scene | ~4.3k | editor（含 ImGui） | ~12k |
| core | ~2.1k | net | ~1.6k |
| assets | ~1.6k | ui | ~1.6k |
| bt | ~1.3k | anim | ~0.8k |
| physics | ~1.2k + Jolt(~9.5万行 vendored) | audio | ~0.4k |

物理已从"能跑 demo"升级为与 Godot 同库的 Jolt；动画/音频从 demo 深度提升为可用工具链
（BlendSpace/IK/Tween/时间线、立体声总线/3D 音效）。

## 2. 差距对照（Godot 4.x vs NeonEngine）

> 以下为 2026-08-24 完成 P0/P1/P2 大部分项之后的"当前差距"。与前版相比，
> 物理（Jolt 同库）、Vulkan 后端、动画、Lua 调试器、编辑器工作流、音频总线/3D、
> 2D 排序/tilemap、网络 RPC/房间/反作弊、资源生命周期、打包发布均已完成。

| 领域 | Godot 4.x | NeonEngine（当前） | 差距 |
| --- | --- | --- | --- |
| 物理 | Jolt 全家桶（刚体/角色/关节/车辆/软体/Area/形状查询） | Jolt v5.0.0 同款：刚体、碰撞层/掩码、角色控制器（CharacterVirtual）、射线/接触；未封装关节/车辆/软体/Area | 小→中（核心同库；缺上层封装与编辑器调试工具） |
| 渲染 | Vulkan/Metal/D3D12/GL；SDFGI/VoxelGI/光照贴图、SSAO、SSR、TAA、体积雾、任意表面贴花、GPU 粒子 | GL3.3 PBR + CSM + IBL + HDR/Bloom/ACES/MSAA；Vulkan 已与 GL 逐像素一致；地面贴花；粒子 CPU 合批 | 中→大：缺 SSAO/SSR/TAA/体积雾/GI/GPU 粒子/任意表面贴花 |
| 动画 | AnimationPlayer/Tree、BlendSpace、Tween、IK、重定向、时间线编辑器 | 状态机+过渡、BlendSpace1D/2D、两骨骼 IK、Lua Tween、.anim.json 时间线编辑器；无重定向/多轨时间线 | 中 |
| 脚本 | GDScript 全功能编辑器（断点/单步/变量/调用栈/补全/文档/远程调试） | Lua 5.4 + 编辑器断点/单步/局部变量/调用栈（顶层帧）/绑定参考/前缀补全；无远程调试/完整调用栈/悬停文档 | 中 |
| 编辑器 | 完整节点系统 + 检查器 + 场景继承/组 + 动画/地形/tilemap/着色器编辑器 + 导入 watch | ImGui：场景继承、组、节点类型、动画时间线、地形笔刷、tilemap、脚本+调试器、shader 热重载、资产 watch | 中（骨架齐全，打磨与深度差一截） |
| UI | Control 节点树 + 锚点/容器/主题 + 可视化 UI 编辑器 | 自研控件树 + 数据驱动 UI 文档 + ImGui 工具层；无可视化 UI 编辑器 | 中 |
| 音频 | 3D 空间、总线/效果器、流式 | 立体声混音、Master/Sfx/Music 总线、3D 空间音效、WAV 加载；无效果器/流式 | 中 |
| 2D | 完整 2D（sprite/tilemap/光照/粒子/物理） | sprite、z 排序、tilemap、2D 相机实体；无 2D 光照/2D 粒子/2D 物理工具 | 中 |
| 网络 | 高层多人同步器 + WebSocket/WebRTC | 自研 UDP 权威 + 确定性 + 快照/AOI/预测 + RPC/房间/反作弊——对 MMO 目标更强；缺 WS/TLS 传输 | 强项 |
| 资源 | Resource 引用计数/导入/重导入/`user://` | 引用计数 + 延迟回收、资产 watch/重导入、ObjectPool；无 `user://` 存档抽象 | 中→小 |
| 平台 | 全平台导出 + 安装器/自动更新 | Windows 优先（图标/版本资源/打包发布件/自动更新脚本）+ CI 覆盖 Win/MSVC、Ubuntu、macOS、Sanitizer；无 WASM/WebGPU | 中（Windows 已自洽） |
| 工程质量 | 成熟（测试/文档/插件生态） | 545 项测试 + 多平台 CI + `--bench` 基准 + 崩溃处理 + 热重载 | 中（无插件生态/文档体系） |

### 2.1 仍存在的关键差距（按 MMO 目标收益排序）

1. **渲染管线深度**（收益最高）：SSAO → 体积雾 → 光照贴图/GI 是"大型 3D 网络游戏"
   观感的主要分水岭；需要先补"深度贴图 + 后处理链"基建（当前 HDR 目标深度是 RBO，
   无法在 shader 中采样）。GPU 粒子与任意表面贴花次之。
2. **编辑器打磨**：骨架已齐，缺的是"体验"——多选/复制粘贴/层级拖拽、动画多轨时间线、
   地形笔刷实时预览、tilemap 调色板拖放、UI 可视化编辑器。这些是纯编辑器投入，收益直观。
3. **脚本体验**：Lua 调试器补完整调用栈、悬停文档、自动补全作用域感知、远程调试
   （编辑器与运行中进程通过内存/网络通道连接）。
4. **音频/2D 纵深**：效果器（低通/混响）、流式背景音乐；2D 光照与 2D 物理工具。
5. **平台扩张**：WASM/WebGPU（需要 Emscripten 工具链与独立后端）、macOS/Linux 实机验证、
   TLS/WebSocket 传输。

### 2.2 建议的下一阶段顺序

```text
1. 渲染后处理链基建（深度贴图 + 全屏 pass 框架）→ SSAO → 体积雾 → 光照贴图/GI
2. 编辑器体验迭代（多选/层级拖拽/动画多轨/UI 编辑器）
3. Lua 调试器补全（完整调用栈/悬停文档/远程调试）
4. 音频效果器/流式 + 2D 光照
5. 平台：WASM/WebGPU、TLS/WebSocket、实机验证
```

## 3. 改进路线

> 本节为 2026-08-24 前的历史计划：P0/P1 与 P2 大部分已按第 0 节状态落地，
> 剩余未完成项见 2.1/2.2（渲染后处理链、编辑器打磨、脚本远程调试、音频/2D 纵深、平台扩张）。
>
优先级按"MMO 目标收益 / 成本"排序。每项含：目标、现状、实施方案、验收标准、涉及模块。

---

### P0-1 物理引擎：接入 Jolt

**目标**：用 Jolt（Godot 4 同款、MIT、C++）替换自研简易物理，获得刚体/角色控制器/
碰撞层/关节/形状查询等 MMO 所需能力。

**现状**：`engine/src/physics/physics.cpp` 约 400 行，只支持动态球 vs 静态 AABB；
场景 `rigidbody` 组件和 Lua `Raycast` 已接这层接口。

**实施方案**：

1. 引入 Jolt（CMake `FetchContent` 或 vendored），仅编入需要模块（Core/Physics/
   Collision；不要 Vehicle/SoftBody 以控体积）。
2. 保持现有 `physics::World` 抽象不变，新增 `JoltWorld` 实现：
   - 把 `physics::Body`/`Shape` 映射为 `JPH::Body`（sphere/box/capsule/convex）；
   - `rigidbody` 组件 schema 的字段（shape/radius/halfExtents/dynamic/mass/
     restitution/friction/damping/gravityScale）映射到 Jolt 设置；
   - `Raycast`、重叠查询、固定 60Hz `Step` 都走 Jolt。
3. 补碰撞层/掩码：场景组件加 `layer`/`mask` 字段，对应 Jolt `ObjectLayer`。
4. 角色控制器：新增 `CharacterBody` 组件（`character` schema），Jolt
   `CharacterVirtual` 或 `Character` 提供移动/落地/碰撞反馈。
5. 确定性策略（重要）：
   - Jolt 在同一平台/编译器/构建选项下确定；跨架构（x86 vs ARM）不保证逐位一致。
   - 服务器与客户端使用相同构建产物时，用现有确定性测试套件验收；
   - 若需跨架构确定性，保留一个"确定性轻量物理"层（球/AABB）作为服务器回退，
     或把 Jolt 的 float 结算纳入确定性测试门禁。
6. 删除/降级自研物理实现，保留接口兼容。

**验收**：

- `test_physics.cpp` 全部通过且扩展：球-盒碰撞、胶囊体、旋转刚体、关节、碰撞层过滤、
  角色控制器落地/爬坡；
- 服务器权威模拟的确定性测试仍逐位一致（至少同一构建产物内）；
- `neon_realm` 的狼群/技能命中在新物理下表现不退化。

**涉及模块**：physics、scene（rigidbody/character schema）、script（绑定）、
tests。

---

### P0-2 Vulkan 后端修复（灰度问题）

**目标**：让 Vulkan 后端渲染与 GL 一致（顶点色/材质/贴图正确），可作为 GL 的稳定备选。

**现状**：`engine/src/gfx/vulkan/vk_backend.cpp` 可构建/运行/截图，但画面灰度。
顶点布局看起来是对的（`Vertex3D` 80 字节、`static_assert`、pipeline stride=80，
`vk_backend.cpp:2512` 与 `lit.vert` 的 location 0-3 对应）。

**排查清单（按概率排序）**：

1. **材质/贴图描述符**：`vk_backend.cpp:1530` 的 `bindings1`（set 1）是否绑定到
   `lit.frag` 的贴图采样器？检查 `lit.frag` 里 `texture(...)` 的 `set/binding` 是否与
   描述符集一致；灰度通常=只有光照无贴图，或 PBR 参数全 0。
2. **颜色写掩码/混合**：`VkPipelineColorBlendStateCreateInfo` 的 writeMask 与混合；
   GL 后端开启混合/颜色写，Vulkan 端若默认 0 会全黑/全灰。
3. **光照 UBO 偏移**：`engine_ubo.glsl` 与 `vk_backend.cpp` 的
   `kVkUniformOffsets` 是否逐字段一致（`kUniformBlockSize=5568`）。任何一处错位都会
   让材质参数读到垃圾（表现为无 PBR）。
4. **渲染顺序/深度**：`PrepareDraw` 里 depth test/write、load/clear 行为是否与 GL 对齐。
5. **截图路径**：`--screenshot` 是否在合成后读帧，确认灰度不是截图路径问题。
6. 用 RenderDoc 抓一帧：看顶点输入、描述符、管线状态，直接对照 GL 帧。

**验收**：`--backend vulkan --screenshot out.png N` 与 GL 同帧画面肉眼一致；
冒烟测试在 vulkan 后端下通过；`neon_realm` 在 Vulkan 下可玩。

**涉及模块**：gfx/vulkan、gfx/renderer（状态路由）、shaders。

---

### P0-3 资源生命周期与流式内存

**目标**：GPU/CPU 资源引用计数 + 可卸载，支撑大世界 chunk 流式。

**现状**：`AssetManager` 有缓存，但生命周期=应用生命周期（README 已注明）；
chunk 卸载只删场景实体，GPU 资源不回收。

**实施方案**：

1. 资源句柄引入引用计数（`std::shared_ptr` 或自研 RC），纹理/网格/材质球统一走
   `AssetHandle`；
2. 渲染器侧维护 GPU 资源缓存：引用归零后延迟一帧回收（避免还在用的帧）；
3. chunk 卸载时释放实体引用的资产；
4. 对象池/内存 arena（路线图 M3 已有此项）：实体、网络消息、粒子复用。

**验收**：反复进出 chunk 窗口后，`AssetManager::Stats` 的已加载纹理/网格数不再单调
增长；内存峰值有界；性能面板显示回收计数。

**涉及模块**：assets、gfx（renderer/backend）、scene（ChunkStreamer）、ecs。

---

### P1-1 编辑器工作流向 Godot 靠拢

**目标**：让"内容生产"成为引擎的核心竞争力，而不是只依赖手写 JSON。

**实施方案**：

1. **场景继承**：场景文件支持 `extends` 父场景 + 实例覆盖（现在只有 prefab 引用），
   编辑器提供"另存为子场景"；这是 Godot 场景体系的核心。
2. **组（groups）**：实体/组件可加 `groups` 字段，脚本按组查询
   （`GetEntitiesInGroup("enemy")`），运行时与编辑器均可编辑。
3. **内置节点类型**：把"类型"从网格键扩展为节点类型表
   （Node / Camera3D / MeshInstance3D / CharacterBody / Sprite / ...），属性面板
   按类型渲染，替代现在的"类型跟着 meshKey 走"。
4. **动画时间线编辑器**：一个 ImGui 面板可视化动画状态机/剪辑（现在只能手写 JSON）；
   支持选实体 → 显示其动画 → 拖关键帧/调过渡。
5. **地形/世界编辑**：高度图刷子（提升/平滑/噪声）、碰撞/出生点/NPC 放置、写入
   chunk 数据格式。
6. **2D tilemap 编辑器**：把 PvZ 网格编辑器泛化成通用 tilemap（笔刷/图层/碰撞）。
7. **着色器/材质**：材质球已有；补 shader 热重载 + 简单参数化着色器面板。
8. **导入管线**：资产目录 watch + 自动重导入（现在手动导入）；保留 glTF/OBJ 路径。

**验收**：用纯编辑器操作完成"创建一个会巡逻的 NPC（场景继承 + 组 + 动画 + 脚本）"，
全程不手写 JSON。

**涉及模块**：editor、scene（格式扩展）、assets（watch）、anim（编辑器）。

---

### P1-2 Lua 调试器与补全

**目标**：Godot GDScript 编辑器最大的生产力杠杆——调试体验。

**实施方案**：

1. Lua 5.4 自带 `debug` 库：编辑器通过 `debug.sethook`（line/count）实现断点与单步；
2. 脚本编辑器增加：断点切换（行号槽位）、运行到光标、变量查看（`debug.getlocal`）、
   调用栈（`debug.getinfo`）；
3. 编辑器与运行中游戏通过内存通道（或现有网络层）连接，实现在线调试；
4. 自动补全：解析项目脚本的全局函数/引擎绑定表，做简单词法补全 + 悬停文档。

**验收**：demo 脚本里下断点 → 单步 → 查看变量 → 修改后热重载继续；
补全能提示引擎绑定函数签名。

**涉及模块**：editor（脚本编辑器）、script（host 调试接口）、net（调试通道）。

---

### P1-3 动画系统补全

**目标**：角色动作表现（BlendTree、Tween、IK）。

**实施方案**：

1. BlendTree/BlendSpace：动画状态机节点增加 blend 类型（1D/2D blend space），
   数据驱动（JSON）；
2. Tween：脚本绑定 `Tween(prop, from, to, time, easing)`，覆盖位置/旋转/缩放/颜色；
3. IK：两骨骼链式 IK（手臂/腿）先行，重定向后补；
4. 动画编辑器与 P1-1-4 共用数据模型。

**验收**：角色跑→走→待机按速度混合过渡；脚本可平滑补间；NPC 手臂指向目标。

**涉及模块**：anim、script（绑定）、scene（组件）。

---

### P2 表现力与生产化

**P2-1 渲染表现**：

- 光照贴图烘焙（最实际）或 SDFGI/VoxelGI 作为远景；SSAO；GPU 粒子；体积雾；贴花。
- 顺序：SSAO → GPU 粒子 → 体积雾/贴花 → GI。
- 验收：场景截图在 AO/雾/粒子上有可感知提升；帧时间预算内。

**P2-2 音频**：

- 3D 空间音频（衰减/多普勒/房间混响）、总线/效果器、流式背景音乐。
- miniaudio 已具备底层能力，主要补抽象层与场景组件。

**P2-3 真实 2D 路径**：

- Sprite 组件已有雏形（spriteTex/翻转）；补 z 排序、tilemap、2D 光照、2D 相机；
- 让 2D 游戏不再依赖"全 Lua 画布"，而是场景组件。

**P2-4 网络生产化**：

- RPC 风格高层 API（`Rpc(name, args)` 自动序列化）；
- matchmaking/房间、WebSocket/TLS 传输、副本服务器（路线图 M4 已列）；
- 反作弊：服务端权威 + 确定性校验已具备，补行为校验与封禁。

**P2-5 平台与导出**：

- WebAssembly/WebGPU 作为未来拓展；macOS/Linux 实机验证（现在 X11/Cocoa 只 CI 编译）；
- 安装包/图标/自动更新管线。

**P2-6 工程质量**：

- shader 热重载；自动化基准（帧时间/内存曲线）；崩溃上报；日志汇聚；
- 内存 arena/对象池（与 P0-3 合并推进）。

---

## 4. 里程碑依赖与顺序

```text
P0-1 物理(Jolt) ──► P1-1 编辑器工作流 ──► P1-3 动画 ──► P2 表现
P0-2 Vulkan    ──► P1-2 Lua 调试器        ──► P2-4 网络生产化
P0-3 资源生命周期 ──► P2-5 平台 ──► P2-6 工程质量
```

建议顺序：**先 P0-1 + P0-3**（物理与内存是 MMO 的地基），**P0-2 按需**（若 GL 在
目标平台足够稳定可降级为低优先级）；随后 **P1-1 编辑器**（产能杠杆最高），
**P1-2 调试器**与 **P1-3 动画**并行；P2 逐项按游戏需求取舍。

## 5. 风险与取舍

- **Jolt 确定性**：跨架构不保证逐位一致。策略见 P0-1-5；若确定性是硬约束，
  可保留轻量自研物理作为服务器回退。
- **Vulkan 修复成本**：可能与 GL 长期并存；若团队资源有限，优先保证 GL 稳定，
   Vulkan 修复放在"渲染质量"之后。
- **编辑器投入产出**：ImGui 编辑器继续投入 vs 换用 Godot 作为编辑器前端（引擎导出
  到 Godot？）——不建议后者，改造成本高于补齐编辑器。
- **不追 Godot 的全平台广度**：软体、SDFGI、全平台导出按需后补，避免分散。
