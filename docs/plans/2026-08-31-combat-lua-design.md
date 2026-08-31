# 战斗玩法下沉 Lua — 设计文档

日期：2026-08-31
状态：待批准
范围：只做引擎架构（补通用能力原语 + Lua 战斗库 + 删 C++ 玩法 API + 迁测试；NeonRealm 项目改到能跑即可）

## 1. 目标

把 NeonRealm 的具体玩法（技能表 / 冷却 / 命中规则 / 投射物行为 / 状态 tick 规则）从引擎
C++ `GameRuntime` 移出，下沉到 Lua 脚本层。引擎只保留「通用能力原语」——脚本做不到、且
跨游戏复用的底层能力。

## 2. 现状（已核实）

- `GameRuntime` 同时承载「编排胶水」和「NeonRealm 玩法」，是上帝对象。
- 战斗状态全部长在 `GameRuntime` 上：`projectiles_`、`skillCooldowns_`、`skills_`（`SkillTable`）、
  `poseSlots_`（lag-comp 历史）、`autoRewindTicks_`。
- 命中检测是 C++ 直接 `world_.ViewAll<SceneHealth>()` 遍历 + 数学判定（弧线 / OBB / 球），
  散落在 `engine/src/scene/game_runtime_combat.cpp`。
- `physics::World` 只有 `Raycast`，**没有 Overlap 空间重叠查询**（这是脚本层最大的能力缺口）。
- 脚本已有原语：`GetHealth`/`SetHealth`/`Raycast`/`Spawn`/`SpawnEntity`/`SpawnPrefab`/
  `EmitParticles`/`ApplyStatus`/`HasStatus`/`StatusMagnitude`/`RemoveStatus`。
- lag-comp（`MeleeAttackLagComp`/`AttackBoxLagComp`）依赖引擎的确定性 pose 环形缓冲
  （`poseSlots_`，`kLagCompHistoryTicks=64`），是权威服务器的「回滚命中」能力，必须保留，
  但不能继续焊死在「近战/盒攻击」玩法 API 里。

## 3. 边界划分

### 3.1 引擎保留（通用能力原语）

| 原语 | 状态 |
|---|---|
| `OverlapSphere(origin, radius, rewindTicks)` → 命中实体列表 | **新增** |
| `OverlapBox(center, half, yaw, rewindTicks)` → 命中实体列表 | **新增** |
| `SpawnProjectile(pos, dir, speed, damage, life, range, hitRadius, caster, onHit)` | 泛化（见 §4.1） |
| `GetHealth`/`SetHealth`/`Raycast`/`Spawn`/`SpawnPrefab`/`EmitParticles` | 保留 |
| `ApplyStatus`/`HasStatus`/`StatusMagnitude`/`RemoveStatus`（StatusComponent 容器机制） | 保留 |
| `SpawnFloatText`/`SetEntityPlate`（HUD 覆盖，渲染能力） | 保留 |
| `SceneHealth`/`StatusComponent`/`SceneTransform`（ECS 组件，数据） | 保留 |

### 3.2 下沉 Lua（NeonRealm 玩法规则）

- 技能表 `skills.json` → Lua 表
- 冷却 / mana → Lua 定时器 + GameVar
- 命中参数（弧线角度 / 盒半宽 / 垂直带 / 投射物速度与半径）→ Lua 传参
- 命中后附加哪些状态 → Lua 回调
- 状态 tick 效果（burning 伤害 / regen 回血 / slow 减速）→ Lua

### 3.3 引擎删除（C++ 玩法 API）

- `MeleeAttack` / `MeleeAttackLagComp` / `MeleeAttackImpl`
- `AttackBox` / `AttackBoxLagComp` / `AttackBoxImpl`
- `CastSkill` / `LoadSkills` / `SkillCooldownLeft` / `TickSkillCooldowns`
- `SkillTable` / `SkillDef` / `SkillStatus`（`scene/skills.hpp`）
- `TickStatuses` 的规则分支 + 内置 `kStatusDefs`（`status.hpp` 的 burning/poison/regen/slow 规则表）

## 4. 设计

### 4.1 空间重叠查询原语（新增）

`OverlapSphere` / `OverlapBox` 是「查询某区域内有 `SceneHealth` 且 `hp > 0` 的实体」的
通用能力。返回实体列表（含每个实体的命中位置，`rewindTicks > 0` 时用回滚后的位置）。
实现直接复用现有 `ViewAll<SceneHealth>()` 遍历，抽成独立函数，位置放在 `scene` 层
（一个 `SpatialQuery` 自由函数集或 `GameRuntime` 的通用成员，避免再建类）。

脚本 binding：`OverlapSphere(origin, radius [, rewindTicks])` / `OverlapBox(center, half, yaw [, rewindTicks])`
返回 `{{entity, x, y, z}, ...}`。

### 4.2 投射物原语（泛化）

`SpawnProjectile` 从「火球」泛化成「可编程飞行弹道」：

- 保留引擎实现的 fixed-step 移动 + 命中检测 + 渲染 + 粒子（这是性能/确定性敏感的引擎能力）。
- 增加 `range`（0 = 按 life）、`hitRadius`、`statuses`（`{name,duration,magnitude}` 列表）
  参数，数据驱动（YAGNI：不引入 `onHit` 回调，`statuses` 已覆盖「命中施加状态」需求）。

火球在 Lua 侧定义为：`SpawnProjectile(origin, dir, 14, 18, 2, caster, 30, 0.8, {{"burning",3,2}})`。

### 4.3 状态 tick 规则下沉

`StatusComponent` + `TickStatus` 机制保留（通用「带计时 buff 容器」）。下沉的只是
`TickStatuses` 里的规则分支（regen/slow/burning/poison 各自做什么）。方式：脚本注册
一个全局 `onStatusTick(id, magnitude, entity)` 处理器；引擎的 `TickStatus` 在每个 interval
边界调用它，处理器在 Lua 里用 `SetHealth`/读 `GameVars` 实现具体效果。

`kStatusDefs`（名称→id、tickInterval）从 C++ 内置表改为数据：由 Lua/数据层在启动时注册
（`RegisterStatus(name, tickInterval)`），或直接约定 id 与 tickInterval 由 Lua 表维护。

### 4.4 基础玩法库（引擎内嵌 Gameplay 脚本层）

沙箱关闭了 `require`/`dofile`，但 `IScriptHost::Load(source)+Run()` 可执行任意内嵌源码。
因此基础玩法库走**引擎内嵌 Lua 字符串**（`engine/generated/gameplay_lib.hpp` 的
`kGameplayLibLua`），在 `GameRuntime::Start` 里于项目脚本之前执行一次，注入全局
`Gameplay` 表。项目脚本（`realm.lua` 及未来项目）直接调用，无需 `require`。

抽象出的通用玩法原语（跨游戏复用，不含 NeonRealm 数值/流程细节）：

| 模块 | 函数 | 抽象来源 |
|---|---|---|
| 命中/AoE | `Gameplay.MeleeArc` / `Gameplay.AoE` / `Gameplay.BoxAttack` | C++ `MeleeAttackImpl` + realm.lua `wolvesInRadius` |
| 投射物 | `Gameplay.Projectile(...)` | `SpawnProjectile` 薄包装 |
| 冷却 | `Gameplay.Cooldowns`（set/ready/left/tick） | realm.lua `skillCd` 表 |
| 状态效果 | `Gameplay.RegisterStatus(name, tickInterval, onTick)` + 内置 burning/poison/regen/slow | C++ `kStatusDefs` + `TickStatuses` |
| 技能系统 | `Gameplay.SkillTable.fromJson` / `.cast(table,name,origin,dir,caster)`（含冷却/mana 分发） | C++ `SkillTable` + `CastSkill` |
| 属性 | `Gameplay.Stats`（HP/MP/XP/level/gold，基于 GameVar） | realm.lua `SetVar/GetVar` 用法 |
| 背包 | `Gameplay.Inventory`（物品定义 id/name/stackable/maxStack + add/remove/count/use 回调 + 堆叠/上限/货币 + 存档） | 全新抽象（通用容器） |
| 第一人称控制 | `Gameplay.FirstPerson.new(hero)`（带状态对象：鼠标视角 + 相对移动 + 相机在眼睛处） | realm.lua `fpsMode`/`lookYaw`/`lookPitch` |
| 第三人称控制 | `Gameplay.ThirdPerson.new(hero)`（带状态对象：轨道相机 + 角色面向移动方向） | realm.lua 轨道视角 |

控制器沿引擎既有约定：相机 GameVar（`cameraFocus`/`cameraYaw`/`cameraPitch`/`cameraDist`/`cameraMouseLock`）+ `input.json` action（`move_forward`/`move_strafe`/`jump`）。

这些函数全部基于阶段 1 的通用原语（`OverlapSphere`/`OverlapBox`/`SpawnProjectile`/
`SetHealth`/`GetHealth`/`ApplyStatus`/`GetVar`/`SetVar`）组合实现。

## 5. 分阶段实施

- **阶段 1（引擎原语，零回归）**：`OverlapSphere`/`OverlapBox`（含 lag-comp 回滚）+ 脚本
  binding + 单测；`SpawnProjectile` 泛化（`range`/`hitRadius`/`statuses` 数据驱动）。现有 C++
  玩法 API 不动。
- **阶段 2（基础玩法库）**：内嵌 `Gameplay` 库加载机制（`engine/generated/gameplay_lib.hpp`
  → `GameRuntime::Start` 注入）；逐模块实现 Stats / Cooldowns / 命中AoE / 投射物 / 状态 /
  技能系统 / 背包 / 第一人称 / 第三人称控制，每个带 Lua 测试。
- **阶段 3（删 C++ 玩法 + 迁移）**：删 C++ 玩法 API（§3.3）；迁移 `test_combat.cpp`/
  `test_lagcomp.cpp` 到「Overlap 原语 + Gameplay 库」测试；`realm.lua` 改用基础库。

## 6. 验收

- `neon_tests` 全绿（迁移后测试数与行为等价；阶段 1 保证零回归）。
- 删掉的 C++ 玩法 API 无残留调用（`grep` 无 `MeleeAttack`/`AttackBox`/`CastSkill`/`SkillTable`）。
- NeonRealm 场景经 `neon_game`/`neon_editor` 试玩可跑，战斗行为等价（火球/近战/状态/冷却）。
- 服务端权威路径（lag-comp 回滚命中）经 `neon_server` 依旧可用。

## 7. 风险

- 投射物/命中检测从 C++ 批量 tick 改为「原语 + Lua 组合」，确定性需重验证（固定 dt、同一
  遍历顺序）。
- status 下沉涉及 id 解析与 tick 规则迁移，牵涉 `test_combat`/技能 JSON 较多用例。
- NeonRealm 项目脚本迁移是内容工程，范围限定「改到能跑」。
