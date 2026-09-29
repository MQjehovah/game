-- ===========================================================================
-- lc_flowadapters.lua — 预设体/系统适配器节点包（"HOW" 原语，不含决策）
-- 每个节点只包装一个引擎调用；参数（决策数值）全部由流程图注入。
-- ===========================================================================
LC = LC or {}
LC.flow = LC.flow or {}

-- === 炮塔 ================================================================
LC.flow.register("game/turret-fire", function(node, g)
  -- 找射程内最近丧尸开火（目标选择/特效是 HOW；冷却/伤害在图参数里）
  local cd = LC.flow.evalVal(g, node.cooldown) or 1.2
  local dmg = LC.flow.evalVal(g, node.damage) or 1
  local range = LC.flow.evalVal(g, node.range) or 10
  if turretT > 0 then return "exec" end  -- 冷却未到（图 timer 已控制频率，此处兜底）
  local best, bestD = nil, range * range
  for i, z in ipairs(horde) do
    local ddx, ddz = z.x - rv.x, z.z - rv.z
    local d2 = ddx * ddx + ddz * ddz
    if d2 < bestD then best, bestD = i, d2 end
  end
  if not best then return "exec" end
  turretT = cd
  local z = horde[best]
  local turret = hasModule("turret")
  local tx, tz = rotOf(turret and turret.lx or 0, turret and turret.lz or 2)
  EmitParticles({ pos = { x = tx, y = 3.1, z = tz }, count = 4,
                  speedMin = 2, speedMax = 5, lifeMin = 0.05, lifeMax = 0.1,
                  sizeStart = 0.14, sizeEnd = 0.03,
                  color = { r = 1, g = 0.9, b = 0.4, a = 0.95 },
                  colorEnd = { r = 1, g = 0.7, b = 0.1, a = 0.0 }, additive = true })
  EmitParticles({ pos = { x = z.x, y = 1.0, z = z.z }, count = 8,
                  speedMin = 0.8, speedMax = 2.5, lifeMin = 0.2, lifeMax = 0.4,
                  sizeStart = 0.16, sizeEnd = 0.04,
                  color = { r = 1, g = 0.75, b = 0.25, a = 0.9 },
                  colorEnd = { r = 1, g = 0.6, b = 0.15, a = 0.0 }, additive = true })
  z.hp = z.hp - dmg
  if z.hp <= 0 then killZombie(best) end
  return "exec"
end)

-- === 车辆状态查询（branch 的 $var 需要数字，桥接 Lua 状态到图变量） ==========
LC.flow.register("game/vehicle-sync", function(node, g)
  -- 把车辆/资源状态同步到图变量（branch/math 可读）
  g.vars.fuel = fuel or 0
  g.vars.fuel_pct = fuel and (fuel / VEH.tank) or 0
  g.vars.dur = dur or 0
  g.vars.dur_pct = dur and (dur / VEH.durMax) or 0
  g.vars.day = day or 1
  return "exec"
end)

-- === 建造统计 =============================================================
LC.flow.register("game/build-stats", function(node, g)
  -- 重新计算模块统计并把结果写入图变量（公式在图上用 flow/math 编排）
  local stats = recomputeStats()
  g.vars.stat_power = stats.powerGen or 0
  g.vars.stat_defense = stats.defense or 0
  g.vars.stat_storage = stats.storage or 0
  g.vars.stat_modules = stats.moduleCount or 0
  return "exec"
end)

-- === 难度注入 =============================================================
LC.flow.register("game/difficulty", function(node, g)
  -- 按天数缩放夜袭参数（公式在图上，注入到 RAID 表）
  local hpBase = LC.flow.evalVal(g, node.hpBase) or RAID.hpBase
  local speed = LC.flow.evalVal(g, node.speed) or RAID.speed
  local spawnEvery = LC.flow.evalVal(g, node.spawnEvery) or RAID.spawnEvery
  local aliveMax = LC.flow.evalVal(g, node.aliveMax) or RAID.aliveMax
  RAID.hpBase = hpBase
  RAID.speed = speed
  RAID.spawnEvery = spawnEvery
  RAID.aliveMax = aliveMax
  return "exec"
end)

-- === 编辑器元数据 ==========================================================
LC.flow.describe("game/turret-fire", "炮塔开火", "战斗", { "exec" },
                 { { "cooldown", "n" }, { "damage", "n" }, { "range", "n" } })
LC.flow.describe("game/vehicle-sync", "车辆状态同步", "系统", { "exec" }, {})
LC.flow.describe("game/build-stats", "建造统计", "系统", { "exec" }, {})
LC.flow.describe("game/difficulty", "难度注入", "系统", { "exec" },
                 { { "hpBase", "n" }, { "speed", "n" }, { "spawnEvery", "n" }, { "aliveMax", "n" } })
