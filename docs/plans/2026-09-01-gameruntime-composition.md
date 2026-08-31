# GameRuntime 组合服务化拆分 设计

日期：2026-09-01
状态：待批准
前置：C14 玩法下沉已完成（战斗/技能/状态规则已移出，GameRuntime 只留通用原语 + 内容运行时编排）

## 1. 目标

把 `GameRuntime`（头 718 行 + 实现 3446 行）从上帝类拆成「编排器 + 组合服务」。每个内聚
子系统抽成独立类，`GameRuntime` 只负责顶层编排（Start/Tick/Draw 转发 + 跨子系统共享状态）。

纯机械拆分（不重新设计接口语义），716 项测试零回归。这是 C1 的收尾：玩法下沉后，
GameRuntime 已退化为「纯内容运行时」，拆分时机成熟。

## 2. 子系统清单（按内聚度分组）

### 阶段 1 —— 最独立（状态少、耦合低，先拆）

| 子系统 | 状态 | 方法 |
|---|---|---|
| `ProjectileSystem` | `projectiles_`, `fireballMesh_` | `Spawn`/`Tick`/`Trail`/`Burst` |
| `HudSystem` | `floatTexts_`, `screenAnchors_`, `plates_`, `lastViewProj_`/`lastCam_`/`lastAspect_`/`lastVpW_`/`lastVpH_` | `SpawnFloatText`/`SetEntityPlate`/`FloatTexts`/`ScreenAnchors`/`EntityPlates`/`WorldToScreen`/`ScreenToWorld`/`DesignWidth` |
| `ParticleSystem` | `particles_`, `particleTex_` | `Emit`/`Update` |
| `TweenSystem` | `tweens_` | `Start`/`Tick` |
| `LagCompSystem` | `poseSlots_`, `poseHead_`, `poseCount_`, `autoRewindTicks_` | `Position`/`SetAutoLagComp`/`AutoLagCompTicks`/`Record` |
| `StatusSystem` | （无自有状态，操作 `world_` 的 `StatusComponent`） | `Tick`/`Has`/`Magnitude` |

### 阶段 2 —— 中等耦合

| 子系统 | 状态 | 方法 |
|---|---|---|
| `PrefabSystem` | `prefs_` | `Load`/`SpawnPrefab` |
| `PluginSystem` | `plugins_` | `Manager`/`DispatchEvent`/`RunCommand` |
| `SceneTreeSystem` | `worldTransforms_` | `Rebuild`/`CachedLocalToWorld`/`LocalToWorld`/`GetChildren`/`GetDescendants` |
| `ScriptCanvas` | `draw2d_` | `Flush` |

### 阶段 3 —— 核心（耦合最深，最难）

| 子系统 | 状态 | 方法 |
|---|---|---|
| `AnimationSystem` | DrawItem 内的 anim 状态（**C2 根源**） | `Play`/`Progress`/`Finished`/`AttachStateMachine`/`SetParam`/`TickAnimations` |
| `ScriptRuntime` | `hosts_`, `scripts_`, `scriptCtx_`, `loadedScripts_`/`scriptFailed_`/`chunkHandlers_` | `AttachScripts`/`AttachOne`/`CallEntity`/`Host`/`Context` |
| `BtRuntime` | `trees_` | `AttachTrees`/`CallOnTree`/`ActivePath`/`BlackboardValue` |
| `PhysicsBridge` | `physics_`, `physicsAccum_`, `pluginPhysics_` | `RegisterBodies`/`RegisterCharacters`/`SyncBodies`/`World` |
| `DrawSystem` | `draws_`, `drawKeys_`, `drawBatches_`, `drawBvh_`, `drawOrder_`, `vegCache_` | `BuildDrawList`/`Resolve*`/`Draw`/`DrawVegetation` |
| `UiSystem` | `ui_` | `Show`/`Hide`/`Clicked`/`Set*` |

## 3. 拆分原则

1. **子系统不持有 `world_`**：以参数或 `ecs::World*` 传入（`GameRuntime` 仍是 `world_` 的拥有者）。
2. **子系统间最小耦合**：通过参数传递或共享引用，不引入新的接口层（纯机械拆分）。
3. **`GameRuntime` 保留跨子系统共享状态**：`world_`、`scriptCtx_`（GameVars 被脚本/BT 共享）、
   `hosts_`（脚本/BT 共用）、`cfg_`、`compReg_`、`hiddenEntities_`、`inputMap_`、`pendingScene_`、
   `signalHandlers_`、`post*`（后处理开关）、`loc_`（本地化）。
4. **每个子系统一个头文件 + 一个实现文件**，放 `engine/src/scene/systems/` + `engine/include/neon/scene/systems/`
   （或沿用 scene 目录结构）。命名 `XxxSystem`。
5. **测试**：每个阶段拆完，全量 `neon_tests` 保持全绿；子系统可独立单测（阶段 1 起补）。

## 4. 关键难点：C2（动画状态塞 DrawItem）

`DrawItem` 目前混装「渲染」（mesh/mat/sprite/terrain）+「动画」（animClip/animSM/animTime）。
这是 C2「headless 空转动画」的根源。阶段 3 拆 `AnimationSystem` 时，把 anim 状态从 `DrawItem`
移到独立的 `AnimationSystem`（`entityKey -> AnimState` 表），`DrawItem` 只保留渲染引用（skinned
模型指针），`Draw` 时从 `AnimationSystem` 取当前 pose。这样 headless 服务器（无 Draw）不再空转动画。

## 5. 分阶段执行

- **阶段 1**：6 个最独立系统（投射物/HUD/粒子/补间/lag-comp/状态）→ 独立类，GameRuntime 持有转发。
- **阶段 2**：预制体/插件/场景树/脚本画布。
- **阶段 3**：动画（解 C2）/脚本/BT/物理/绘制/UI。

每阶段一个独立 commit，阶段间全量测试全绿。

## 6. 验收

1. `neon_tests` 全程全绿（716 基线，拆分后测试数不变或增加子系统单测）。
2. `game_runtime.hpp` 行数显著下降（目标 < 300 行，只留编排 + 共享状态）。
3. `engine/src/scene/game_runtime*.cpp` 拆成 `systems/*.cpp`，单文件不再超 500 行。
4. C2 消除：headless 服务器 `Tick` 不触碰动画状态（阶段 3 后）。
5. 三个入口（game/editor/server）行为不变。

## 7. 风险

- 子系统间的隐式耦合（如 `TickProjectiles` 需要 `particles_` 发射 VFX、`Overlap` 需要
  `poseSlots_` + `world_`）需在拆分时显式参数化，不能破坏。
- 阶段 3 的 `ScriptRuntime`/`BtRuntime` 共享 `scriptCtx_`/`hosts_`，拆分边界需谨慎。
- 机械拆分不动语义，但大量代码移动有引入笔误风险——靠全量测试兜底。
