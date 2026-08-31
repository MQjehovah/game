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
- 增加可选 `onHit(entity)` Lua 回调：命中时调用，Lua 在此施加附加状态或覆盖伤害。
- `damage` 退化为「命中后对 SceneHealth 的默认伤害」，`onHit` 存在时可忽略或叠加。

火球在 Lua 侧定义为：`SpawnProjectile{speed=14, damage=18, onHit=施加burning}`。

### 4.3 状态 tick 规则下沉

`StatusComponent` + `TickStatus` 机制保留（通用「带计时 buff 容器」）。下沉的只是
`TickStatuses` 里的规则分支（regen/slow/burning/poison 各自做什么）。方式：脚本注册
一个全局 `onStatusTick(id, magnitude, entity)` 处理器；引擎的 `TickStatus` 在每个 interval
边界调用它，处理器在 Lua 里用 `SetHealth`/读 `GameVars` 实现具体效果。

`kStatusDefs`（名称→id、tickInterval）从 C++ 内置表改为数据：由 Lua/数据层在启动时注册
（`RegisterStatus(name, tickInterval)`），或直接约定 id 与 tickInterval 由 Lua 表维护。

### 4.4 Lua 战斗库（引擎内置脚本能力层）

新增 `combat.lua`（随引擎分发，项目 `require`）：

- `Combat.SkillTable`：从 JSON 文本加载技能表（`{name, kind, damage, cooldown, manaCost, ...}`）。
- `Combat.CastSkill(name, origin, dir, caster)`：检查冷却/mana，按 kind 分发到
  `projectile`（`SpawnProjectile`）/ `melee`（`OverlapSphere` + 弧线过滤）/ `box`（`OverlapBox`）。
- `Combat.Tick(dt)`：冷却衰减。
- `Combat.OnStatusTick`：状态 tick 规则（burning/poison 伤害、regen 回血、slow 减速）。

## 5. 分阶段实施

- **阶段 1（纯新增，零回归）**：`OverlapSphere`/`OverlapBox`（含 lag-comp 回滚）原语 +
  脚本 binding + 单测。`SpawnProjectile` 加 `onHit` 回调（向后兼容）。现有 C++ 玩法 API 不动。
- **阶段 2**：`combat.lua` 库 + `RegisterStatus`/`onStatusTick` 状态下沉机制。
- **阶段 3**：删 C++ 玩法 API（§3.3）；迁移 `test_combat.cpp`/`test_lagcomp.cpp` 及相关
  `test_game_runtime*.cpp` 战斗用例到「Overlap 原语 + combat.lua」测试；NeonRealm 项目脚本改到能跑。

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
