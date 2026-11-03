-- lc_scenery.lua
-- 山林越野场景：松林/岩石/仙人掌/灌木回收循环 + 标记/篝火/油桶

function sceneryScale(kind)
  if kind == "rock" then
    return 1.2 + math.random() * 1.6
  elseif kind == "cactus" then
    return 0.8 + math.random() * 0.9
  elseif kind == "tree" then
    return 0.9 + math.random() * 0.9
  elseif kind == "grass" then
    return 1.2 + math.random() * 1.0
  elseif kind == "flower" then
    return 1.2 + math.random() * 0.7
  elseif kind == "bush" then
    return 0.8 + math.random() * 0.8
  elseif kind == "patch" then
    return 0.9 + math.random() * 0.5
  end
  return 1
end

local roadPaths = {}

function distToRoadSq(x, z)
  local best = 1e18
  for _, s in ipairs(roadPaths) do
    local dx, dz = s.bx - s.ax, s.bz - s.az
    local len2 = dx * dx + dz * dz
    local t = 0
    if len2 > 0.001 then
      t = ((x - s.ax) * dx + (z - s.az) * dz) / len2
      t = math.max(0, math.min(1, t))
    end
    local px, pz = s.ax + dx * t, s.az + dz * t
    local ddx, ddz = x - px, z - pz
    local d2 = ddx * ddx + ddz * ddz
    if d2 < best then best = d2 end
  end
  return best
end

function placeScenery(p, fx, fz, fMin, fMax)
  local meadow = p.meadow == true
  local big = p.kind == "patch"
  for _ = 1, 6 do
    local f = fMin + math.random() * (fMax - fMin)
    local lat = (math.random() * 2 - 1) * (meadow and 26 or 44)
    if math.abs(lat) >= 6 or fMin < 0 then
      local wx = rv.x + fx * f - fz * lat
      local wz = rv.z + fz * f + fx * lat
      local blocked = false
      local rdx, rdz = wx - rv.x, wz - rv.z
      local rvR2 = big and 20 or (meadow and 9 or 100)
      if rdx * rdx + rdz * rdz < rvR2 then blocked = true end
      for _, nd in ipairs(NODES) do
        local ddx, ddz = wx - nd.x, wz - nd.z
        if ddx * ddx + ddz * ddz < (meadow and 49 or 121) then
          blocked = true break
        end
        -- Meadow plants must also stay out of the POI footprint.
        if meadow then
          local pcx, pcz = nd.x + 6.5, nd.z + 6.5
          if math.abs(wx - pcx) < 6.2 and math.abs(wz - pcz) < 5.6 then
            blocked = true break
          end
        end
      end
      for _, pond in ipairs(ponds) do
        local ddx, ddz = wx - pond.x, wz - pond.z
        if ddx * ddx + ddz * ddz < (meadow and 64 or 144) then
          blocked = true break
        end
      end
      if not blocked and distToRoadSq(wx, wz) <
         (big and 30 or (meadow and 6.25 or 36)) then
        blocked = true
      end
      -- Keep the roadside embankment clear of scattered plants (its own crest
      -- plants are placed by spawnHillPlants so they sit on the slope).
      if not blocked and meadow and wx > 4.2 and wx < 39 and wz > -26 and wz < 48 then
        blocked = true
      end
      if not blocked then
        local s = sceneryScale(p.kind)
        local gy = (terrainHeight and terrainHeight(wx, wz)) or 0
        local y = gy
        if p.kind == "scrub" then
          y = gy + 0.015
          SetScale(p.ent, 0.5 + math.random(), 0.03 + math.random() * 0.04, 0.5 + math.random())
        else
          SetScale(p.ent, s, s, s)
          if p.kind == "rock" then y = gy + s * 0.55 end
        end
        SetPosition(p.ent, { x = wx, y = y, z = wz })
        local yaw = math.random() * math.pi * 2
        SetRotationY(p.ent, yaw)
        p.baseYaw = yaw
        if p.phase == nil then p.phase = math.random() * math.pi * 2 end
        p.x, p.z = wx, wz
        -- 树/岩石 = 静态碰撞体（越野障碍），回收 = 传送刚体
        if p.kind == "tree" or p.kind == "rock" then
          local half = (p.kind == "rock") and s * 0.7 or 0.3
          local cy = gy + ((p.kind == "rock") and s * 0.55 or 1.0)
          if p.body then
            PhysicsSetPosition(p.body, { x = wx, y = cy, z = wz })
          else
            p.body = PhysicsAddBox({ x = wx, y = cy, z = wz },
                                   { x = half, y = cy - gy, z = half }, false, {})
          end
        end
        return
      end
    end
  end
  local wx = rv.x + fx * (fMax + 20) - fz * 30
  local wz = rv.z + fz * (fMax + 20) + fx * 30
  SetPosition(p.ent, { x = wx, y = ((terrainHeight and terrainHeight(wx, wz)) or 0) + 0.3, z = wz })
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
  for _, t in ipairs(MEADOW or {}) do
    for _ = 1, t.n do
      local ent = SpawnPrefab(t.pre, { x = 0, y = -50, z = 0 })
      if ent ~= nil then
        local p = { ent = ent, kind = t.kind, x = 0, z = 0, meadow = true }
        placeScenery(p, 0, 1, -26, 34)
        scenery[#scenery + 1] = p
      end
    end
  end
end

function recycleScenery(fx, fz)
  for _, p in ipairs(scenery) do
    if not p.fixed then
      local dx, dz = p.x - rv.x, p.z - rv.z
      local f = dx * fx + dz * fz
      local lat = dx * (-fz) + dz * fx
      if f < -40 or f > 95 or math.abs(lat) > 55 then
        if p.meadow then
          placeScenery(p, fx, fz, 15, 38)
        else
          placeScenery(p, fx, fz, 25, 75)
        end
      end
    end
  end
end

-- Plants anchored on the embankment crest (they must not be recycled away).
local hillSpawned = false

function spawnHillPlants()
  if hillSpawned then return end
  hillSpawned = true
  local kinds = {
    { "rv_grass_tuft", 34 }, { "rv_grass_tuft_b", 26 },
    { "rv_flower_white", 6 }, { "rv_flower_blue", 7 },
    { "rv_flower_yellow", 6 }, { "rv_bush", 7 },
  }
  for _, k in ipairs(kinds) do
    for _ = 1, k[2] do
      local x = 16.0 + math.random() * 7.0
      local z = -18.0 + math.random() * 58.0
      local y = terrainHeight and terrainHeight(x, z) or 0
      local ent = SpawnPrefab(k[1], { x = x, y = y, z = z })
      if ent ~= nil then
        local s = 0.9 + math.random() * 0.7
        SetScale(ent, s, s, s)
        local yaw = math.random() * math.pi * 2
        SetRotationY(ent, yaw)
        scenery[#scenery + 1] = {
          ent = ent, kind = (k[1] == "rv_bush") and "bush" or "grass",
          x = x, z = z, meadow = true, fixed = true,
          baseYaw = yaw, phase = math.random() * math.pi * 2,
        }
      end
    end
  end
  print(string.format("[meadow] hill plants spawned (%d)", #scenery))
end

-- Vegetation wind is now per-vertex: the prefabs carry `windSway` and the
-- engine's lit vertex shader bends each blade around its base (height-scaled
-- amplitude, per-vertex phase). The old 20 Hz whole-entity yaw is gone; this
-- keeps the clock advancing for anything that still reads windT.
local windT = 0.0
function updateMeadowWind(dt)
  windT = windT + dt
end

-- ===========================================================================
-- 世界固定公路网：不再跟随/随车头旋转
-- ===========================================================================
local roadLinks = {}
local roadLinkSeq = 0

function spawnRoadBetween(ax, az, bx, bz)
  local dx, dz = bx - ax, bz - az
  local len = math.sqrt(dx * dx + dz * dz)
  if len < 1.0 then return end
  local key = string.format("%.1f,%.1f|%.1f,%.1f",
                            math.min(ax, bx), math.min(az, bz),
                            math.max(ax, bx), math.max(az, bz))
  if roadLinks[key] then return end
  roadLinks[key] = true
  roadPaths[#roadPaths + 1] = { ax = ax, az = az, bx = bx, bz = bz }
  roadLinkSeq = roadLinkSeq + 1
  local yaw = dirYaw(dx, dz)
  local ux, uz = dx / len, dz / len
  -- Roads follow the heightfield: lift each 10 m slab to the sampled height and
  -- tilt it along the local slope, so a road crossing a knoll rolls with it
  -- instead of tunnelling through the terrain.
  local function placeAt(ent, d, lift)
    local cx, cz = ax + ux * d, az + uz * d
    local gy = (terrainHeight and terrainHeight(cx, cz)) or 0
    local pitch, roll = 0, 0
    if terrainSlope ~= nil then
      local dhx, dhz = terrainSlope(cx, cz)
      local dhF = dhx * ux + dhz * uz
      local dhR = dhx * uz - dhz * ux
      -- Cap the grade: a 10 m asphalt slab rotated 40 deg reads as a ramp
      -- flying through the scene. Short segments + a 10 deg cap keep the road
      -- cut into the hillside like a graded track.
      pitch = math.max(-0.18, math.min(0.18, -math.atan(dhF)))
      roll = math.max(-0.18, math.min(0.18, math.atan(dhR)))
    end
    SetPosition(ent, { x = cx, y = gy + lift, z = cz })
    if SetRotationEuler ~= nil then
      SetRotationEuler(ent, pitch, roll, yaw)
    else
      SetRotationY(ent, yaw)
    end
  end
  local SEG = 5.0
  local count = math.max(1, math.ceil(len / SEG))
  for i = 0, count - 1 do
    local d = math.min(len, i * SEG + SEG * 0.5)
    local ent = SpawnPrefab("rv_road_seg", { x = 0, y = -50, z = 0 })
    if ent ~= nil then
      SetScale(ent, 7.5, 0.02, SEG + 0.3)
      placeAt(ent, d, 0.08 + (roadLinkSeq % 6) * 0.0015)
      roadSegments[#roadSegments + 1] = { ent = ent }
    end
  end
  for d = 6, len - 2, 6 do
    local ent = SpawnPrefab("rv_road_dash", { x = 0, y = -50, z = 0 })
    if ent ~= nil then
      placeAt(ent, d, 0.12 + (roadLinkSeq % 6) * 0.0015)
      roadDashes[#roadDashes + 1] = { ent = ent }
    end
  end
end

-- 公路电线杆 + 电线（纯装饰，强化公路感）
function spawnPolesAlong(ax, az, bx, bz, side)
  local dx, dz = bx - ax, bz - az
  local len = math.sqrt(dx * dx + dz * dz)
  if len < 20 then return end
  local ux, uz = dx / len, dz / len
  local rx, rz = uz * (side or 1), -ux * (side or 1)
  local yaw = dirYaw(dx, dz)
  local lastX, lastZ
  for d = 12, len - 8, 28 do
    local x = ax + ux * d + rx * 6.2
    local z = az + uz * d + rz * 6.2
    local pole = SpawnPrefab("rv_powerpole", { x = x, y = 3.2, z = z })
    if pole ~= nil then SetScale(pole, 0.16, 6.4, 0.16) end
    local arm = SpawnPrefab("rv_panel_dark", { x = x, y = 5.9, z = z })
    if arm ~= nil then
      SetScale(arm, 1.5, 0.10, 0.10)
      SetRotationY(arm, yaw)
    end
    -- 相邻两根杆之间拉两条电线（沿横担两端，略低于杆顶）
    if lastX ~= nil then
      local wdx, wdz = x - lastX, z - lastZ
      local wlen = math.sqrt(wdx * wdx + wdz * wdz)
      local wmx, wmz = (x + lastX) * 0.5, (z + lastZ) * 0.5
      for _, off in ipairs({ -0.55, 0.55 }) do
        local wire = SpawnPrefab("rv_wire", {
          x = wmx + rx * off, y = 5.86, z = wmz + rz * off })
        if wire ~= nil then
          SetScale(wire, 0.035, 0.035, wlen)
          SetRotationY(wire, yaw)
          roadWires[#roadWires + 1] = wire
        end
      end
    end
    lastX, lastZ = x, z
  end
end

function spawnRoadsidePoles()
  spawnPolesAlong(0, -220, 0, 0, 1)
  spawnPolesAlong(0, 60, 0, 220, -1)
end

-- 主路两侧的废弃车/石块：交错分布，营造“有人来过又废弃”的公路感。
function spawnRoadDebris()
  roadDebris = {}
  local function clearOf(x, z)
    if x * x + z * z < 24 * 24 then return false end
    for _, nd in ipairs(NODES) do
      local dx, dz = nd.x - x, nd.z - z
      if dx * dx + dz * dz < 10 * 10 then return false end
      local cx, cz = poiCenter(nd)
      local cdx, cdz = cx - x, cz - z
      if cdx * cdx + cdz * cdz < 10 * 10 then return false end
    end
    return true
  end
  local n = 0
  for z = -196, 196, 46 do
    n = n + 1
    local side = (n % 2 == 0) and 1 or -1
    local x = side * (8.5 + math.random() * 2.5)
    if clearOf(x, z) then
      local pre = (n % 3 == 0) and "rv_rock" or "rv_wreck"
      local e = SpawnPrefab(pre, { x = x, y = (terrainHeight and terrainHeight(x, z)) or 0,
                                   z = z })
      if e ~= nil then
        local s = 0.75 + math.random() * 0.5
        SetScale(e, s, s, s)
        SetRotationY(e, math.random() * math.pi * 2)
        roadDebris[#roadDebris + 1] = { ent = e, x = x, z = z }
        -- 路肩外的废弃车/石块是实体的：给一个静态碰撞盒（不挡主路）
        if PhysicsAddBox ~= nil then
          PhysicsAddBox({ x = x, y = ((terrainHeight and terrainHeight(x, z)) or 0) + 0.55 * s,
                          z = z },
                        { x = 0.62 * s, y = 0.55 * s, z = 0.62 * s }, false, {})
          roadDebrisColliders = (roadDebrisColliders or 0) + 1
        end
      end
    end
  end
end

-- 主路两侧的窄土肩：沥青边缘不再直接切到草地，远处看有碎石/泥过渡。
function spawnRoadShoulders(ax, az, bx, bz)
  local dx, dz = bx - ax, bz - az
  local len = math.sqrt(dx * dx + dz * dz)
  if len < 20 then return end
  local ux, uz = dx / len, dz / len
  local rx, rz = uz, -ux
  local yaw = dirYaw(dx, dz)
  for d = 5, len - 5, 10 do
    for _, side in ipairs({ -1, 1 }) do
      local x = ax + ux * d + rx * 4.35 * side
      local z = az + uz * d + rz * 4.35 * side
      -- 营地与地点建筑附近不铺，避免土带穿过地板/门廊
      local clear = (x * x + z * z) >= 12 * 12
      if clear then
        for _, nd in ipairs(NODES) do
          local ndx, ndz = nd.x - x, nd.z - z
          if ndx * ndx + ndz * ndz < 6.0 * 6.0 then clear = false break end
          local cx, cz = poiCenter(nd)
          local cdx, cdz = cx - x, cz - z
          if cdx * cdx + cdz * cdz < 7.5 * 7.5 then clear = false break end
        end
      end
      if clear then
        local gy = (terrainHeight and terrainHeight(x, z)) or 0
        local e = SpawnPrefab("rv_road_shoulder", { x = x, y = gy + 0.055, z = z })
        if e ~= nil then
          SetScale(e, 1.40, 0.020, 10.30)
          SetRotationY(e, yaw)
          roadShoulders[#roadShoulders + 1] = e
        end
      end
    end
  end
end

-- 公路轮廓桩：沿路肩每 18m 一根奶油色反光桩，白天给道路节奏、
-- 夜里靠自发光勾出弯道，纯装饰不参与碰撞。
function spawnRoadMarkers()
  roadMarkers = {}
  for _, p in ipairs(roadPaths) do
    local dx, dz = p.bx - p.ax, p.bz - p.az
    local len = math.sqrt(dx * dx + dz * dz)
    if len >= 24 then
      local ux, uz = dx / len, dz / len
      local rx, rz = uz, -ux
      local yaw = dirYaw(dx, dz)
      local startD = 10
      for d = startD, len - 6, 18 do
        for _, side in ipairs({ -1, 1 }) do
          local x = p.ax + ux * d + rx * 4.55 * side
          local z = p.az + uz * d + rz * 4.55 * side
          -- 已有电线杆/地点建筑的位置不再插桩，避免穿模
          local clear = true
          if x * x + z * z < 12 * 12 then clear = false end
          for _, nd in ipairs(NODES) do
            local ndx, ndz = nd.x - x, nd.z - z
            if ndx * ndx + ndz * ndz < 6.5 * 6.5 then clear = false break end
            local cx, cz = poiCenter(nd)
            local cdx, cdz = cx - x, cz - z
            if cdx * cdx + cdz * cdz < 7.5 * 7.5 then clear = false break end
          end
          if clear then
            local gy = (terrainHeight and terrainHeight(x, z)) or 0
            local e = SpawnPrefab("rv_delineator", { x = x, y = gy + 0.42, z = z })
            if e ~= nil then
              SetScale(e, 0.14, 0.84, 0.14)
              SetRotationY(e, yaw)
              roadMarkers[#roadMarkers + 1] = { ent = e, x = x, z = z }
            end
          end
        end
      end
    end
  end
end

function preGeneratePois(count)
  for _ = 1, (count or 6) do
    local before = #NODES
    trySpawnPoi(true)
    if #NODES == before then return end
  end
end

function spawnRoadNetwork()
  roadSegments, roadDashes = {}, {}
  -- 营地南侧主路（出生点在营地里，需要身后也有路）
  spawnRoadBetween(0, -220, 0, 0)
  spawnRoadShoulders(0, -220, 0, 0)
  for _, nd in ipairs(NODES) do
    for _, lid in ipairs(nd.links or {}) do
      local o = nodeById(lid)
      if o then spawnRoadBetween(nd.x, nd.z, o.x, o.z) end
    end
  end
  -- 加油站向北延伸的主干道
  spawnRoadBetween(0, 60, 0, 220)
  spawnRoadShoulders(0, 60, 0, 220)
end

-- 水塘（钓鱼点）+ 木栈桥
function spawnDock(x, z)
  local dock = { x = x, z = z, parts = {} }
  local gy = 0.10
  for i = 0, 4 do
    local pz = z + 6.4 - i * 0.92
    local plank = SpawnPrefab("rv_camp_wood",
                             { x = x, y = gy, z = pz })
    if plank ~= nil then
      SetScale(plank, 1.70, 0.08, 0.92)
      dock.parts[#dock.parts + 1] = plank
    end
  end
  for _, off in ipairs({ { -0.85, 6.3 }, { 0.85, 6.3 },
                         { -0.85, 2.7 }, { 0.85, 2.7 } }) do
    local post = SpawnPrefab("rv_camp_wood",
                            { x = x + off[1], y = 0.55, z = z + off[2] })
    if post ~= nil then
      SetScale(post, 0.14, 1.30, 0.14)
      dock.parts[#dock.parts + 1] = post
    end
  end
  -- 桥头小凳
  local stool = SpawnPrefab("rv_camp_wood", { x = x + 1.2, y = 0.32, z = z + 6.2 })
  if stool ~= nil then
    SetScale(stool, 0.42, 0.10, 0.42)
    dock.parts[#dock.parts + 1] = stool
  end
  docks[#docks + 1] = dock
  return dock
end

-- 判断某个点是否站在栈桥桥板上（供钓鱼加成使用）
function nearDock(x, z)
  for _, d in ipairs(docks) do
    if math.abs(x - d.x) <= 1.0 and z >= d.z + 2.2 and z <= d.z + 7.0 then
      return true
    end
  end
  return false
end

function spawnPond(x, z)
  local ent = SpawnPrefab("rv_pond", { x = x, y = 0.02, z = z })
  if ent ~= nil then
    ponds[#ponds + 1] = { ent = ent, x = x, z = z }
  end
  if spawnDock ~= nil then spawnDock(x, z) end
end

local pondsSeeded = false

function spawnPonds()
  if pondsSeeded then return end
  pondsSeeded = true
  -- 放在营地左后方：避开开局去废宅/加油站的直线路径，避免房车开进水里
  spawnPond(-18, -12)
  local lake = nodeById("lakecamp")
  if lake ~= nil then spawnPond(lake.x - 8, lake.z - 7) end
end

-- ---------------------------------------------------------------------------
-- Breakable display windows (transparent pane + shatter burst)
-- ---------------------------------------------------------------------------
poiGlass = {}
poiShards = {}

-- Per-kind pane layout in building-local space:
--   x, y, z (offset from the POI centre), width, height, yaw.
-- Every front window faces +Z; the table makes it easy to add side panes later.
local GLASS_LAYOUTS = {
  store = { { -1.95, 1.45, 2.52, 1.70, 1.10, 0 },
            { 1.95, 1.45, 2.52, 1.70, 1.10, 0 } },
  gas = { { -1.95, 1.45, 2.52, 1.70, 1.10, 0 },
          { 1.95, 1.45, 2.52, 1.70, 1.10, 0 } },
  clinic = { { -1.95, 1.45, 2.52, 1.70, 1.10, 0 },
             { 1.95, 1.45, 2.52, 1.70, 1.10, 0 } },
  mall = { { -1.95, 1.90, 2.52, 1.70, 2.00, 0 },
           { 1.95, 1.90, 2.52, 1.70, 2.00, 0 } },
  cabin = { { -1.95, 1.50, 2.52, 1.70, 1.00, 0 },
            { 1.95, 1.50, 2.52, 1.70, 1.00, 0 } },
  depot = { { -1.95, 1.90, 2.52, 1.70, 1.40, 0 },
            { 1.95, 1.90, 2.52, 1.70, 1.40, 0 } },
}

local function glassKind(nd)
  local nm = nd.name or ""
  if nd.id == "ruin" or nm:find("废宅") then return "store" end
  if nd.id == "gasstop" or nm:find("加油站") then return "gas" end
  if nd.id == "clinic" or nm:find("诊所") or nm:find("医院") then return "clinic" end
  if nd.id == "mall" or nm:find("商场") then return "mall" end
  if nd.id == "lakecamp" or nm:find("营地") then return "cabin" end
  if nd.id == "depot" or nm:find("仓库") or nm:find("物流") then return "depot" end
  return nil
end

-- Spawn one transparent pane for every opening in the POI's layout.
function spawnPoiWindows(nd)
  if poiGlass[nd.id] ~= nil then return end
  local kind = glassKind(nd)
  local layout = kind and GLASS_LAYOUTS[kind]
  if layout == nil then return end
  local cx, cz = nd.x + 6.5, nd.z + 6.5
  local panes = {}
  for _, p in ipairs(layout) do
    local px, py, pz = cx + p[1], p[2], cz + p[3]
    local e = SpawnPrefab("rv_window_glass", { x = px, y = py, z = pz })
    if e ~= nil then
      SetScale(e, p[4], p[5], 1.0)
      if p[6] ~= 0 then SetRotationY(e, p[6]) end
      panes[#panes + 1] = {
        ent = e, x = px, y = py, z = pz,
        sx = p[4], sy = p[5], rot = p[6] or 0,
      }
    end
  end
  poiGlass[nd.id] = { panes = panes, broken = false, x = cx, z = cz }
  print(string.format("[glass] intact panes at %s = %d (%s)", nd.id, #panes, kind))
end

-- Impact-stress Voronoi fracture.  Each break picks a seeded impact point
-- inside the pane, scatters extra seed points around it (stress zones) plus a
-- uniform set for the rest of the glass, then builds the Voronoi cell of every
-- point by clipping the pane rectangle against the perpendicular bisectors.
-- Cells near the impact are therefore smaller (highest stress) and every shard
-- gets its own polygon -> orientation, size and launch speed.  The server seed
-- drives the LCG so every peer rebuilds the identical cell set.
local GLASS_VARIANTS = 12
local GLASS_IMPACT_BIAS = 0.34

-- Deterministic LCG (Numerical Recipes constants, modulus 2^31) so the
-- server's seed maps to the same cells on every peer. Operands stay below
-- 2^53, so the double arithmetic is exact.
local function glassRand(seed, k)
  local x = (seed + k * 1013904223 + 1664525) % 2147483648
  x = (x * 1664525 + 1013904223) % 2147483648
  return x / 2147483648
end

-- Clip a convex polygon against the half-plane n . p <= d.
local function clipGlassHalf(poly, nx, ny, d)
  local out = {}
  local n = #poly
  if n == 0 then return out end
  local px, py = poly[n][1], poly[n][2]
  local pd = nx * px + ny * py - d
  for i = 1, n do
    local cx, cy = poly[i][1], poly[i][2]
    local cd = nx * cx + ny * cy - d
    if cd <= 0 then
      if pd > 0 then
        local t = pd / (pd - cd)
        out[#out + 1] = { px + (cx - px) * t, py + (cy - py) * t }
      end
      out[#out + 1] = { cx, cy }
    elseif pd <= 0 then
      local t = pd / (pd - cd)
      out[#out + 1] = { px + (cx - px) * t, py + (cy - py) * t }
    end
    px, py, pd = cx, cy, cd
  end
  return out
end

-- Voronoi cells of the impact-biased point set (normalised pane space).
-- Exposed for the self-test: cells partition the unit pane and the area near
-- the impact is smaller than the area far from it.
function glassVoronoiCells(seed)
  local iu = (glassRand(seed, 11) - 0.5) * GLASS_IMPACT_BIAS
  local iv = (glassRand(seed, 23) - 0.5) * (GLASS_IMPACT_BIAS * 0.8)
  local pts = {}
  local nNear = 7 + math.floor(glassRand(seed, 5) * 3)   -- 7..9 stress seeds
  for k = 1, nNear do
    local a = glassRand(seed, 40 + k) * math.pi * 2
    local r = 0.05 + glassRand(seed, 80 + k) * 0.34
    pts[#pts + 1] = { iu + math.cos(a) * r, iv + math.sin(a) * r }
  end
  local nFar = 8 + math.floor(glassRand(seed, 7) * 4)    -- 8..11 pane seeds
  for k = 1, nFar do
    pts[#pts + 1] = { -0.47 + glassRand(seed, 200 + k) * 0.94,
                      -0.47 + glassRand(seed, 300 + k) * 0.94 }
  end

  local cells = {}
  local edges = 0
  for i = 1, #pts do
    local poly = { { -0.5, -0.5 }, { 0.5, -0.5 }, { 0.5, 0.5 }, { -0.5, 0.5 } }
    local pi = pts[i]
    for j = 1, #pts do
      if i ~= j then
        local pj = pts[j]
        -- Points closer to i than to j: 2p.(pj-pi) <= |pj|^2 - |pi|^2.
        local nx, ny = pj[1] - pi[1], pj[2] - pi[2]
        local d = (pj[1] * pj[1] + pj[2] * pj[2] -
                   pi[1] * pi[1] - pi[2] * pi[2]) * 0.5
        poly = clipGlassHalf(poly, nx, ny, d)
        edges = edges + 1
        if #poly < 3 then break end
      end
    end
    if #poly >= 3 then
      local area, cu, cv = 0.0, 0.0, 0.0
      local minU, maxU, minV, maxV = 1e9, -1e9, 1e9, -1e9
      local bestLen, angle = 0.0, 0.0
      for k = 1, #poly do
        local ax, ay = poly[k][1], poly[k][2]
        local bx, by = poly[k % #poly + 1][1], poly[k % #poly + 1][2]
        local cross = ax * by - bx * ay
        area = area + cross
        cu = cu + (ax + bx) * cross
        cv = cv + (ay + by) * cross
        local dx, dy = bx - ax, by - ay
        local len = dx * dx + dy * dy
        if len > bestLen then
          bestLen = len
          angle = math.atan(dx, dy)   -- yaw of the longest edge
        end
        minU = math.min(minU, ax); maxU = math.max(maxU, ax)
        minV = math.min(minV, ay); maxV = math.max(maxV, ay)
      end
      area = area * 0.5
      if area > 0.0015 then
        cu = cu / (6 * area)
        cv = cv / (6 * area)
        local du, dv = cu - iu, cv - iv
        local dist = math.sqrt(du * du + dv * dv)
        cells[#cells + 1] = {
          cu = cu, cv = cv, area = area,
          w = maxU - minU, h = maxV - minV, angle = angle,
          stress = 1 / (1 + dist * 3.2),
        }
      end
    end
  end
  return cells, iu, iv, edges
end

-- Cell statistics helpers for the self-test / logs.
function glassVoronoiStats(cells, iu, iv)
  local total, near, far, nearN, farN = 0.0, 0.0, 0.0, 0, 0
  for _, c in ipairs(cells or {}) do
    total = total + c.area
    local du, dv = c.cu - (iu or 0), c.cv - (iv or 0)
    if math.sqrt(du * du + dv * dv) < 0.25 then
      near = near + c.area; nearN = nearN + 1
    else
      far = far + c.area; farN = farN + 1
    end
  end
  local nearAvg = nearN > 0 and (near / nearN) or 0
  local farAvg = farN > 0 and (far / farN) or 0
  return #(cells or {}), total, nearAvg, farAvg, nearN, farN
end

-- "id:seed:count" token used by the co-op glass list.
function glassToken(id, seed, count)
  return string.format("%s:%d:%d", tostring(id), tonumber(seed) or 0,
                       tonumber(count) or 0)
end

function glassParseToken(tok)
  if tok == nil then return nil, 0, 0 end
  local id, seed, count = string.match(tostring(tok), "^(.-):(%d+):(%d+)$")
  if id ~= nil then return id, tonumber(seed), tonumber(count) end
  id, seed = string.match(tostring(tok), "^(.-):(%d+)$")
  if id ~= nil then return id, tonumber(seed), 0 end
  return tostring(tok), 0, 0
end

-- Fresh seed per break. The engine RNG is fixed-seed per process, so mix in
-- the simulated clock to vary the first break between sessions.
local function nextGlassSeed()
  local t = 0
  if NMath ~= nil and NMath.Time ~= nil then
    local ok, v = pcall(NMath.Time)
    if ok and type(v) == "number" then t = math.floor(v * 1000) end
  end
  return ((math.random(1, 1000000) + t * 7919) % 1000000) + 1
end

-- One-shot break: remove the intact pane, then build the crack shard set.
function breakPoiWindows(nd, reason, seed)
  if nd == nil then return false end
  local g = poiGlass[nd.id]
  if g == nil or g.broken then return false end
  g.broken = true
  seed = math.floor(tonumber(seed) or nextGlassSeed())
  g.seed = seed
  local panes = g.panes
  g.panes = {}
  local made = 0
  local totalEdges = 0
  for pi, pane in ipairs(panes) do
    Despawn(pane.ent)
    -- Pane normal (local +Z rotated by yaw) and its in-plane tangent.
    local nx, nz = math.sin(pane.rot), math.cos(pane.rot)
    local tx, tz = math.cos(pane.rot), -math.sin(pane.rot)
    local ps = seed + pi * 977
    local cells, iu, iv, edges = glassVoronoiCells(ps)
    totalEdges = totalEdges + edges
    for ci, cell in ipairs(cells) do
      local stress = cell.stress or 1 / (1 +
        math.sqrt((cell.cu - iu) ^ 2 + (cell.cv - iv) ^ 2) * 3.2)
      local variant = 1 + math.floor(glassRand(ps, 300 + ci) * GLASS_VARIANTS)
      if variant > GLASS_VARIANTS then variant = GLASS_VARIANTS end
      local ox, oy = cell.cu * pane.sx, cell.cv * pane.sy
      local sx = pane.x + tx * ox + nx * 0.05
      local sy = pane.y + oy
      local sz = pane.z + tz * ox + nz * 0.05
      local e = SpawnPrefab("rv_glass_frag" .. variant, { x = sx, y = sy, z = sz })
      if e ~= nil then
        SetScale(e, cell.w * pane.sx, 1.0, cell.h * pane.sy)
        -- Align the shard with the cell's longest edge (Voronoi boundary).
        local yaw = pane.rot + cell.angle +
                    (glassRand(ps, 400 + ci) - 0.5) * 0.45
        SetRotationY(e, yaw)
        -- Stress falloff: pieces near the impact leave faster and spin more.
        local outward = (0.45 + 2.6 * stress) *
                        (0.8 + glassRand(ps, 500 + ci) * 0.6)
        poiShards[#poiShards + 1] = {
          ent = e, x = sx, y = sy, z = sz,
          vx = nx * outward + tx * (cell.cu * 2.6),
          vy = (0.7 + 2.2 * stress) + math.max(0, cell.cv) * pane.sy * 2.0,
          vz = nz * outward + tz * (cell.cu * 2.6),
          yaw = yaw,
          spin = (glassRand(ps, 700 + ci) - 0.5) * (1.4 + stress * 3.2),
          settled = false,
        }
        made = made + 1
      end
    end
  end
  g.count = made
  print(string.format(
    "[glass] broke windows at %s (%s, %d voronoi shards, %d edges, seed=%d)",
    nd.id, reason or "enter", made, totalEdges, seed))
  return true
end

-- Break every intact POI window whose footprint contains (x, z).
function breakPoiWindowsAt(x, z, reason)
  local n = 0
  for _, nd in ipairs(NODES) do
    local g = poiGlass[nd.id]
    if g ~= nil and not g.broken then
      if math.abs(x - g.x) < POI_HALF_X and math.abs(z - g.z) < POI_HALF_Z then
        if breakPoiWindows(nd, reason) then n = n + 1 end
      end
    end
  end
  return n
end

-- Gravitate the fragments, bounce once or twice, then let them rest on the
-- ground as persistent debris (no despawn; the break is one-shot per pane).
function updatePoiWindows(dt)
  local floor = 0.10
  for i = 1, #poiShards do
    local s = poiShards[i]
    if not s.settled then
      s.vy = s.vy - 9.8 * dt
      s.x = s.x + s.vx * dt
      s.y = s.y + s.vy * dt
      s.z = s.z + s.vz * dt
      s.yaw = (s.yaw or 0) + (s.spin or 0) * dt
      if s.y <= floor then
        s.y = floor
        if s.vy < -0.6 then
          s.vy = -s.vy * 0.28
          s.vx = s.vx * 0.5
          s.vz = s.vz * 0.5
          s.spin = (s.spin or 0) * 0.4
        else
          s.vy = 0
          s.vx = 0
          s.vz = 0
          s.spin = 0
          s.settled = true
        end
      end
      SetPosition(s.ent, { x = s.x, y = s.y, z = s.z })
      SetRotationY(s.ent, s.yaw or 0)
    end
  end
end

-- Broken-window "id:seed" pairs for the authoritative co-op state field
-- ("-" = none). The seed lets every client rebuild the same crack layout.
function poiGlassBrokenList()
  local ids = {}
  for _, nd in ipairs(NODES) do
    local g = poiGlass[nd.id]
    if g ~= nil and g.broken then
      ids[#ids + 1] = glassToken(nd.id, g.seed or 0, g.count or 0)
    end
  end
  return table.concat(ids, ",")
end

-- Apply a comma-separated "id:seed" list broadcast by the server.
function applyPoiGlassState(csv)
  if csv == nil or csv == "" then return 0 end
  local n = 0
  for item in string.gmatch(tostring(csv), "[^,]+") do
    local id, seed, wantCount = glassParseToken(item)
    local nd = nodeById(id)
    if nd ~= nil and breakPoiWindows(nd, "server", seed) then
      n = n + 1
      local g = poiGlass[nd.id]
      local got = (g and g.count) or 0
      if wantCount and wantCount > 0 and got ~= wantCount then
        print(string.format("[glass] rebuild MISMATCH %s seed=%d count %d~=%d",
                            id, seed, got, wantCount))
      else
        print(string.format("[glass] rebuild ok %s seed=%d shards=%d",
                            id, seed, got))
      end
    end
  end
  return n
end

-- ---------------------------------------------------------------------------
-- Animated warehouse roll-up door (server-authoritative in co-op)
-- ---------------------------------------------------------------------------
poiDoors = {}

function spawnPoiDoor(nd)
  if poiDoors[nd.id] ~= nil then return end
  local cx, cz = nd.x + 6.5, nd.z + 6.5
  local ent = SpawnPrefab("rv_rollup_door", { x = cx, y = 1.3, z = cz + 2.52 })
  if ent == nil then return end
  poiDoors[nd.id] = {
    ent = ent, x = cx, z = cz + 2.52,
    height = 2.6, topY = 2.62,
    cur = 0.0, target = 0.0,
  }
  print(string.format("[depot-door] spawned at %s (closed)", nd.id))
end

function setPoiDoorOpen(ndId, open)
  local d = poiDoors[ndId]
  if d == nil then return end
  local t = open and 1.0 or 0.0
  if t ~= d.target then
    d.target = t
    print(string.format("[depot-door] %s target=%s (client)", ndId, tostring(open)))
  end
end

-- Roll-up: shrink scaleY around the fixed top edge.
function updatePoiDoors(dt)
  for id, d in pairs(poiDoors) do
    if d.cur ~= d.target then
      local step = dt * 2.2
      if d.cur < d.target then
        d.cur = math.min(d.target, d.cur + step)
      else
        d.cur = math.max(d.target, d.cur - step)
      end
      d.cur = math.max(0, math.min(1, d.cur))
      local s = 1.0 - 0.94 * d.cur
      SetScale(d.ent, 1.0, s, 1.0)
      SetPosition(d.ent, { x = d.x, y = d.topY - d.height * s * 0.5, z = d.z })
    end
    if d.cur == d.target and d.logged ~= d.target then
      d.logged = d.target
      print(string.format("[depot-door] %s now %s (client, cur=%.2f)",
            id, d.cur > 0.5 and "open" or "closed", d.cur))
    end
  end
end

-- Single-player proximity trigger, mirroring the co-op server logic.
function updatePoiDoorProximity(x, z)
  for id, _ in pairs(poiDoors) do
    local nd = nodeById(id)
    if nd ~= nil then
      local cx, cz = poiCenter(nd)
      local near = math.abs(x - cx) < 3.6 and
                   (z - cz) > 0.5 and (z - cz) < 4.5
      setPoiDoorOpen(id, near)
    end
  end
end

-- ---------------------------------------------------------------------------
-- Animated barn sliding door (server-authoritative in co-op)
-- ---------------------------------------------------------------------------
poiBarnDoors = {}

-- Panel local centre at y = 1.23 (2.42 m tall), parked 2.05 m to the +X side.
function spawnPoiSlidingDoor(nd)
  if poiBarnDoors[nd.id] ~= nil then return end
  local cx, cz = nd.x + 6.5, nd.z + 6.5
  local ent = SpawnPrefab("rv_sliding_door",
                          { x = cx, y = 1.23, z = cz + 2.70 })
  if ent == nil then return end
  poiBarnDoors[nd.id] = {
    ent = ent, x = cx, y = 1.23, z = cz + 2.70,
    slide = 2.05, cur = 0.0, target = 0.0,
  }
  print(string.format("[barn-door] spawned at %s (closed)", nd.id))
end

function setPoiSlidingDoorOpen(ndId, open)
  local d = poiBarnDoors[ndId]
  if d == nil then return end
  local t = open and 1.0 or 0.0
  if t ~= d.target then
    d.target = t
    print(string.format("[barn-door] %s target=%s (client)", ndId, tostring(open)))
  end
end

-- Slide the panel sideways; the whole 1.86 m panel parks over the front wall.
function updatePoiSlidingDoors(dt)
  for id, d in pairs(poiBarnDoors) do
    if d.cur ~= d.target then
      local step = dt * 1.6
      if d.cur < d.target then
        d.cur = math.min(d.target, d.cur + step)
      else
        d.cur = math.max(d.target, d.cur - step)
      end
      d.cur = math.max(0, math.min(1, d.cur))
      SetPosition(d.ent, { x = d.x + d.slide * d.cur, y = d.y, z = d.z })
    end
    if d.cur == d.target and d.logged ~= d.target then
      d.logged = d.target
      print(string.format("[barn-door] %s now %s (client, cur=%.2f)",
            id, d.cur > 0.5 and "open" or "closed", d.cur))
    end
  end
end

-- Single-player proximity trigger, mirroring the co-op server logic.
function updatePoiBarnDoorProximity(x, z)
  for id, _ in pairs(poiBarnDoors) do
    local nd = nodeById(id)
    if nd ~= nil then
      local cx, cz = poiCenter(nd)
      local near = math.abs(x - cx) < 3.6 and
                   (z - cz) > 0.5 and (z - cz) < 4.5
      setPoiSlidingDoorOpen(id, near)
    end
  end
end

-- Doorway collision: a roll-up/sliding panel is solid until it is almost fully
-- open. Called from collidePoiWalls on both the authoritative server and the
-- client's local prediction so a closed door blocks in both places.
local DOOR_PASS_CUR = 0.9

function poiDoorBlocking(ndId)
  if poiDoors ~= nil then
    local d = poiDoors[ndId]
    if d ~= nil and d.cur < DOOR_PASS_CUR then return true end
  end
  if poiBarnDoors ~= nil then
    local b = poiBarnDoors[ndId]
    if b ~= nil and b.cur < DOOR_PASS_CUR then return true end
  end
  return false
end

-- 简易路边建筑：一个带门洞/窗的废弃小屋（纯装饰，原型级）
-- 小镇水塔：4 条支架 + 圆柱水箱 + 顶盖，夜里可作地标
function spawnWaterTower(x, z)
  local gy = (terrainHeight and terrainHeight(x, z)) or 0
  local tower = { x = x, z = z, parts = {} }
  for _, off in ipairs({ { -1.15, -1.15 }, { 1.15, -1.15 },
                         { -1.15, 1.15 }, { 1.15, 1.15 } }) do
    local e = SpawnPrefab("rv_powerpole", { x = x + off[1], y = gy + 2.1,
                                            z = z + off[2] })
    if e ~= nil then
      SetScale(e, 0.16, 4.2, 0.16)
      tower.parts[#tower.parts + 1] = e
    end
  end
  local tank = SpawnPrefab("rv_furn_water_tank", { x = x, y = gy + 5.2, z = z })
  if tank ~= nil then
    SetScale(tank, 3.0, 2.2, 3.0)
    tower.parts[#tower.parts + 1] = tank
  end
  local cap = SpawnPrefab("rv_panel_dark", { x = x, y = gy + 6.45, z = z })
  if cap ~= nil then
    SetScale(cap, 3.3, 0.16, 3.3)
    tower.parts[#tower.parts + 1] = cap
  end
  local ladder = SpawnPrefab("rv_camp_wood", { x = x + 1.15, y = gy + 2.6,
                                               z = z })
  if ladder ~= nil then
    SetScale(ladder, 0.10, 5.0, 0.10)
    tower.parts[#tower.parts + 1] = ladder
  end
  waterTowers[#waterTowers + 1] = tower
  return tower
end

-- 仓库旁的无线电塔：三角支架 + 横撑 + 顶端红色信标（夜间闪烁）
function spawnRadioMast(x, z)
  local gy = (terrainHeight and terrainHeight(x, z)) or 0
  local mast = { x = x, z = z, parts = {}, beacon = nil, t = 0, lit = false }
  local legs = { { -0.9, -0.5 }, { 0.9, -0.5 }, { 0.0, 0.95 } }
  for _, off in ipairs(legs) do
    local e = SpawnPrefab("rv_powerpole", { x = x + off[1], y = gy + 7.0,
                                            z = z + off[2] })
    if e ~= nil then
      SetScale(e, 0.14, 14.0, 0.14)
      mast.parts[#mast.parts + 1] = e
    end
  end
  for _, h in ipairs({ 4.0, 8.0, 12.0 }) do
    local bar = SpawnPrefab("rv_panel_dark", { x = x, y = gy + h, z = z })
    if bar ~= nil then
      SetScale(bar, 1.9, 0.08, 0.08)
      SetRotationY(bar, h * 0.7)
      mast.parts[#mast.parts + 1] = bar
    end
  end
  local beacon = SpawnPrefab("rv_taillight", { x = x, y = gy + 14.2, z = z })
  if beacon ~= nil then
    SetScale(beacon, 0.34, 0.34, 0.34)
    SetVisible(beacon, false)
    mast.beacon = beacon
    mast.parts[#mast.parts + 1] = beacon
  end
  radioMast = mast
  return mast
end

function updateRadioMast(dt)
  if radioMast == nil or radioMast.beacon == nil then return end
  radioMast.t = (radioMast.t or 0) + (dt or 0)
  local lit
  if radioMastFixed then
    lit = true  -- 修好后红灯常亮，远距离也能定位仓库
  else
    local night = (isNight ~= nil) and isNight() or false
    lit = night and ((radioMast.t % 1.6) < 0.55)
  end
  if lit ~= radioMast.lit then
    radioMast.lit = lit
    SetVisible(radioMast.beacon, lit)
  end
end

-- 长线目标：用金属/电子件修好无线电塔
RADIO_MAST_COST = { metal = 6, electronics = 3 }

function repairRadioMast()
  if radioMast == nil then
    toast.text = "附近没有可修的无线电塔"
    toast.t = 2.0
    return false
  end
  if radioMastFixed then
    toast.text = "无线电塔已经修好了"
    toast.t = 2.0
    return false
  end
  if not canAfford(RADIO_MAST_COST) then
    toast.text = "修塔材料不足：" .. costText(RADIO_MAST_COST)
    toast.t = 2.8
    return false
  end
  payCost(RADIO_MAST_COST)
  radioMastFixed = true
  if radioMast.beacon ~= nil then
    SetVisible(radioMast.beacon, true)
    radioMast.lit = true
  end
  -- 修好后的实际回报：信标常亮 + 一份应急补给
  if addItem ~= nil then
    addItem("fuelcan", 1)
    addItem("repairkit", 1)
  end
  if PlaySfx then PlaySfx("repair") end
  if addJournal ~= nil then
    addJournal("修好了仓库无线电塔（补给：燃油罐+修理包）", "goal")
  end
  toast.text = "无线电塔修好了——信标常亮，收到一份应急补给（燃油罐+修理包）"
  toast.t = 5.0
  return true
end

function spawnRuinFor(nd)
  if nd.ruinSpawned then return end
  nd.ruinSpawned = true
  local cx, cz = nd.x + 6.5, nd.z + 6.5
  -- 门头招牌：夜里发光，白天熄灭，远处也能认出建筑
  local sign = SpawnPrefab("rv_poi_sign", { x = cx, y = 2.45,
                                           z = cz + POI_HALF_Z + 0.38 })
  if sign ~= nil then
    SetScale(sign, 2.00, 0.30, 0.06)
    poiSigns[#poiSigns + 1] = sign
    SetVisible(sign, false)
  end
  local nm = nd.name or ""
  local isStore = (nd.id == "ruin") or (nm:find("废宅") ~= nil)
  local isGas = (nd.id == "gasstop") or (nm:find("加油站") ~= nil)
  local isWarehouse = (nd.id == "depot") or (nm:find("仓库") ~= nil) or
                      (nm:find("物流") ~= nil)
  local isFarm = (nd.id == "farm") or (nm:find("农场") ~= nil)
  local isMall = (nd.id == "mall") or (nm:find("商场") ~= nil)
  local isCabin = (nd.id == "lakecamp") or (nm:find("营地") ~= nil)
  local isTown = (nd.id == "town") or (nm:find("小镇") ~= nil)
  local isClinic = (nd.id == "clinic") or (nm:find("诊所") ~= nil) or
                   (nm:find("医院") ~= nil)
  if nd.id == "ruin" or nd.id == "gasstop" or nd.id == "depot" or
     nd.id == "farm" or nd.id == "mall" or nd.id == "lakecamp" or
     nd.id == "clinic" then
    print(string.format("[poi] spawn shell node=%s center=(%.1f,%.1f)",
          nd.id, cx, cz))
  end
  if isClinic then
    -- 可进入的路边诊所：玻璃门 + 红十字 + 接待台/候诊椅/诊床/药柜/窗帘
    local clinic = SpawnPrefab("rv_clinic", { x = cx, y = 0, z = cz })
    if clinic ~= nil then SetRotationY(clinic, 0) end
    for _, lp in ipairs({ { -1.40, -1.55 }, { 1.35, 1.00 } }) do
      local lamp = SpawnPrefab("rv_lamp", { x = cx + lp[1], y = 2.35, z = cz + lp[2] })
      if lamp ~= nil then SetScale(lamp, 1.4, 1.4, 1.4) end
    end
    -- Extra fill light directly in front of the medicine cabinet so the
    -- packaged medicine boxes stay readable when the glass front is closed.
    local med = SpawnPrefab("rv_lamp", { x = cx - 1.40, y = 1.95, z = cz - 1.05 })
    if med ~= nil then SetScale(med, 0.9, 0.9, 0.9) end
    spawnPoiWindows(nd)
    return
  end
  if isCabin then
    -- 可进入的湖边/林地木屋：门廊 + 壁炉 + 床 + 桌椅 + 厨房 + 货架
    local cabin = SpawnPrefab("rv_cabin", { x = cx, y = 0, z = cz })
    if cabin ~= nil then SetRotationY(cabin, 0) end
    for _, lp in ipairs({ { -1.20, -0.60 }, { 1.20, 0.80 } }) do
      local lamp = SpawnPrefab("rv_lamp", { x = cx + lp[1], y = 2.15, z = cz + lp[2] })
      if lamp ~= nil then SetScale(lamp, 1.3, 1.3, 1.3) end
    end
    spawnPoiWindows(nd)
    return
  end
  if isMall then
    -- 可进入购物中心：玻璃店面/红蓝招牌 + 收银台/货架/服装架/模特/外摆
    local mall = SpawnPrefab("rv_mall", { x = cx, y = 0, z = cz })
    if mall ~= nil then SetRotationY(mall, 0) end
    for _, lp in ipairs({ { -1.40, -1.20 }, { 1.40, -1.20 },
                          { -1.40, 1.20 }, { 1.40, 1.20 } }) do
      local lamp = SpawnPrefab("rv_lamp", { x = cx + lp[1], y = 2.85, z = cz + lp[2] })
      if lamp ~= nil then SetScale(lamp, 1.6, 1.6, 1.6) end
    end
    spawnPoiWindows(nd)
    return
  end
  if isFarm then
    -- 可进入旧农场：红谷仓/滑动门/干草阁楼 + 畜栏/干草/饲料槽/货架
    local barn = SpawnPrefab("rv_barn", { x = cx, y = 0, z = cz })
    if barn ~= nil then SetRotationY(barn, 0) end
    for _, lp in ipairs({ { -1.50, -1.20 }, { 1.50, -1.20 },
                          { -1.50, 1.20 }, { 1.50, 1.20 } }) do
      local lamp = SpawnPrefab("rv_lamp", { x = cx + lp[1], y = 2.60, z = cz + lp[2] })
      if lamp ~= nil then SetScale(lamp, 2.0, 2.0, 2.0) end
    end
    spawnPoiSlidingDoor(nd)
    -- 谷仓外的干草捆（避开正门与侧墙）
    for i, off in ipairs({ { -4.3, 1.4 }, { -4.3, 2.8 }, { 4.4, 1.6 },
                           { 4.2, -1.2 }, { -3.9, -2.9 } }) do
      local hx, hz = cx + off[1], cz + off[2]
      local e = SpawnPrefab("rv_hay_bale", {
        x = hx, y = ((terrainHeight and terrainHeight(hx, hz)) or 0) + 0.40,
        z = hz })
      if e ~= nil then
        SetScale(e, 1.25, 0.80, 0.85)
        SetRotationY(e, i * 0.7 + 0.2)
        hayBales[#hayBales + 1] = e
      end
    end
    -- 农场风车（低多边形地标，叶片持续旋转）
    local wx, wz = cx + 8.5, cz - 5.5
    local wy = (terrainHeight and terrainHeight(wx, wz)) or 0
    local tower = SpawnPrefab("rv_camp_wood", { x = wx, y = wy + 3.5, z = wz })
    if tower ~= nil then SetScale(tower, 0.55, 7.0, 0.55) end
    local hub = SpawnPrefab("rv_camp_wood", { x = wx, y = wy + 7.0, z = wz })
    if hub ~= nil then SetScale(hub, 0.34, 0.34, 0.34) end
    windmill = { x = wx, y = wy + 7.0, z = wz + 0.35, angle = 0, blades = {} }
    for i = 1, 4 do
      local b = SpawnPrefab("rv_panel_light", { x = wx, y = wy + 7.0, z = wz + 0.35 })
      if b ~= nil then
        SetScale(b, 0.28, 2.80, 0.08)
        windmill.blades[#windmill.blades + 1] = b
      end
    end
    return
  end
  if isWarehouse then
    -- 可进入物流仓库：高棚 + 装卸门/月台 + 内部货架/托盘 + 顶部高窗
    local wh = SpawnPrefab("rv_warehouse", { x = cx, y = 0, z = cz })
    if wh ~= nil then SetRotationY(wh, 0) end
    for _, lx in ipairs({ -1.70, 1.70 }) do
      local lamp = SpawnPrefab("rv_lamp", { x = cx + lx, y = 3.15, z = cz })
      if lamp ~= nil then SetScale(lamp, 1.5, 1.5, 1.5) end
    end
    spawnPoiWindows(nd)
    spawnPoiDoor(nd)
    if spawnRadioMast ~= nil then spawnRadioMast(cx - 9.0, cz - 7.0) end
    return
  end
  if isGas then
    -- 可进入加油站：glTF 商店（门洞/窗户/货架/柜台）+ 四柱雨棚 + 两台加油机
    local gas = SpawnPrefab("rv_gasstation", { x = cx, y = 0, z = cz })
    if gas ~= nil then SetRotationY(gas, 0) end
    for _, lx in ipairs({ -1.50, 1.40 }) do
      local lamp = SpawnPrefab("rv_lamp", { x = cx + lx, y = 2.42, z = cz })
      if lamp ~= nil then SetScale(lamp, 1.3, 1.3, 1.3) end
    end
    spawnPoiWindows(nd)
    return
  end
  if isStore then
    -- 真实可进入建筑：独立 glTF（实心墙体/门洞/屋檐/招牌/柜台/货架）
    local store = SpawnPrefab("rv_store", { x = cx, y = 0, z = cz })
    if store ~= nil then SetRotationY(store, 0) end
    for _, lx in ipairs({ -1.60, 1.40 }) do
      local lamp = SpawnPrefab("rv_lamp", { x = cx + lx, y = 2.42, z = cz })
      if lamp ~= nil then SetScale(lamp, 1.4, 1.4, 1.4) end
    end
    local box = SpawnPrefab("rv_storage", { x = cx - 1.80, y = 0.20, z = cz + 1.35 })
    if box ~= nil then
      SetScale(box, 0.7, 0.7, 0.7)
      SetRotationY(box, 0.45)
    end
    spawnPoiWindows(nd)
    return
  end

  -- 小镇水塔：作为远处可辨认的地标（放在建筑群侧面，不挡门）
  if isTown then
    spawnWaterTower(cx + 8.5, cz - 5.5)
  end

  poiShellParts = poiShellParts or {}
  local shell = {}
  poiShellParts[nd.id] = shell
  local function cube(pre, x, y, z, sx, sy, sz)
    local e = SpawnPrefab(pre, { x = x, y = y, z = z })
    if e ~= nil then
      SetScale(e, sx, sy, sz)
      shell[#shell + 1] = { ent = e, x = x, y = y, z = z, sx = sx, sy = sy, sz = sz }
    end
    return e
  end
  -- 地板 / 后墙 / 两侧墙 / 屋顶
  cube("rv_panel_dark",  cx, 0.03, cz, 6.2, 0.06, 5.2)
  cube("rv_panel_light", cx, 1.25, cz - 2.55, 6.2, 2.5, 0.16)
  cube("rv_panel_light", cx - 3.05, 1.25, cz, 0.16, 2.5, 5.2)
  cube("rv_panel_light", cx + 3.05, 1.25, cz, 0.16, 2.5, 5.2)
  cube("rv_panel_dark",  cx, 2.55, cz, 6.4, 0.16, 5.4)
  -- 内衬：立方体材质单面，站在室内看到的是外墙背面（被剔除）。
  -- 补一层朝内的墙板/顶板，让建筑真正可以走进去。
  cube("rv_panel_light", cx, 1.25, cz - 2.44, 6.0, 2.50, 0.06)
  cube("rv_panel_light", cx - 2.94, 1.25, cz, 0.06, 2.50, 5.0)
  cube("rv_panel_light", cx + 2.94, 1.25, cz, 0.06, 2.50, 5.0)
  cube("rv_panel_light", cx, 2.47, cz, 6.0, 0.06, 5.0)
  -- 前墙内衬同样留门洞
  cube("rv_panel_light", cx - 1.90, 1.25, cz + 2.44, 2.20, 2.50, 0.06)
  cube("rv_panel_light", cx + 1.90, 1.25, cz + 2.44, 2.20, 2.50, 0.06)
  -- 前墙两段（中间留门洞）
  cube("rv_panel_light", cx - 1.9, 1.25, cz + 2.55, 2.3, 2.5, 0.16)
  cube("rv_panel_light", cx + 1.9, 1.25, cz + 2.55, 2.3, 2.5, 0.16)
  -- 窗与门框
  cube("rv_panel_window", cx - 3.0, 1.55, cz + 0.3, 0.04, 0.7, 1.4)
  cube("rv_panel_window", cx + 3.0, 1.55, cz + 0.3, 0.04, 0.7, 1.4)

  -- 门框 + 半开的门板：所有建筑统一，正门是唯一墙体开口
  cube("rv_panel_dark", cx - 0.86, 1.20, cz + 2.55, 0.20, 2.40, 0.24)
  cube("rv_panel_dark", cx + 0.86, 1.20, cz + 2.55, 0.20, 2.40, 0.24)
  cube("rv_panel_dark", cx, 2.36, cz + 2.55, 1.92, 0.22, 0.24)
  local leaf = SpawnPrefab("rv_panel_dark", { x = cx - 0.86, y = 1.14, z = cz + 2.62 })
  if leaf ~= nil then
    SetScale(leaf, 0.07, 2.04, 0.92)
    SetRotationY(leaf, -1.92)
  end

  -- 其余地点保留原来的立方体拼装（商场已改为可进入 glTF）
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
function trySpawnPoi(silent)
  if #NODES >= 15 then return end
  for _ = 1, 13 do
    -- 以现有地点为锚点生成，而不是以车头方向生成：路网在开局就固定
    local anchor = NODES[math.random(1, #NODES)]
    local a = math.random() * math.pi * 2
    local dist = 70 + math.random() * 90
    local x = anchor.x + math.sin(a) * dist
    local z = anchor.z + math.cos(a) * dist
    local ok = true
    for _, nd in ipairs(NODES) do
      local dx, dz = x - nd.x, z - nd.z
      if dx * dx + dz * dz < 45 * 45 then ok = false break end
    end
    if ok and terrainHeight ~= nil then
      -- Keep new roads off the knolls: a level asphalt ribbon crossing a hill
      -- would hover/tunnel. Reject a route that meets relief, and never drop a
      -- building on a slope (the POI shell has a flat floor).
      local rx, rz = x - anchor.x, z - anchor.z
      local steps = math.max(1, math.floor(math.sqrt(rx * rx + rz * rz) / 8))
      for s = 1, steps do
        local t = s / steps
        if terrainHeight(anchor.x + rx * t, anchor.z + rz * t) > 0.25 then
          ok = false
          break
        end
      end
      local bcx, bcz = x + 6.5, z + 6.5
      if ok and (terrainHeight(x, z) > 0.4 or terrainHeight(bcx, bcz) > 0.4) then
        ok = false
      end
    end
    if ok then
      repeat poiSeq = poiSeq + 1 until nodeById("poi_" .. poiSeq) == nil
      local kind = POI_KINDS[math.random(1, #POI_KINDS)]
      local nearest = anchor
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
      spawnRoadBetween(nearest.x, nearest.z, x, z)
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
      if not silent then
        toast.text = "发现地点：" .. nd.name
        toast.t = 4.0
      end
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
  campfire = { x = 1.8, z = 2.0 }
  -- 营火暖光：隐藏灯罩网格，只留点光；每帧轻微跳动模拟火光
  local glow = SpawnPrefab("rv_lamp", { x = 1.8, y = 0.95, z = 2.0 })
  if glow ~= nil then
    SetScale(glow, 0.40, 0.20, 0.40)
    SetVisible(glow, false)
    campfireLight = glow
  end
  -- 三层火苗（橙 + 黄），由 updateCampfireLight 抖动缩放
  for i, f in ipairs({ { 1.8, 0.42, 2.0, 0.34, 0.50, 0.34 },
                       { 1.8, 0.62, 2.0, 0.24, 0.38, 0.24 },
                       { 1.8, 0.78, 2.0, 0.14, 0.24, 0.14 } }) do
    local e = SpawnPrefab("rv_flame", { x = f[1], y = f[2], z = f[3] })
    if e ~= nil then
      SetScale(e, f[4], f[5], f[6])
      campfireFlames[#campfireFlames + 1] = {
        ent = e, x = f[1], y = f[2], z = f[3], sx = f[4], sy = f[5], sz = f[6] }
    end
  end
  local gas = nodeById("gasstop")
  if gas then
    for _, off in ipairs({ {2.0, 1.0}, {2.6, -0.4}, {1.2, -0.6} }) do
      local ent = SpawnPrefab("rv_fuelcan",
                              { x = gas.x + off[1], y = 0.35, z = gas.z + off[2] })
      if ent ~= nil then SetScale(ent, 0.5, 0.7, 0.5) end
    end
  end
end

-- 门头招牌夜间点亮（白天隐藏）
function updatePoiSigns()
  local lit = (isNight ~= nil) and isNight() or false
  if poiSignsLit == lit then return end
  poiSignsLit = lit
  for _, e in ipairs(poiSigns) do SetVisible(e, lit) end
end

-- 白天鸟群：低多边形剪影绕当前区域飞行，夜间隐藏
function birdsActive()
  return not (isNight ~= nil and isNight())
end

function spawnBirds()
  birds = {}
  for i = 1, 7 do
    local e = SpawnPrefab("rv_bird", { x = 0, y = -60, z = 0 })
    if e ~= nil then
      SetScale(e, 0.18, 0.05, 0.32)
      birds[#birds + 1] = {
        ent = e,
        angle = math.random() * math.pi * 2,
        radius = 7 + math.random() * 11,
        height = 5.5 + math.random() * 5.5,
        speed = 0.18 + math.random() * 0.22,
        phase = math.random() * math.pi * 2,
        flap = 7 + math.random() * 4,
      }
    end
  end
end

function updateBirds(dt)
  if #birds == 0 then return end
  local active = birdsActive()
  birdsT = (birdsT or 0) + dt
  for _, b in ipairs(birds) do
    SetVisible(b.ent, active)
    if active then
      b.angle = b.angle + b.speed * dt
      local x = rv.x + math.cos(b.angle) * b.radius
      local z = rv.z + math.sin(b.angle) * b.radius
      local y = (rv.groundY or 0) + b.height +
                math.sin(b.angle * 3 + b.phase) * 0.5
      SetPosition(b.ent, { x = x, y = y, z = z })
      SetRotationY(b.ent, -b.angle + math.pi * 0.5)
      local flap = 0.35 + 0.65 * math.abs(math.sin(birdsT * b.flap + b.phase))
      SetScale(b.ent, 0.18, 0.02 + 0.05 * flap, 0.32)
    end
  end
end

-- 雨天积水：在房车周围随机铺几块反光水洼，雨停自动清除
function spawnPuddles()
  for _, p in ipairs(puddles) do Despawn(p.ent) end
  puddles = {}
  puddleCenter = { x = rv.x, z = rv.z }
  for i = 1, 7 do
    local a = math.random() * math.pi * 2
    local r = 4 + math.random() * 15
    local x = rv.x + math.sin(a) * r
    local z = rv.z + math.cos(a) * r
    local e = SpawnPrefab("rv_puddle", {
      x = x, y = ((terrainHeight and terrainHeight(x, z)) or 0) + 0.06, z = z })
    if e ~= nil then
      local s = 1.1 + math.random() * 1.5
      SetScale(e, s, 0.012, s * (0.7 + math.random() * 0.5))
      SetRotationY(e, math.random() * math.pi * 2)
      puddles[#puddles + 1] = { ent = e, x = x, z = z }
    end
  end
end

-- 白天野兔：在草甸上绕当前区域慢慢蹦跳，夜间隐藏
function rabbitsActive()
  return not (isNight ~= nil and isNight())
end

function spawnRabbits()
  for _, r in ipairs(rabbits) do Despawn(r.ent) end
  rabbits = {}
  for i = 1, 5 do
    local e = SpawnPrefab("rv_rabbit", { x = 0, y = -60, z = 0 })
    if e ~= nil then
      SetScale(e, 0.26, 0.22, 0.34)
      rabbits[#rabbits + 1] = {
        ent = e,
        angle = math.random() * math.pi * 2,
        radius = 5 + math.random() * 9,
        speed = 0.12 + math.random() * 0.15,
        phase = math.random() * math.pi * 2,
      }
    end
  end
  rabbitRespawnT = 12
end

-- 猎取一只兔子：消耗 1 份野味（生食材）
function huntRabbit(index)
  local r = rabbits[index]
  if r == nil then return false end
  local wx, wz = rv.x, rv.z
  if r.ent ~= nil then
    local p = GetPosition(r.ent)
    if p ~= nil then wx, wz = p.x, p.z end
    Despawn(r.ent)
  end
  table.remove(rabbits, index)
  addItem("rawfood", 1)
  if PlaySfx then PlaySfx("shot") end
  SpawnFloatText(wx, 1.2, wz, "野味 +1", false, 1.2, 0.98, 0.86, 0.4)
  toast.text = "猎到一只野兔 — 回篝火烤熟"
  toast.t = 2.4
  return true
end

-- 准星命中检测：屏幕上离准星最近、且 22m 内的兔子
function huntRabbitAtScreen(cx, cy)
  if #rabbits == 0 or WorldToScreen == nil then return false end
  local best, bestD = nil, 1e9
  for i, r in ipairs(rabbits) do
    if r.ent ~= nil then
      local p = GetPosition(r.ent)
      if p ~= nil then
        local dx, dz = p.x - rv.x, p.z - rv.z
        if dx * dx + dz * dz < 22 * 22 then
          local sp = WorldToScreen(p.x, (p.y or 0) + 0.25, p.z)
          if sp ~= nil then
            local d = (sp.x - cx) ^ 2 + (sp.y - cy) ^ 2
            if d < 30 * 30 and d < bestD then best, bestD = i, d end
          end
        end
      end
    end
  end
  if best == nil then return false end
  return huntRabbit(best)
end

function updateRabbits(dt)
  if #rabbits == 0 then return end
  local active = rabbitsActive()
  rabbitsT = (rabbitsT or 0) + dt
  -- 猎杀后慢慢补回种群（最多 5 只）
  if #rabbits < 5 then
    rabbitRespawnT = (rabbitRespawnT or 12) - dt
    if rabbitRespawnT <= 0 then
      rabbitRespawnT = 12
      local e = SpawnPrefab("rv_rabbit", { x = 0, y = -60, z = 0 })
      if e ~= nil then
        SetScale(e, 0.26, 0.22, 0.34)
        rabbits[#rabbits + 1] = {
          ent = e,
          angle = math.random() * math.pi * 2,
          radius = 8 + math.random() * 9,
          speed = 0.12 + math.random() * 0.15,
          phase = math.random() * math.pi * 2,
        }
      end
    end
  end
  for _, r in ipairs(rabbits) do
    SetVisible(r.ent, active)
    if active then
      r.angle = r.angle + r.speed * dt
      local x = rv.x + math.cos(r.angle) * r.radius
      local z = rv.z + math.sin(r.angle) * r.radius
      local gy = (terrainHeight and terrainHeight(x, z)) or 0
      local hop = math.abs(math.sin(rabbitsT * 5 + r.phase)) * 0.16
      SetPosition(r.ent, { x = x, y = gy + 0.12 + hop, z = z })
      SetRotationY(r.ent, -r.angle + math.pi * 0.5)
    end
  end
end

-- 白天蝴蝶：在草甸/花丛附近成对翅膀飞舞，夜间或雨天隐藏
function butterfliesActive()
  if weather == "rain" then return false end
  return not (isNight ~= nil and isNight())
end

function spawnButterflies()
  for _, b in ipairs(butterflies) do
    if b.l ~= nil then Despawn(b.l) end
    if b.r ~= nil then Despawn(b.r) end
  end
  butterflies = {}
  for i = 1, 6 do
    local l = SpawnPrefab("rv_butterfly", { x = 0, y = -60, z = 0 })
    local r = SpawnPrefab("rv_butterfly", { x = 0, y = -60, z = 0 })
    if l ~= nil and r ~= nil then
      butterflies[#butterflies + 1] = {
        l = l, r = r,
        angle = math.random() * math.pi * 2,
        radius = 3 + math.random() * 8,
        speed = 0.25 + math.random() * 0.35,
        phase = math.random() * math.pi * 2,
        bob = 0.7 + math.random() * 0.8,
      }
    end
  end
end

function updateButterflies(dt)
  if #butterflies == 0 then return end
  local active = butterfliesActive()
  butterfliesT = (butterfliesT or 0) + dt
  for _, b in ipairs(butterflies) do
    SetVisible(b.l, active)
    SetVisible(b.r, active)
    if active then
      b.angle = b.angle + b.speed * dt
      local t = butterfliesT * 1.7 + b.phase
      local x = rv.x + math.cos(b.angle) * b.radius + math.sin(t) * 0.55
      local z = rv.z + math.sin(b.angle) * b.radius + math.cos(t * 0.8) * 0.55
      local gy = (terrainHeight and terrainHeight(x, z)) or 0
      local y = gy + b.bob + math.sin(t * 2.1) * 0.22
      local flap = 0.35 + 0.65 * math.abs(math.sin(butterfliesT * 16 + b.phase))
      local ox, oz = math.cos(b.angle) * 0.055, -math.sin(b.angle) * 0.055
      SetPosition(b.l, { x = x - ox, y = y, z = z - oz })
      SetPosition(b.r, { x = x + ox, y = y, z = z + oz })
      SetRotationY(b.l, -b.angle)
      SetRotationY(b.r, -b.angle)
      SetScale(b.l, 0.11 * flap, 0.02, 0.14)
      SetScale(b.r, 0.11 * flap, 0.02, 0.14)
    end
  end
end

-- 农场风车：4 片叶片绕轮毂旋转
function updateWindmill(dt)
  if windmill == nil or #(windmill.blades or {}) == 0 then return end
  windmill.angle = (windmill.angle or 0) + (dt or 0) * 0.55
  for i, b in ipairs(windmill.blades) do
    local a = windmill.angle + (i - 1) * math.pi * 0.5
    SetPosition(b, { x = windmill.x + math.sin(a) * 1.45,
                     y = windmill.y + math.cos(a) * 1.45,
                     z = windmill.z })
    if SetRotationEuler ~= nil then
      SetRotationEuler(b, 0, 0, -a)
    else
      SetRotationY(b, -a)
    end
  end
end

function updatePuddles(dt)
  if weather == "rain" then
    if #puddles == 0 then
      spawnPuddles()
    elseif puddleCenter ~= nil then
      local dx, dz = rv.x - (puddleCenter.x or rv.x), rv.z - (puddleCenter.z or rv.z)
      if dx * dx + dz * dz > 32 * 32 then spawnPuddles() end
    end
  elseif #puddles > 0 then
    for _, p in ipairs(puddles) do Despawn(p.ent) end
    puddles = {}
    puddleCenter = nil
  end
end

-- 营火暖光的轻微跳动：没有灯光强度 API，用位置抖动 + 高度起伏模拟火光
function updateCampfireLight(dt)
  if campfireLight == nil or campfire == nil then return end
  campfireLightT = (campfireLightT or 0) + dt
  local j = math.sin(campfireLightT * 11.0) * 0.09 +
            math.sin(campfireLightT * 17.3) * 0.05
  SetPosition(campfireLight, {
    x = campfire.x + j,
    y = 0.95 + math.abs(j) * 0.45,
    z = campfire.z + j * 0.7 })
  -- 火苗抖动：缩放 + 轻微横向摆动
  for i, f in ipairs(campfireFlames or {}) do
    local s = 0.82 + 0.20 * math.sin(campfireLightT * (9.0 + i * 2.7) + i)
    local sway = math.sin(campfireLightT * (6.0 + i * 1.9) + i * 1.7) * 0.06
    SetScale(f.ent, f.sx * s, f.sy * (0.85 + 0.25 * s), f.sz * s)
    SetPosition(f.ent, { x = f.x + sway, y = f.y + math.abs(sway) * 0.6,
                         z = f.z + j * 0.3 })
  end
end

-- 营地陈设：遮阳棚 + 木桌凳 + 物资箱 + 暖光营地灯，让出生点像一个
-- 真正驻扎下来的落脚点，而不是空旷路口。纯装饰，不参与碰撞。
function spawnCampProps()
  campProps = {}
  local function prop(pre, x, y, z, sx, sy, sz, rot)
    local e = SpawnPrefab(pre, { x = x, y = y, z = z })
    if e ~= nil then
      SetScale(e, sx, sy, sz)
      if rot then SetRotationY(e, rot) end
      campProps[#campProps + 1] = { ent = e, x = x, z = z }
    end
    return e
  end
  local function gy(x, z)
    return (terrainHeight and terrainHeight(x, z)) or 0
  end
  -- 遮阳棚：四根木柱 + 浅色棚顶
  for _, p in ipairs({ { 3.6, 4.2 }, { 5.4, 4.2 }, { 3.6, 6.0 }, { 5.4, 6.0 } }) do
    prop("rv_camp_wood", p[1], gy(p[1], p[2]) + 1.15, p[2], 0.12, 2.30, 0.12)
  end
  prop("rv_panel_light", 4.5, gy(4.5, 5.1) + 2.33, 5.1, 2.50, 0.10, 2.30, 0.10)
  -- 木桌 + 四条腿 + 两侧长凳
  local tx, tz = 4.5, 5.0
  local ty = gy(tx, tz)
  prop("rv_camp_wood", tx, ty + 0.78, tz, 1.70, 0.08, 0.85)
  for _, p in ipairs({ { -0.72, -0.32 }, { 0.72, -0.32 },
                       { -0.72, 0.32 }, { 0.72, 0.32 } }) do
    prop("rv_camp_wood", tx + p[1], ty + 0.39, tz + p[2], 0.09, 0.78, 0.09)
  end
  prop("rv_camp_wood", tx, ty + 0.44, tz - 0.92, 1.70, 0.07, 0.28)
  prop("rv_camp_wood", tx, ty + 0.44, tz + 0.92, 1.70, 0.07, 0.28)
  -- 桌上的营地收音机（可开关）
  campRadio = { x = tx, z = tz, y = ty + 0.90 }
  prop("rv_panel_dark", tx, ty + 0.90, tz, 0.34, 0.18, 0.16, 0.2)
  prop("rv_taillight", tx + 0.11, ty + 0.94, tz + 0.09, 0.05, 0.05, 0.02, 0.2)
  -- 两把面向桌子的木椅（座面 + 靠背 + 四条腿）
  for _, c in ipairs({ { 3.55, 5.0, math.pi * 0.5 },
                       { 5.45, 5.0, -math.pi * 0.5 } }) do
    local cx, cz, yaw = c[1], c[2], c[3]
    local cy = gy(cx, cz)
    prop("rv_camp_wood", cx, cy + 0.46, cz, 0.42, 0.07, 0.42, yaw)
    local bx = cx - math.sin(yaw) * 0.20
    local bz = cz - math.cos(yaw) * 0.20
    prop("rv_camp_wood", bx, cy + 0.68, bz, 0.42, 0.42, 0.07, yaw)
    for _, off in ipairs({ { -0.15, -0.15 }, { 0.15, -0.15 },
                           { -0.15, 0.15 }, { 0.15, 0.15 } }) do
      local lx = off[1] * math.cos(yaw) + off[2] * math.sin(yaw)
      local lz = -off[1] * math.sin(yaw) + off[2] * math.cos(yaw)
      prop("rv_camp_wood", cx + lx, cy + 0.23, cz + lz, 0.07, 0.46, 0.07, yaw)
    end
  end
  -- 篝火旁的小柴堆
  for i = 1, 3 do
    local lx, lz = 0.45 + i * 0.18, 2.85 + (i % 2) * 0.14
    prop("rv_camp_wood", lx, gy(lx, lz) + 0.10, lz, 0.55, 0.13, 0.15, i * 0.5)
  end
  -- 晾衣绳：两根木柱 + 一条细线
  local p1x, p1z = 3.3, 7.0
  local p2x, p2z = 5.9, 7.0
  prop("rv_camp_wood", p1x, gy(p1x, p1z) + 0.95, p1z, 0.10, 1.90, 0.10)
  prop("rv_camp_wood", p2x, gy(p2x, p2z) + 0.95, p2z, 0.10, 1.90, 0.10)
  local line = SpawnPrefab("rv_wire", {
    x = (p1x + p2x) * 0.5, y = gy(p1x, p1z) + 1.82, z = (p1z + p2z) * 0.5 })
  if line ~= nil then
    SetScale(line, 0.02, 0.02, 2.6)
    SetRotationY(line, math.pi * 0.5)
    campProps[#campProps + 1] = { ent = line, x = p1x, z = p1z }
  end
  -- 水桶与木箱堆
  prop("rv_furn_water_tank", 6.5, gy(6.5, 6.6) + 0.35, 6.6, 0.8, 0.6, 0.8)
  prop("rv_loot_crate", 2.6, gy(2.6, 3.6) + 0.28, 3.6, 0.55, 0.55, 0.55, 0.3)
  prop("rv_loot_crate", 2.6, gy(2.6, 3.6) + 0.80, 3.6, 0.45, 0.45, 0.45, 0.9)
  -- 物资箱 / 油桶
  prop("rv_loot_crate", 6.5, gy(6.5, 2.4) + 0.30, 2.4, 0.60, 0.60, 0.60, 0.40)
  prop("rv_loot_crate", 6.9, gy(6.9, 3.2) + 0.24, 3.2, 0.48, 0.48, 0.48, 1.10)
  prop("rv_fuelcan", 5.9, gy(5.9, 2.2) + 0.34, 2.2, 0.50, 0.70, 0.50, 0.30)
  -- 营地灯：木杆 + 暖光灯罩
  prop("rv_marker", 5.9, gy(5.9, 5.9) + 1.30, 5.9, 0.09, 2.60, 0.09)
  prop("rv_lamp", 5.9, gy(5.9, 5.9) + 2.56, 5.9, 0.18, 0.10, 0.18)
end
