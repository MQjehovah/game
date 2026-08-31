# NeonEngine 架构评审：高内聚 / 低耦合分析

日期：2026-08-31
状态：已评审（分层 / 依赖方向 / 内聚耦合），改进项见本文件第 5 节，具体缺陷清单见 [`TODO.md`](../TODO.md)。

## 1. 结论摘要

项目有清晰的分层**意图**（目录与命名规范，`engine/` 下 17 个子域 + 3 个应用层 + 插件系统），但分层**没有被强制**：所有引擎代码编译进一个 `neon_engine` 静态库，没有依赖方向规则。两大最深耦合：

1. **上帝对象 `GameRuntime`**（`scene/game_runtime.hpp` 733 行 / 53 个 include，揽下渲染、物理、脚本、UI、战斗、网络）。
2. **玩法逻辑混进引擎核心**（`scene/skills.cpp`、`scene/status.cpp`、`scene/game_runtime_combat.cpp` 承载技能/状态/弹道/攻击盒——类型专属玩法，不是引擎通用能力）。

其余为模块级耦合（`scene↔script` 循环依赖、编辑器上帝文件、测试跨层抓源码），详见第 4 节。

## 2. 现状模块地图

```
neon_engine（一个 STATIC 库，~80 源文件，17 子域）
├─ math / core / io / ecs / net / nav / plugin / assets   ← 平台无关基础
├─ gfx（renderer/gl/vulkan/mesh/terrain/particles/...）    ← 渲染
├─ physics（自研 + Jolt）                                  ← 物理
├─ scene（scene_file/game_runtime/skills/status/combat）   ← 场景 + 运行时
├─ script（lua/js host + bindings）                        ← 脚本
└─ audio / ui / bt / anim                                  ← 各子系统

应用层：game/（neon_rush demo, neon_game 播放器）· server/（无头）· editor/（单 exe）· plugins/
```

分层意图正确（这部分是优点）。问题出在**依赖方向与模块边界未被约束**。

## 3. 目标架构与依赖规则

核心改进一句话：**建立分层，用 CMake target 依赖强制"只能向下依赖"，把玩法从引擎搬出去。**

```
L0  neon_math         纯数学，零依赖
L1  neon_core         core/io/ecs/net/nav/plugin/assets（数据层）  ← 无渲染
L2  neon_gfx          渲染（mesh/material/renderer/gl/vk/...）      ← 依赖 L0/L1
L3  neon_scene        场景编排（scene_file/component_schema/       ← 依赖 L1/L2
                      skinned_model/通用运行时）
L4  neon_script       lua/js host + 绑定注入接口                    ← 依赖 L1
──────────────────────────────────────────────────────────────
应用层：
    neon_editor_lib   （库化，拆 panels）
    game/             （含 combat/skills/status 玩法）
    server/           （无头）
    plugins/          （native DLL / lua / js）
```

依赖规则：**只能向下依赖**。用 `target_link_libraries` 白名单 + include 白名单强制，越层 include 直接编译失败，而非靠自觉。

## 4. 问题清单（按严重度）

### 4.1 玩法逻辑混进引擎核心（最严重的内聚问题）

战斗/技能/状态/弹道/攻击盒/延迟补偿命中——类型专属玩法，住在引擎 `scene` 模块：

- `engine/src/scene/game_runtime_combat.cpp`：`SpawnProjectile`、`LagCompPosition`、攻击盒命中
- `engine/include/neon/scene/skills.hpp`：`SkillDef{ kind: projectile/melee/box }`、冷却、蓝耗
- `engine/include/neon/scene/status.hpp`：`kStatusBurning/Poison/Regen`（燃烧/中毒/回血）

证据：`game_runtime.hpp` 中 `#include "neon/scene/skills.hpp"` / `status.hpp`。

**后果**：换一个游戏类型，引擎核心要跟着改；"引擎"与"某个具体游戏"边界消失。与地形问题同源——引擎核心塞了太多"内容"。

**改进**：`combat/skills/status` 移到 `game/`（或做成可插拔游戏模块），引擎只留通用运行时。

### 4.2 上帝对象 `GameRuntime`

`game_runtime.hpp`：733 行、53 个 include（渲染 camera/font/material/mesh/particles、物理 physics+jolt、脚本 bindings/gamevars/script、UI document/ui_system、战斗 skills/status、导航、网络）。实现已拆三文件（`game_runtime.cpp` 2858 行 + `_combat` + `_content`），但**类还是一个类**。详见 TODO `C1`（已部分拆分）。

### 4.3 scene ↔ script 循环依赖

- `game_runtime.hpp` → `script/bindings.hpp`（scene 依赖 script）
- `bindings.cpp` → `scene/scene_file.hpp` + `scene/status.hpp`（script 依赖 scene）
- `bindings.hpp` 同时依赖 `gfx/physics/platform/ecs`

→ `scene` 与 `script` 双向引用。靠头文件顺序"刚好能编过"，无规则约束会持续腐化。

**改进**：`bindings` 接口化注入——引擎定义 `ScriptBindings` 接口，`GameRuntime` 只依赖接口，具体绑定由 game 层注册。

### 4.4 编辑器上帝文件 + 未库化

- `editor/src/panels.cpp` 4143 行，装全部面板（地形/瓦片/打包/性能/输入/导航/UI/本地化/调试）
- `editor.hpp` 937 行，`EditorApp` 一个类装下所有面板方法 + 状态
- 编辑器是单一 exe（不是库），详见 TODO `C3`。

### 4.5 测试边界跨层抓源码

`CMakeLists.txt` 中 `neon_tests` 直接编译 `editor/src/history.cpp`、`packager.cpp`、`server/src/game_server.cpp`、`game/src/client_sync.cpp` 源码，include 目录含 `editor/src server/src game/src plugins`。暴露一个事实：**editor/server/game 未库化，测试只能"抓 .cpp 源码"复用**，层间无公共可链接边界。

## 5. 分阶段改进计划

| 阶段 | 动作 | 收益 | 风险 | 对应 TODO |
|---|---|---|---|---|
| P1a | CMake 按模块 `add_subdirectory` 拆分 | 解 676 行单文件 | 低 | C12 |
| P1b | 编辑器库化 + panels.cpp 拆面板 | 解上帝文件，测试可链接 | 低 | C3 |
| P1c | 玩法抽离：combat/skills/status 从 engine → game | 引擎变通用 | 低（纯移动） | C1 延伸 |
| P2 | 破 scene↔script 循环（bindings 接口化） | 依赖单向化 | 中 | 新增 |
| P3 | 拆 neon_engine 为 core/gfx/scene 分层库 + CMake 依赖规则 | 强制分层 | 中 | C12 延伸 |

**暂缓**（YAGNI）：把 GameRuntime 全面拆成微服务式接口、每个子域独立 DLL、render graph（C4）——先解决玩法混入 + 上帝文件两个最痛点。

## 6. 与地形 / 程序化生成的衔接

地形编辑耦合是上述架构问题的一个缩影：所有东西堆在一个 `EditorApp` + 一个 `panels.cpp` + 一个 `GameRuntime` 里。完成 P1b/P1c 并建立 P3 分层后，"地形/生成器作为独立模块"自然落位：

- 生成器接口（`IGenerator`）作为稳定契约，位于 L3/L4 之上
- 玩法（含地形数据、战斗）不再污染引擎核心
- 插件（Lua/native）通过接口注入，而非 `bindings.cpp` 硬绑

详见后续《程序化生成器框架设计》文档（另行建立）。
