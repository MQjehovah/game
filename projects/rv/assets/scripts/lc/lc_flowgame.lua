-- lc_flowgame.lua — 房车玩法节点包：把游戏操作注册进流程图
-- （lc_flow 是与游戏解耦的运行时；本文件把夜袭/搜刮/车辆等节点接进去）
LC = LC or {}
LC.flow = LC.flow or {}

LC.flow.register("game/toast", function(node, g)
  toast.text = tostring(node.text or "")
  toast.t = node.duration or 2.5
  return "exec"
end)

LC.flow.register("game/horde", function(node, g)
  -- 夜袭尸群：绕房车环刷 count 只
  for _ = 1, (node.count or 3) do spawnZombie() end
  return "exec"
end)

LC.flow.register("game/loot", function(node, g)
  -- 材料入库（停靠层激活时进搜获袋，否则直接入库存）
  local mat = tostring(node.mat or "metal")
  local lo = node.min or 1
  local hi = node.max or (lo + 2)
  local n = lo + math.random(0, math.max(0, hi - lo))
  if atNode then
    stopBag[mat] = (stopBag[mat] or 0) + n
  else
    materials[mat] = (materials[mat] or 0) + n
  end
  return "exec"
end)

LC.flow.register("game/fuel", function(node, g)
  fuel = math.max(0, math.min(VEH.tank, fuel + (node.amount or 10)))
  return "exec"
end)

LC.flow.register("game/durability", function(node, g)
  dur = math.max(0, math.min(VEH.durMax, dur + (node.amount or -3)))
  return "exec"
end)

LC.flow.register("game/animparam", function(node, g)
  -- 目标：player / rv（占位：player 用得最多）
  if onFoot and player.active then
    SetAnimParam(player.ent, tostring(node.name or ""), node.value or 1)
  end
  return "exec"
end)

-- ---------------------------------------------------------------------------
-- 节点元数据声明（可视化编辑器扫描源码发现；类型本体在上面 register）
-- ---------------------------------------------------------------------------
LC.flow.describe("game/toast", "提示文字", "玩法", { "exec" },
                 { { "text", "s" }, { "duration", "n" } })
LC.flow.describe("game/horde", "尸群刷新", "玩法", { "exec" }, { { "count", "n" } })
LC.flow.describe("game/loot", "搜刮材料", "玩法", { "exec" },
                 { { "mat", "s" }, { "min", "n" }, { "max", "n" } })
LC.flow.describe("game/fuel", "油量增减", "玩法", { "exec" }, { { "amount", "n" } })
LC.flow.describe("game/durability", "耐久增减", "玩法", { "exec" }, { { "amount", "n" } })
LC.flow.describe("game/animparam", "动画参数", "玩法", { "exec" },
                 { { "target", "s" }, { "param", "s" }, { "value", "n" } })

-- ---------------------------------------------------------------------------
-- 停靠远征循环节点（停靠→搜刮→威胁→夜袭→撤离→入库，全部可上流程图）
-- ---------------------------------------------------------------------------
LC.flow.register("game/crates", function(node)
  spawnCrates()  -- 物资箱生成（数量/散布仍由 STOP 配置驱动）
  return "exec"
end)

LC.flow.register("game/clear", function(node)
  despawnCrates()
  despawnHorde()
  stopThreat = 0
  return "exec"
end)

g_bankKeep = g_bankKeep  -- 撤离折扣：调用方在触发 leave 前设置（nil = 全额）
LC.flow.register("game/bank", function(node)
  lastBankText = bankStopBag(g_bankKeep or node.keep or 1.0)
  g_bankKeep = nil
  return "exec"
end)

LC.flow.register("game/day", function(node)
  day = day + (node.amount or 1)
  atNode = nil
  return "exec"
end)

LC.flow.register("game/evac", function(node)
  if evacT < 0 then evacT = STOP.evacTime end
  return "exec"
end)

LC.flow.describe("game/crates", "生成物资箱", "玩法", { "exec" }, {})
LC.flow.describe("game/clear", "清场", "玩法", { "exec" }, {})
LC.flow.describe("game/bank", "搜获入库", "玩法", { "exec" }, { { "keep", "n" } })
LC.flow.describe("game/day", "推进天数", "玩法", { "exec" }, { { "amount", "n" } })
LC.flow.describe("game/evac", "开始撤离", "玩法", { "exec" }, {})
