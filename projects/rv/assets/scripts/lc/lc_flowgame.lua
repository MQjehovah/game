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
