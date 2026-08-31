# 战斗玩法下沉 Lua + Gameplay 基础库 实现计划

> **For Claude:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task.

**Goal:** 把 NeonRealm 玩法从引擎 C++ `GameRuntime` 下沉到 Lua，并抽象出可复用的 `Gameplay` 基础库（技能系统 / 背包 / 第一人称 / 第三人称控制），供项目快速做游戏。

**Architecture:** 引擎新增 `OverlapSphere`/`OverlapBox` 空间查询原语（含 lag-comp 回滚）、`SpawnProjectile` 泛化、状态 tick 规则 Lua 化；内嵌 `Gameplay` Lua 库（`engine/generated/gameplay_lib.hpp`）在 `GameRuntime::Start` 注入全局；删除 C++ 玩法 API；迁移测试与 `realm.lua`。

**Tech Stack:** C++17、Lua 5.4（`neon_lua`）、`neon_tests`（CHECK 宏）、CMake + MSVC。

**关键上下文（已核实）：**
- 战斗实现 `engine/src/scene/game_runtime_combat.cpp`；状态在 `game_runtime.hpp`（`projectiles_`/`skills_`/`skillCooldowns_`/`poseSlots_`）。
- 命中检测是 `world_.ViewAll<SceneHealth>()` 遍历；`physics::World` 只有 `Raycast`。
- 脚本经 `script::ScriptContext` hooks 访问玩法，接线在 `game_runtime.cpp` `Start`（约 504-676 行）。
- NeonRealm 战斗在 `projects/neon_realm/assets/scripts/realm.lua`，仅 2 处 C++ 玩法调用：`CastSkill("fireball",...)`（321 行）、`MeleeAttack(...)`（374 行）；`skills.json` 定义 fireball/heal。
- Lua 沙箱无 `require`/`dofile`（`lua_host.cpp:272-295`），但有 `IScriptHost::Load(source)+Run()` 可执行内嵌源码。
- 实体在 Lua 里的表示：带 `.id`/`.gen` 字段（见 realm.lua 981 行 `a.entity.id`/`a.entity.gen`）；引擎有 `EntityFromValue` 反向解析，序列化方向复用 `ScreenAnchors`/`EntityPlates` 的实体值格式。

**构建/测试命令（全程）：**
- 构建：`cmake --build build-msvc --config Release`
- 单测：`& "build-msvc\Release\neon_tests.exe"`（基线 698 项全绿）

---

## 阶段 1：引擎能力原语（纯新增，零回归）

### Task 1: `GameRuntime` 新增 `OverlapSphere` / `OverlapBox`

**Files:** `engine/include/neon/scene/game_runtime.hpp`、`engine/src/scene/game_runtime_combat.cpp`、`tests/test_combat.cpp`

```cpp
// game_runtime.hpp（公开区，combat 段）
struct HealthHit { ecs::Entity entity; math::Vec3 pos; };
std::vector<HealthHit> OverlapSphere(const math::Vec3& center, float radius,
                                     uint32_t rewindTicks = 0) const;
std::vector<HealthHit> OverlapBox(const math::Vec3& center, const math::Vec3& half,
                                  float yaw, uint32_t rewindTicks = 0) const;
```

```cpp
// game_runtime_combat.cpp
std::vector<GameRuntime::HealthHit> GameRuntime::OverlapSphere(
    const math::Vec3& center, float radius, uint32_t rewindTicks) const {
    std::vector<HealthHit> out;
    auto view = world_.ViewAll<SceneHealth>();
    for (size_t i = 0; i < view.Size(); ++i) {
        ecs::Entity ent = world_.EntityAt<SceneHealth>(i);
        const SceneHealth* h = world_.Get<SceneHealth>(ent);
        const SceneTransform* t = world_.Get<SceneTransform>(ent);
        if (!h || !t || h->hp <= 0.0f) continue;
        math::Vec3 p = t->pos;
        if (rewindTicks > 0) LagCompPosition(ent, rewindTicks, p);
        if ((p - center).LengthSq() <= radius * radius) out.push_back({ent, p});
    }
    return out;
}
// OverlapBox 同理：d = p-center; lx = c*dx - s*dz, ly = dy, lz = s*dx + c*dz
// |lx|<=half.x && |ly|<=half.y && |lz|<=half.z 命中
```

**Step 1:** 在 `test_combat.cpp` 写失败测试（复用 `kCombatScene`）：

```cpp
TEST(OverlapSphereQueriesHealthEntities) {
    scene::GameRuntime runtime;
    scene::GameRuntimeConfig cfg; cfg.headless = true;
    CHECK(runtime.Start(kCombatScene, cfg).Ok());
    CHECK_EQ(runtime.OverlapSphere({0,0,0}, 3.0f).size(), 1u); // wolf_front
    CHECK_EQ(runtime.OverlapSphere({0,0,0}, 5.0f).size(), 2u); // + wolf_side
    CHECK_EQ(runtime.OverlapBox({0,0,0}, {0.5f,10.0f,2.5f}, 0.0f).size(), 1u);
}
```

**Step 2:** 构建失败（未声明）。**Step 3:** 加声明+实现。**Step 4:** 全绿。**Step 5:** `git commit -m "feat: OverlapSphere/OverlapBox 空间查询原语"`

---

### Task 2: `SpawnProjectile` 泛化（`range`/`hitRadius`/`statuses` 数据驱动）

**Files:** `game_runtime.hpp`、`game_runtime_combat.cpp`、`engine/include/neon/script/bindings.hpp`、`engine/src/script/bindings.cpp`、`engine/src/scene/game_runtime.cpp`、`tests/test_combat.cpp`

```cpp
// game_runtime.hpp：新签名（追加带默认值参数）
void SpawnProjectile(const math::Vec3& pos, const math::Vec3& dir, float speed, float damage,
                     float life, ecs::Entity caster = {}, float range = 0.0f,
                     float hitRadius = 0.8f, const std::vector<SkillStatus>& statuses = {});
```

`game_runtime_combat.cpp` 的 `SpawnProjectile` 填 `p.range = range; p.hitRadius = hitRadius; p.statuses = statuses;`（`Projectile` 结构已有这些字段）。

`bindings.hpp` 的 `spawnProjectile` hook 扩展为 9 参；`NativeSpawnProjectile` 读第 7-9 参（`range`/`hitRadius`/`statuses` table，逐项 `{name=,duration=,magnitude=}` → `SkillStatus`）。`game_runtime.cpp` 接线转发。

**Step 1:** 失败测试：

```cpp
TEST(SpawnProjectileWithRangeHitRadiusAndStatuses) {
    scene::GameRuntime runtime;
    scene::GameRuntimeConfig cfg; cfg.headless = true;
    CHECK(runtime.Start(kCombatScene, cfg).Ok());
    const ecs::Entity wolf = runtime.FindNamedEntity("wolf_front");
    const ecs::Entity hero = runtime.FindNamedEntity("hero");
    runtime.SpawnProjectile({0,1,0}, {0,0,-1}, 14, 10, 2.0f, hero, 30.0f, 1.0f,
                            {{"burning", 3.0f, 2.0f}});
    for (int i = 0; i < 30; ++i) runtime.Tick(1.0f/60.0f);
    CHECK_NEAR(runtime.EntityHealth(wolf).first, 40.0f, 1e-3);
    CHECK(runtime.HasStatus(wolf, scene::kStatusBurning));
}
```

**Step 2-4:** 改签名+实现+hook+binding，构建全绿。**Step 5:** `git commit -m "feat: SpawnProjectile 泛化 (range/hitRadius/statuses)"`

---

### Task 3: `OverlapSphere`/`OverlapBox` 脚本 binding

**Files:** `bindings.hpp`、`bindings.cpp`、`game_runtime.cpp`、`tests/test_combat.cpp`

`ScriptContext` 加：

```cpp
std::function<script::Value(const math::Vec3&, float, uint32_t)> overlapSphere;
std::function<script::Value(const math::Vec3&, const math::Vec3&, float, uint32_t)> overlapBox;
```

`bindings.cpp` 加 `NativeOverlapSphere`/`NativeOverlapBox`（参数解析同 `NativeMeleeAttack` 风格），`Register("OverlapSphere", ...)`。返回值为 `script::Value` 数组，每项 `{entity=..., x=, y=, z=}`；entity 用与 `ScreenAnchors` 相同的实体值格式（`.id`/`.gen` 字段的 Lua 表）。

`game_runtime.cpp` `Start` 接线：`scriptCtx_.overlapSphere = [this](center, r, rewind){ return HitsToValue(OverlapSphere(center, r, rewind)); }`。实现 `HitsToValue(std::vector<HealthHit>)` 转 `script::Value`。

**Step 1:** 失败测试（Lua 侧，`readScript` 方式）：

```cpp
TEST(OverlapBindingsViaLua) {
    const char* scene = /* kCombatScene + 携带本脚本的实体 */;
    const char* lua = R"(
      function on_start(e)
        SetVar("n", #OverlapSphere({x=0,y=0,z=0}, 5))
      end
    )";
    // Start + CHECK_NEAR(runtime.GameVar("n"), 2.0, 1e-6)
}
```

**Step 2-4:** 实现，全绿。**Step 5:** `git commit -m "feat: Overlap 脚本 binding"`

---

## 阶段 2：Gameplay 基础库

### Task 4: 基础库加载机制（内嵌 + Start 注入）

**Files:**
- Create: `engine/generated/gameplay_lib.hpp`（`inline const char* kGameplayLibLua = R"LUA(...)LUA";`）
- Modify: `engine/src/scene/game_runtime.cpp`（`Start` 里在 `RegisterEngineBindings` 之后、`AttachScripts` 之前执行 `Load(kGameplayLibLua)+Run()`）
- Modify: `CMakeLists.txt`（确认 `engine/generated` 在 include 路径；若是生成目录需确保 `gameplay_lib.hpp` 被 git 跟踪）
- Test: `tests/test_combat.cpp`

**Step 1:** 先放一个最小库 `kGameplayLibLua = R"LUA(Gameplay = { version = "0.1.0" })LUA"`。
**Step 2:** 写失败测试：场景脚本 `on_start` 读 `Gameplay.version` 设 GameVar：

```cpp
TEST(GameplayLibInjected) {
    const char* lua = R"(function on_start(e) SetVar("v", Gameplay.version) end)";
    // Start 后 CHECK(runtime.GameVar("v") 为字符串 "0.1.0")
}
```

**Step 3:** 在 `Start` 注入（注意：headless server 也走此路径，Lua host 存在时注入；JS host 场景不注入或同步注入，先只做 Lua）。**Step 4:** 全绿。**Step 5:** `git commit -m "feat: Gameplay 基础库加载机制（内嵌注入）"`

---

### Task 5: `Stats` + `Cooldowns`

**Files:** `engine/generated/gameplay_lib.hpp`、`tests/test_combat.cpp`

在 `kGameplayLibLua` 追加：

```lua
-- 属性：基于 GameVar 的键值存取（HP/MP/XP/level/gold 等）
Gameplay.Stats = {}
function Gameplay.Stats.Get(k) return GetVar(k) end
function Gameplay.Stats.Set(k, v) SetVar(k, v) end
function Gameplay.Stats.Add(k, d) SetVar(k, (GetVar(k) or 0) + d) end

-- 冷却：名字 -> 剩余秒数
Gameplay.Cooldowns = {}
function Gameplay.Cooldowns.new() return { cds = {} } end
function Gameplay.Cooldowns.set(self, name, sec) self.cds[name] = sec end
function Gameplay.Cooldowns.left(self, name) return self.cds[name] or 0 end
function Gameplay.Cooldowns.ready(self, name) return (self.cds[name] or 0) <= 0 end
function Gameplay.Cooldowns.tick(self, dt)
  for k, v in pairs(self.cds) do
    if v > 0 then self.cds[k] = math.max(0, v - dt) end
  end
end
```

**Step 1:** Lua 测试：`Stats.Set/Add/Get` 往返、`Cooldowns.set/ready/tick/left` 递减到 0。
**Step 2-4:** 实现，全绿。**Step 5:** `git commit -m "feat: Gameplay.Stats + Cooldowns"`

---

### Task 6: 命中/AoE + 投射物

**Files:** `engine/generated/gameplay_lib.hpp`、`tests/test_combat.cpp`

```lua
-- 弧线近战：原点+方向+范围+张角+伤害，命中返回数量
Gameplay.MeleeArc = function(origin, dir, range, arcDeg, damage, caster)
  local cosArc = math.cos(math.rad(arcDeg * 0.5))
  local hits = OverlapSphere(origin, range)
  local n = 0
  for _, h in ipairs(hits) do
    if caster == nil or h.entity ~= caster then
      local dx, dz = h.x - origin.x, h.z - origin.z
      local horiz = math.sqrt(dx*dx + dz*dz)
      if horiz > 1e-4 and math.abs(h.y - origin.y) <= 2.0 then
        local dot = (dx/horiz)*dir.x + (dz/horiz)*dir.z
        if dot >= cosArc then
          local hp = GetHealth(h.entity)
          if hp ~= nil then SetHealth(h.entity, math.max(0, hp - damage)) end
          n = n + 1
        end
      end
    end
  end
  return n
end
-- AoE 圆形 / BoxAttack 盒：同结构，无弧线过滤（Box 用 OverlapBox(center, half, yawDeg)）
Gameplay.AoE = function(origin, radius, damage, caster) ... end
Gameplay.BoxAttack = function(center, half, yawDeg, damage) ... end
-- 投射物：SpawnProjectile 薄包装（statuses 直接透传）
Gameplay.Projectile = function(origin, dir, speed, damage, life, range, hitRadius, caster, statuses)
  SpawnProjectile(origin, dir, speed, damage, life, caster, range, hitRadius, statuses)
end
```

**Step 1:** Lua 测试：`MeleeArc` 弧线内外命中数、`AoE` 圆形、`BoxAttack` 盒、`Projectile` 命中+statuses（用 `kCombatScene` 的 wolf_front/wolf_side 布局断言血量）。
**Step 2-4:** 实现，全绿。**Step 5:** `git commit -m "feat: Gameplay 命中/AoE + 投射物"`

---

### Task 7: 状态 tick 规则下沉（`OnStatusTick`）

**Files:** `engine/src/scene/game_runtime.cpp`（`TickStatuses` 改写）、`bindings.hpp`/`bindings.cpp`（可选新 hook）、`engine/generated/gameplay_lib.hpp`、`tests/test_combat.cpp`

**设计（务实）：** 保留 `StatusComponent` 容器 + `ApplyStatus/HasStatus/StatusMagnitude/RemoveStatus` + id 常量（`kStatusBurning` 等，序列化稳定）+ `StatusIdByName`（脚本用 `"burning"` 字符串）。只删 `TickStatuses` 里的规则分支，改为对每个 tick 调用脚本全局 `OnStatusTick(entity, id, magnitude)`（未定义则不处理）。

```cpp
// game_runtime.cpp TickStatuses 改为：
TickStatus(*c, dt, [this, ent](uint32_t id, float mag) {
    // 下沉：脚本定义效果；引擎不再写死 regen/slow/burning 分支
    if (auto* host = hosts_.lua.get()) {
        if (host->HasFunction("OnStatusTick"))
            (void)host->Call("OnStatusTick", { /* entity 值 */, Value::Num(id), Value::Num(mag) });
    }
});
```

基础库提供默认 `OnStatusTick` + `RegisterStatus`：

```lua
Gameplay.statusDefs = {
  burning = { id = 1, interval = 1.0, tick = function(ent, mag) local h = GetHealth(ent); if h ~= nil then SetHealth(ent, math.max(0, h - mag)) end end },
  poison  = { id = 2, interval = 1.0, tick = function(ent, mag) local h = GetHealth(ent); if h ~= nil then SetHealth(ent, math.max(0, h - mag)) end end },
  regen   = { id = 3, interval = 1.0, tick = function(ent, mag) local h = GetHealth(ent); if h ~= nil then SetHealth(ent, math.min(GetVar("max_hp") or 100, h + mag)) end end },
  slow    = { id = 4, interval = 1.0, tick = function() end },
}
Gameplay.RegisterStatus = function(name, interval, tick) Gameplay.statusDefs[name] = { id = 0, interval = interval, tick = tick } end
function OnStatusTick(ent, id, mag)
  for _, d in pairs(Gameplay.statusDefs) do
    if d.id == id and d.tick then d.tick(ent, mag) end
  end
end
```

**Step 1:** 失败测试：Lua 定义 `OnStatusTick`，`ApplyStatus(ent, "burning", 2, 3)` 后 2 秒血量按回调减少。
**Step 2-4:** 实现，全绿。**Step 5:** `git commit -m "feat: 状态 tick 规则下沉 Lua（OnStatusTick）"`

---

### Task 8: 技能系统（`SkillTable`）

**Files:** `engine/generated/gameplay_lib.hpp`、`tests/test_combat.cpp`

```lua
-- 技能表：JSON 文本 -> 表；cast 检查冷却/mana，按 kind 分发
Gameplay.SkillTable = {}
function Gameplay.SkillTable.fromJson(json)
  local parsed = Json.Parse(json)          -- 引擎 Json.Parse binding（已存在）
  local tbl = { skills = {}, cds = Gameplay.Cooldowns.new() }
  for name, def in pairs(parsed.skills or {}) do tbl.skills[name] = def end
  return tbl
end
function Gameplay.SkillTable.cast(tbl, name, origin, dir, caster)
  local def = tbl.skills[name]
  if def == nil then return 0 end
  if not tbl.cds:ready(name) then return 0 end
  local mana = Gameplay.Stats.Get("mana") or 0
  if (def.manaCost or 0) > 0 and mana < def.manaCost then return 0 end
  if (def.manaCost or 0) > 0 then Gameplay.Stats.Set("mana", mana - def.manaCost) end
  if (def.cooldown or 0) > 0 then tbl.cds:set(name, def.cooldown) end
  if def.kind == "projectile" then
    Gameplay.Projectile(origin, dir, def.speed or 12, def.damage, def.life or 2,
                        def.range or 0, 0.8, caster, def.statuses or {})
  elseif def.kind == "melee" then
    Gameplay.MeleeArc(origin, dir, def.meleeRange or 2, def.arcDeg or 90, def.damage, caster)
  else -- box
    local yaw = math.atan(dir.x, dir.z)
    Gameplay.BoxAttack(origin, {def.boxHalfX or 1, def.boxHalfY or 1, def.boxHalfZ or 1},
                       math.deg(yaw), def.damage)
  end
  return 1
end
```

**Step 1:** Lua 测试：`fromJson` 解析 `kSkillsJson`，`cast("fireball"/"cleave"/"slam")` 触发投射物/近战/盒，冷却与 mana 拦截。
**Step 2-4:** 实现，全绿。**Step 5:** `git commit -m "feat: Gameplay.SkillTable 技能系统"`

---

### Task 9: 背包系统（`Inventory`）

**Files:** `engine/generated/gameplay_lib.hpp`、`tests/test_combat.cpp`

```lua
-- 完整通用背包：物品定义 + 堆叠/上限 + 货币 + 使用回调 + 存档
Gameplay.Inventory = {}
function Gameplay.Inventory.new(capacity)
  return { items = {}, capacity = capacity or 24, currency = {} }
end
function Gameplay.Inventory.add(bag, itemId, count, def)
  -- def: { id, name, stackable, maxStack, onUse }
  local n = count or 1
  if def.stackable then
    local slot = bag.items[itemId] or { def = def, count = 0 }
    slot.count = slot.count + n
    if def.maxStack and slot.count > def.maxStack then return false end
    bag.items[itemId] = slot
  else
    local used = 0; for _ in pairs(bag.items) do used = used + 1 end
    if used >= bag.capacity then return false end
    bag.items[itemId .. "_" .. (used + 1)] = { def = def, count = 1 }
  end
  return true
end
function Gameplay.Inventory.count(bag, itemId) local s = bag.items[itemId]; return s and s.count or 0 end
function Gameplay.Inventory.remove(bag, itemId, count) ... end
function Gameplay.Inventory.use(bag, itemId, ent)  -- 调 def.onUse(ent)，扣数量
  local s = bag.items[itemId]
  if not s or s.count <= 0 then return false end
  if s.def.onUse then s.def.onUse(ent) end
  s.count = s.count - 1; if s.count <= 0 then bag.items[itemId] = nil end
  return true
end
function Gameplay.Inventory.addCurrency(bag, name, amount) bag.currency[name] = (bag.currency[name] or 0) + amount end
function Gameplay.Inventory.save(bag, path) WriteText(path, /* JSON */) end
function Gameplay.Inventory.load(bag, path) /* ReadText + Json.Parse 恢复 */ end
```

**Step 1:** Lua 测试：add/堆叠上限/count/remove/use 回调触发/货币/存档往返。
**Step 2-4:** 实现，全绿。**Step 5:** `git commit -m "feat: Gameplay.Inventory 背包系统"`

---

### Task 10: 第一人称 / 第三人称控制

**Files:** `engine/generated/gameplay_lib.hpp`、`tests/test_combat.cpp`

```lua
-- 第一人称（带状态对象）：鼠标视角 + 相对移动 + 相机在眼睛处
Gameplay.FirstPerson = {}
function Gameplay.FirstPerson.new(hero)
  return { hero = hero, yaw = 0, pitch = 0.32, eyeH = 1.6, camDist = 2.0,
           sens = 0.003, grounded = true, yvel = 0, groundY = 0.9 }
end
function Gameplay.FirstPerson.tick(c, dt)
  c.yaw = c.yaw - InputMouseX() * c.sens
  c.pitch = math.max(-1.2, math.min(1.2, c.pitch + InputMouseY() * c.sens))
  local cy = c.yaw
  local ix, iz = ActionAxis("move_strafe"), ActionAxis("move_forward")
  local fwd = { x = -math.sin(cy), z = -math.cos(cy) }
  local right = { x = math.cos(cy), z = -math.sin(cy) }
  local dx = right.x*ix + fwd.x*iz; local dz = right.z*ix + fwd.z*iz
  local pos = GetPosition(c.hero); if pos == nil then return end
  if ActionDown("jump") and c.grounded then c.yvel = 8; c.grounded = false end
  if not c.grounded then
    c.yvel = c.yvel - 20*dt; pos.y = pos.y + c.yvel*dt
    if pos.y <= c.groundY then pos.y = c.groundY; c.yvel = 0; c.grounded = true end
  end
  pos.x = pos.x + dx*6*dt; pos.z = pos.z + dz*6*dt
  SetPosition(c.hero, pos)
  SetRotationY(c.hero, c.yaw)
  -- 相机：眼睛点 = pos + eyeH；focus 在视线前方 camDist
  local cd = math.cos(c.pitch)
  local ex, ey, ez = pos.x, pos.y + c.eyeH, pos.z
  SetVar("cameraMouseLock", 1)
  SetVar("cameraYaw", c.yaw)
  SetVar("cameraPitch", c.pitch)
  SetVar("cameraDist", c.camDist)
  SetVar("cameraFocus", { x = ex - math.sin(c.yaw)*cd*c.camDist,
                          y = ey - math.sin(c.pitch)*c.camDist,
                          z = ez - math.cos(c.yaw)*cd*c.camDist })
end

-- 第三人称：轨道相机 + 面向移动方向（移动逻辑同 FirstPerson，相机 Focus=头顶、Dist 拉远）
Gameplay.ThirdPerson = {}
function Gameplay.ThirdPerson.new(hero) ... end
function Gameplay.ThirdPerson.tick(c, dt) ... end
```

**Step 1:** Lua 测试：模拟 `InputMouseX`/`ActionAxis` 输入（用可注入的 `Input` 或直接验证相机 GameVar 被写），断言 `cameraMouseLock`/`cameraFocus` 被正确设置、移动改变 `GetPosition`。
**Step 2-4:** 实现，全绿。**Step 5:** `git commit -m "feat: Gameplay 第一/第三人称控制"`

---

## 阶段 3：删 C++ 玩法 + 迁移

### Task 11: 删除 C++ 玩法 API

**Files（删/改）：**
- Delete: `engine/include/neon/scene/skills.hpp` + `engine/src/scene/skills.cpp`（若存在）
- Modify: `game_runtime.hpp`（删 `LoadSkills`/`CastSkill`/`SkillCooldownLeft`/`MeleeAttack*`/`AttackBox*`/`MeleeAttackImpl`/`AttackBoxImpl`/`ApplySkillStatuses`/`ApplyHit`/`TickSkillCooldowns` 声明 + `skills_`/`skillCooldowns_` 成员）
- Modify: `game_runtime_combat.cpp`（删对应实现；保留 `OverlapSphere`/`OverlapBox`/`SpawnProjectile`/`TickProjectiles`/`ProjectileTrail`/`ProjectileBurst`/`LagCompPosition`）
- Modify: `status.hpp`（删 `kStatusDefs` 规则表的 tick 语义，保留 id 常量 + 容器函数 + `StatusIdByName`）
- Modify: `bindings.hpp`/`bindings.cpp`（删 `meleeAttack`/`attackBox`/`castSkill`/`sceneSkillCooldown` hook + 对应 Native 函数 + `Register`）
- Modify: `game_runtime.cpp`（删 `Start` 里对应接线 + `TickStatuses` 旧分支）

**Step 1:** `grep -rE "MeleeAttack|AttackBox|CastSkill|LoadSkills|SkillCooldown|SkillTable|skills\.hpp"` 列引用点，逐一确认（含 server/editor/tests）。
**Step 2-4:** 删声明→实现→bindings→接线，构建到引擎库可编译（`neon_game`/`neon_server`/`neon_editor` 引用点见 Task 13）。
**Step 5:** `git commit -m "refactor: 删除 C++ 玩法 API，保留通用原语"`

---

### Task 12: 迁移 `test_combat.cpp` / `test_lagcomp.cpp`

**Files:** `tests/test_combat.cpp`、`tests/test_lagcomp.cpp`

- **保留**：`StatusApplyQueryRemove`/`StatusTicksAndExpires`/`WorldAddIdempotent`（status 容器机制；删 `StatusIdByName` 断言若已移）。
- **删除**：`SkillTableParsesAndValidates`（SkillTable 已删）、`CombatSkillCooldownAndMana`/`CombatMeleeSkillArc`/`CombatAttackBoxOriented`（行为转 Task 8 的 Gameplay.SkillTable Lua 测试）。
- **改写**：`CombatProjectileSkillDamagesAndBurns` → 用 `SpawnProjectile` + statuses + Lua `OnStatusTick` 验证 burning。
- **改写 lagcomp**：`MeleeAttackLagComp`/`AttackBoxLagComp` → `OverlapSphere(..., rewindTicks)`/`OverlapBox(..., rewindTicks)`；`LagCompPosition`/`LagCompHistoryRingCapped`/`LagCompServerRewindsByClientRtt` 机制测试保留（若 `SetAutoLagComp` 仍被 server 用）。

**Step 1-3:** 逐测试迁移/删除，构建全绿（测试数变化属预期）。
**Step 4:** `git commit -m "test: 迁移战斗/lagcomp 测试到 Overlap 原语 + Gameplay 库"`

---

### Task 13: 迁移 `realm.lua`

**Files:** `projects/neon_realm/assets/scripts/realm.lua`、`projects/neon_realm/skills.json`（若不再需要则删）

**迁移点：**
1. `CastSkill("fireball", origin, attack_dir(), hero)`（321 行）→ `Gameplay.Projectile(origin, attack_dir(), 16, 30, 2.5, 40, 0.8, hero, {{name="burning",duration=3,magnitude=3}})`。
2. `MeleeAttack(origin, attack_dir(), 2.2, 100, 28)`（374 行）→ `Gameplay.MeleeArc(origin, attack_dir(), 2.2, 100, 28, hero)`。
3. 元素技能（`wolvesInRadius`/`damageWolf`）可改用 `Gameplay.AoE`/`OverlapSphere`（可选，保持行为等价）。
4. 第一/第三人称切换（`fpsMode`）可改用 `Gameplay.FirstPerson`/`ThirdPerson`（可选，验证控制器可跑）。
5. 若 `skills.json` 无其它引用则删除，并从 server/editor 加载路径移除（`grep skills.json`/`skillsJson`）。

**Step 1-3:** 迁移 + 构建 + `neon_editor` F5 试玩验证火球/近战/元素技能/状态/冷却/HUD 行为等价。
**Step 4:** `git commit -m "refactor: realm.lua 改用 Gameplay 基础库"`

---

## 验收（全部完成后）

1. `& "build-msvc\Release\neon_tests.exe"` 全绿。
2. `grep -rE "MeleeAttack|AttackBox|CastSkill|SkillTable|LoadSkills|skills\.hpp" engine/ game/ server/ editor/` 无残留。
3. `neon_game`/`neon_server`/`neon_editor` 全量构建通过。
4. NeonRealm 经编辑器 F5 或 `neon_game` 试玩：火球/近战/元素技能/状态/冷却/HUD 行为等价，第一/第三人称可切换。
5. 基础库 `Gameplay`（技能/背包/控制器/属性）可由任意项目在 `on_start` 直接调用。

## 风险与回滚

- 每任务独立 commit，阶段 1 零回归；阶段 2 纯 Lua 增量，风险低；阶段 3 删除类任务若阻塞可停在上一 commit。
- lag-comp 机制（`poseSlots_`/`SetAutoLagComp`/`AutoLagCompTicks`）若 server 仍依赖则保留为引擎能力。
- 控制器依赖引擎相机/输入约定，迁移 `realm.lua` 时需逐项对照其现有 `fpsMode`/相机逻辑。
