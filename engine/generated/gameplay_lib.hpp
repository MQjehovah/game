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
)LUA";

} // namespace neon::embedded
