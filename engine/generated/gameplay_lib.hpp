#pragma once

namespace neon::embedded {

// Gameplay 基础玩法库（引擎内嵌 Lua）。GameRuntime::Start 在加载项目脚本
// 之前对 Lua host 执行一次，注入全局 `Gameplay` 表供项目脚本复用。
inline const char* kGameplayLibLua = R"LUA(
Gameplay = { version = 1 }

-- 属性：基于 GameVar 的键值存取（HP/MP/XP/level/gold 等）
Gameplay.Stats = {}
function Gameplay.Stats.Get(k) return GetVar(k) end
function Gameplay.Stats.Set(k, v) SetVar(k, v) end
function Gameplay.Stats.Add(k, d) SetVar(k, (GetVar(k) or 0) + d) end

-- 冷却：名字 -> 剩余秒数（带状态对象）
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

-- 命中/AoE/投射物（依赖 OverlapSphere/OverlapBox/SpawnProjectile/GetHealth/SetHealth）
local function SameEntity(a, b)
  if a == nil or b == nil then return false end
  return a.id == b.id and a.gen == b.gen
end

-- 弧线近战：原点+方向+范围+张角(度)+伤害，命中返回数量（caster 不伤己）
Gameplay.MeleeArc = function(origin, dir, range, arcDeg, damage, caster)
  local cosArc = math.cos(math.rad(arcDeg * 0.5))
  local dlen = math.sqrt(dir.x*dir.x + dir.y*dir.y + dir.z*dir.z)
  local fx, fz = dir.x, dir.z
  if dlen > 1e-3 then fx, fz = dir.x / dlen, dir.z / dlen end
  local hits = OverlapSphere(origin, range)
  local n = 0
  for _, h in ipairs(hits) do
    if not SameEntity(h.entity, caster) then
      local dx, dz = h.x - origin.x, h.z - origin.z
      local horiz = math.sqrt(dx*dx + dz*dz)
      if horiz > 1e-4 and math.abs(h.y - origin.y) <= 2.0 then
        local dot = (dx/horiz)*fx + (dz/horiz)*fz
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

-- 圆形 AoE
Gameplay.AoE = function(origin, radius, damage, caster)
  local hits = OverlapSphere(origin, radius)
  local n = 0
  for _, h in ipairs(hits) do
    if not SameEntity(h.entity, caster) then
      local hp = GetHealth(h.entity)
      if hp ~= nil then SetHealth(h.entity, math.max(0, hp - damage)) end
      n = n + 1
    end
  end
  return n
end

-- 盒攻击（yawDeg 角度）
Gameplay.BoxAttack = function(center, half, yawDeg, damage)
  local hits = OverlapBox(center, half, yawDeg)
  local n = 0
  for _, h in ipairs(hits) do
    local hp = GetHealth(h.entity)
    if hp ~= nil then SetHealth(h.entity, math.max(0, hp - damage)) end
    n = n + 1
  end
  return n
end

-- 投射物：SpawnProjectile 薄包装
Gameplay.Projectile = function(origin, dir, speed, damage, life, range, hitRadius, caster, statuses)
  SpawnProjectile(origin, dir, speed, damage, life, caster, range, hitRadius, statuses)
end

-- 状态效果定义 + 默认 tick 规则（名称对应 C++ 的 kStatusBurning=1 等 id 常量）。
-- 引擎 TickStatuses 在每个 interval 边界调用全局 OnStatusTick(entity, id, magnitude)；
-- 项目脚本可覆盖 OnStatusTick 以改写默认 tick 行为。
Gameplay.statusDefs = {
  burning = { id = 1, tick = function(ent, mag) local h = GetHealth(ent); if h > 0 then SetHealth(ent, math.max(0, h - mag)) end end },
  poison  = { id = 2, tick = function(ent, mag) local h = GetHealth(ent); if h > 0 then SetHealth(ent, math.max(0, h - mag)) end end },
  regen   = { id = 3, tick = function(ent, mag) local h = GetHealth(ent); if h > 0 then SetHealth(ent, math.min(GetMaxHealth(ent), h + mag)) end end },
  slow    = { id = 4, tick = function() end },
}
-- 覆盖已有状态的 tick 规则（项目脚本可 RegisterStatus("burning", myTick) 改写引擎默认）。
Gameplay.RegisterStatus = function(name, tick)
  local def = Gameplay.statusDefs[name]
  if def then def.tick = tick end
end
-- 引擎每个状态 tick 调用此全局函数；默认实现按 statusDefs 分发。
function OnStatusTick(ent, id, mag)
  for _, d in pairs(Gameplay.statusDefs) do
    if d.id == id and d.tick then d.tick(ent, mag) end
  end
end

-- 技能系统：JSON 文本 -> 技能表；cast 检查冷却/mana，按 kind 分发
Gameplay.SkillTable = {}
function Gameplay.SkillTable.fromJson(json)
  local parsed = Json.Parse(json)          -- 引擎 Json.Parse binding（返回 Lua table）
  local tbl = { skills = {}, cds = Gameplay.Cooldowns.new() }
  for name, def in pairs(parsed.skills or {}) do
    tbl.skills[name] = def
  end
  return tbl
end
function Gameplay.SkillTable.cast(tbl, name, origin, dir, caster)
  local def = tbl.skills[name]
  if def == nil then return 0 end
  if not Gameplay.Cooldowns.ready(tbl.cds, name) then return 0 end
  local manaCost = def.manaCost or 0
  if manaCost > 0 then
    local mana = Gameplay.Stats.Get("mana") or 0
    if mana < manaCost then return 0 end
    Gameplay.Stats.Set("mana", mana - manaCost)
  end
  if (def.cooldown or 0) > 0 then Gameplay.Cooldowns.set(tbl.cds, name, def.cooldown) end
  if def.kind == "projectile" then
    Gameplay.Projectile(origin, dir, def.speed or 12, def.damage or 0, def.life or 2,
                        def.range or 0, 0.8, caster, def.statuses or {})
  elseif def.kind == "melee" then
    Gameplay.MeleeArc(origin, dir, def.meleeRange or 2, def.arcDeg or 90, def.damage or 0, caster)
  else -- "box"
    local yaw = math.atan(dir.x, dir.z)
    Gameplay.BoxAttack(origin,
                       { def.boxHalfX or 1, def.boxHalfY or 1, def.boxHalfZ or 1 },
                       math.deg(yaw), def.damage or 0)
  end
  return 1
end

-- 背包系统：物品定义 def = { id, name, stackable, maxStack, onUse }，onUse 是可选
-- 回调 function(ent)。slots 为 map[slotKey] = { def=..., count=... }：堆叠物品
-- slotKey = def.id，非堆叠物品 slotKey = def.id .. "#" .. n（每个占一个独立槽）。
Gameplay.Inventory = {}

local function CountSlots(bag)
  local n = 0
  for _ in pairs(bag.slots) do n = n + 1 end
  return n
end

-- 非堆叠物品的下一个空闲槽键（避开 remove 留下的空洞）。
local function NextNonStackKey(bag, id)
  local prefix = id .. "#"
  local maxN = 0
  for key in pairs(bag.slots) do
    if key:sub(1, #prefix) == prefix then
      local n = tonumber(key:sub(#prefix + 1))
      if n and n > maxN then maxN = n end
    end
  end
  return prefix .. (maxN + 1)
end

function Gameplay.Inventory.new(capacity)
  return { slots = {}, capacity = capacity or 24, currency = {} }
end

function Gameplay.Inventory.add(bag, def, count)
  count = count or 1
  if count <= 0 then return true end
  local id = def.id
  if def.stackable then
    local maxStack = def.maxStack or 1
    local slot = bag.slots[id]
    if slot then
      if slot.count + count > maxStack then return false end
      slot.count = slot.count + count
      return true
    end
    if CountSlots(bag) >= bag.capacity then return false end
    if count > maxStack then return false end
    bag.slots[id] = { def = def, count = count }
    return true
  end
  if CountSlots(bag) + count > bag.capacity then return false end
  for _ = 1, count do
    bag.slots[NextNonStackKey(bag, id)] = { def = def, count = 1 }
  end
  return true
end

function Gameplay.Inventory.remove(bag, itemId, count)
  count = count or 1
  local removed = 0
  local toClear = {}
  for key, s in pairs(bag.slots) do
    if removed < count and s.def.id == itemId then
      local take = math.min(s.count, count - removed)
      s.count = s.count - take
      removed = removed + take
      if s.count <= 0 then toClear[key] = true end
    end
  end
  for key in pairs(toClear) do bag.slots[key] = nil end
  return removed
end

function Gameplay.Inventory.count(bag, itemId)
  local total = 0
  for _, s in pairs(bag.slots) do
    if s.def.id == itemId then total = total + s.count end
  end
  return total
end

function Gameplay.Inventory.use(bag, itemId, ent)
  local foundKey, foundSlot = nil, nil
  for key, s in pairs(bag.slots) do
    if s.def.id == itemId then foundKey, foundSlot = key, s break end
  end
  if foundSlot == nil then return false end
  if foundSlot.def.onUse then
    if foundSlot.def.onUse(ent) == false then return false end
  end
  foundSlot.count = foundSlot.count - 1
  if foundSlot.count <= 0 then bag.slots[foundKey] = nil end
  return true
end

function Gameplay.Inventory.addCurrency(bag, name, amount)
  bag.currency[name] = (bag.currency[name] or 0) + amount
end

function Gameplay.Inventory.getCurrency(bag, name)
  return bag.currency[name] or 0
end

-- 最小 JSON 字符串转义（物品 id / 货币名）。回调 onUse 不序列化。
local function JsonQuote(s)
  s = tostring(s)
  s = s:gsub("\\", "\\\\")
  s = s:gsub('"', '\\"')
  s = s:gsub("\n", "\\n"):gsub("\r", "\\r"):gsub("\t", "\\t")
  return '"' .. s .. '"'
end

function Gameplay.Inventory.save(bag)
  local out = { '{"slots":[' }
  local first = true
  for _, s in pairs(bag.slots) do
    if not first then out[#out + 1] = "," end
    first = false
    out[#out + 1] = '{"id":' .. JsonQuote(s.def.id) .. ',"count":' .. s.count .. '}'
  end
  out[#out + 1] = '],"currency":{'
  first = true
  for name, amount in pairs(bag.currency) do
    if not first then out[#out + 1] = "," end
    first = false
    out[#out + 1] = JsonQuote(name) .. ":" .. (tonumber(amount) or 0)
  end
  out[#out + 1] = '}}'
  return table.concat(out)
end

-- load(bag, json, defs)：defs 为 { [id] = def }，用于恢复 def 引用与 onUse 回调。
function Gameplay.Inventory.load(bag, json, defs)
  local data = Json.Parse(json)
  bag.slots = {}
  bag.currency = {}
  if data == nil then return bag end
  if data.slots then
    for _, s in pairs(data.slots) do
      local def = defs and s and s.id and defs[s.id]
      if def then
        if def.stackable then
          bag.slots[def.id] = { def = def, count = s.count or 0 }
        else
          for _ = 1, (s.count or 0) do
            bag.slots[NextNonStackKey(bag, def.id)] = { def = def, count = 1 }
          end
        end
      end
    end
  end
  if data.currency then
    for name, amount in pairs(data.currency) do bag.currency[name] = amount end
  end
  return bag
end

-- 第一人称（带状态对象）：鼠标视角 + 相对移动 + 相机在眼睛处
Gameplay.FirstPerson = {}
function Gameplay.FirstPerson.new(hero)
  return { hero = hero, yaw = 0, pitch = 0.32, eyeH = 1.6, camDist = 2.0,
           sens = 0.003, grounded = true, yvel = 0, groundY = 0.9, speed = 6 }
end
function Gameplay.FirstPerson.tick(c, dt)
  c.yaw = c.yaw - InputMouseX() * c.sens
  c.pitch = math.max(-1.2, math.min(1.2, c.pitch + InputMouseY() * c.sens))
  local cy = c.yaw
  local ix, iz = ActionAxis("move_strafe"), ActionAxis("move_forward")
  local fwd = { x = -math.sin(cy), z = -math.cos(cy) }
  local right = { x = math.cos(cy), z = -math.sin(cy) }
  local dx = right.x * ix + fwd.x * iz
  local dz = right.z * ix + fwd.z * iz
  local dlen = math.sqrt(dx * dx + dz * dz)
  if dlen > 1 then dx, dz = dx / dlen, dz / dlen end
  local pos = GetPosition(c.hero)
  if pos == nil then return end
  if ActionDown("jump") and c.grounded then c.yvel = 8; c.grounded = false end
  if not c.grounded then
    c.yvel = c.yvel - 20 * dt
    pos.y = pos.y + c.yvel * dt
    if pos.y <= c.groundY then pos.y = c.groundY; c.yvel = 0; c.grounded = true end
  end
  pos.x = pos.x + dx * c.speed * dt
  pos.z = pos.z + dz * c.speed * dt
  SetPosition(c.hero, pos)
  SetRotationY(c.hero, c.yaw)
  local cd = math.cos(c.pitch)
  local ex, ey, ez = pos.x, pos.y + c.eyeH, pos.z
  SetVar("cameraMouseLock", 1)
  SetVar("cameraYaw", c.yaw)
  SetVar("cameraPitch", c.pitch)
  SetVar("cameraDist", c.camDist)
  SetVar("cameraFocus", { x = ex - math.sin(c.yaw) * cd * c.camDist,
                          y = ey - math.sin(c.pitch) * c.camDist,
                          z = ez - math.cos(c.yaw) * cd * c.camDist })
end

-- 第三人称：轨道相机 + 面向移动方向
Gameplay.ThirdPerson = {}
function Gameplay.ThirdPerson.new(hero)
  return { hero = hero, yaw = 0, grounded = true, yvel = 0, groundY = 0.9, speed = 6,
           camDist = 7.5, camPitch = 0.42 }
end
function Gameplay.ThirdPerson.tick(c, dt)
  local ix, iz = ActionAxis("move_strafe"), ActionAxis("move_forward")
  local fwd = { x = -math.sin(c.yaw), z = -math.cos(c.yaw) }
  local right = { x = math.cos(c.yaw), z = -math.sin(c.yaw) }
  local dx = right.x * ix + fwd.x * iz
  local dz = right.z * ix + fwd.z * iz
  local len = math.sqrt(dx * dx + dz * dz)
  if len > 1 then dx, dz = dx / len, dz / len end
  local pos = GetPosition(c.hero)
  if pos == nil then return end
  if ActionDown("jump") and c.grounded then c.yvel = 8; c.grounded = false end
  if not c.grounded then
    c.yvel = c.yvel - 20 * dt
    pos.y = pos.y + c.yvel * dt
    if pos.y <= c.groundY then pos.y = c.groundY; c.yvel = 0; c.grounded = true end
  end
  pos.x = pos.x + dx * c.speed * dt
  pos.z = pos.z + dz * c.speed * dt
  if len > 0.01 then
    c.yaw = math.atan(dx, -dz)
    SetRotationY(c.hero, c.yaw)
  end
  SetPosition(c.hero, pos)
  SetVar("cameraMouseLock", 0)
  SetVar("cameraYaw", c.yaw)
  SetVar("cameraPitch", c.camPitch)
  SetVar("cameraDist", c.camDist)
  SetVar("cameraFocus", { x = pos.x, y = pos.y + 1.2, z = pos.z })
end
)LUA";

} // namespace neon::embedded
