-- lc_scavenge.lua
-- 停靠搜刮：物资箱/材料与物品 Roll/搜获袋/收工入库

function nodeLootRoll(nd)
  local out = {}
  for mat, range in pairs(nd.loot or {}) do
    local lo, hi = range[1] or 0, range[2] or 0
    if hi > lo then
      local n = lo + math.random(0, hi - lo)
      if math.random() < 0.12 then n = n + hi end
      if n > 0 then out[mat] = (out[mat] or 0) + n end
    end
  end
  if not next(out) then out.cloth = 1 end
  return out
end

function rollLootItems(nd)
  local out = {}
  local rolls = 1 + ((math.random() < 0.35) and 1 or 0)
  for _ = 1, rolls do
    local total = 0
    for _, e in ipairs(LOOT_ITEMS) do total = total + e.w end
    local r = math.random() * total
    for _, e in ipairs(LOOT_ITEMS) do
      r = r - e.w
      if r <= 0 then
        local n = e.min + math.random(0, math.max(0, e.max - e.min))
        out[e.id] = (out[e.id] or 0) + n
        break
      end
    end
  end
  -- 危险地点更可能出修理包
  if (nd.danger or 0) >= 2 and math.random() < 0.18 then
    out.repairkit = (out.repairkit or 0) + 1
  end
  return out
end

function despawnCrates()
  for _, c in ipairs(crates) do Despawn(c.ent) end
  crates = {}
end

function spawnCrates()
  despawnCrates()
  local nd = atNode and nodeById(atNode) or nil
  local baseX, baseZ = rv.x, rv.z
  local rMin, rMax = 4.0, 9.0
  local interior = false
  -- 废宅/商店/加油站：物资箱刷在建筑内部，玩家必须从正门进入；其他地点保持屋外散落
  local isShop = nd ~= nil and (nd.id == "ruin" or nd.id == "gasstop" or
    nd.id == "depot" or nd.id == "farm" or nd.id == "mall" or nd.id == "lakecamp" or
    nd.id == "clinic" or
    (nd.name or ""):find("废宅") ~= nil or (nd.name or ""):find("加油站") ~= nil or
    (nd.name or ""):find("仓库") ~= nil or (nd.name or ""):find("物流") ~= nil or
    (nd.name or ""):find("农场") ~= nil or (nd.name or ""):find("商场") ~= nil or
    (nd.name or ""):find("营地") ~= nil or (nd.name or ""):find("诊所") ~= nil or
    (nd.name or ""):find("医院") ~= nil)
  if nd and atNode ~= "camp" and isShop then
    baseX, baseZ = poiCenter(nd)
    interior = true
  elseif nd and atNode ~= "camp" then
    baseX, baseZ = nd.x + 6.5, nd.z + 6.5
    rMin, rMax = 1.4, 3.2
  end
  local slots = {
    { -1.90, 1.40 }, { 0.40, 1.60 }, { 2.00, 1.20 },
    { -2.00, -0.80 }, { 0.20, -0.60 }, { 1.60, -0.20 },
  }
  for i = 1, STOP.crates do
    local ent = SpawnPrefab("rv_loot_crate", { x = 0, y = -50, z = 0 })
    if ent ~= nil then
      local cx, cz
      if interior then
        local slot = slots[(i - 1) % #slots + 1]
        cx, cz = baseX + slot[1], baseZ + slot[2]
      else
        for _ = 1, 14 do
          local a = math.random() * math.pi * 2
          local r = rMin + math.random() * (rMax - rMin)
          local wx = baseX + math.sin(a) * r
          local wz = baseZ + math.cos(a) * r
          local rdx, rdz = wx - rv.x, wz - rv.z
          if rdx * rdx + rdz * rdz >= 9 then
            local ok = true
            for _, c in ipairs(crates) do
              local ddx, ddz = wx - c.x, wz - c.z
              if ddx * ddx + ddz * ddz < 1.96 then ok = false break end
            end
            if ok then cx, cz = wx, wz break end
          end
        end
        if cx == nil then cx, cz = baseX + 1.2, baseZ + 1.2 end
      end
      SetPosition(ent, { x = cx, y = 0.28, z = cz })
      SetScale(ent, 0.55, 0.55, 0.55)
      SetRotationY(ent, math.random() * math.pi * 2)
      crates[#crates + 1] = { ent = ent, x = cx, z = cz }
    end
  end
end

function searchCrate(idx)
  local c = crates[idx]
  if c == nil then return end
  local nd = nodeById(atNode or curNode) or { loot = {}, danger = 0 }
  local loot = nodeLootRoll(nd)
  local items = rollLootItems(nd)
  local parts = {}
  for mat, n in pairs(loot) do
    stopBag[mat] = (stopBag[mat] or 0) + n
    parts[#parts + 1] = MAT_NAMES[mat] .. "+" .. n
  end
  for id, n in pairs(items) do
    addItem(id, n)
    parts[#parts + 1] = itemName(id) .. "+" .. n
  end
  SpawnFloatText(c.x, 0.9, c.z, table.concat(parts, "  "),
                 false, 1.4, 1.0, 0.85, 0.4)
  if PlaySfx then PlaySfx("pickup") end
  Despawn(c.ent)
  table.remove(crates, idx)

  -- 低强度威胁：夜里或危险地点，小概率惊动 1 只感染者
  local night = isNight() and 1 or 0
  local chance = 0.06 + (nd.danger or 0) * 0.05 + night * 0.12
  if math.random() < chance then spawnAmbientZombie() end
end

function bankStopBag(keepRatio)
  local got = {}
  for mat, n in pairs(stopBag) do
    local v = math.floor(n * (keepRatio or 1) + 0.5)
    if v > 0 then
      materials[mat] = (materials[mat] or 0) + v
      got[#got + 1] = MAT_NAMES[mat] .. "+" .. v
    end
  end
  stopBag = {}
  return table.concat(got, " ")
end

function endScavenge(keepRatio)
  g_bankKeep = keepRatio or 1.0
  LC.flow.emit("raid_end")
  LC.flow.call("stop_cycle", "leave")
end
