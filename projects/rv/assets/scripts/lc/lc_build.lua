-- lc_build.lua
-- 建造系统：走道连通性 / 放置 / 拆除 / 属性统计

function computeReachability()
  local reach = {}
  local queue = {}
  for cx = 0, GW - 1 do
    local idx = cellIndex(cx, GH - 1)
    if occupancy[idx] == nil then
      reach[idx] = true
      queue[#queue + 1] = { cx = cx, cz = GH - 1 }
    end
  end
  local head = 1
  while queue[head] do
    local c = queue[head]
    head = head + 1
    for _, dv in ipairs({ {1,0}, {-1,0}, {0,1}, {0,-1} }) do
      local nx, nz = c.cx + dv[1], c.cz + dv[2]
      if nx >= 0 and nx < GW and nz >= 0 and nz < GH then
        local idx = cellIndex(nx, nz)
        if occupancy[idx] == nil and not reach[idx] then
          reach[idx] = true
          queue[#queue + 1] = { cx = nx, cz = nz }
        end
      end
    end
  end
  unreachableNames = {}
  for _, inst in pairs(instances) do
    if inst.free then
      inst.reachable = true
    elseif inst.cat.layer == "roof" then
      inst.reachable = true
    else
      local cells = cellsOf(inst.cat, inst.cx, inst.cz, inst.rot)
      inst.reachable = false
      if cells then
        for _, c in ipairs(cells) do
          for _, dv in ipairs({ {1,0}, {-1,0}, {0,1}, {0,-1} }) do
            local nx, nz = c.x + dv[1], c.z + dv[2]
            if nx >= 0 and nx < GW and nz >= 0 and nz < GH then
              if reach[cellIndex(nx, nz)] then inst.reachable = true end
            end
          end
        end
      end
      if not inst.reachable then
        unreachableNames[#unreachableNames + 1] = inst.cat.name
      end
    end
  end
  table.sort(unreachableNames)
end

-- 放置 / 拆除 / 统计
function spawnModule(cat, cx, cz, r)
  local ent = SpawnPrefab("rv_" .. cat.id, { x = 0, y = 0, z = 0 })
  if ent == nil then return nil end
  local w, h = footprint(cat, r)
  local baseY = (cat.layer == "roof") and ROOF_Y or FLOOR_Y
  local ox = ORIGIN_X + cx * CELL + w * CELL * 0.5
  local oz = ORIGIN_Z + cz * CELL + h * CELL * 0.5
  local ly = cat.modelScale and (baseY + 0.01) or (baseY + cat.mh * 0.5 + 0.005)
  local wx, wz = rotOf(ox, oz)
  if cat.modelScale then
    SetPosition(ent, { x = wx, y = ly, z = wz })
    local sc = cat.modelScale
    SetScale(ent, sc, sc, sc)
  else
    SetPosition(ent, { x = wx, y = ly, z = wz })
    SetScale(ent, w * CELL - 0.03, cat.mh, h * CELL - 0.03)
  end
  local lrot = (r % 2 == 1) and (math.pi * 0.5) or 0
  SetRotationY(ent, rv.yaw + lrot)
  local sx, sy, sz
  if cat.modelScale then
    sx, sy, sz = cat.modelScale, cat.modelScale, cat.modelScale
  else
    sx, sy, sz = w * CELL - 0.03, cat.mh, h * CELL - 0.03
  end
  Tween(ent, 2, { x = sx * 0.6, y = sy * 0.6, z = sz * 0.6 },
              { x = sx, y = sy, z = sz }, 0.18, 1)
  return ent, ox, ly, oz, lrot
end

function place(cat, cx, cz, r, free)
  if cat.layer == "roof" and curLayer ~= "roof" then return false end
  if cat.layer ~= "roof" and curLayer == "roof" then return false end
  if not fits(cat, cx, cz, r) then return false end
  if not free and not canAfford(cat.cost or {}) then return false end
  local ent, ox, ly, oz, lrot = spawnModule(cat, cx, cz, r)
  if ent == nil then return false end
  if not free then payCost(cat.cost or {}) end
  local id = nextId
  nextId = nextId + 1
  local w, h = footprint(cat, r)
  local occ = occGrid(cat.layer)
  for dz = 0, h - 1 do
    for dx = 0, w - 1 do
      occ[cellIndex(cx + dx, cz + dz)] = id
    end
  end
  instances[id] = { id = id, cat = cat, ent = ent, cx = cx, cz = cz, rot = r,
                    layer = cat.layer, lx = ox, ly = ly, lz = oz, lrot = lrot }
  lastPlacedInst = instances[id]
  computeReachability()
  return true
end

function removeAt(cx, cz)
  local inst = findInstanceAt(cx, cz)
  if not inst then
    local occ = roofOcc
    for idx, id in pairs(occ) do
      local cand = instances[id]
      if cand then
        local w, h = footprint(cand.cat, cand.rot)
        for dz = 0, h - 1 do
          for dx = 0, w - 1 do
            if cand.cx + dx == cx and cand.cz + dz == cz then
              Despawn(cand.ent)
              for dz2 = 0, h - 1 do
                for dx2 = 0, w - 1 do
                  occ[cellIndex(cand.cx + dx2, cand.cz + dz2)] = nil
                end
              end
              instances[id] = nil
              refundCost(cand.cat.cost or {})
              computeReachability()
              return true
            end
          end
        end
      end
    end
    return false
  end
  local w, h = footprint(inst.cat, inst.rot)
  for dz = 0, h - 1 do
    for dx = 0, w - 1 do
      occupancy[cellIndex(inst.cx + dx, inst.cz + dz)] = nil
    end
  end
  Despawn(inst.ent)
  instances[inst.id] = nil
  refundCost(inst.cat.cost or {})
  computeReachability()
  return true
end

-- ===========================================================================
-- 自由摆放（第一人称，无网格）
-- ===========================================================================
function spawnModuleFree(cat, lx, lz, rotY)
  local ent = SpawnPrefab("rv_" .. cat.id, { x = 0, y = 0, z = 0 })
  if ent == nil then return nil end
  local baseY = (cat.layer == "roof") and ROOF_Y or FLOOR_Y
  local ly = cat.modelScale and (baseY + 0.01) or (baseY + cat.mh * 0.5 + 0.005)
  local wx, wz = rotOf(lx, lz)
  SetPosition(ent, { x = wx, y = ly, z = wz })
  SetRotationY(ent, rv.yaw + rotY)
  local sx, sy, sz
  if cat.modelScale then
    sx, sy, sz = cat.modelScale, cat.modelScale, cat.modelScale
  else
    sx, sy, sz = cat.w * CELL - 0.03, cat.mh, cat.h * CELL - 0.03
  end
  SetScale(ent, sx, sy, sz)
  Tween(ent, 2, { x = sx * 0.7, y = sy * 0.7, z = sz * 0.7 },
              { x = sx, y = sy, z = sz }, 0.16, 1)
  return ent, ly
end

function placeFree(cat, lx, lz, rotY, free)
  if cat.layer ~= curLayer then return false, "layer" end
  if not freeFits(cat, lx, lz, rotY) then return false, "overlap" end
  if not free and not canAfford(cat.cost or {}) then return false, "cost" end
  local ent, ly = spawnModuleFree(cat, lx, lz, rotY)
  if ent == nil then return false, "spawn" end
  if not free then payCost(cat.cost or {}) end
  local id = nextId
  nextId = nextId + 1
  instances[id] = { id = id, cat = cat, ent = ent, free = true, layer = cat.layer,
                    lx = lx, lz = lz, ly = ly, lrot = rotY }
  lastPlacedInst = instances[id]
  if PlaySfx then PlaySfx("click") end
  computeReachability()
  recomputeStats()
  return true
end

function removeInstance(inst)
  if inst == nil then return false end
  if not inst.free then
    local w, h = footprint(inst.cat, inst.rot)
    for dz = 0, h - 1 do
      for dx = 0, w - 1 do
        occupancy[cellIndex(inst.cx + dx, inst.cz + dz)] = nil
      end
    end
  end
  Despawn(inst.ent)
  for id, cand in pairs(instances) do
    if cand == inst then instances[id] = nil break end
  end
  refundCost(inst.cat.cost or {})
  if PlaySfx then PlaySfx("click") end
  computeReachability()
  recomputeStats()
  return true
end

-- 准星指向的家具（自由摆放拆除用）
function findAimedInstance(maxDist)
  local fx, fz = math.sin(player.yaw), math.cos(player.yaw)
  local best, bestScore = nil, -1
  for _, inst in pairs(instances) do
    local wx, wz = rotOf(inst.lx, inst.lz)
    local dx, dz = wx - player.x, wz - player.z
    local d = math.sqrt(dx * dx + dz * dz)
    if d < (maxDist or 4.5) then
      local dot = (dx * fx + dz * fz) / math.max(d, 0.001)
      local score = dot - d * 0.05
      if dot > 0.40 and score > bestScore then
        best, bestScore = inst, score
      end
    end
  end
  return best
end

function recomputeStats()
  local s = { powerGen = 0, powerDraw = 0, battery = 0, waterStore = 0, waterDraw = 0,
              weight = 0, comfort = 0, count = 0, storage = 0 }
  for _, inst in pairs(instances) do
    local c = inst.cat
    local active = inst.on ~= false
    if active then
      local gen = c.powerGen or 0
      if c.id == "solar" then
        if solarFactor ~= nil then gen = gen * solarFactor() end
        if weather ~= nil then
          if weather == "rain" then gen = gen * 0.25
          elseif weather == "fog" then gen = gen * 0.5
          elseif weather == "cloudy" then gen = gen * 0.7 end
        end
      end
      s.powerGen   = s.powerGen   + gen
      s.powerDraw  = s.powerDraw  + (c.powerDraw  or 0)
    end
    s.battery    = s.battery    + (c.battery    or 0)
    s.waterStore = s.waterStore + (c.waterStore or 0)
    s.waterDraw  = s.waterDraw  + (c.waterDraw  or 0)
    s.weight     = s.weight     + (c.weight     or 0)
    s.storage    = s.storage    + (c.storage    or 0)
    s.comfort    = s.comfort    + ((c.comfort or 0) * (inst.reachable == false and 0 or 1))
    s.count      = s.count + 1
  end
  -- 顺路货运：货物重量直接压到整备质量上（超载 -> 加速/极速下降、油耗上升）
  if jobCargoWeight ~= nil then s.weight = s.weight + (jobCargoWeight() or 0) end
  stats = s
end
