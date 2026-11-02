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
      if not blocked and distToRoadSq(wx, wz) < 36 then blocked = true end  -- 公路 6m
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
  if nd.id == "ruin" or nd.id == "gasstop" or nd.id == "depot" then
    print(string.format("[poi] spawn shell node=%s center=(%.1f,%.1f)",
          nd.id, cx, cz))
  end
  if isWarehouse then
    -- 可进入物流仓库：高棚 + 装卸门/月台 + 内部货架/托盘 + 顶部高窗
    local wh = SpawnPrefab("rv_warehouse", { x = cx, y = 0, z = cz })
    if wh ~= nil then SetRotationY(wh, 0) end
    for _, lx in ipairs({ -1.70, 1.70 }) do
      local lamp = SpawnPrefab("rv_lamp", { x = cx + lx, y = 3.15, z = cz })
      if lamp ~= nil then SetScale(lamp, 1.5, 1.5, 1.5) end
    end
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

  if nm:find("农场") then
    -- 红顶谷仓
    local bx, bz = nd.x - 5.0, nd.z + 6.5
    cube("rv_panel_light", bx, 1.5, bz, 6.0, 3.0, 4.6)
    cube("rv_light_red", bx, 3.15, bz, 6.4, 0.3, 5.0)
    cube("rv_panel_dark", bx, 1.0, bz + 2.35, 1.8, 2.0, 0.08)
  elseif nm:find("商场") then
    -- 更大的废弃商铺
    local mx, mz = nd.x - 6.0, nd.z + 6.5
    cube("rv_panel_light", mx, 2.0, mz, 8.0, 4.0, 6.0)
    cube("rv_panel_dark", mx, 4.1, mz, 8.3, 0.2, 6.3)
    cube("rv_panel_window", mx, 2.4, mz + 3.05, 3.2, 1.6, 0.06)
  end
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
