-- lc_scavenge.lua
-- 停靠搜刮：物资箱/战利品 Roll/搜获袋/收工入库

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

function despawnCrates()
  for _, c in ipairs(crates) do Despawn(c.ent) end
  crates = {}
end

function spawnCrates()
  despawnCrates()
  for i = 1, STOP.crates do
    local ent = SpawnPrefab("rv_loot_crate", { x = 0, y = -50, z = 0 })
    if ent ~= nil then
      local cx, cz
      for _ = 1, 12 do
        local a = math.random() * math.pi * 2
        local r = 4.0 + math.random() * 5.0
        local wx, wz = rotOf(math.sin(a) * r, math.cos(a) * r)
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
      if cx == nil then cx, cz = rotOf(3.6, -2.0 + 1.4 * (i - 1)) end
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
  local parts = {}
  for mat, n in pairs(loot) do
    stopBag[mat] = (stopBag[mat] or 0) + n
    parts[#parts + 1] = MAT_NAMES[mat] .. "+" .. n
  end
  SpawnFloatText(c.x, 0.9, c.z, table.concat(parts, "  "),
                 false, 1.2, 1.0, 0.85, 0.4)
  Despawn(c.ent)
  table.remove(crates, idx)
  stopThreat = math.min(STOP.threatMax,
                        stopThreat + STOP.searchThreat + (nd.danger or 0))
  if stopThreat >= STOP.threatMax and evacT < 0 then
    -- 威胁满：撤离倒计时 + 夜袭子图接管（stop_cycle 数据驱动）
    LC.flow.emit("threat_full")
  end
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
  -- 收工全流程交给 stop_cycle 图（入库/清场/天数/提示），keep 折扣随行
  g_bankKeep = keepRatio
  LC.flow.emit("raid_end")
  LC.flow.call("stop_cycle", "leave")
end
