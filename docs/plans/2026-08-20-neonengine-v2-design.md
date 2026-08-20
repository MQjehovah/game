# NeonEngine v2 设计：数据驱动游戏创作工具链（含 P0–P4 全量）

日期：2026-08-20
状态：已与用户确认（脚本后端 = Lua 为主 + IScriptHost 多后端）

## 1. 目标与愿景

把 NeonEngine 从"硬编码 demo + 编辑器"升级为**游戏创作工具链**：

- 内容全部数据驱动（场景/预制体/行为树/脚本），编辑器内可编辑一切。
- 编辑器内 **F5 即时试玩**（无需重编译）。
- **一键打包**出独立可运行的游戏（`neon_game.exe` + `game.pack`），双击即玩、可分发。
- 同步完成渲染（阴影/HDR/IBL/Vulkan）、平台（miniaudio）、大世界（分区流式/ECS 并行）、网络（客户端/服务器）演进，覆盖 ROADMAP M1–M4 与编辑工具 M5。

## 2. 关键决策记录

| 决策 | 理由 |
| --- | --- |
| 脚本：Lua 5.4 vendor 静态编译为主后端 | ~300KB、C API 稳定、零运行时依赖，打包产物自包含，契合项目零第三方依赖理念 |
| 脚本抽象：`IScriptHost` + 多后端 | Python/JS 经同一接口作为可选后端，运行时动态加载，缺失自动降级到数据驱动+行为树 |
| "编译游戏" = 打包数据+资产 | 玩法逻辑全在脚本/行为树/数据，通用 `neon_game.exe` 播放器运行任意数据驱动游戏，无需重编 C++ |
| 行为树可视化引擎 | 提供无代码编辑逻辑的可视层；脚本用于需要任意逻辑的场合 |
| 网络：固定 tick + 确定性模拟 + 版本化序列化 | 客户端/服务器同构（复用同一 ECS+模拟系统），为快照/预测/回滚打基础 |
| 物理：维持轻量内置（球/胶囊 vs AABB），保留 Jolt 替换接口 | 本期不引入重型依赖 |

## 3. 架构变更

### 3.1 新增引擎模块

#### script（`engine/script`）
- `IScriptHost`：加载/执行/绑定/热重载的纯接口；`ScriptManager` 管理多后端。
- Lua 后端（vendor `third_party/lua`）：`lua_State` 封装、错误上报（stderr + 引擎日志）、每场景沙箱状态、绑定注册表。
- 绑定层：注册 ECS（spawn/destroy/get/set 组件）、场景、物理（射线/施加力）、音频（播放）、渲染（相机/特效）、行为树（启动/信号）、游戏变量（GameVars）、JSON。
- 可选后端：`python`/`js` 动态加载（`LoadLibrary`/`dlopen`），实现同一 `IScriptHost`；缺库时 `ScriptManager` 报告"后端不可用"并降级。
- 确定性要求：脚本内 `os.clock`/`math.random` 等非确定调用做沙箱替换（提供确定性 RNG），保证服务器/客户端一致。

#### bt（`engine/bt`）
- 节点类型：
  - 组合：`sequence` / `selector` / `random_selector` / `parallel`（门限成功数）
  - 装饰：`invert` / `cooldown` / `repeat` / `until_fail` / `blackboard_set`
  - 行为：`move_to` / `attack` / `dialogue` / `spawn` / `wait` / `play_sfx` / `run_script`
  - 条件：`in_range` / `has_target` / `quest_state` / `health_below` / `blackboard_cmp` / `gamevar_cmp` / `script_bool`
- `Blackboard`（实体级键值）+ `GameVars`（全局键值）两种上下文。
- JSON 序列化（`.bt.json`），编辑器可视化编辑，运行时 `BehaviorTree::Tick(dt, ctx)` 执行。
- 每个可挂载实体一个行为树实例；支持树间信号（`blackboard_set` + 条件联动）。

#### anim（`engine/anim`）
- glTF 蒙皮导入：accessor 解析 JOINTS/WEIGHTS、skin（inverseBindMatrices、joints）、节点骨骼层级。
- 动画通道采样：translation/rotation/scale keyframes，线性/STEP/CUBICSPLINE 插值。
- GPU 蒙皮：每骨骼 `jointMatrix` 传入 vertex shader（uniform 数组，上限 64 骨骼）。
- `Skeleton`/`Pose`/`AnimationClip`；`Animator`（动画状态机：clip、参数、状态过渡、1D/2D 混合），`AnimationSystem` 每帧更新实体 Pose。

### 3.2 数据驱动场景格式

场景 JSON 从"位置/缩放/颜色"扩展为**组件化**：

```json
{
  "entities": [
    {
      "name": "Wolf",
      "prefab": "wolf",
      "components": {
        "transform": {"pos": [..], "rot": [..], "scale": [..]},
        "mesh": {"meshKey": "gltf:assets/wolf.glb", "material": {...}},
        "rigidbody": {"radius": 0.6, "type": "dynamic"},
        "animator": {"stateMachine": "sm:wolf", "initialState": "idle"},
        "behaviorTree": {"tree": "bt:wolf_ai"},
        "script": {"backend": "lua", "path": "scripts/wolf.lua", "vars": {...}},
        "health": {"hp": 50, "maxHp": 50},
        "spawner": {...}, "pickup": {...}, "dialogue": {...}
      }
    }
  ],
  "gameVars": {...}
}
```

- **Prefab**（`prefabs/*.json`）：组件模板，场景实体可 `prefab` 引用 + 实例级覆盖。
- **清单 `game.json`**：起始场景、窗口/分辨率设置、资源列表、启动脚本。

### 3.3 编辑器 → 试玩 → 打包闭环

```
编辑项目目录 project/
  scenes/*.json
  prefabs/*.json
  behaviors/*.bt.json
  scripts/*.lua (.py/.js)
  assets/**            (gltf/obj/png/wav...)
  game.json
```

- **F5 试玩**（编辑器内）：在编辑器进程中启动隔离的游戏运行时（独立 `Application`/World/物理/渲染相机），加载当前场景 + 脚本 + 行为树；编辑状态不污染试玩世界；Stop 销毁。工具栏 Play/Stop/Pause。
- **一键打包** `--package <project> --out <dir>`：
  1. 校验（缺失资产/脚本语法/行为树引用）。
  2. 收集全部资源 → `game.pack`（自研容器格式：目录树 + zlib 块 + 文件表/哈希/版本）。
  3. 拷贝 `neon_game.exe`（预编译通用播放器）与 `neon_runtime.dll`（若动态库）。
  4. 生成 `run.bat` / shell 脚本。
- **`neon_game` 播放器**：无玩法硬编码，读取 `game.pack` → 进入 `game.json` 指定场景 → 运行。即"任意数据驱动游戏共用同一个播放器"。

### 3.4 渲染演进（M1）

- 阴影贴图：方向光 **CSM**（3 级 cascade）+ 点光 cubemap；FBO 深度渲染（修复/绕过 Intel 驱动 VAO 缺陷，提供 `--disable-fbo` 回退 CPU 接触阴影）。
- 后处理：**HDR + Bloom**（降采样/升采样 + 高斯模糊）、色调映射（ACES）、可选 MSAA。
- **IBL**：天空渐变 → 预过滤环境贴图（diffuse irradiance + prefiltered specular + BRDF LUT），离线或启动时生成。
- **Vulkan 后端**：实现冻结的 `IRenderBackend`；与 GL 后端共享渲染器高层；CI 一致性冒烟（同帧截图对比灰度/哈希）。

### 3.5 平台与性能（M3）

- 音频：miniaudio（vendor）替换 WinMM/Null，三平台统一。
- 资产：纹理压缩（BC1-7 / ASTC，vendor stb_dxt 或预压缩）、异步解码线程池、LOD 资产链（`.lod` 清单）。
- 世界分区：chunk（默认 64×64）按玩家位置异步加载/卸载；场景实体以 chunk 存储，进入/离开触发加载回调。
- ECS 演进：archetype 存储 + 批量迭代 + job/并行调度（无数据竞争保证：组件版本号 + 只读视图共享）；确定性快照（`SerializeWorld`）。

### 3.6 网络化（M4）

- 传输：UDP + 序列号 + ACK/重传；消息编解码用版本化二进制（继承核心序列化）。
- 服务器：`neon_server`（headless，无窗口无渲染），复用同一 ECS+模拟系统，权威物理/战斗，固定 tick 60Hz。
- 客户端：快照插值 + 输入预测/回滚；AOI 兴趣管理（九宫格/视野掩码）控制同步量。
- 账号/登录/角色选择：占位协议（版本 0：单机局域网 demo），预留扩展。
- 确定性：固定 tick、确定性 RNG、Lua 沙箱非确定函数替换、浮点约定统一。

### 3.7 编辑器深化与工程

- 编辑器：gizmo（ImGuizmo，平移/旋转/缩放）、撤销/重做（命令模式栈）、材质编辑器（贴图槽/金属度/粗糙度/自发光）、多相机视口、资产缩略图、热重载（shader/脚本/资产）。
- 新增面板：**玩法对象**（游戏对象树 + 组件增删改）、**行为树编辑器**（节点画布、拖拽连线、参数面板、实时调试高亮）、**脚本面板**（附加脚本、配置变量、错误列表）、**试玩工具栏**、**打包面板**（目标目录/选项/进度/日志）。
- 工程：git 初始化（P0）、CI 增补（Lua vendor 编译、packaging 冒烟、Vulkan 编译开关）。

## 4. 数据流（F5 试玩一帧）

```
编辑器 Play → 创建 GameRuntime（隔离 Application）
  SceneJSON 反序列化 → World（组件化实体，prefab 展开）
  挂载 Animator（每实体状态机） / BehaviorTree / ScriptHost(Lua)
OnUpdate(dt)：
  PlayerSystem → MovementSystem → Physics::Step
  BehaviorTreeSystem（tick 行为树，驱动 AI/任务/对话/生成）
  ScriptSystem（Lua 事件：on_start/on_update/on_hit/on_dialogue）
  AnimationSystem（采样骨骼 + ASM 过渡）→ AnimSyncSystem
  WaveSystem / QuestSystem（读 GameVars）
OnRender：相机 → CSM 阴影 pass → 场景(GPU 蒙皮) → 粒子/线框 → 后处理(HDR+Bloom) → HUD
Stop → 销毁 GameRuntime，编辑器状态不受影响
```

## 5. 执行阶段与验证检查点

| 阶段 | 内容 | 验证 |
| --- | --- | --- |
| P1 地基 | git init；测试扩展（物理/序列化/OBJ/glTF 回环）；版本化序列化；pack 容器格式 | `neon_tests` 全绿；pack 打包/解包回环测试 |
| P2 内容核心 | Lua vendor + IScriptHost + 绑定；行为树引擎 + JSON；组件化场景/prefab；编辑器→游戏数据转换 | 单测：脚本执行/绑定、行为树 tick 序；编辑器可摆实体并 F5 跑 |
| P3 表现 | 骨骼动画+ASM；CSM 阴影；HDR/Bloom；IBL | 单测：蒙皮矩阵/动画采样；截图对比（阴影/泛光） |
| P4 编辑器深度 | gizmo/undo/材质编辑器/行为树可视化/脚本面板/试玩工具栏/打包按钮 | 冒烟测试扩展：编辑→F5→打包→运行全链路 |
| P5 平台性能 | miniaudio；纹理压缩+异步；chunk 流式；ECS archetype/job | 三平台编译；流式加载卸载测试；性能基线 |
| P6 网络 | 协议/编解码；neon_server；快照/预测/AOI | 编解码单测；局域网双进程 demo（确定性回放一致） |
| P7 Vulkan+集成 | Vulkan 后端 + 一致性冒烟；全链路验收 | 编辑→试玩→打包→在无开发环境机器运行 |

## 6. 测试策略

- 单元：Lua 执行/绑定、行为树 tick 结果、蒙皮矩阵、动画插值、序列化回环、pack 格式、物理、网络编解码。
- 集成：headless 试玩（跑 N tick 断言状态）、打包→运行 player→截图校验。
- 确定性：同一输入两次模拟状态一致（含 Lua 脚本）。
- 跨后端：GL/Vulkan 同帧输出对比。

## 7. 风险

- 范围跨多阶段：严格按阶段验收，避免蔓延。
- CPython 可选后端体积/依赖：通过 IScriptHost 隔离，默认不打包。
- Vulkan 与网络为独立大块：置于 P6/P7，先保证内容闭环。
- Intel 驱动 FBO 缺陷：保留 `--disable-fbo` 回退路径，新功能不阻塞。
