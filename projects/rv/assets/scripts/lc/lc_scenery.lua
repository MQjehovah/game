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

local function distToRoadSq(x, z)
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
        local y = 0
        if p.kind == "scrub" then
          y = 0.015
          SetScale(p.ent, 0.5 + math.random(), 0.03 + math.random() * 0.04, 0.5 + math.random())
        else
          SetScale(p.ent, s, s, s)
          if p.kind == "rock" then y = s * 0.55 end
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

local HX = { 5.0, 8.0, 11.5, 15.0, 19.0, 23.5, 28.0, 33.0, 38.0 }
local HY = { 0.0, 0.70, 1.90, 3.00, 3.50, 3.55, 3.00, 1.50, 0.0 }

local function hillBase(x)
  for i = 1, #HX - 1 do
    if x >= HX[i] and x <= HX[i + 1] then
      local t = (x - HX[i]) / (HX[i + 1] - HX[i])
      return HY[i] + (HY[i + 1] - HY[i]) * t
    end
  end
  return 0.0
end

local function hillHeight(x, z)
  local y = hillBase(x) + 0.28 * math.sin(z * 0.23 + x * 0.35) +
            0.12 * math.sin(z * 0.61)
  local taper = math.min(1.0,
                         math.max(0.0, (z + 24.0) / 6.0),
                         math.max(0.0, (46.0 - z) / 6.0))
  return y * taper
end

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
      local y = hillHeight(x, z)
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

-- Gentle wind: rotate grass/flower clumps around their base. Blades are
-- authored with a common lean direction, so yaw sway reads as leaning.
-- Updates run at 20 Hz and walk one of four buckets per tick, keeping the
-- per-frame SetRotationY count low with ~2000 plants.
local windT = 0.0
local windAcc = 0.0
local windBucket = 0
local WIND_BUCKETS = 4
function updateMeadowWind(dt)
  windT = windT + dt
  windAcc = windAcc + dt
  if windAcc < 0.05 then return end
  windAcc = 0
  windBucket = (windBucket % WIND_BUCKETS) + 1
  local cx, cz = rv.x, rv.z
  if player ~= nil and player.active and player.x ~= nil then
    cx, cz = player.x, player.z
  end
  local limit = 42 * 42
  local n = #scenery
  for i = windBucket, n, WIND_BUCKETS do
    local p = scenery[i]
    if p.meadow and p.baseYaw then
      local dx, dz = p.x - cx, p.z - cz
      if dx * dx + dz * dz < limit then
        local amp = (p.kind == "grass") and 0.20 or 0.10
        local s = math.sin(windT * 1.25 + p.phase) +
                  0.35 * math.sin(windT * 2.7 + p.phase * 2.1)
        SetRotationY(p.ent, p.baseYaw + s * amp)
      end
    end
  end
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
  local y = 0.08 + (roadLinkSeq % 6) * 0.0015
  local yaw = dirYaw(dx, dz)
  local ux, uz = dx / len, dz / len
  local count = math.max(1, math.ceil(len / 10))
  for i = 0, count - 1 do
    local d = math.min(len, i * 10 + 5)
    local ent = SpawnPrefab("rv_road_seg",
                            { x = ax + ux * d, y = y, z = az + uz * d })
    if ent ~= nil then
      SetRotationY(ent, yaw)
      roadSegments[#roadSegments + 1] = { ent = ent }
    end
  end
  for d = 6, len - 2, 6 do
    local ent = SpawnPrefab("rv_road_dash",
                            { x = ax + ux * d, y = y + 0.04, z = az + uz * d })
    if ent ~= nil then
      SetRotationY(ent, yaw)
      roadDashes[#roadDashes + 1] = { ent = ent }
    end
  end
end

-- 公路电线杆（纯装饰，强化公路感）
function spawnPolesAlong(ax, az, bx, bz, side)
  local dx, dz = bx - ax, bz - az
  local len = math.sqrt(dx * dx + dz * dz)
  if len < 20 then return end
  local ux, uz = dx / len, dz / len
  local rx, rz = uz * (side or 1), -ux * (side or 1)
  local yaw = dirYaw(dx, dz)
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
  end
end

function spawnRoadsidePoles()
  spawnPolesAlong(0, -220, 0, 0, 1)
  spawnPolesAlong(0, 60, 0, 220, -1)
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
  for _, nd in ipairs(NODES) do
    for _, lid in ipairs(nd.links or {}) do
      local o = nodeById(lid)
      if o then spawnRoadBetween(nd.x, nd.z, o.x, o.z) end
    end
  end
  -- 加油站向北延伸的主干道
  spawnRoadBetween(0, 60, 0, 220)
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

-- Stress-driven impact fracture.  Each break picks a seeded impact point inside
-- the pane, then grows 4..6 radial crack trajectories outward in four
-- concentric bands.  Every band the crack angle wanders a little (seeded) and
-- some nodes spawn a shorter branch crack, so the pane is split along a
-- branching radial network.  Cells between adjacent cracks and neighbouring
-- bands become shards: rings are closer near the impact point, so fragments
-- are smaller and more numerous where stress is highest.  The 12-variant mesh
-- pool supplies the prism shape; scale, yaw and velocity come from the cell.
-- The server seed drives the LCG, so every peer rebuilds the identical network.
local GLASS_VARIANTS = 12
local GLASS_BANDS = 4
local GLASS_RADII = { 0.02, 0.15, 0.33, 0.58, 0.85 }

-- Deterministic LCG (Numerical Recipes constants, modulus 2^31) so the
-- server's seed maps to the same cells on every peer. Operands stay below
-- 2^53, so the double arithmetic is exact.
local function glassRand(seed, k)
  local x = (seed + k * 1013904223 + 1664525) % 2147483648
  x = (x * 1664525 + 1013904223) % 2147483648
  return x / 2147483648
end

-- Sutherland-Hodgman clip against the pane rectangle [-0.5, 0.5]^2.
local function clipGlassRect(poly)
  local out = poly
  for bi = 1, 4 do
    local nx, ny, d
    if bi == 1 then nx, ny, d = 1, 0, 0.5
    elseif bi == 2 then nx, ny, d = -1, 0, 0.5
    elseif bi == 3 then nx, ny, d = 0, 1, 0.5
    else nx, ny, d = 0, -1, 0.5 end
    local input = out
    out = {}
    local n = #input
    if n == 0 then break end
    local px, py = input[n][1], input[n][2]
    local pd = nx * px + ny * py - d
    for i = 1, n do
      local cx, cy = input[i][1], input[i][2]
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
  end
  return out
end

-- Radial crack network -> shard cells (normalised pane space).
local function crackCells(seed)
  local iu = (glassRand(seed, 11) - 0.5) * 0.42
  local iv = (glassRand(seed, 23) - 0.5) * 0.34
  local nMain = 4 + math.floor(glassRand(seed, 3) * 3)   -- 4..6 main cracks
  local mainA = {}
  for i = 0, nMain - 1 do
    mainA[i] = (i / nMain) * math.pi * 2 +
               (glassRand(seed, 40 + i) - 0.5) * 0.5
  end
  local cells = {}
  local cracks = 0
  local gap = math.pi * 2 / nMain
  for b = 1, GLASS_BANDS do
    local r0, r1 = GLASS_RADII[b], GLASS_RADII[b + 1]
    local bounds = {}
    for i = 0, nMain - 1 do
      -- The crack keeps wandering as it grows outward.
      mainA[i] = mainA[i] + (glassRand(seed, 90 + b * 31 + i) - 0.5) * 0.22
      bounds[#bounds + 1] = mainA[i]
      cracks = cracks + 1
      -- Branch cracks split off in the high-stress inner bands.
      if b < GLASS_BANDS and glassRand(seed, 500 + b * 19 + i) < 0.55 then
        bounds[#bounds + 1] = mainA[i] +
          gap * (0.32 + glassRand(seed, 700 + b * 23 + i) * 0.36)
        cracks = cracks + 1
      end
    end
    table.sort(bounds)
    local n = #bounds
    for i = 1, n do
      local a0 = bounds[i]
      local a1 = bounds[i % n + 1]
      if i == n then a1 = a1 + math.pi * 2 end
      local steps = 3
      local poly = {}
      for s = 0, steps do
        local a = a0 + (a1 - a0) * (s / steps)
        poly[#poly + 1] = { iu + math.cos(a) * r0, iv + math.sin(a) * r0 }
      end
      for s = steps, 0, -1 do
        local a = a0 + (a1 - a0) * (s / steps)
        poly[#poly + 1] = { iu + math.cos(a) * r1, iv + math.sin(a) * r1 }
      end
      poly = clipGlassRect(poly)
      if #poly >= 3 then
        local minU, maxU, minV, maxV = 1e9, -1e9, 1e9, -1e9
        local cu, cv = 0.0, 0.0
        for _, p in ipairs(poly) do
          minU = math.min(minU, p[1]); maxU = math.max(maxU, p[1])
          minV = math.min(minV, p[2]); maxV = math.max(maxV, p[2])
          cu = cu + p[1]; cv = cv + p[2]
        end
        local w, h = maxU - minU, maxV - minV
        if w > 0.045 and h > 0.045 then
          cells[#cells + 1] = {
            cu = cu / #poly, cv = cv / #poly,
            w = w, h = h, angle = (a0 + a1) * 0.5,
          }
        end
      end
    end
  end
  return cells, iu, iv, cracks
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
  local totalCracks = 0
  for pi, pane in ipairs(panes) do
    Despawn(pane.ent)
    -- Pane normal (local +Z rotated by yaw) and its in-plane tangent.
    local nx, nz = math.sin(pane.rot), math.cos(pane.rot)
    local tx, tz = math.cos(pane.rot), -math.sin(pane.rot)
    local ps = seed + pi * 977
    local cells, iu, iv, cracks = crackCells(ps)
    totalCracks = totalCracks + cracks
    for ci, cell in ipairs(cells) do
      local du, dv = cell.cu - iu, cell.cv - iv
      local dist = math.sqrt(du * du + dv * dv)
      local stress = 1 / (1 + dist * 3.2)
      local variant = 1 + math.floor(glassRand(ps, 300 + ci) * GLASS_VARIANTS)
      if variant > GLASS_VARIANTS then variant = GLASS_VARIANTS end
      local ox, oy = cell.cu * pane.sx, cell.cv * pane.sy
      local sx = pane.x + tx * ox + nx * 0.05
      local sy = pane.y + oy
      local sz = pane.z + tz * ox + nz * 0.05
      local e = SpawnPrefab("rv_glass_frag" .. variant, { x = sx, y = sy, z = sz })
      if e ~= nil then
        SetScale(e, cell.w * pane.sx, 1.0, cell.h * pane.sy)
        -- Align the shard with the radial crack direction.
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
  print(string.format(
    "[glass] broke windows at %s (%s, %d stress shards, %d cracks, seed=%d)",
    nd.id, reason or "enter", made, totalCracks, seed))
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
      ids[#ids + 1] = nd.id .. ":" .. tostring(g.seed or 0)
    end
  end
  return table.concat(ids, ",")
end

-- Apply a comma-separated "id:seed" list broadcast by the server.
function applyPoiGlassState(csv)
  if csv == nil or csv == "" then return 0 end
  local n = 0
  for item in string.gmatch(tostring(csv), "[^,]+") do
    local id, seed = string.match(item, "^(.-):(%d+)$")
    if id == nil then id, seed = item, "0" end
    local nd = nodeById(id)
    if nd ~= nil and breakPoiWindows(nd, "server", tonumber(seed)) then
      n = n + 1
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
function spawnRuinFor(nd)
  if nd.ruinSpawned then return end
  nd.ruinSpawned = true
  local cx, cz = nd.x + 6.5, nd.z + 6.5
  local nm = nd.name or ""
  local isStore = (nd.id == "ruin") or (nm:find("废宅") ~= nil)
  local isGas = (nd.id == "gasstop") or (nm:find("加油站") ~= nil)
  local isWarehouse = (nd.id == "depot") or (nm:find("仓库") ~= nil) or
                      (nm:find("物流") ~= nil)
  local isFarm = (nd.id == "farm") or (nm:find("农场") ~= nil)
  local isMall = (nd.id == "mall") or (nm:find("商场") ~= nil)
  local isCabin = (nd.id == "lakecamp") or (nm:find("营地") ~= nil)
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
  local gas = nodeById("gasstop")
  if gas then
    for _, off in ipairs({ {2.0, 1.0}, {2.6, -0.4}, {1.2, -0.6} }) do
      local ent = SpawnPrefab("rv_fuelcan",
                              { x = gas.x + off[1], y = 0.35, z = gas.z + off[2] })
      if ent ~= nil then SetScale(ent, 0.5, 0.7, 0.5) end
    end
  end
end
