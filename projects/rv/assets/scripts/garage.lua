-- ===========================================================================
-- Last Caravan 主脚本：阶段状态机 / 输入编排 / 生命周期
-- 功能模块在 scripts/lc/（加载顺序见 garage.json scripts.items）
-- ===========================================================================

function setPhase(m)
  if phase == m then return end
  local prev = phase
  phase = m
  if m == "parked" then
    rv.speed = 0
    if rvBody then PhysicsSetVelocity(rvBody, { x = 0, y = 0, z = 0 }) end
    if prev == "driving" then
      if nearNode then
        atNode = nearNode.id
        curNode = atNode
        stopBag, stopThreat, evacT = {}, 0, -1
        killCount, hordeT, fireT, turretT, raidT = 0, 0, 0, 0, 0
        spawnCrates()
        local nd = nodeById(atNode)
        local stars = string.rep("★", nd and nd.danger or 0)
        toast.text = "停靠 " .. nodeName(atNode)
                     .. (stars ~= "" and ("（危险 " .. stars .. "）") or "")
                     .. " — 左键搜箱  建造随时可用"
        if atNode == "gasstop" and fuel < VEH.tank - 0.5 then
          fuel = VEH.tank
          fuelWarned = false
          toast.text = toast.text .. " · ⛽ 已加满油"
        end
      else
        atNode = nil
        toast.text = "荒野停车 — 可改装"
      end
    end
    toast.t = 4.5
  else  -- driving
    if atNode then
      -- 开走 = 收工：物资入库 + 天数 +1
      despawnCrates()
      despawnHorde()
      day = day + 1
      local got = bankStopBag(1.0)
      stopThreat = 0
      evacT = -1
      atNode = nil
      toast.text = (got == "" and "启程" or "物资入库: " .. got)
                   .. " — 第 " .. day .. " 天"
    else
      toast.text = "上路！W/S 油门·刹车  A/D 转向  空格 停车  C 切换视角"
    end
    toast.t = 4.5
  end
  if ghostEnt ~= nil then SetVisible(ghostEnt, false) end
  if hoverDecalEnt ~= nil then SetVisible(hoverDecalEnt, false) end
  hover = nil
  if gridFloorEnt ~= nil then
    SetVisible(gridFloorEnt, (m == "parked") and gridVisible or false)
  end
  if gridRoofEnt ~= nil then
    SetVisible(gridRoofEnt, (m == "parked") and gridVisible and curLayer == "roof" or false)
  end
end
function saveLayout()
  local rows = {}
  for _, inst in pairs(instances) do
    rows[#rows + 1] = { id = inst.cat.id, cx = inst.cx, cz = inst.cz,
                        rot = inst.rot, layer = inst.layer }
  end
  local data = { cell = CELL, gw = GW, gh = GH, materials = materials,
                 rv = { x = rv.x, z = rv.z, yaw = rv.yaw, node = atNode or curNode },
                 vehicle = { fuel = fuel, dur = dur },
                 day = day, modules = rows }
  local ok = WriteText("saves/layout.json", jsonEncode(data))
  toast.text = ok and "布局已保存 (saves/layout.json)" or "保存失败"
  toast.t = 2.5
end

function clearAll()
  for _, inst in pairs(instances) do Despawn(inst.ent) end
  instances = {}
  occupancy = {}
  roofOcc = {}
end

function loadLayout()
  local text = ReadText("saves/layout.json")
  if not text or text == "" then
    toast.text = "没有存档 (saves/layout.json)"
    toast.t = 2.5
    return
  end
  local data = jsonDecode(text)
  if type(data) ~= "table" or type(data.modules) ~= "table" then
    toast.text = "存档格式无效"
    toast.t = 2.5
    return
  end
  if type(data.materials) == "table" then
    for k, v in pairs(data.materials) do
      materials[k] = math.max(materials[k] or 0, v or 0)
    end
  end
  clearAll()
  local byId = {}
  for _, cat in ipairs(CATALOG) do byId[cat.id] = cat end
  for _, row in ipairs(data.modules) do
    local cat = byId[row.id]
    if cat then
      local saved = curLayer
      curLayer = row.layer or cat.layer or "floor"
      place(cat, math.floor(row.cx), math.floor(row.cz), math.floor(row.rot or 0), true)
      curLayer = saved
    end
  end
  computeReachability()
  if type(data.rv) == "table" then
    rv.x = tonumber(data.rv.x) or 0
    rv.z = tonumber(data.rv.z) or 0
    rv.yaw = tonumber(data.rv.yaw) or 0
    curNode = tostring(data.rv.node or "camp")
    atNode = nil  -- 读档回到纯建造状态
    applyRvTransform()
    if rvBody then
      PhysicsSetPosition(rvBody, { x = rv.x, y = 0.9, z = rv.z })
      PhysicsSetVelocity(rvBody, { x = 0, y = 0, z = 0 })
    end
  end
  if type(data.vehicle) == "table" then
    fuel = math.min(VEH.tank, tonumber(data.vehicle.fuel) or VEH.tank)
    dur = math.min(VEH.durMax, tonumber(data.vehicle.dur) or VEH.durMax)
  end
  day = tonumber(data.day) or 1
  toast.text = "布局已读取"
  toast.t = 2.5
end
function setGhostColor(valid)
  if ghostEnt ~= nil then Despawn(ghostEnt) end
  ghostEnt = SpawnPrefab(valid and "rv_ghost_ok" or "rv_ghost_bad",
                         { x = 0, y = 0, z = 0 })
end

function ensureHelpers()
  if ghostEnt == nil then
    setGhostColor(true)
    SetVisible(ghostEnt, false)
  end
  if gridFloorEnt == nil then
    gridFloorEnt = SpawnDecal("assets/sprites/grid_4x14.png", 0, FLOOR_Y, 0,
                              1.0, 0.55, 1, 1, 1, true, 0.1)
    SetScale(gridFloorEnt, 2.0, 1.0, 7.0)
  end
  if gridRoofEnt == nil then
    gridRoofEnt = SpawnDecal("assets/sprites/grid_4x14.png", 0, ROOF_Y, 0,
                             1.0, 0.55, 1, 1, 1, true, 0.1)
    SetScale(gridRoofEnt, 2.0, 1.0, 7.0)
    SetVisible(gridRoofEnt, false)
  end
  if hoverDecalEnt == nil then
    hoverDecalEnt = SpawnDecal("assets/sprites/decal_disc.png", 0, FLOOR_Y, 0,
                               1.0, 0.45, 1, 0.95, 0.75, true, 0.6)
    SetVisible(hoverDecalEnt, false)
  end
end
function on_start()
  camEnt = FindNamedEntity("Main Camera")
  groundEnt = FindNamedEntity("Ground")
  collectRvParts()
  -- 合并 worldmap.json：名字/危险度/战利品表/链接
  do
    local text = ReadText("assets/data/worldmap.json")
    if text and text ~= "" then
      local data = jsonDecode(text)
      if type(data) == "table" and type(data.nodes) == "table" then
        for _, nd in ipairs(data.nodes) do
          local mine = nodeById(nd.id)
          if mine then
            mine.name = nd.name or mine.name
            mine.danger = nd.danger or 0
            mine.loot = nd.loot or {}
            mine.links = nd.links or {}
          end
        end
      end
    end
  end
  if groundEnt ~= nil then SetScale(groundEnt, 800, 0.1, 800) end
  spawnScenery()
  spawnMarkers()
  do  -- 验证钩子：scriptBaseDir 下放 autopilot_on.txt 即自动驾驶全循环
    local flag = ReadText("autopilot_on.txt")
    autoPilot = (flag ~= nil and flag ~= "")
  end
  ensureHelpers()
  -- 初始示例布局
  place(CATALOG[1], 1, 0, 0, true)
  place(CATALOG[3], 0, 4, 0, true)
  place(CATALOG[5], 0, 6, 0, true)
  place(CATALOG[9], 3, 6, 0, true)
  local saved = curLayer
  curLayer = "roof"
  place(CATALOG[17], 0, 8, 0, true)
  place(CATALOG[19], 2, 9, 0, true)
  curLayer = saved
  computeReachability()
  recomputeStats()
  local text = ReadText("saves/layout.json")
  if text and text ~= "" then loadLayout() end
  applyRvTransform()
  -- RV 动态刚体：质量随载重
  rvBody = PhysicsAddBox({ x = rv.x, y = 0.9, z = rv.z },
                         { x = 1.2, y = 0.9, z = 4.3 }, true,
                         { mass = math.max(800, 1200 + stats.weight * 0.8),
                           friction = 0.4, restitution = 0.05 })
  if autoPilot then setPhase("driving") end  -- 验证钩子：直接上路
end

function on_update(ent, dt)
  if toast.t > 0 then toast.t = toast.t - dt end
  ensureHelpers()
  if ActionPressed("m") then mapOpen = not mapOpen end
  if hitMarkT > 0 then hitMarkT = hitMarkT - dt end
  if recoilT > 0 then recoilT = recoilT - dt end
  updateGrenades(dt)

  -- ======================= 行驶 =======================
  if phase == "driving" then
    if ActionPressed("c") then
      driveView = (driveView == "chase") and "cab" or "chase"
    end
    if ActionPressed("space") then
      setPhase("parked")
    end
    updateDrive(dt)
    updateCamera(dt)
    return
  end

  -- ======================= 驻车（建造 + 停靠层 + 主角） =======================
  if ActionPressed("t") and not onFoot then setPhase("driving") end
  if ActionPressed("f") then
    if onFoot then
      local dx, dz = player.x - rv.x, player.z - rv.z
      if dx * dx + dz * dz < 12 then
        despawnPlayer()
        toast.text = "上车 — 可改装"
        toast.t = 2.5
      else
        toast.text = "走回到房车旁才能上车（F）"
        toast.t = 2.0
      end
    else
      spawnPlayer()
      if onFoot then
        toast.text = "下车 — W/S 行走  A/D 转向  F 上车"
        toast.t = 3.5
      end
    end
  end
  if ActionPressed("h") and dur < VEH.durMax and not onFoot then
    if canAfford(VEH.repairCost) then
      payCost(VEH.repairCost)
      dur = math.min(VEH.durMax, dur + VEH.repairAmt)
      recomputeStats()
      toast.text = string.format("修理车体 +%.0f（车况 %.0f%%）", VEH.repairAmt, dur)
    else
      toast.text = "材料不足: 金属x2 木材x1"
    end
    toast.t = 2.2
  end
  -- 停靠层（驻车在节点旁时）：撤离 / 夜袭 / 搜箱
  if atNode then
    if ActionPressed("e") and evacT < 0 then
      endScavenge(1.0)
    end
    if evacT >= 0 then
      evacT = evacT - dt
      if evacT <= 0 then
        toast.text = "尸群冲垮了停靠点！丢下部分物资…"
        toast.t = 3.0
        endScavenge(STOP.keepRatio)
      end
    end
    -- 夜袭期：尸群逼近 + 炮塔 + 左键射击（准星附近优先，不与建造抢点击）
    if evacT >= 0 then
      updateHorde(dt)
      turretFire(dt)
      fireT = fireT - dt
      if InputMouseDown(0) and fireT <= 0 and player.reloadT <= 0 then
        if player.mag <= 0 then
          player.reloadT = 1.6
        else
        local mp = InputMousePos()
        local nearest = false
        for _, z in ipairs(horde) do
          local sp = WorldToScreen(z.x, 1.0, z.z)
          if sp then
            local ddx, ddz = sp.x - mp.x, sp.y - mp.y
            if ddx * ddx + ddz * ddz < RAID.hitRadius * RAID.hitRadius then
              nearest = true
              break
            end
          end
        end
        if nearest then
          fireT = RAID.fireCd
          player.mag = player.mag - 1
          shootAt(mp.x, mp.y)
        end
        end
      end
    end
    -- 自动化钩子：搜箱 / 夜袭射击
    if autoPilot then
      if evacT < 0 then
        autoT = autoT + dt
        if autoT > 1.2 then
          autoT = 0
          if #crates > 0 then
            searchCrate(1)
          else
            autoPilot = false
            endScavenge(1.0)
          end
        end
      else
        autoT = autoT + dt
        if autoT > 0.35 then
          autoT = 0
          if #horde > 0 then
            local sp = WorldToScreen(horde[1].x, 1.0, horde[1].z)
            if sp then shootAt(sp.x, sp.y) end
          end
        end
      end
    end
  end
  updatePlayer(dt)
  updateCamera(dt)

  -- ---------- 建造交互（驻车时永远可用；下车也能造） ----------
  for i, key in ipairs(HOTKEYS) do
    if ActionPressed(key) then selected = i end
  end
  if ActionPressed("r") then rot = (rot + 1) % 2 end
  if ActionPressed("tab") then
    curLayer = (curLayer == "roof") and "floor" or "roof"
    SetVisible(gridFloorEnt, curLayer == "floor" and gridVisible or false)
    SetVisible(gridRoofEnt, curLayer == "roof" and gridVisible or false)
  end
  if ActionPressed("g") then
    gridVisible = not gridVisible
    SetVisible(gridFloorEnt, gridVisible)
    SetVisible(gridRoofEnt, gridVisible and curLayer == "roof")
  end
  if ActionPressed("f5") then saveLayout() end
  if ActionPressed("f9") then loadLayout() end

  local mp = InputMousePos()
  local hitSlot = hotbarHit(mp.x, mp.y)
  local overUi = hitSlot ~= nil
  local clickUsed = false

  -- 停靠层点击优先级：搜箱 -> 夜袭射击 -> 否则落入建造
  if atNode and InputMousePressed(0) then
    local best, bestD = nil, STOP.pickRadius * STOP.pickRadius
    for i, c in ipairs(crates) do
      local sp = WorldToScreen(c.x, 0.5, c.z)
      if sp then
        local ddx, ddz = sp.x - mp.x, sp.y - mp.y
        local d2 = ddx * ddx + ddz * ddz
        if d2 < bestD then best, bestD = i, d2 end
      end
    end
    if best then
      searchCrate(best)
      clickUsed = true
    elseif evacT >= 0 then
      for _, z in ipairs(horde) do
        local sp = WorldToScreen(z.x, 1.0, z.z)
        if sp then
          local ddx, ddz = sp.x - mp.x, sp.y - mp.y
          if ddx * ddx + ddz * ddz < RAID.hitRadius * RAID.hitRadius then
            if fireT <= 0 then
              fireT = RAID.fireCd
              shootAt(mp.x, mp.y)
            end
            clickUsed = true
            break
          end
        end
      end
    end
  end

  if not clickUsed then
    local pick = PickGround(mp)
    hover = nil
    if pick then
      local cx, cz = worldToCell(pick.x, pick.z)
      if cx then
        local inst = findInstanceAt(cx, cz)
        local cat = CATALOG[selected]
        local layerOk = (cat.layer == "roof") == (curLayer == "roof")
        local valid = layerOk and fits(cat, cx, cz, rot) and not overUi
        hover = { cx = cx, cz = cz, valid = valid, inst = inst, uiSlot = hitSlot }
        local w, h = footprint(cat, rot)
        local baseY = (curLayer == "roof") and ROOF_Y or FLOOR_Y
        local ox = ORIGIN_X + cx * CELL + w * CELL * 0.5
        local oz = ORIGIN_Z + cz * CELL + h * CELL * 0.5
        local gwx, gwz = rotOf(ox, oz)
        SetVisible(ghostEnt, true)
        SetPosition(ghostEnt, { x = gwx, y = baseY + 0.035, z = gwz })
        SetRotationY(ghostEnt, rv.yaw)
        SetScale(ghostEnt, w * CELL - 0.04, 0.06, h * CELL - 0.04)
        if ghostValidLast ~= valid then
          setGhostColor(valid)
          ghostValidLast = valid
        end
        SetVisible(hoverDecalEnt, inst ~= nil)
        if inst then
          local iw, ih = footprint(inst.cat, inst.rot)
          local baseYI = (inst.layer == "roof") and ROOF_Y or FLOOR_Y
          local hlx = ORIGIN_X + inst.cx * CELL + iw * CELL * 0.5
          local hlz = ORIGIN_Z + inst.cz * CELL + ih * CELL * 0.5
          local hwx, hwz = rotOf(hlx, hlz)
          SetPosition(hoverDecalEnt, { x = hwx, y = baseYI, z = hwz })
          SetRotationY(hoverDecalEnt, rv.yaw)
          SetScale(hoverDecalEnt, iw * CELL, 1.0, ih * CELL)
          SetDecal(hoverDecalEnt, 1.0, 0.45, inst.cat.mh + 0.25)
        end
      end
    end
    if hover == nil then
      SetVisible(ghostEnt, false)
      SetVisible(hoverDecalEnt, false)
    end

    if InputMousePressed(0) then
      if hitSlot then
        selected = hitSlot
      elseif hover and hover.valid then
        if place(CATALOG[selected], hover.cx, hover.cz, rot) then
          recomputeStats()
        end
      elseif hover and not hover.valid and not overUi then
        local cat = CATALOG[selected]
        if fits(cat, hover.cx, hover.cz, rot) and not canAfford(cat.cost or {}) then
          toast.text = "材料不足: 需要 " .. costText(cat.cost or {})
          toast.t = 1.6
        end
      end
    end
    if InputMousePressed(1) and not overUi and hover then
      if removeAt(hover.cx, hover.cz) then recomputeStats() end
    end
    if ActionPressed("x") and hover then
      if removeAt(hover.cx, hover.cz) then recomputeStats() end
    end
  else
    hover = nil
    SetVisible(ghostEnt, false)
    SetVisible(hoverDecalEnt, false)
  end
end

-- 2D HUD 必须在 on_render 里画：draw2d 上下文只在渲染期接线
function on_render()
  if phase == "driving" then
    drawDriveHud()
  else
    drawHud()
    if atNode then drawScavengeOverlay() end
  end
  if mapOpen then drawMapHud() end
end
