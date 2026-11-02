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
      for _, p in ipairs(ponds) do
        local ddx, ddz = wx - p.x, wz - p.z
        if ddx * ddx + ddz * ddz < 144 then blocked = true break end  -- 水塘 12m
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

-- 水塘（钓鱼点）
function spawnPond(x, z)
  local ent = SpawnPrefab("rv_pond", { x = x, y = 0.02, z = z })
  if ent ~= nil then
    ponds[#ponds + 1] = { ent = ent, x = x, z = z }
  end
end

function spawnPonds()
  if #ponds > 0 then return end
  -- 放在营地左后方：避开开局去废宅/加油站的直线路径，避免房车开进水里
  spawnPond(-18, -12)
end

-- 简易路边建筑：一个带门洞/窗的废弃小屋（纯装饰，原型级）
function spawnRuinFor(nd)
  if nd.ruinSpawned then return end
  nd.ruinSpawned = true
  local cx, cz = nd.x + 6.5, nd.z + 6.5
  local function cube(pre, x, y, z, sx, sy, sz)
    local e = SpawnPrefab(pre, { x = x, y = y, z = z })
    if e ~= nil then SetScale(e, sx, sy, sz) end
  end
  -- 地板 / 后墙 / 两侧墙 / 屋顶
  cube("rv_panel_dark",  cx, 0.03, cz, 6.2, 0.06, 5.2)
  cube("rv_panel_light", cx, 1.25, cz - 2.55, 6.2, 2.5, 0.16)
  cube("rv_panel_light", cx - 3.05, 1.25, cz, 0.16, 2.5, 5.2)
  cube("rv_panel_light", cx + 3.05, 1.25, cz, 0.16, 2.5, 5.2)
  cube("rv_panel_dark",  cx, 2.55, cz, 6.4, 0.16, 5.4)
  -- 前墙两段（中间留门洞）
  cube("rv_panel_light", cx - 1.9, 1.25, cz + 2.55, 2.3, 2.5, 0.16)
  cube("rv_panel_light", cx + 1.9, 1.25, cz + 2.55, 2.3, 2.5, 0.16)
  -- 窗与门框
  cube("rv_panel_window", cx - 3.0, 1.55, cz + 0.3, 0.04, 0.7, 1.4)
  cube("rv_panel_window", cx + 3.0, 1.55, cz + 0.3, 0.04, 0.7, 1.4)
end

-- 节点标记柱
function spawnMarkerFor(nd)
  if nd.id == "camp" then return end
  local ent = SpawnPrefab("rv_marker", { x = nd.x, y = 1.6, z = nd.z })
  if ent ~= nil then
    SetScale(ent, 0.16, 3.2, 0.16)
    PhysicsAddBox({ x = nd.x, y = 1.6, z = nd.z },
                  { x = 0.09, y = 1.6, z = 0.09 }, false, {})
  end
end

-- 程序化公路地点：向前方推进一段里程后生成
local poiSeq = 0
function trySpawnPoi()
  if #NODES >= 15 then return end
  for _ = 1, 13 do
    local a = rv.yaw + (math.random() * 2 - 1) * 0.9
    local dist = 110 + math.random() * 70
    local x = rv.x + math.sin(a) * dist
    local z = rv.z + math.cos(a) * dist
    local ok = true
    for _, nd in ipairs(NODES) do
      local dx, dz = x - nd.x, z - nd.z
      if dx * dx + dz * dz < 45 * 45 then ok = false break end
    end
    if ok then
      repeat poiSeq = poiSeq + 1 until nodeById("poi_" .. poiSeq) == nil
      local kind = POI_KINDS[math.random(1, #POI_KINDS)]
      local nearest, bestD = NODES[1], 1e18
      for _, nd in ipairs(NODES) do
        local dx, dz = x - nd.x, z - nd.z
        local d2 = dx * dx + dz * dz
        if d2 < bestD then nearest, bestD = nd, d2 end
      end
      local nd = {
        id = "poi_" .. poiSeq,
        name = kind.name .. " #" .. poiSeq,
        x = x, z = z,
        danger = kind.danger,
        loot = kind.loot,
        links = { nearest.id },
        generated = true,
      }
      NODES[#NODES + 1] = nd
      generatedPois[#generatedPois + 1] = {
        id = nd.id, name = nd.name, x = nd.x, z = nd.z,
        danger = nd.danger, loot = nd.loot, links = nd.links,
      }
      spawnMarkerFor(nd)
      spawnRuinFor(nd)
      if kind.pond then spawnPond(x - 7, z + 7) end
      -- 地点周围的残骸/碎石（纯装饰，不挡路）
      for k = 1, 2 do
        local ox = (math.random() * 2 - 1) * 5
        local oz = (math.random() * 2 - 1) * 5
        local prop = SpawnPrefab(k == 1 and "rv_wreck" or "rv_rock",
                                 { x = x + ox, y = 0.2, z = z + oz })
        if prop ~= nil then
          local s = 0.6 + math.random() * 0.5
          SetScale(prop, s, s, s)
          SetRotationY(prop, math.random() * math.pi * 2)
        end
      end
      toast.text = "前方发现地点：" .. nd.name
      toast.t = 4.0
      return
    end
  end
end

-- 节点标记柱 + 营地篝火 + 加油站油桶
function spawnMarkers()
  for _, nd in ipairs(NODES) do
    spawnMarkerFor(nd)
    if nd.id ~= "camp" then spawnRuinFor(nd) end
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
