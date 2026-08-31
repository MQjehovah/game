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
  local hits = OverlapSphere(origin, range)
  local n = 0
  for _, h in ipairs(hits) do
    if not SameEntity(h.entity, caster) then
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
)LUA";

} // namespace neon::embedded
