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
      -- Butt the slabs edge-to-edge instead of overlapping them: coplanar
      -- overlapping tops z-fight into flickering dark patches on the road.
      SetScale(ent, 7.5, 0.02, SEG)
      placeAt(ent, d, 0.08 + (roadLinkSeq % 6) * 0.0015)
      roadSegments[#roadSegments + 1] = { ent = ent }
    end
  end
  for d = 6, len - 2, 6 do
    local ent = SpawnPrefab("rv_road_dash", { x = 0, y = -50, z = 0 })
    if ent ~= nil then
      -- Keep the paint clear of the asphalt's polygon offset and its normal
      -- map; otherwise the line vanishes or turns into road texture at
      -- grazing angles.
      placeAt(ent, d, 0.14 + (roadLinkSeq % 6) * 0.0015)
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
  local lastX, lastZ, lastGY
  for d = 12, len - 8, 28 do
    local x = ax + ux * d + rx * 6.2
    local z = az + uz * d + rz * 6.2
    local gy = (terrainHeight and terrainHeight(x, z)) or 0
    -- Blender 建模的木电线杆：底座/锥形杆身/横担/绝缘子/变压器/脚钉
    local pole = SpawnPrefab("rv_utility_pole", { x = x, y = gy, z = z })
    if pole ~= nil then
      SetRotationY(pole, yaw)
      roadPoles[#roadPoles + 1] = pole
    end
    -- 相邻两根杆之间拉两条电线（沿横担两端，略低于杆顶）
    if lastX ~= nil then
      local wdx, wdz = x - lastX, z - lastZ
      local wlen = math.sqrt(wdx * wdx + wdz * wdz)
      local wmx, wmz = (x + lastX) * 0.5, (z + lastZ) * 0.5
      local wy = (gy + (lastGY or gy)) * 0.5 + 6.08
      for _, off in ipairs({ -0.55, 0.55 }) do
        local wire = SpawnPrefab("rv_wire", {
          x = wmx + rx * off, y = wy, z = wmz + rz * off })
        if wire ~= nil then
          SetScale(wire, 0.035, 0.035, wlen)
          SetRotationY(wire, yaw)
          roadWires[#roadWires + 1] = wire
        end
      end
    end
    lastX, lastZ, lastGY = x, z, gy
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
        local pitch, roll = 0, 0
        if terrainSlope ~= nil then
          local dhx, dhz = terrainSlope(x, z)
          local dhF = dhx * ux + dhz * uz
          local dhR = dhx * uz - dhz * ux
          pitch = math.max(-0.18, math.min(0.18, -math.atan(dhF)))
          roll = math.max(-0.18, math.min(0.18, math.atan(dhR)))
        end
        local e = SpawnPrefab("rv_road_shoulder", { x = x, y = gy + 0.055, z = z })
        if e ~= nil then
          -- No 0.3 m overlap: coplanar shoulder tops also z-fight.
          SetScale(e, 1.40, 0.020, 10.0)
          if SetRotationEuler ~= nil then
            SetRotationEuler(e, pitch, roll, yaw)
          else
            SetRotationY(e, yaw)
          end
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
  local model = SpawnPrefab("rv_water_tower", { x = x, y = gy, z = z })
  if model ~= nil then
    tower.model = model
    tower.parts[#tower.parts + 1] = model
  end
  waterTowers[#waterTowers + 1] = tower
  return tower
end

-- 仓库旁的无线电塔：Blender 建模三角格构塔 + 顶端红色信标（夜间闪烁）
function spawnRadioMast(x, z)
  local gy = (terrainHeight and terrainHeight(x, z)) or 0
  local mast = { x = x, z = z, parts = {}, beacon = nil, t = 0, lit = false }
  local model = SpawnPrefab("rv_radio_mast", { x = x, y = gy, z = z })
  if model ~= nil then
    mast.model = model
    mast.parts[#mast.parts + 1] = model
  end
  local beacon = SpawnPrefab("rv_taillight", { x = x, y = gy + 13.75, z = z })
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
    addJournal("修好了仓库无线电塔（地图全解锁 + 燃油罐 + 修理包）", "goal")
  end
  toast.text = "无线电塔修好了——信标常亮、地图地点全解锁，收到应急补给；去废弃小镇恢复信号中继"
  toast.t = 6.0
  if autoSave ~= nil then autoSave() end
  return true
end

-- 长线目标：用金属/木材修好小镇水塔，镇上停靠可免费补水
WATER_TOWER_COST = { metal = 5, wood = 4 }

function nearestWaterTower(px, pz)
  local best, bestD = nil, math.huge
  for _, tw in ipairs(waterTowers or {}) do
    local d = math.sqrt((tw.x - (px or 0)) ^ 2 + (tw.z - (pz or 0)) ^ 2)
    if d < bestD then best, bestD = tw, d end
  end
  return best, bestD
end

function repairWaterTower()
  local px = (player and player.x) or rv.x or 0
  local pz = (player and player.z) or rv.z or 0
  local tw = nearestWaterTower(px, pz)
  if tw == nil then
    toast.text = "附近没有可修的水塔"
    toast.t = 2.0
    return false
  end
  if waterTowerFixed then
    toast.text = "水塔已经修好了"
    toast.t = 2.0
    return false
  end
  if not canAfford(WATER_TOWER_COST) then
    toast.text = "修水塔材料不足：" .. costText(WATER_TOWER_COST)
    toast.t = 2.8
    return false
  end
  payCost(WATER_TOWER_COST)
  waterTowerFixed = true
  local before = water or 0
  if addWater ~= nil then addWater(20) end
  if PlaySfx then PlaySfx("repair") end
  if addJournal ~= nil then
    addJournal("修好了小镇水塔（镇上停靠可免费补水）", "goal")
  end
  toast.text = string.format("小镇水塔修好了——镇上停靠免费补水（+%.0fL）",
                             (water or 0) - before)
  toast.t = 5.0
  if autoSave ~= nil then autoSave() end
  return true
end

-- 程序化地点的外景装饰风格（按名字识别）
function poiDecorStyle(name)
  local nm = name or ""
  if nm:find("木屋") ~= nil then return "woodcabin" end
  if nm:find("邮局") ~= nil then return "postoffice" end
  if nm:find("护林") ~= nil then return "ranger" end
  return nil
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
    -- 农田稻草人（立柱 + 横杆 + 布身 + 草头 + 帽子）
    do
      local scX, scZ = cx - 6.2, cz - 4.5
      local scY = (terrainHeight and terrainHeight(scX, scZ)) or 0
      local function scProp(pre, x, y, z, sx, sy, sz, rot)
        local e = SpawnPrefab(pre, { x = x, y = y, z = z })
        if e ~= nil then
          SetScale(e, sx, sy, sz)
          if rot ~= nil then SetRotationY(e, rot) end
          farmScarecrow[#farmScarecrow + 1] = e
        end
        return e
      end
      scProp("rv_camp_wood", scX, scY + 0.90, scZ, 0.12, 1.80, 0.12)
      scProp("rv_camp_wood", scX, scY + 1.35, scZ, 1.35, 0.10, 0.10)
      scProp("rv_panel_light", scX, scY + 1.05, scZ, 0.55, 0.75, 0.30, 0.2)
      scProp("rv_rock", scX, scY + 1.55, scZ, 0.28, 0.30, 0.28)
      scProp("rv_panel_dark", scX, scY + 1.74, scZ, 0.55, 0.06, 0.55, 0.3)
    end
    -- 农田作物：谷仓西侧成排的低多边形作物，补全农田区域
    do
      for row = 0, 3 do
        for col = 0, 5 do
          local cropX = cx - 10.0 + col * 1.15
          local cropZ = cz - 3.0 + row * 1.35
          local cropY = (terrainHeight and terrainHeight(cropX, cropZ)) or 0
          local e = SpawnPrefab("rv_plant", {
            x = cropX, y = cropY + 0.18, z = cropZ })
          if e ~= nil then
            local s = 0.55 + ((row + col) % 3) * 0.08
            SetScale(e, s, s * 1.5, s)
            SetRotationY(e, (row * 0.7 + col * 0.4) % math.pi)
            farmCrops[#farmCrops + 1] = e
          end
        end
      end
    end
    -- 农场风车（Blender 建模：格构塔 + 独立叶片，叶片继续由 updateWindmill 旋转）
    local wx, wz = cx + 8.5, cz - 5.5
    local wy = (terrainHeight and terrainHeight(wx, wz)) or 0
    local tower = SpawnPrefab("rv_windmill_tower", { x = wx, y = wy, z = wz })
    if tower ~= nil then SetRotationY(tower, 0) end
    windmill = { x = wx, y = wy + 7.0, z = wz + 0.35, angle = 0,
                 blades = {}, tower = tower }
    for i = 1, 4 do
      local b = SpawnPrefab("rv_windmill_blade",
                            { x = wx, y = wy + 7.0, z = wz + 0.35 })
      if b ~= nil then
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
    -- 可修复的油泵交互点（长线目标：修好后加油永久 7 折）
    nd.fuelPump = { x = cx + 2.6, z = cz + 2.2 }
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
  -- Blender 建模的通用路边建筑：木结构、门廊、坡屋顶、窗框与内部柜台；
  -- 保留 6.2 x 5.2 的碰撞/搜刮 Footprint，门洞在 +Z 正前方。
  local house = SpawnPrefab("rv_roadhouse", { x = cx, y = -0.14, z = cz })
  if house ~= nil then
    SetScale(house, 1, 1, 1)
    shell[#shell + 1] = { ent = house, x = cx, y = -0.14, z = cz,
                          sx = 1, sy = 1, sz = 1 }
  end
  -- 门廊与室内各一盏暖光，避免阴影面完全发黑
  local porchLamp = SpawnPrefab("rv_lamp", {
    x = cx - 1.65, y = 2.35, z = cz + 2.66 })
  if porchLamp ~= nil then SetScale(porchLamp, 0.85, 0.85, 0.85) end
  local roomLamp = SpawnPrefab("rv_lamp", {
    x = cx, y = 2.06, z = cz - 0.7 })
  if roomLamp ~= nil then SetScale(roomLamp, 0.55, 0.55, 0.55) end
  -- 不可见的补光，从门廊前方照亮阴影里的立面
  SpawnPrefab("rv_poi_light", { x = cx, y = 2.0, z = cz + 6.5 })

  -- 类型化外景装饰：让程序化地点不只是一间通用小屋
  local style = poiDecorStyle(nd.name)
  if style == "woodcabin" then
    -- 林间木屋：门口三层柴堆 + 屋顶烟囱
    for i = 1, 3 do
      local log = SpawnPrefab("rv_camp_wood", {
        x = cx - 3.9, y = 0.20 + (i - 1) * 0.26, z = cz + 1.3 })
      if log ~= nil then
        SetScale(log, 1.7, 0.24, 0.26)
        SetRotationY(log, 0.18 + i * 0.14)
      end
    end
    local chimney = SpawnPrefab("rv_panel_dark", { x = cx - 1.9, y = 3.15, z = cz - 1.3 })
    if chimney ~= nil then SetScale(chimney, 0.62, 1.15, 0.62) end
  elseif style == "postoffice" then
    -- 废弃邮局：门口邮箱 + 散落的包裹箱
    local box = SpawnPrefab("rv_panel_light", { x = cx - 2.5, y = 0.82, z = cz + 3.05 })
    if box ~= nil then SetScale(box, 0.52, 0.86, 0.46) end
    local flag = SpawnPrefab("rv_panel_dark", { x = cx - 2.5, y = 1.28, z = cz + 2.80 })
    if flag ~= nil then SetScale(flag, 0.06, 0.30, 0.18) end
    -- 可查看一次的邮箱（首次打开给补给，之后为空）
    nd.mailbox = { x = cx - 2.5, z = cz + 3.05, taken = false }
    for i = 1, 2 do
      local crate = SpawnPrefab("rv_cargo_crate", {
        x = cx + 2.1 + i * 0.75, y = 0.28, z = cz + 2.7 })
      if crate ~= nil then
        SetScale(crate, 0.58, 0.58, 0.58)
        SetRotationY(crate, i * 0.55)
      end
    end
  elseif style == "ranger" then
    -- 林间护林站：旁边一座 Blender 建模的木制瞭望塔 + 门口地图板
    local tx, tz = cx + 6.0, cz - 3.0
    local ty = (terrainHeight and terrainHeight(tx, tz)) or 0
    local tower = SpawnPrefab("rv_ranger_tower", { x = tx, y = ty, z = tz })
    if tower ~= nil then SetRotationY(tower, 0) end
    local board = SpawnPrefab("rv_panel_light", {
      x = cx - 3.6, y = 1.1, z = cz + 2.6 })
    if board ~= nil then SetScale(board, 1.4, 0.9, 0.08) end
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

-- 野生动物群不跟车移动：停车且离锚点 >45m 时迁到新区域
function wildlifeAnchorStep()
  if wildlifeAnchor == nil then
    wildlifeAnchor = { x = rv.x, z = rv.z }
  end
  if math.abs(rv.speed or 0) >= 0.5 then return false end
  local dx, dz = rv.x - wildlifeAnchor.x, rv.z - wildlifeAnchor.z
  if dx * dx + dz * dz <= 45 * 45 then return false end
  wildlifeAnchor.x, wildlifeAnchor.z = rv.x, rv.z
  if spawnBirds ~= nil then spawnBirds() end
  if spawnRabbits ~= nil then spawnRabbits() end
  if spawnButterflies ~= nil then spawnButterflies() end
  return true
end

function spawnBirds()
  for _, b in ipairs(birds) do Despawn(b.ent) end
  birds = {}
  for i = 1, 7 do
    local e = SpawnPrefab("rv_bird", { x = 0, y = -60, z = 0 })
    if e ~= nil then
      SetScale(e, 0.75, 0.75, 0.75)
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
  local ax, az = (wildlifeAnchor and wildlifeAnchor.x) or rv.x,
                 (wildlifeAnchor and wildlifeAnchor.z) or rv.z
  local gy = (terrainHeight and terrainHeight(ax, az)) or (rv.groundY or 0)
  birdsT = (birdsT or 0) + dt
  for _, b in ipairs(birds) do
    SetVisible(b.ent, active)
    if active then
      b.angle = b.angle + b.speed * dt
      local x = ax + math.cos(b.angle) * b.radius
      local z = az + math.sin(b.angle) * b.radius
      local y = gy + b.height +
                math.sin(b.angle * 3 + b.phase) * 0.5
      SetPosition(b.ent, { x = x, y = y, z = z })
      SetRotationY(b.ent, -b.angle)
      local flap = 0.35 + 0.65 * math.abs(math.sin(birdsT * b.flap + b.phase))
      SetScale(b.ent, 0.75, 0.75 * (0.90 + 0.10 * flap), 0.75)
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
      SetScale(e, 0.95, 0.95, 0.95)
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

-- 猎鹿：准星对准 26m 内的农场鹿，获得 2 份野味
function huntAmbientDeer(index)
  local d = ambientDeer and ambientDeer[index] or nil
  if d == nil or d.ent == nil then return false end
  local p = GetPosition(d.ent)
  local wx = p and p.x or d.ax
  local wz = p and p.z or d.az
  Despawn(d.ent)
  table.remove(ambientDeer, index)
  addItem("rawfood", 2)
  if PlaySfx then PlaySfx("shot") end
  SpawnFloatText(wx, 1.2, wz, "鹿肉 +2", false, 1.2, 0.98, 0.86, 0.4)
  toast.text = "猎到一只鹿 → 野味 +2"
  toast.t = 2.6
  return true
end

function huntDeerAtScreen(cx, cy)
  if #(ambientDeer or {}) == 0 or WorldToScreen == nil then return false end
  local best, bestD = nil, 1e9
  for i, d in ipairs(ambientDeer) do
    if d.ent ~= nil then
      local p = GetPosition(d.ent)
      if p ~= nil then
        local dx, dz = p.x - rv.x, p.z - rv.z
        if dx * dx + dz * dz < 26 * 26 then
          local sp = WorldToScreen(p.x, (p.y or 0) + 0.5, p.z)
          if sp ~= nil then
            local dist = (sp.x - cx) ^ 2 + (sp.y - cy) ^ 2
            if dist < 34 * 34 and dist < bestD then best, bestD = i, dist end
          end
        end
      end
    end
  end
  if best == nil then return false end
  return huntAmbientDeer(best)
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
        SetScale(e, 0.95, 0.95, 0.95)
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
      local ax = (wildlifeAnchor and wildlifeAnchor.x) or rv.x
      local az = (wildlifeAnchor and wildlifeAnchor.z) or rv.z
      local x = ax + math.cos(r.angle) * r.radius
      local z = az + math.sin(r.angle) * r.radius
      local gy = (terrainHeight and terrainHeight(x, z)) or 0
      local hop = math.abs(math.sin(rabbitsT * 5 + r.phase)) * 0.16
      SetPosition(r.ent, { x = x, y = gy + hop, z = z })
      SetRotationY(r.ent, -r.angle)
    end
  end
end

-- 农场附近的白天鹿群：在谷仓周围缓慢游走，夜间隐藏（放大的兔子模型）
function deerActive()
  return not (isNight ~= nil and isNight())
end

function spawnAmbientDeer()
  for _, d in ipairs(ambientDeer) do Despawn(d.ent) end
  ambientDeer = {}
  local nd = (nodeById ~= nil) and nodeById("farm") or nil
  if nd == nil then return end
  for i = 1, 2 do
    local e = SpawnPrefab("rv_deer", { x = 0, y = -60, z = 0 })
    if e ~= nil then
      SetScale(e, 0.90, 0.90, 0.90)
      ambientDeer[#ambientDeer + 1] = {
        ent = e,
        ax = nd.x + (i == 1 and -9.0 or 10.0),
        az = nd.z + (i == 1 and 8.0 or -7.0),
        angle = math.random() * math.pi * 2,
        radius = 3.5 + math.random() * 3.0,
        speed = 0.08 + math.random() * 0.10,
        phase = math.random() * math.pi * 2,
      }
    end
  end
end

function updateAmbientDeer(dt)
  if #ambientDeer == 0 then return end
  local active = deerActive()
  ambientDeerT = (ambientDeerT or 0) + (dt or 0)
  for _, d in ipairs(ambientDeer) do
    if (d.fleeT or 0) > 0 then
      d.fleeT = math.max(0, d.fleeT - (dt or 0))
      SetVisible(d.ent, false)
    else
      SetVisible(d.ent, active)
      if active then
        d.angle = d.angle + d.speed * dt
        local x = d.ax + math.cos(d.angle) * d.radius
        local z = d.az + math.sin(d.angle) * d.radius
        local gy = (terrainHeight and terrainHeight(x, z)) or 0
        local bob = math.abs(math.sin(ambientDeerT * 2.2 + d.phase)) * 0.06
        SetPosition(d.ent, { x = x, y = gy + bob, z = z })
        SetRotationY(d.ent, -d.angle)
      end
    end
  end
end

-- 水塘上的野鸭：白天缓慢游动，夜间隐藏
function ducksActive()
  return not (isNight ~= nil and isNight())
end

function spawnPondDucks()
  for _, d in ipairs(pondDucks or {}) do Despawn(d.ent) end
  pondDucks = {}
  local p = ponds and ponds[1] or nil
  if p == nil then return end
  for i = 1, 3 do
    local e = SpawnPrefab("rv_duck", { x = p.x, y = 0.1, z = p.z })
    if e ~= nil then
      SetScale(e, 0.90, 0.90, 0.90)
      pondDucks[#pondDucks + 1] = {
        ent = e, cx = p.x, cz = p.z,
        angle = math.random() * math.pi * 2,
        radius = 1.6 + (i - 1) * 0.9,
        speed = 0.10 + math.random() * 0.08,
        phase = math.random() * math.pi * 2,
      }
    end
  end
end

function updatePondDucks(dt)
  if #(pondDucks or {}) == 0 then return end
  local active = ducksActive()
  pondDucksT = (pondDucksT or 0) + (dt or 0)
  for _, d in ipairs(pondDucks) do
    if (d.fleeT or 0) > 0 then
      d.fleeT = math.max(0, d.fleeT - (dt or 0))
      SetVisible(d.ent, false)
    else
      SetVisible(d.ent, active)
      if active then
        d.angle = d.angle + d.speed * dt
        local x = d.cx + math.cos(d.angle) * d.radius
        local z = d.cz + math.sin(d.angle) * d.radius
        local gy = (terrainHeight and terrainHeight(x, z)) or 0
        local bob = math.sin(pondDucksT * 1.8 + d.phase) * 0.03
        SetPosition(d.ent, { x = x, y = gy + 0.02 + bob, z = z })
        SetRotationY(d.ent, -d.angle)
      end
    end
  end
end

-- 鸣笛惊走水塘野鸭
function scarePondDucks(x, z, radius)
  local r2 = (radius or 24) * (radius or 24)
  local n = 0
  for _, d in ipairs(pondDucks or {}) do
    local wx = d.cx + math.cos(d.angle) * d.radius
    local wz = d.cz + math.sin(d.angle) * d.radius
    local dx, dz = wx - (x or 0), wz - (z or 0)
    if dx * dx + dz * dz <= r2 then
      d.fleeT = 20
      n = n + 1
    end
  end
  return n
end

-- 鸣笛惊走农场附近的鹿群：隐藏一段时间后回到原地
function scareAmbientDeer(x, z, radius)
  local r2 = (radius or 24) * (radius or 24)
  local n = 0
  for _, d in ipairs(ambientDeer or {}) do
    local wx = d.ax + math.cos(d.angle) * d.radius
    local wz = d.az + math.sin(d.angle) * d.radius
    local dx, dz = wx - (x or 0), wz - (z or 0)
    if dx * dx + dz * dz <= r2 then
      d.fleeT = 20
      n = n + 1
    end
  end
  return n
end

-- 鸣笛惊走附近的鸟和野兔（返回惊走的数量，供喇叭使用）
function scareSmallGameNear(x, z, radius)
  local r2 = (radius or 24) * (radius or 24)
  local ax = (wildlifeAnchor and wildlifeAnchor.x) or (rv and rv.x) or 0
  local az = (wildlifeAnchor and wildlifeAnchor.z) or (rv and rv.z) or 0
  local n = 0
  for i = #rabbits, 1, -1 do
    local r = rabbits[i]
    local wx = ax + math.cos(r.angle) * r.radius
    local wz = az + math.sin(r.angle) * r.radius
    local dx, dz = wx - (x or 0), wz - (z or 0)
    if dx * dx + dz * dz <= r2 then
      if r.ent ~= nil then Despawn(r.ent) end
      table.remove(rabbits, i)
      n = n + 1
    end
  end
  for i = #birds, 1, -1 do
    local b = birds[i]
    local wx = ax + math.cos(b.angle) * b.radius
    local wz = az + math.sin(b.angle) * b.radius
    local dx, dz = wx - (x or 0), wz - (z or 0)
    if dx * dx + dz * dz <= r2 then
      if b.ent ~= nil then Despawn(b.ent) end
      table.remove(birds, i)
      n = n + 1
    end
  end
  return n
end

-- 白天蝴蝶：在草甸/花丛附近成对翅膀飞舞，夜间或雨天隐藏
function butterfliesActive()
  if weather == "rain" or weather == "snow" then return false end
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
      local ax = (wildlifeAnchor and wildlifeAnchor.x) or rv.x
      local az = (wildlifeAnchor and wildlifeAnchor.z) or rv.z
      local x = ax + math.cos(b.angle) * b.radius + math.sin(t) * 0.55
      local z = az + math.sin(b.angle) * b.radius + math.cos(t * 0.8) * 0.55
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

-- 雪天地面斑块：房车周围铺一层浅白色积雪，雪停自动清除，开远重铺
function spawnSnowPatches()
  for _, p in ipairs(snowPatches) do Despawn(p.ent) end
  snowPatches = {}
  snowCenter = { x = rv.x, z = rv.z }
  for i = 1, 10 do
    local a = math.random() * math.pi * 2
    local r = 3 + math.random() * 16
    local x = rv.x + math.sin(a) * r
    local z = rv.z + math.cos(a) * r
    local e = SpawnPrefab("rv_snow_patch", {
      x = x, y = ((terrainHeight and terrainHeight(x, z)) or 0) + 0.045, z = z })
    if e ~= nil then
      local s = 1.4 + math.random() * 2.4
      SetScale(e, s, 0.014, s * (0.7 + math.random() * 0.6))
      SetRotationY(e, math.random() * math.pi * 2)
      snowPatches[#snowPatches + 1] = { ent = e, x = x, z = z }
    end
  end
end

function updateSnowPatches(dt)
  if weather == "snow" then
    if #snowPatches == 0 then
      spawnSnowPatches()
    elseif snowCenter ~= nil then
      local dx, dz = rv.x - (snowCenter.x or rv.x), rv.z - (snowCenter.z or rv.z)
      if dx * dx + dz * dz > 32 * 32 then spawnSnowPatches() end
    end
  elseif #snowPatches > 0 then
    for _, p in ipairs(snowPatches) do Despawn(p.ent) end
    snowPatches = {}
    snowCenter = nil
  end
end

-- 是否压在积雪斑块上（雪地局部更滑）
function onSnowPatch(x, z, radius)
  local r2 = (radius or 2.0) * (radius or 2.0)
  for _, p in ipairs(snowPatches or {}) do
    local dx, dz = (x or 0) - p.x, (z or 0) - p.z
    if dx * dx + dz * dz <= r2 then return true end
  end
  return false
end

-- 篝火噼啪声：靠近火堆时每隔 2.2–3.6s 播一次 3D 音效
function campfireCrackleInterval()
  return 2.2 + math.random() * 1.4
end

function campfireAudible(dist)
  return (dist or 1e9) < 14
end

-- 夜间蟋蟀：晴天/阴天的夜晚随机鸣叫，雨雪天安静
function nightCricketsActive()
  if not (isNight ~= nil and isNight()) then return false end
  if weather == "rain" or weather == "snow" then return false end
  return true
end

function cricketInterval()
  return 3.5 + math.random() * 3.0
end

function updateNightCrickets(dt)
  if not nightCricketsActive() then
    cricketT = 0
    return
  end
  cricketT = (cricketT or 0) - (dt or 0)
  if cricketT > 0 then return end
  cricketT = cricketInterval()
  cricketPlays = (cricketPlays or 0) + 1
  if PlaySfx then PlaySfx("cricket") end
end

-- 白天鸟鸣：晴天/阴天的白天随机鸟叫，雨雪与夜晚安静
function dayBirdsActive()
  if isNight ~= nil and isNight() then return false end
  if weather == "rain" or weather == "snow" then return false end
  return true
end

function birdChirpInterval()
  return 4.0 + math.random() * 5.0
end

function updateDayBirds(dt)
  if not dayBirdsActive() then
    birdChirpT = 0
    return
  end
  birdChirpT = (birdChirpT or 0) - (dt or 0)
  if birdChirpT > 0 then return end
  birdChirpT = birdChirpInterval()
  birdChirpPlays = (birdChirpPlays or 0) + 1
  if PlaySfx then PlaySfx("birds") end
end

-- 远处狼嚎：夜晚、非雨雪、远离营地时偶尔传来（营地附近安全）
function howlActive()
  if not (isNight ~= nil and isNight()) then return false end
  if weather == "rain" or weather == "snow" then return false end
  local lx, lz = rv.x or 0, rv.z or 0
  if onFoot and player ~= nil and player.active then
    lx, lz = player.x or lx, player.z or lz
  end
  local camp = (nodeById ~= nil) and nodeById("camp") or nil
  local cx, cz = 0, 0
  if camp ~= nil then cx, cz = camp.x, camp.z end
  local dx, dz = lx - cx, lz - cz
  return dx * dx + dz * dz > 80 * 80
end

function howlInterval()
  return 45 + math.random() * 45
end

function updateNightHowls(dt)
  if not howlActive() then
    howlT = 0
    return
  end
  howlT = (howlT or 0) - (dt or 0)
  if howlT > 0 then return end
  howlT = howlInterval()
  howlPlays = (howlPlays or 0) + 1
  if PlaySfx then PlaySfx("howl") end
end

-- 夜间猫头鹰：晴朗夜晚偶尔传来两声低鸣
function owlsActive()
  if not (isNight ~= nil and isNight()) then return false end
  return weather ~= "rain" and weather ~= "snow"
end

function owlInterval()
  return 35 + math.random() * 40
end

function updateNightOwls(dt)
  if not owlsActive() then
    owlT = 0
    return
  end
  owlT = (owlT or 0) - (dt or 0)
  if owlT > 0 then return end
  owlT = owlInterval()
  owlPlays = (owlPlays or 0) + 1
  if PlaySfx then PlaySfx("owl") end
end

-- 营地附近的浆果丛：采摘后约 12 秒再生
function spawnBerryBushes()
  for _, b in ipairs(berryBushes) do Despawn(b.ent) end
  berryBushes = {}
  local spots = { { -8.0, 5.0 }, { -3.5, 8.5 }, { 7.5, 6.0 },
                  { 5.8, -6.5 } }
  -- 农场与湖边营地也各有一丛，鼓励探索时顺手采集
  local farm = (nodeById ~= nil) and nodeById("farm") or nil
  if farm ~= nil then
    spots[#spots + 1] = { farm.x + 6.5, farm.z + 6.0 }
  end
  local lake = (nodeById ~= nil) and nodeById("lakecamp") or nil
  if lake ~= nil then
    spots[#spots + 1] = { lake.x - 5.0, lake.z + 4.5 }
  end
  for i, p in ipairs(spots) do
    local x, z = p[1], p[2]
    local y = (terrainHeight and terrainHeight(x, z)) or 0
    local e = SpawnPrefab("rv_bush", { x = x, y = y + 0.32, z = z })
    if e ~= nil then
      SetScale(e, 1.15, 1.30, 1.15)
      SetRotationY(e, i * 0.9)
      local berries = {}
      for k = 1, 3 do
        local be = SpawnPrefab("rv_flower_red", {
          x = x + math.sin(k * 2.1) * 0.32,
          y = y + 0.72 + (k % 2) * 0.16,
          z = z + math.cos(k * 2.1) * 0.32 })
        if be ~= nil then
          SetScale(be, 0.20, 0.20, 0.20)
          berries[#berries + 1] = be
        end
      end
      berryBushes[#berryBushes + 1] = {
        ent = e, berries = berries, x = x, z = z, ready = true, t = 0 }
    end
  end
end

function updateBerryBushes(dt)
  for _, b in ipairs(berryBushes) do
    if not b.ready then
      b.t = (b.t or 0) - (dt or 0)
      if b.t <= 0 then
        b.ready = true
        for _, be in ipairs(b.berries or {}) do SetVisible(be, true) end
      end
    end
  end
end

-- 最近的成熟浆果丛（联机服务器按玩家位置判定用）
function nearestReadyBush(x, z, maxDist)
  local best, bestD = nil, maxDist or 2.8
  for i, b in ipairs(berryBushes or {}) do
    if b.ready then
      local dx, dz = b.x - (x or 0), b.z - (z or 0)
      local d = math.sqrt(dx * dx + dz * dz)
      if d < bestD then best, bestD = i, d end
    end
  end
  return best
end

-- 主路旁的补给箱：搜刮后约 180 游戏分钟再生
function spawnRoadCaches()
  for _, c in ipairs(roadCaches) do Despawn(c.ent) end
  roadCaches = {}
  for i, p in ipairs({ { 5.6, 24.0 }, { -5.6, 42.0 } }) do
    local x, z = p[1], p[2]
    local y = (terrainHeight and terrainHeight(x, z)) or 0
    local e = SpawnPrefab("rv_loot_crate", { x = x, y = y + 0.28, z = z })
    if e ~= nil then
      SetScale(e, 0.62, 0.52, 0.62)
      SetRotationY(e, i * 0.8)
      roadCaches[#roadCaches + 1] = {
        ent = e, x = x, z = z, ready = true, t = 0 }
    end
  end
end

function updateRoadCaches(dt)
  for _, c in ipairs(roadCaches) do
    if not c.ready then
      c.t = (c.t or 0) - (dt or 0)
      if c.t <= 0 then
        c.ready = true
        SetVisible(c.ent, true)
      end
    end
  end
end

function nearestReadyCache(x, z, maxDist)
  local best, bestD = nil, maxDist or 2.9
  for i, c in ipairs(roadCaches or {}) do
    if c.ready then
      local dx, dz = c.x - (x or 0), c.z - (z or 0)
      local d = math.sqrt(dx * dx + dz * dz)
      if d < bestD then best, bestD = i, d end
    end
  end
  return best
end

-- 加油站自动售货机：靠近按 E 花 1 金属买食物/水
function spawnVendingMachine()
  local nd = nodeById("gasstop")
  if nd == nil then return false end
  local x, z = nd.x + 2.8, nd.z + 2.2
  local y = (terrainHeight and terrainHeight(x, z)) or 0
  local e = SpawnPrefab("rv_fridge", { x = x, y = y + 0.55, z = z })
  if e == nil then return false end
  SetScale(e, 0.55, 0.85, 0.45)
  SetRotationY(e, 0.3)
  vendingMachine = { x = x, z = z, ent = e }
  return true
end

-- 路边搭车客：白天偶发，停车可给 5L 燃料换回补给
function spawnHitchhiker()
  if hitchhiker ~= nil then return false end
  local side = (math.random() < 0.5) and -1 or 1
  local wx, wz = rotOf(side * 6.2, 30)
  local gy = (terrainHeight and terrainHeight(wx, wz)) or 0
  local e = SpawnPrefab("rv_player", { x = wx, y = gy, z = wz })
  if e == nil then return false end
  SetRotationY(e, (rv.yaw or 0) + math.pi)
  hitchhiker = { x = wx, z = wz, ent = e }
  toast.text = "路边有人招手搭车——停车靠近按 E"
  toast.t = 3.6
  return true
end

function despawnHitchhiker()
  if hitchhiker == nil then return end
  if hitchhiker.ent ~= nil then Despawn(hitchhiker.ent) end
  hitchhiker = nil
end

-- 路边抛锚车：偶发的可搜刮事件
function spawnStrandedCar()
  if strandedCar ~= nil then return false end
  local side = (math.random() < 0.5) and -1 or 1
  local wx, wz = rotOf(side * 7.5, 26)
  local gy = (terrainHeight and terrainHeight(wx, wz)) or 0
  local body = SpawnPrefab("rv_panel_dark", { x = wx, y = gy + 0.7, z = wz })
  if body == nil then return false end
  SetScale(body, 2.0, 0.8, 3.6)
  SetRotationY(body, (rv.yaw or 0) + side * 0.4)
  local lamp = SpawnPrefab("rv_flame", { x = wx, y = gy + 0.95, z = wz })
  if lamp ~= nil then SetScale(lamp, 0.18, 0.18, 0.18) end
  strandedCar = { x = wx, z = wz, body = body, lamp = lamp, looted = false }
  toast.text = "路边有辆抛锚车——停车看看（E 搜刮）"
  toast.t = 3.4
  return true
end

function despawnStrandedCar()
  if strandedCar == nil then return end
  if strandedCar.body ~= nil then Despawn(strandedCar.body) end
  if strandedCar.lamp ~= nil then Despawn(strandedCar.lamp) end
  strandedCar = nil
end

-- 地图上要标出的路边事件（搭车客 / 抛锚车）
function roadEventMarkers()
  local out = {}
  if hitchhiker ~= nil then
    out[#out + 1] = { x = hitchhiker.x, z = hitchhiker.z, kind = "hitchhiker" }
  end
  if strandedCar ~= nil and not strandedCar.looted then
    out[#out + 1] = { x = strandedCar.x, z = strandedCar.z, kind = "stranded" }
  end
  return out
end

-- 路牌笔记：在各地 POI 附近放一张可阅读的纸片（已读的隐藏）
function spawnLoreNotes()
  for _, n in ipairs(loreNotes or {}) do Despawn(n.ent) end
  loreNotes = {}
  for i, def in ipairs(LORE_NOTES or {}) do
    local nd = nodeById(def.node)
    if nd ~= nil then
      local x = nd.x + 1.4 + (i % 2) * 0.9
      local z = nd.z + 1.1 - (i % 3) * 0.6
      local y = (terrainHeight and terrainHeight(x, z)) or 0
      local e = SpawnPrefab("rv_cargo_med", { x = x, y = y + 0.05, z = z })
      if e ~= nil then
        SetScale(e, 0.24, 0.03, 0.32)
        SetRotationY(e, i * 0.6)
        loreNotes[#loreNotes + 1] = {
          ent = e, x = x, z = z, id = def.id, title = def.title,
          node = def.node }
      end
    end
  end
  if refreshLoreNotes ~= nil then refreshLoreNotes() end
end

function refreshLoreNotes()
  for _, n in ipairs(loreNotes or {}) do
    if n.ent ~= nil then
      SetVisible(n.ent, not notesFound[n.id])
    end
  end
end

-- 按位置找最近的未读笔记（联机/交互共用）
function nearestLoreNote(x, z, maxDist)
  local best, bestD = nil, maxDist or 2.4
  for i, n in ipairs(loreNotes or {}) do
    if not notesFound[n.id] then
      local dx, dz = n.x - (x or 0), n.z - (z or 0)
      local d = math.sqrt(dx * dx + dz * dz)
      if d < bestD then best, bestD = i, d end
    end
  end
  return best
end

-- 未读笔记是否标在地图上：需要该地点已经探索过
function loreNoteMapVisible(note)
  if note == nil or notesFound[note.id] then return false end
  return visitedPois ~= nil and visitedPois[note.node] == true
end

function updateCampfireAudio(dt)
  if campfire == nil then return end
  if not campfireOn then return end
  local lx, lz = rv.x, rv.z
  if onFoot and player ~= nil and player.active then
    lx, lz = player.x, player.z
  end
  local dx, dz = campfire.x - lx, campfire.z - lz
  local dist = math.sqrt(dx * dx + dz * dz)
  if not campfireAudible(dist) then return end
  fireCrackleT = (fireCrackleT or 0) - (dt or 0)
  if fireCrackleT > 0 then return end
  fireCrackleT = campfireCrackleInterval()
  fireCracklePlays = (fireCracklePlays or 0) + 1
  if PlaySfx3D ~= nil then
    PlaySfx3D("fire", { x = campfire.x, y = (campfire.y or 0) + 0.5,
                        z = campfire.z })
  elseif PlaySfx ~= nil then
    PlaySfx("fire")
  end
end

-- 营火炊烟：持续上升的灰白烟柱，远处也能看见营地
function campfireSmokeRate()
  return (campfire ~= nil and campfireOn) and 4 or 0
end

function updateCampfireSmoke(dt)
  local rate = campfireSmokeRate()
  if rate <= 0 then campfireSmokeT = 0 return end
  campfireSmokeT = (campfireSmokeT or 0) + (dt or 0) * rate
  while campfireSmokeT >= 1 do
    campfireSmokeT = campfireSmokeT - 1
    campfireSmokeCount = (campfireSmokeCount or 0) + 1
    local wx = campfire.x + (math.random() * 2 - 1) * 0.25
    local wz = campfire.z + (math.random() * 2 - 1) * 0.25
    EmitParticles({
      pos = { x = wx, y = (campfire.y or 0) + 0.9, z = wz },
      count = 1, speedMin = 0.15, speedMax = 0.35,
      vel = { x = 0.12, y = 0.55, z = 0.05 },
      lifeMin = 3.0, lifeMax = 5.0,
      sizeStart = 0.18, sizeEnd = 1.1,
      color = { r = 0.55, g = 0.57, b = 0.58, a = 0.22 },
      colorEnd = { r = 0.55, g = 0.57, b = 0.58, a = 0.0 },
      additive = false, gravity = -0.02,
    })
  end
end

-- 篝火火星：燃烧时持续升起橙色小火星（与炊烟/火光呼应）
function campfireEmberRate()
  return (campfire ~= nil and campfireOn) and 6 or 0
end

function updateCampfireEmbers(dt)
  local rate = campfireEmberRate()
  if rate <= 0 then emberT = 0 return end
  emberT = (emberT or 0) + (dt or 0) * rate
  while emberT >= 1 do
    emberT = emberT - 1
    emberCount = (emberCount or 0) + 1
    EmitParticles({
      pos = { x = campfire.x + (math.random() * 2 - 1) * 0.28,
              y = (campfire.y or 0) + 0.55,
              z = campfire.z + (math.random() * 2 - 1) * 0.28 },
      count = 1, speedMin = 0.2, speedMax = 0.6,
      vel = { x = 0.05, y = 0.9, z = 0.02 },
      lifeMin = 0.8, lifeMax = 1.6,
      sizeStart = 0.05, sizeEnd = 0.01,
      color = { r = 1.0, g = 0.58, b = 0.18, a = 0.9 },
      colorEnd = { r = 1.0, g = 0.30, b = 0.05, a = 0.0 },
      additive = true, gravity = -0.05,
    })
  end
end

-- 统一设置篝火剩余燃料（联机客机按服务器值同步）
function setCampfireFuel(v)
  campfireFuelMin = math.max(0, tonumber(v) or 0)
end

-- 统一设置篝火点燃/熄灭（单机、服务器与客机同步共用）
function setCampfireLit(on)
  campfireOn = on and true or false
  for _, f in ipairs(campfireFlames or {}) do SetVisible(f.ent, campfireOn) end
  if campfireLight ~= nil then SetVisible(campfireLight, campfireOn) end
end

-- 雨雪天篝火消耗更快（雨水降温/雪天更冷）
function campfireFuelScale(weatherValue)
  if weatherValue == "rain" then return 1.4 end
  if weatherValue == "snow" then return 1.6 end
  return 1.0
end

-- 营火燃料：燃烧时按游戏分钟消耗；烧尽后火苗/暖光熄灭
function updateCampfireFuel(gm)
  if campfire == nil or not campfireOn then return end
  -- 联机客机的燃料由服务器权威推进，本地不消耗
  if coopRole == "client" then return end
  local scale = campfireFuelScale(weather)
  campfireFuelMin = math.max(0, (campfireFuelMin or 0) -
                                (gm or 0) * scale)
  if campfireFuelMin <= 0 then
    campfireFuelMin = 0
    setCampfireLit(false)
    toast.text = "篝火烧尽了——靠近按 G 添柴"
    toast.t = 3.2
    if addJournal ~= nil then addJournal("营火熄灭了", "note") end
  end
end

-- 在指定位置尝试添柴：消耗 1 木材、补充 8 游戏小时燃料并重新点燃（不负责提示）
function campfireAddWoodAt(px, pz)
  if campfire == nil then return false end
  local dx, dz = campfire.x - (px or 0), campfire.z - (pz or 0)
  if dx * dx + dz * dz > 3.6 * 3.6 then return false end
  if (materials.wood or 0) < 1 then return false end
  materials.wood = materials.wood - 1
  campfireFuelMin = math.min(campfireWoodMax or 2160,
                             (campfireFuelMin or 0) + 480)
  if not campfireOn then setCampfireLit(true) end
  return true
end

-- 添柴：消耗 1 木材，补充 8 游戏小时燃料并重新点燃
function addCampfireWood()
  if campfire == nil then return false end
  local lx, lz = rv.x, rv.z
  if onFoot and player ~= nil and player.active then
    lx, lz = player.x, player.z
  end
  local dx, dz = campfire.x - lx, campfire.z - lz
  if dx * dx + dz * dz > 3.6 * 3.6 then
    toast.text = "走到篝火旁才能添柴"
    toast.t = 2.0
    return false
  end
  if (materials.wood or 0) < 1 then
    toast.text = "没有木柴了——搜刮营地/农场可获得"
    toast.t = 2.4
    return false
  end
  if not campfireAddWoodAt(lx, lz) then return false end
  if PlaySfx then PlaySfx("pickup") end
  toast.text = string.format("添了一根木柴（燃料约 %.0f 游戏分钟）",
                             campfireFuelMin)
  toast.t = 2.6
  return true
end

-- 篝火燃料提示：靠近营地火堆时，低燃料/熄灭给一条 HUD 提示
function campfireHintText()
  if campfire == nil then return nil end
  local lx, lz = rv.x, rv.z
  if onFoot and player ~= nil and player.active then
    lx, lz = player.x, player.z
  end
  local dx, dz = campfire.x - lx, campfire.z - lz
  if dx * dx + dz * dz > 8 * 8 then return nil end
  if not campfireOn then
    return "篝火已熄灭——按 G 添柴（木材 x1）"
  end
  if (campfireFuelMin or 0) < 120 then
    return string.format("篝火燃料将尽（约 %.0f 游戏分钟）——按 G 添柴",
                         campfireFuelMin or 0)
  end
  return nil
end

-- 营火暖光的轻微跳动：没有灯光强度 API，用位置抖动 + 高度起伏模拟火光
function updateCampfireLight(dt)
  if SURVIVAL ~= nil then
    updateCampfireFuel((dt or 0) * (SURVIVAL.timeScale or 1))
  end
  if campfireLight == nil or campfire == nil then return end
  if not campfireOn then
    SetVisible(campfireLight, false)
    return
  end
  SetVisible(campfireLight, true)
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
