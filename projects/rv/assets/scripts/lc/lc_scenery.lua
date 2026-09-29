-- lc_scenery.lua
-- 山林越野场景：松林/岩石/仙人掌/灌木回收循环 + 标记/篝火/油桶

function sceneryScale(kind)
  if kind == "rock" then
    return 1.2 + math.random() * 1.6
  elseif kind == "cactus" then
    return 0.8 + math.random() * 0.9
  elseif kind == "tree" then
    return 0.9 + math.random() * 0.9
  end
  return 1
end

function placeScenery(p, fx, fz, fMin, fMax)
  for _ = 1, 6 do
    local f = fMin + math.random() * (fMax - fMin)
    local lat = (math.random() * 2 - 1) * 44
    if math.abs(lat) >= 6 or fMin < 0 then
      local wx = rv.x + fx * f - fz * lat
      local wz = rv.z + fz * f + fx * lat
      local blocked = false
      local rdx, rdz = wx - rv.x, wz - rv.z
      if rdx * rdx + rdz * rdz < 100 then blocked = true end  -- 房车 10m 内
      for _, nd in ipairs(NODES) do
        local ddx, ddz = wx - nd.x, wz - nd.z
        if ddx * ddx + ddz * ddz < 121 then blocked = true break end  -- 节点 11m
      end
      if not blocked then
        local s = sceneryScale(p.kind)
        local y = 0
        if p.kind == "scrub" then
          y = 0.015
          SetScale(p.ent, 0.5 + math.random(), 0.03 + math.random() * 0.04, 0.5 + math.random())
        else
          SetScale(p.ent, s, s, s)
          if p.kind == "rock" then y = s * 0.55 end
        end
        SetPosition(p.ent, { x = wx, y = y, z = wz })
        SetRotationY(p.ent, math.random() * math.pi * 2)
        p.x, p.z = wx, wz
        -- 树/岩石 = 静态碰撞体（越野障碍），回收 = 传送刚体
        if p.kind == "tree" or p.kind == "rock" then
          local half = (p.kind == "rock") and s * 0.7 or 0.3
          local cy = (p.kind == "rock") and s * 0.55 or 1.0
          if p.body then
            PhysicsSetPosition(p.body, { x = wx, y = cy, z = wz })
          else
            p.body = PhysicsAddBox({ x = wx, y = cy, z = wz },
                                   { x = half, y = cy, z = half }, false, {})
          end
        end
        return
      end
    end
  end
  local wx = rv.x + fx * (fMax + 20) - fz * 30
  local wz = rv.z + fz * (fMax + 20) + fx * 30
  SetPosition(p.ent, { x = wx, y = 0.3, z = wz })
  p.x, p.z = wx, wz
end

local scenery = {}
function spawnScenery()
  if #scenery > 0 then return end
  for _, t in ipairs(SCENERY) do
    for _ = 1, t.n do
      local ent = SpawnPrefab(t.pre, { x = 0, y = -50, z = 0 })
      if ent ~= nil then
        local p = { ent = ent, kind = t.kind, x = 0, z = 0, body = nil }
        placeScenery(p, 0, 1, -45, 70)  -- 初始绕营地铺开
        scenery[#scenery + 1] = p
      end
    end
  end
end

function recycleScenery(fx, fz)
  for _, p in ipairs(scenery) do
    local dx, dz = p.x - rv.x, p.z - rv.z
    local f = dx * fx + dz * fz
    local lat = dx * (-fz) + dz * fx
    if f < -40 or f > 95 or math.abs(lat) > 55 then
      placeScenery(p, fx, fz, 25, 75)
    end
  end
end

-- 节点标记柱 + 营地篝火 + 加油站油桶
function spawnMarkers()
  for _, nd in ipairs(NODES) do
    if nd.id ~= "camp" then
      local ent = SpawnPrefab("rv_marker", { x = nd.x, y = 1.6, z = nd.z })
      if ent ~= nil then
        SetScale(ent, 0.16, 3.2, 0.16)
        PhysicsAddBox({ x = nd.x, y = 1.6, z = nd.z },
                      { x = 0.09, y = 1.6, z = 0.09 }, false, {})
      end
    end
  end
  local fire = SpawnPrefab("rv_campfire", { x = 1.8, y = 0.12, z = 2.0 })
  if fire ~= nil then SetScale(fire, 0.9, 0.9, 0.9) end
  local gas = nodeById("gasstop")
  if gas then
    for _, off in ipairs({ {2.0, 1.0}, {2.6, -0.4}, {1.2, -0.6} }) do
      local ent = SpawnPrefab("rv_fuelcan",
                              { x = gas.x + off[1], y = 0.35, z = gas.z + off[2] })
      if ent ~= nil then SetScale(ent, 0.5, 0.7, 0.5) end
    end
  end
end
