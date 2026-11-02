-- lc_interact.lua
-- 第一人称交互：靠近物资箱/家具/房车按 E

local function forwardDot(dx, dz, dist)
  if dist < 0.001 then return 0 end
  local fx, fz = math.sin(player.yaw), math.cos(player.yaw)
  return (dx * fx + dz * fz) / dist
end

function nearestCrate()
  local best, bestScore, bestD = nil, -1, 1e9
  for i, c in ipairs(crates) do
    local dx, dz = c.x - player.x, c.z - player.z
    local d = math.sqrt(dx * dx + dz * dz)
    if d < 2.9 then
      local dot = forwardDot(dx, dz, d)
      local score = dot - d * 0.08
      if dot > 0.05 and score > bestScore then
        best, bestScore, bestD = i, score, d
      end
    end
  end
  return best
end

function nearestStation()
  local best, bestScore, bestD = nil, -1, 1e9
  for _, inst in pairs(instances) do
    if inst.layer == "floor" and inst.cat.use ~= nil then
      local wx, wz = rotOf(inst.lx, inst.lz)
      local dx, dz = wx - player.x, wz - player.z
      local d = math.sqrt(dx * dx + dz * dz)
      -- 大件家具按占地放宽一点
      local reach = 2.2 + math.max(inst.cat.w, inst.cat.h) * CELL * 0.25
      if d < reach then
        local dot = forwardDot(dx, dz, d)
        local score = dot - d * 0.10
        if dot > 0.15 and score > bestScore then
          best, bestScore, bestD = inst, score, d
        end
      end
    end
  end
  return best
end

function nearRv()
  local dx, dz = rv.x - player.x, rv.z - player.z
  return dx * dx + dz * dz < 100  -- 约 10m（车长 8.6m，站在车旁即可）
end

function updateInteraction()
  nearAction = nil
  if buildMode or phase ~= "parked" then return end

  local ci = nearestCrate()
  if ci then
    local c = crates[ci]
    nearAction = { kind = "crate", index = ci, verb = "搜刮物资箱",
                   detail = "左键/E 搜索", x = c.x, z = c.z }
    return
  end

  local inst = nearestStation()
  if inst then
    local verb, detail, enabled = stationPrompt(inst)
    if verb then
      nearAction = { kind = "station", inst = inst, verb = verb, detail = detail,
                     enabled = enabled, x = inst.lx, z = inst.lz }
      local wx, wz = rotOf(inst.lx, inst.lz)
      nearAction.x, nearAction.z = wx, wz
      return
    end
  end

  for _, p in ipairs(ponds) do
    local dx, dz = p.x - player.x, p.z - player.z
    local d = math.sqrt(dx * dx + dz * dz)
    if d < 4.8 then
      local dot = forwardDot(dx, dz, d)
      if dot > 0.12 then
        nearAction = { kind = "fish", verb = "钓鱼", detail = "E 抛竿（约 5-9 秒）",
                       x = p.x, z = p.z }
        return
      end
    end
  end

  if nearRv() then
    nearAction = { kind = "rv", verb = "驾驶房车", detail = "W 油门 / 空格停车 / B 改装",
                   x = rv.x, z = rv.z }
  end
end

function doInteract()
  if nearAction == nil then return false end
  if nearAction.kind == "crate" then
    searchCrate(nearAction.index)
    updateInteraction()
    return true
  elseif nearAction.kind == "station" then
    if nearAction.enabled == false then
      toast.text = nearAction.detail or "现在不能用"
      toast.t = 2.2
    elseif nearAction.inst.cat.use == "stove" then
      stationUI = { kind = "stove", inst = nearAction.inst }
    else
      useStation(nearAction.inst)
    end
    updateInteraction()
    return true
  elseif nearAction.kind == "fish" then
    startFishing()
    return true
  elseif nearAction.kind == "rv" then
    setPhase("driving")
    return true
  end
  return false
end
