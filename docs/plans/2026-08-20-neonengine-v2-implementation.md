# NeonEngine v2 实施计划（P0–P4 全量 + 创作工具链）

> **For Claude:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task.

**Goal:** 将 NeonEngine 升级为数据驱动游戏创作工具链：编辑器内可编辑场景/预制体/行为树/脚本，F5 即时试玩，一键打包独立运行的游戏；并完成渲染（阴影/HDR/IBL/Vulkan）、平台（miniaudio）、大世界（流式/ECS 并行）、网络（客户端/服务器）演进。

**Architecture:** 新增 `script`（IScriptHost，Lua 主后端 vendor）/ `bt`（行为树）/ `anim`（骨骼动画）模块；场景 JSON 组件化 + prefab + game.json 清单；`neon_game` 通用播放器运行 `game.pack`；`neon_server` 复用同一套 ECS+模拟系统；渲染器高层与 GL/Vulkan 后端解耦。

**Tech Stack:** C++17、CMake ≥3.15、自研 JSON/GL/资源管线、Lua 5.4（vendor）、Dear ImGui（编辑器）、miniaudio（音频）、Vulkan（P7）。

**约定**：每个任务遵循 TDD（先写测试→跑失败→实现→跑通过→提交）；提交信息用仓库已有风格；所有模块归 `neon_engine` 库或新库，统一 `neon/` 头文件路径。

---

## P1 地基

### Task 1.1: 测试框架扩展（依赖注入 + 断言增强）

**Files:**
- Modify: `tests/test_main.cpp`（注册表已有，补 `CHECK_NEAR`/`CHECK_THROW`/子测试分组）
- Create: `tests/helpers.hpp`（内存文件读写、临时目录、浮点容差宏）

**Step 1:** 在 `helpers.hpp` 增加 `CHECK_NEAR(a,b,eps)` 与 `TempDir` RAII（Win32/Linux 跨平台 `mkdtemp`/`CreateDirectory`）。
**Step 2:** `neon_tests` 先编译通过，`./build/neon_tests` 现有 13 项全绿。
**Step 3:** 提交 `test: expand harness with CHECK_NEAR and TempDir`.

### Task 1.2: 版本化二进制序列化模块

**Files:**
- Create: `engine/include/neon/core/serialize.hpp`
- Create: `engine/src/core/serialize.cpp`
- Test: `tests/test_serialize.cpp`

**Step 1:** 写失败测试：`Serializer` 写入 `{u32,u64,f32,string,vector<Vec3>}` → `Deserializer` 读出相等；写入带 `version` 头，读错版本报错。
**Step 2:** 实现 `neon::core::Serialize`（流式 big-endian，长度前缀字符串/容器，`writeVersion(4)/readVersion`，`magic + CRC32` 校验）。
**Step 3:** 跑测试通过，提交 `feat(core): versioned binary serializer`.

### Task 1.3: 资产打包容器 game.pack

**Files:**
- Create: `engine/include/neon/core/pack.hpp`
- Create: `engine/src/core/pack.cpp`
- Test: `tests/test_pack.cpp`

**Step 1:** 失败测试：写入 `{a.txt,b/子目录/c.png}` → `PackWriter` 产出字节流 → `PackReader` 枚举/按路径读取字节相等；缺文件返回 `Result::NotFound`；哈希校验损坏返回失败。
**Step 2:** 实现 `PackWriter`（目录树 + 块偏移表 + zlib 压缩 + CRC32 + 魔数头）与 `PackReader`（仅读索引按需解压）。
**Step 3:** 测试通过，提交 `feat(core): pack container format`.

### Task 1.4: 补齐核心单元测试

**Files:**
- Create: `tests/test_physics.cpp`、`tests/test_obj.cpp`、`tests/test_gltf.cpp`、`tests/test_json.cpp`

**Step 1:** 物理：球下落重力、球-AABB 碰撞、射线命中、球-球分离（`physics::World`）。
**Step 2:** OBJ：MTL Kd 颜色、多组材质分组、法线回退（解析 `assets/kenney_nature` 一个模型 + 自带样例）。
**Step 3:** glTF：JSON DOM 往返、accessor/byteStride 读取、节点 TRS（用 `assets/models/DamagedHelmet`）。
**Step 4:** JSON：UTF-8 转义/代理对、深嵌套、round-trip（`core::Json` + `JsonWriter`）。
**Step 5:** 全绿后提交 `test: cover physics, obj, gltf, json`.

---

## P2 内容创作核心

### Task 2.1: vendor Lua 5.4

**Files:**
- Create: `third_party/lua/`（官方 5.4.x 源码：lua.c 除外，含 lapi.c…lvm.c、lua.h/lauxlib.h/lualib.h、lua.rc 可选）
- Modify: `CMakeLists.txt`（新静态库 `neon_lua`）

**Step 1:** 下载解压 Lua 5.4.x 源码到 `third_party/lua`，不包含 `lua.c`（交互 shell）与 `luac.c`。
**Step 2:** `CMakeLists.txt` 增加 `add_library(neon_lua STATIC third_party/lua/*.c)`，`C99`，`-DLUA_USE_...` 按平台。
**Step 3:** 提交 `build: vendor Lua 5.4 as neon_lua`.

### Task 2.2: IScriptHost + LuaHost + ScriptManager

**Files:**
- Create: `engine/include/neon/script/script.hpp`（IScriptHost、ScriptError、脚本事件签名）
- Create: `engine/include/neon/script/lua_host.hpp`
- Create: `engine/src/script/lua_host.cpp`
- Create: `engine/src/script/script_manager.cpp`
- Test: `tests/test_script.cpp`

**Step 1:** 失败测试：`LuaHost::Load("x = 10")` 后 `GetGlobal("x")` 返回 10；`Run` 抛语法错时返回 `ScriptError{line,msg}`；字符串 UTF-8 往返。
**Step 2:** 实现 `IScriptHost { Init/Shutdown/Load/Run/Call(event, args)/SetGlobal/GetGlobal/Register }`。
**Step 3:** `LuaHost`：`luaL_newstate` + 标准库选择（只开基础 + 数学/字符串/表，禁 io/os/package 用于确定性），错误经 `lua_pcall` 捕获到 `ScriptError`。
**Step 4:** 测试通过，提交 `feat(script): IScriptHost + Lua backend`.

### Task 2.3: 引擎绑定注册表

**Files:**
- Create: `engine/include/neon/script/bindings.hpp`
- Create: `engine/src/script/bindings.cpp`
- Test: `tests/test_script_bindings.cpp`

**Step 1:** 失败测试（Lua 端断言）：
```lua
local e = Spawn("wolf", {x=1, y=0, z=2})      -- 返回实体句柄
assert(GetPosition(e).x == 1)
SetPosition(e, {x=5, y=0, z=5})
assert(GetVar("gold") == nil)
SetVar("gold", 42)                              -- GameVars
assert(GetVar("gold") == 42)
assert(Raycast({x=0,y=5,z=0}, {x=0,y=0,z=0}))  -- 物理
```
**Step 2:** 实现绑定：ECS `Spawn/Despawn/GetComponent/SetComponent`、`CTransform` 字段、GameVars 键值、物理 `Raycast`、音频 `PlaySfx(name)`、行为树 `SetBlackboard(ent, k, v)`、`Json.Parse`。
**Step 3:** 测试通过，提交 `feat(script): engine bindings (ecs/physics/audio/gamevars)`.

### Task 2.4: Lua 确定性沙箱

**Files:**
- Modify: `engine/src/script/lua_host.cpp`
- Test: `tests/test_script_determinism.cpp`

**Step 1:** 失败测试：脚本用 `NMath.Random(seed)` 两次运行序列一致；`os.clock()` 返回引擎注入的固定值；`os.execute/io` 调用返回"禁用"错误。
**Step 2:** 实现：替换 `math.random`（确定性 xorshift RNG 注册表）、覆盖 `os.clock`/`os.time`、`io/os/require` 返回禁用错误。
**Step 3:** 测试通过，提交 `feat(script): deterministic sandbox for server reuse`.

### Task 2.5: 行为树引擎

**Files:**
- Create: `engine/include/neon/bt/behavior_tree.hpp`、`engine/include/neon/bt/nodes.hpp`
- Create: `engine/src/bt/behavior_tree.cpp`
- Test: `tests/test_bt.cpp`

**Step 1:** 失败测试（以 JSON 定义树）：
```json
{"root":{"type":"sequence","children":[
  {"type":"condition","name":"in_range","args":{"distance":5}},
  {"type":"action","name":"move_to","args":{"speed":3}},
  {"type":"action","name":"attack"}
]}}
```
断言：条件不满足时整树返回 FAIL 且不执行 action；满足时按序执行并返回 SUCCESS。
**Step 2:** 实现 `Node` 接口（`Tick(ctx)->Status`）、`Composite/Decorator/Behavior/Condition` 基类、`Blackboard`（实体级）+ `GameVars`（全局）上下文、JSON 反序列化（含 arg 默认值）。
**Step 3:** 实现内建节点：`move_to/attack/dialogue/spawn/wait/play_sfx/run_script`；`in_range/has_target/quest_state/health_below/blackboard_cmp/gamevar_cmp/script_bool`。
**Step 4:** 测试通过，提交 `feat(bt): behavior tree engine + json`.

### Task 2.6: 组件化场景格式 + 加载器

**Files:**
- Create: `engine/include/neon/scene/scene_file.hpp`（`SceneFile`/`EntityDef`/`ComponentDef` 结构）
- Create: `engine/src/scene/scene_file.cpp`（JSON ↔ 结构、prefab 展开、校验）
- Test: `tests/test_scene_file.cpp`

**Step 1:** 失败测试：从组件化 JSON 加载，验证 prefab 展开 + 实例覆盖（如 prefab 缩放覆盖）、未知组件字段报错、缺 `transform` 报错。
**Step 2:** 实现 `SceneFile::Load(json)` 解析 + `SceneFile::Instantiate(world, assets)` 建实体组件（transform/mesh/rigidbody/animator/behaviorTree/script/health）。
**Step 3:** 测试通过，提交 `feat(scene): componentized scene format + prefabs`.

### Task 2.7: 游戏清单 game.json

**Files:**
- Create: `engine/include/neon/scene/game_manifest.hpp`
- Create: `engine/src/scene/game_manifest.cpp`
- Test: `tests/test_manifest.cpp`

**Step 1:** 失败测试：解析 `{startScene, window:{w,h,title}, packages:[...]}`；缺 startScene 报错。
**Step 2:** 实现 `GameManifest::Load/Validate`。
**Step 3:** 测试通过，提交 `feat(scene): game.json manifest`.

### Task 2.8: 编辑器场景导出为组件化格式

**Files:**
- Modify: `editor/src/editor.cpp`、`editor/src/panels.cpp`
- Modify: `engine/src/scene/scene_file.cpp`（`SceneFile::Export(world)`）

**Step 1:** 在编辑器工具栏加"导出游戏场景"：把当前编辑实体（位置/旋转/缩放/颜色/金属度/粗糙度/网格）序列化为组件化场景 JSON 到项目目录。
**Step 2:** 新增"项目目录"配置（`editor_scene.json` 旁或菜单设置），冒烟测试校验导出→加载回环一致。
**Step 3:** 提交 `feat(editor): export scene to componentized format`.

### Task 2.9: 编辑器内 F5 试玩运行时

**Files:**
- Create: `editor/src/playtest.hpp` / `editor/src/playtest.cpp`（隔离 GameRuntime）
- Modify: `editor/src/editor.cpp`（工具栏 Play/Stop）

**Step 1:** `GameRuntime`：独立 `core::Application`（无窗口的 headless tick）或复用编辑器窗口但独立 World/渲染状态；加载当前场景 → 每帧 tick 行为树/脚本/动画/物理。
**Step 2:** 视口在 Play 模式渲染试玩世界（相机用玩家/自由相机），Stop 销毁运行时恢复编辑器场景状态。
**Step 3:** `--smoke-test` 加"载入样例场景试玩 120 tick 无崩溃"断言。提交 `feat(editor): in-editor playtest (F5)`.

---

## P3 表现

### Task 3.1: glTF 蒙皮导入

**Files:**
- Modify: `engine/include/neon/gfx/mesh.hpp`（`Mesh` 增 `joints/weights/boneWeights/rootBone` 数据 + `skinned` 标志）
- Modify: `engine/src/assets/asset_manager.cpp`（glTF skin/accessor 解析）
- Test: `tests/test_gltf_skin.cpp`

**Step 1:** 失败测试：解析含蒙皮的 glTF（生成最小 skin 夹具），校验 JOINTS/WEIGHTS accessor 分量、inverseBindMatrices 数量。
**Step 2:** 实现 `Skin` 结构 + 导入（复用现有 accessor 读取）。
**Step 3:** 测试通过，提交 `feat(gfx): glTF skin import`.

### Task 3.2: 动画采样 + 状态机

**Files:**
- Create: `engine/include/neon/anim/anim.hpp`（`Skeleton/Pose/AnimationClip/AnimationStateMachine/Animator`）
- Create: `engine/src/anim/anim.cpp`
- Test: `tests/test_anim.cpp`

**Step 1:** 失败测试：线性插值关键帧（t=0.5 双通道）、循环 wrap、状态机过渡（条件满足→切换→混合系数 0→1）、骨骼局部→全局矩阵累加。
**Step 2:** 实现 `AnimationClip`（每骨骼 TRS 通道）、`Skeleton::LocalToGlobal`、`StateMachine`（`transition {from,to,condition,duration}`）。
**Step 3:** 测试通过，提交 `feat(anim): clips + skeleton + state machine`.

### Task 3.3: GPU 蒙皮着色

**Files:**
- Modify: `engine/include/neon/gfx/renderer.hpp`、`engine/src/gfx/renderer.cpp`、`engine/src/gfx/gl/gl_backend.cpp`
- Modify: shader 资源（`assets/generated` 或内嵌 `lit` shader 加 `uBoneMatrices` + skin 输入）

**Step 1:** lit shader 增 `SKINNED` 宏：attribute `aJointIds/aWeights`，uniform `uBoneMatrices[64]`，`pos = uM*Σw*(uBoneMatrices[i]*pos)`。
**Step 2:** 渲染器为 `skinned` mesh 提交骨骼矩阵（从 Animator 取 Pose 全局矩阵）。
**Step 3:** 视口验证：带动画样例模型骨骼驱动；提交 `feat(gfx): GPU skinning in lit shader`.

### Task 3.4: 方向光 CSM 阴影

**Files:**
- Modify: `engine/include/neon/gfx/renderer.hpp`、`engine/src/gfx/renderer.cpp`
- Modify: `engine/src/gfx/gl/gl_backend.cpp`（FBO/depth 纹理支持）
- Modify: lit shader（`uShadowMap` + PCF）

**Step 1:** FBO 深度渲染测试（`--disable-fbo` 回退保留）：离屏渲染深度 → 采样比对。
**Step 2:** 实现 CSM：3 cascade（视锥切分），每 cascade 渲染深度，场景 shader 按片段深度选 cascade + 4-tap PCF。
**Step 3:** 截图对比：有阴影/无阴影像素差 > 阈值；提交 `feat(gfx): directional CSM shadows`.

### Task 3.5: 点光阴影（cubemap）

**Files:**
- Modify: renderer + gl backend（深度 cubemap + 分层渲染）

**Step 1:** 点光源 6 面深度渲染 → 场景片段使用 `uPointShadowMap` 采样。
**Step 2:** 截图验证；提交 `feat(gfx): point light shadow cubemap`.

### Task 3.6: HDR + Bloom 后处理

**Files:**
- Modify: renderer（中间 FBO 链）、lit shader（HDR 输出）
- Create: `engine/src/gfx/post.cpp`（降采样 → 高斯模糊 → 升采样合成）
- Modify: `engine/src/gfx/gl/gl_backend.cpp`

**Step 1:** 亮度阈值提取 + 两级降采样高斯 → 与主场景相加。
**Step 2:** 截图对比亮部泛光；提交 `feat(gfx): HDR + bloom post-processing`.

### Task 3.7: 色调映射 + MSAA

**Files:**
- Modify: post shader（ACES tonemap）、renderer（MSAA resolve）

**Step 1:** 实现 ACES 近似色调映射；MSAA 4x resolve 到 LDR FBO。
**Step 2:** 截图对比；提交 `feat(gfx): ACES tonemap + MSAA`.

### Task 3.8: IBL 环境光

**Files:**
- Create: `engine/src/gfx/ibl.cpp`（天空渐变 → irradiance + prefiltered specular + BRDF LUT）
- Modify: lit shader（`uIrradianceMap/uPrefilterMap/uBrdfLUT`）

**Step 1:** 启动时生成三张环境贴图（低分辨率预计算）。
**Step 2:** PBR 漫反射/镜面加环境项；截图对比非 IBL；提交 `feat(gfx): IBL environment lighting`.

---

## P4 编辑器深度

### Task 4.1: gizmo（ImGuizmo）

**Files:**
- Create: `third_party/ImGuizmo/`（ImGuizmo.cpp/h + Example 排除）
- Modify: `CMakeLists.txt`、`editor/src/editor.cpp`（选中实体显示 gizmo）

**Step 1:** vendor ImGuizmo 进 `neon_imgui`。
**Step 2:** 视口叠加 gizmo，拖拽修改选中实体 transform（与撤销栈联动）。
**Step 3:** 冒烟测试校验 gizmo 操作写回；提交 `feat(editor): transform gizmo`.

### Task 4.2: 撤销/重做

**Files:**
- Create: `editor/src/history.hpp` / `history.cpp`（命令栈）
- Modify: 面板操作接入命令

**Step 1:** `Command{Apply/Undo/Redo}` + `HistoryManager`（栈 + 合并策略）。
**Step 2:** 移动/改属性/增删实体接入；Ctrl+Z/Ctrl+Y。
**Step 3:** 冒烟测试：do→undo→redo 状态回环；提交 `feat(editor): undo/redo command stack`.

### Task 4.3: 材质编辑器

**Files:**
- Modify: `editor/src/panels.cpp`（材质面板：贴图槽/金属度/粗糙度/AO/自发光）
- Modify: `engine/src/scene/scene_file.cpp`（材质随场景持久化）

**Step 1:** 属性面板扩展为材质子面板 + 贴图拖入（走资产面板）。
**Step 2:** 提交 `feat(editor): material editor panel`.

### Task 4.4: 行为树可视化编辑器

**Files:**
- Create: `editor/src/bt_editor.hpp` / `bt_editor.cpp`（节点画布：绘制/拖拽/连线）
- Modify: `engine/src/bt/behavior_tree.cpp`（节点列表/参数反射，供画布读取）

**Step 1:** 画布：拖拽创建节点、连线父子、节点参数编辑、保存/加载 `.bt.json`。
**Step 2:** 运行态调试：F5 试玩时高亮当前执行节点。
**Step 3:** 提交 `feat(editor): visual behavior tree editor`.

### Task 4.5: 脚本面板

**Files:**
- Modify: `editor/src/panels.cpp`（脚本面板：项目脚本列表、附加到实体、配置 vars、错误列表）
- Modify: `engine/src/script/lua_host.cpp`（`CompileCheck(path)` 语法校验）

**Step 1:** 列出项目 scripts/*.lua，语法校验显示错误行。
**Step 2:** 选中实体附加脚本 + 配置 `vars`（JSON 编辑）。
**Step 3:** 提交 `feat(editor): script panel with attach/configure`.

### Task 4.6: 打包面板 + CLI

**Files:**
- Create: `editor/src/packager.hpp` / `packager.cpp`（校验 + 打包 + 拷贝播放器）
- Modify: `editor/src/main.cpp`（`--package <project> <out>`）
- Create: `tools/package.ps1`（或内联）

**Step 1:** 校验：缺失资产/脚本语法/行为树引用/场景引用。
**Step 2:** 打包：`core::Pack` 收集项目 → `game.pack`；拷贝 `neon_game.exe`（先以 `neon_rush` 占位验证闭环）+ 生成 `run.bat`。
**Step 3:** 打包冒烟测试：打包样例项目 → 运行产物 → 截图；提交 `feat(editor): one-click packaging`.

### Task 4.7: neon_game 通用播放器

**Files:**
- Create: `game/src/player_main.cpp`、`game/src/player.cpp`（无玩法硬编码：读 pack → manifest → 场景 → 运行）
- Modify: `CMakeLists.txt`（新目标 `neon_game`）

**Step 1:** 播放器加载 `game.pack`（`--pack path`），进入 startScene，运行 ECS + 行为树 + 脚本 + 动画系统（复用 demo 的 PlayerSystem 泛化为基础系统集）。
**Step 2:** 用纯数据项目（无 C++ 玩法）试运行并截图。
**Step 3:** 提交 `feat(player): generic neon_game runtime`.

### Task 4.8: 资产缩略图 / 多相机 / 热重载 / 性能面板

**Files:**
- Modify: `editor/src/panels.cpp`、`editor/src/editor.cpp`
- Modify: `engine/src/gfx/asset_manager.cpp`（mtime 缓存 + 重载回调）

**Step 1:** 资产缩略图（贴图/网格渲染到小图）。
**Step 2:** 视口多相机（正交顶视/前视）+ 相机切换。
**Step 3:** 热重载：脚本/资产 mtime 变化自动重载（编辑器有 `--hot` 开关）。
**Step 4:** 性能面板：帧时间/实体数/物理体数/行为树数/内存统计。
**Step 5:** 提交 `feat(editor): thumbnails, multi-cam, hot reload, profiler`.

---

## P5 平台与性能

### Task 5.1: miniaudio 音频后端

**Files:**
- Create: `third_party/miniaudio/`
- Create: `engine/src/audio/miniaudio/ma_audio.cpp`（实现 `IAudioBackend`）
- Modify: `CMakeLists.txt`、`engine/src/audio/audio.cpp`（后端选择）

**Step 1:** vendor miniaudio（`miniaudio.h` 单头）。
**Step 2:** 实现 `MiniAudioBackend`（设备 + 混合队列 + 程序化音源），替换 WinMM；保留 Null 兜底。
**Step 3:** demo 音效/音乐走新后端；提交 `feat(audio): miniaudio backend cross-platform`.

### Task 5.2: 纹理压缩 + 异步解码

**Files:**
- Create: `third_party/stb/stb_dxt.h`（或复用既有 stb）
- Create: `engine/src/assets/async_loader.cpp`（线程池 + 请求队列）
- Modify: `engine/src/assets/asset_manager.cpp`

**Step 1:** 纹理加载可选择 BC 压缩（运行时压缩到 `glCompressedTexImage2D`）。
**Step 2:** 异步：`AssetManager::LoadAsync(path, cb)`，解码线程池。
**Step 3:** 提交 `feat(assets): texture compression + async decode`.

### Task 5.3: LOD 资产链

**Files:**
- Modify: `engine/src/scene/scene_file.cpp`（`lod` 字段）
- Modify: 渲染器（按距离选 LOD 网格）
- Test: `tests/test_lod.cpp`

**Step 1:** 网格带 `lod` 链（`meshKey` 列表），按相机距离选择级别。
**Step 2:** 测试距离切换；提交 `feat(gfx): LOD asset chains`.

### Task 5.4: 世界分区流式

**Files:**
- Create: `engine/include/neon/scene/world_chunk.hpp`
- Create: `engine/src/scene/world_chunk.cpp`
- Modify: `engine/src/scene/scene_file.cpp`（chunk 数据）

**Step 1:** `WorldChunk`（64×64 坐标块，含实体列表/资产引用）。
**Step 2:** `ChunkStreamer`：按玩家位置维护 3×3 加载窗口，异步加载/卸载（复用 async loader），到达/离开触发回调。
**Step 3:** 测试：玩家移动→块加载卸载计数正确；提交 `feat(scene): chunk streaming`.

### Task 5.5: ECS archetype + 并行

> **范围决策（已执行）**：不做存储层重写（SparseSet `Pool<T>` 被 game/editor/engine 约 100 处调用，重写会破坏整个运行链路）。改为**增量交付**：在现有存储上提供批量迭代 API（`View<T>::ForEach`/`ParallelForEach`、`View<T,U>`）与确定性并行 job 工具（`neon::ecs::parallel::ParallelFor` + 线程池）。archetype 存储布局留作后续重构，批量迭代 API 已就位，届时只需替换存储后端。提交信息用 `feat(ecs): batch iteration + parallel jobs`。

**Files:**
- Modify: `engine/include/neon/ecs/world.hpp`、`engine/src/ecs/world.cpp`（批量迭代 + `View<T,U>` + 并行变更契约）
- Create: `engine/include/neon/ecs/parallel.hpp`、`engine/src/ecs/parallel.cpp`（确定性 ParallelFor 线程池）
- Test: `tests/test_ecs_parallel.cpp`

**Step 1:** `View<T>` 增加 `ForEach`（串行批量迭代）；`View<T,U>` 双组件视图（只访问同时持有 T+U 的实体）；`World::ViewAll<T,U>()`。
**Step 2:** `parallel::ParallelFor(count, fn)`：固定切分 + 持久线程池（MinGW 无 `std::thread`，用 Win32 CreateThread / POSIX pthread），join 后返回；无 worker 回退串行。`parallel::Reducer<T>` 按 chunk 槽位归约。
**Step 3:** 测试：10 万实体迭代正确 + 并行与串行逐位一致 + 跨运行一致 + 无 worker 回退 + `View<T,U>` 只访问双组件实体；并行期间世界变更由 debug assert 契约禁止。提交 `feat(ecs): batch iteration + parallel jobs`.

---

## P6 网络化

### Task 6.1: 消息编解码 + 协议

**Files:**
- Create: `engine/include/neon/net/protocol.hpp`、`engine/src/net/protocol.cpp`
- Test: `tests/test_protocol.cpp`

**Step 1:** 失败测试：`MsgJoin/MsgSnapshot/MsgInput/MsgSpawn` 编码→解码回环；未知消息 id 报错。
**Step 2:** 实现基于 `core::Serializer` 的消息框架（`version` + `msgId` + 载荷 + CRC）。
**Step 3:** 提交 `feat(net): message codec`.

### Task 6.2: UDP 传输 + 可靠层

**Files:**
- Create: `engine/include/neon/net/socket.hpp`、`engine/src/net/socket.cpp`（UDP socket + 序列号/ACK/重传）
- Modify: `engine/src/platform/`（无头平台路径）

**Step 1:** 本地回环测试：发 1000 包，丢包率模拟下可靠送达（超时重传），乱序按序列号重排。
**Step 2:** 提交 `feat(net): UDP transport with reliability`.

### Task 6.3: 无头服务器 neon_server

**Files:**
- Create: `server/src/main.cpp`、`server/src/game_server.cpp`
- Modify: `CMakeLists.txt`（新目标 `neon_server`，无窗口链接）

**Step 1:** 复用 ECS + 行为树 + 物理 + 确定性 RNG 运行权威模拟（固定 tick 60），无渲染/音频。
**Step 2:** 接收客户端输入 → 模拟 → 广播快照。
**Step 3:** 提交 `feat(server): headless authoritative server`.

### Task 6.4: 快照插值 + 预测回滚

**Files:**
- Create: `engine/src/net/client_sync.cpp`
- Modify: `game/src/player.cpp`（客户端预测路径）

**Step 1:** 客户端：本地预测 + 服务器快照插值；冲突时回滚并重放输入。
**Step 2:** 确定性回放测试：服务器与"预测客户端"最终位置一致（容差内）。
**Step 3:** 提交 `feat(net): snapshot interpolation + prediction`.

### Task 6.5: AOI 兴趣管理

**Files:**
- Modify: `engine/src/net/game_server.cpp`（九宫格 AOI + 视野掩码）

**Step 1:** 服务器按九宫格维护每实体可见集，只向可见客户端发快照。
**Step 2:** 测试：实体移动跨块 → 发送集变化正确；提交 `feat(net): AOI interest management`.

### Task 6.6: 登录/角色选择（占位）

**Files:**
- Modify: `engine/src/net/protocol.cpp`（MsgLogin/MsgCharSelect，v0 匿名）

**Step 1:** 登录/选角流程占位（本地匿名 + 预留账号库接口）。
**Step 2:** 提交 `feat(net): placeholder auth flow`.

### Task 6.7: 局域网双进程 demo + 确定性验收

**Files:**
- Create: `server/src/main.cpp`（`--host` 模式）
- Modify: `game/src/player_main.cpp`（`--connect localhost:port`）

**Step 1:** 启动 `neon_server` + 两个 `neon_game --connect` 客户端，观察同步。
**Step 2:** 确定性验收：服务器与客户端各自跑同一输入序列，快照一致（哈希比对）。
**Step 3:** 提交 `feat(net): LAN demo + determinism acceptance`.

---

## P7 Vulkan + 全链路集成

### Task 7.1: Vulkan 后端

**Files:**
- Modify: `engine/src/gfx/vulkan/vk_backend.cpp`（完整实现 IRenderBackend）
- Create: `engine/src/gfx/vulkan/`（pipeline/command/swapchain 辅助）
- Modify: `CMakeLists.txt`（`NEON_ENABLE_VULKAN` 开关 + 头文件查找）

**Step 1:** 实例→设备→交换链→RenderPass→Pipeline→命令缓冲→帧同步（对齐 `VULKAN_ROADMAP.md`）。
**Step 2:** 逐项实现 IRenderBackend 接口（shader/texture/mesh/draw/drawInstanced/primitive）。
**Step 3:** 冒烟：`--backend vulkan --smoke-test` 同帧截图与 GL 灰度差 < 阈值（或 `--disable-fbo` 兜底可配）。
**Step 4:** 提交 `feat(gfx): Vulkan render backend`.

### Task 7.2: 跨后端一致性测试

**Files:**
- Create: `tests/test_backend_conformance.cpp`（headless 双后端渲染对比）
- Modify: `engine/src/core/app.cpp`（后端选择参数）

**Step 1:** 同一场景分别用 GL/Vulkan 渲染固定帧，比较输出哈希。
**Step 2:** 提交 `test: cross-backend conformance`.

### Task 7.3: 全链路验收

**Files:**
- Modify: `tools/`（验收脚本）

**Step 1:** 自动脚本：构建全部目标 → 编辑器冒烟 → 样例项目 F5 试玩 600 tick → 打包 → 在干净目录运行 `neon_game` 截图。
**Step 2:** 更新 `docs/ROADMAP.md` / `README.md` / `ARCHITECTURE.md` 状态。
**Step 3:** 提交 `docs: v2 acceptance + roadmap update`.

---

## 执行顺序与依赖

```
P1 → P2 → P3 ──► P4（依赖 P2 试玩/P3 表现）
     └──► P5 → P6（依赖 P2 确定性/P5 性能）
                       └──► P7（独立后端 + 集成验收）
```
每阶段结束跑：`cmake --build build -j && ./build/neon_tests && ./build/neon_editor --smoke-test 120` 全绿再进入下一阶段。
