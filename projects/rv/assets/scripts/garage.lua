-- ===========================================================================
-- Last Caravan 主脚本（Nomad Drive 式第一人称生存改造）
-- 阶段状态机 / 输入编排 / 生命周期
-- 功能模块在 scripts/lc/（加载顺序见 garage.json scripts.items）
-- ===========================================================================

function setBuildMode(on)
  if buildMode == on then return end
  buildMode = on
  gridVisible = false
  if gridFloorEnt ~= nil then SetVisible(gridFloorEnt, false) end
  if gridRoofEnt ~= nil then SetVisible(gridRoofEnt, false) end
  if on then
    SetVar("cameraMouseLock", 1)
    placeRot = 0
    selected = 8
    fishingT = 0
    stationUI = nil  -- 默认选储物箱（1x1，车内最容易放下）
    toast.text = "自由摆放 — 准星对准地面，左键放置 / 右键拆除 / R 旋转"
    toast.t = 4.0
  else
    if ghostEnt ~= nil then SetVisible(ghostEnt, false) end
    if hoverDecalEnt ~= nil then SetVisible(hoverDecalEnt, false) end
    hover = nil
    placeValid = false
    toast.text = "回到探索 — E 交互 / T 驾驶"
    toast.t = 2.5
  end
end

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
        despawnHorde()
        -- 停靠流程交给 stop_cycle 图（生成物资箱/提示，可热调）
        LC.flow.emit("arrive")
        local nd = nodeById(atNode)
        local stars = string.rep("★", nd and nd.danger or 0)
        toast.text = "停靠 " .. nodeName(atNode)
                     .. (stars ~= "" and ("（危险 " .. stars .. "）") or "")
                     .. " — E 搜物资箱"
        if atNode == "gasstop" or atNode == "camp" then
          fuel = VEH.tank
          water = SURVIVAL.waterMax
          fuelWarned = false
          toast.text = toast.text .. " · ⛽ 油/水已补满"
        end
        -- 首次抵达的一次性补给（每个地点只发一次）
        local visitText = firstVisitBonus ~= nil and firstVisitBonus(nd) or nil
        if visitText ~= nil then
          toast.text = toast.text .. " · " .. visitText
          if PlaySfx then PlaySfx("pickup") end
        end
      else
        atNode = nil
        toast.text = "荒野停车 — E 交互 / B 改装 / T 继续上路"
      end
    end
    buildMode = false
    driveLookYaw, driveLookPitch = 0, 0
    if not onFoot then spawnPlayer() end
    setPlayerVisible(false)
    SetVar("cameraMouseLock", 1)
    if PlayMusic then PlayMusic("wind", 0.10) end
    applyRvTransform()
    updateInteraction()
    toast.t = 4.5
  else  -- driving
    if buildMode then setBuildMode(false) end
    stationUI = nil
    fishingT = 0
    if atNode then
      g_bankKeep = 1.0
      LC.flow.emit("raid_end")
      LC.flow.call("stop_cycle", "leave")
    end
    atNode = nil
    despawnHorde()
    if onFoot then despawnPlayer() end
    driveView = "cab"
    driveLookYaw, driveLookPitch = 0, 0
    SetVar("cameraMouseLock", 1)
    if PlayMusic then PlayMusic("engine", 0.22) end
    toast.text = "上路！W 油门  S 刹车  A/D 转向  空格 停车  C 视角"
    toast.t = 4.5
  end
  if ghostEnt ~= nil then SetVisible(ghostEnt, false) end
  if hoverDecalEnt ~= nil then SetVisible(hoverDecalEnt, false) end
  hover = nil
  if gridFloorEnt ~= nil then SetVisible(gridFloorEnt, false) end
  if gridRoofEnt ~= nil then SetVisible(gridRoofEnt, false) end
end

function saveLayout()
  local rows = {}
  for _, inst in pairs(instances) do
    if inst.free then
      rows[#rows + 1] = { id = inst.cat.id, free = true, lx = inst.lx, lz = inst.lz,
                          rot = inst.lrot or 0, layer = inst.layer,
                          plant = inst.plant, on = inst.on }
    else
      rows[#rows + 1] = { id = inst.cat.id, cx = inst.cx, cz = inst.cz,
                          rot = inst.rot, layer = inst.layer, plant = inst.plant,
                          on = inst.on }
    end
  end
  local data = {
    cell = CELL, gw = GW, gh = GH, materials = materials,
    rv = { x = rv.x, z = rv.z, yaw = rv.yaw, node = atNode or curNode },
    vehicle = { fuel = fuel, dur = dur, parts = parts, upgrades = upgrades },
    survival = { health = needs.health, hunger = needs.hunger, thirst = needs.thirst,
                 energy = needs.energy, hygiene = needs.hygiene },
    inventory = inv, water = water,
    day = day, clock = clock, modules = rows, pois = generatedPois,
  }
  do
    local visitedList = {}
    for id, seen in pairs(visitedPois) do
      if seen then visitedList[#visitedList + 1] = id end
    end
    data.progress = {
      tripDistance = tripDistance or 0,
      tripFuel = tripFuel or 0,
      jobs = jobDelivered or 0,
      visited = visitedList,
    }
  end
  if coopRole == "server" and coopSaveExtra ~= nil then
    local extra = coopSaveExtra()
    if extra ~= nil then data.coop = extra end
  end
  local ok = WriteText("saves/layout.json", jsonEncode(data))
  local where = "saves/layout.json"
  if not ok then
    -- 打包运行时 saves/ 可能还没建目录：退回数据根目录
    ok = WriteText("layout.json", jsonEncode(data))
    where = "layout.json"
  end
  toast.text = ok and ("已保存 (" .. where .. ")") or "保存失败"
  toast.t = 2.5
  return ok
end

function clearAll()
  for _, inst in pairs(instances) do Despawn(inst.ent) end
  instances = {}
  occupancy = {}
  roofOcc = {}
end

function loadLayout()
  local text = ReadText("saves/layout.json")
  if not text or text == "" then text = ReadText("layout.json") end
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
      if row.free then
        placeFree(cat, tonumber(row.lx) or 0, tonumber(row.lz) or 0,
                  tonumber(row.rot) or 0, true)
      else
        place(cat, math.floor(row.cx or 0), math.floor(row.cz or 0),
              math.floor(row.rot or 0), true)
      end
      if lastPlacedInst then
        if type(row.plant) == "table" then lastPlacedInst.plant = row.plant end
        if type(row.on) == "boolean" then lastPlacedInst.on = row.on end
      end
      curLayer = saved
    end
  end
  computeReachability()

  -- 程序化地点存档
  if type(data.pois) == "table" then
    generatedPois = {}
    for _, p in ipairs(data.pois) do
      if type(p) == "table" and p.id and nodeById(p.id) == nil then
        local nd = { id = p.id, name = p.name or p.id, x = tonumber(p.x) or 0,
                     z = tonumber(p.z) or 0, danger = tonumber(p.danger) or 1,
                     loot = p.loot or { cloth = { 1, 2 } },
                     links = p.links or {}, generated = true }
        NODES[#NODES + 1] = nd
        generatedPois[#generatedPois + 1] = p
        spawnMarkerFor(nd)
        spawnRuinFor(nd)
        for _, lid in ipairs(nd.links or {}) do
          local o = nodeById(lid)
          if o then spawnRoadBetween(o.x, o.z, nd.x, nd.z) end
        end
      end
    end
  end

  if type(data.rv) == "table" then
    rv.x = tonumber(data.rv.x) or 0
    rv.z = tonumber(data.rv.z) or 0
    rv.yaw = tonumber(data.rv.yaw) or 0
    curNode = tostring(data.rv.node or "camp")
    atNode = nil  -- 读档回到纯建造/驻车状态
    applyRvTransform()
    if rvBody then
      local gy = (terrainHeight and terrainHeight(rv.x, rv.z)) or 0
      PhysicsSetPosition(rvBody, { x = rv.x, y = 0.9 + gy, z = rv.z })
      PhysicsSetVelocity(rvBody, { x = 0, y = 0, z = 0 })
      if rvQuat ~= nil then PhysicsSetRotation(rvBody, rvQuat()) end
      rv.susp = nil
      if resetRvWheelBodies ~= nil then resetRvWheelBodies() end
    end
  end
  if type(data.vehicle) == "table" then
    fuel = math.min(VEH.tank, tonumber(data.vehicle.fuel) or VEH.tank)
    dur = math.min(VEH.durMax, tonumber(data.vehicle.dur) or VEH.durMax)
    if type(data.vehicle.parts) == "table" then
      for k, v in pairs(data.vehicle.parts) do
        if parts[k] ~= nil then
          parts[k] = math.max(0, math.min(VEH.partMax, tonumber(v) or 100))
        end
      end
    end
    if type(data.vehicle.upgrades) == "table" then
      for k, v in pairs(data.vehicle.upgrades) do
        if upgrades[k] ~= nil then
          upgrades[k] = math.max(0, math.min(3, tonumber(v) or 0))
        end
      end
    end
  end
  if type(data.survival) == "table" then
    needs.health = tonumber(data.survival.health) or needs.health
    needs.hunger = tonumber(data.survival.hunger) or needs.hunger
    needs.thirst = tonumber(data.survival.thirst) or needs.thirst
    needs.energy = tonumber(data.survival.energy) or needs.energy
    needs.hygiene = tonumber(data.survival.hygiene) or needs.hygiene
  end
  if type(data.progress) == "table" then
    tripDistance = tonumber(data.progress.tripDistance) or tripDistance
    tripFuel = tonumber(data.progress.tripFuel) or tripFuel
    jobDelivered = math.floor(tonumber(data.progress.jobs) or jobDelivered)
    if type(data.progress.visited) == "table" then
      visitedPois = {}
      visitedCount = 0
      for _, id in ipairs(data.progress.visited) do
        if type(id) == "string" and id ~= "" then
          visitedPois[id] = true
          visitedCount = visitedCount + 1
        end
      end
    end
  end
  if type(data.inventory) == "table" then
    inv = {}
    for k, v in pairs(data.inventory) do
      if ITEMS[k] then inv[k] = tonumber(v) or 0 end
    end
  end
  water = math.min(SURVIVAL.waterMax, tonumber(data.water) or water)
  day = tonumber(data.day) or day
  clock = tonumber(data.clock) or clock
  if onFoot and player.active then
    player.x, player.z = rotOf(0, -5.0)
    player.yaw = rv.yaw
    player.pitch = 0
  end
  recomputeStats()
  toast.text = "存档已读取"
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
    SetVisible(gridFloorEnt, false)
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
  local nr = GetVar("netRole")
  if nr == "server" or nr == "client" or nr == "spectate" then
    coopRole = nr
  else
    coopRole = nil
  end
  local saveText = ReadText("saves/layout.json")
  if not saveText or saveText == "" then saveText = ReadText("layout.json") end
  local hasSave = saveText ~= nil and saveText ~= ""
  if coopRole ~= nil then hasSave = false end
  camEnt = FindNamedEntity("Main Camera")
  groundEnt = FindNamedEntity("Ground")
  collectRvParts()
  if spawnRvTaillights ~= nil then spawnRvTaillights() end
  spawnRvShell()
  spawnRvWheels()
  spawnInterior()
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
  -- 用薄立方体而不是贴花：贴花是深度投影，会把路面画到房车/家具上。
  -- 世界固定公路网 + 开局固定生成的地点（驾驶中不再改路）
  spawnRoadNetwork()
  -- 有存档时由 loadLayout 恢复已发现地点，避免每次启动又随机生成一批
  if not hasSave then preGeneratePois(6) end
  spawnRoadsidePoles()
  spawnRoadMarkers()
  LC.flow.load("assets/flow/raid_wave.flow.json")
  LC.flow.load("assets/flow/stop_cycle.flow.json")
  LC.flow.load("assets/flow/drive_event.flow.json")
  LC.flow.load("assets/flow/zombie_death.flow.json")
  LC.flow.load("assets/flow/vehicle_warnings.flow.json")
  LC.flow.load("assets/flow/difficulty_scaling.flow.json")
  spawnPonds()
  spawnScenery()
  spawnHillPlants()
  -- Camp-wide heightfield (rolling knolls + the roadside ridge) generated by
  -- tools/gen_rv_terrain.py; lc_terrain samples the same grid. The visible mesh
  -- is split into 4x4 chunks so each has a distance LOD chain (4 m near,
  -- 8 m / 12 m far) and can be frustum-culled independently.
  local span = 72.0
  terrainChunks = {}
  for iz = 0, 3 do
    for ix = 0, 3 do
      local cx = -144.0 + span * (ix + 0.5)
      local cz = -144.0 + span * (iz + 0.5)
      local stage = string.format("rv_terrain_%d%d", ix, iz)
      local levels = {}
      for _, suffix in ipairs({ "", "_lod2", "_lod3" }) do
        local chunk = SpawnPrefab(stage .. suffix, { x = cx, y = 0, z = cz })
        if chunk ~= nil then
          SetRotationY(chunk, 0)
          SetVisible(chunk, false)
        end
        levels[#levels + 1] = chunk
      end
      terrainChunks[#terrainChunks + 1] = { x = cx, z = cz, stage = stage,
                                           levels = levels }
    end
  end
  if updateTerrainLod ~= nil then updateTerrainLod(0, true) end
  spawnMarkers()
  if spawnCampProps ~= nil then spawnCampProps() end
  if spawnJobBoard ~= nil then spawnJobBoard() end
  do  -- 验证钩子
    local flag = ReadText("autopilot_on.txt")
    autoPilot = (flag ~= nil and flag ~= "")
    local sm = ReadText("start_mode.txt")
    if sm and sm ~= "" then
      startMode = string.lower((sm:gsub("%s+", "")))
    end
    if startMode == "glassbreak" and breakPoiWindows ~= nil then
      local gnd = nodeById and nodeById("ruin")
      if gnd ~= nil then breakPoiWindows(gnd, "test") end
    end
    if startMode == "sinkview" then
      -- Test-only: the sculpted lathed sink basin in front of the camera.
      SpawnPrefab("rv_furn_sink", { x = 5.0, y = 0.0, z = 1.8 })
    end
    -- Test-only: start in the outside chase camera (autopilot screenshots).
    if startMode == "chase" or startMode == "tiretest" or
       startMode == "northdrive" then
      driveView = "chase"
    end
    -- Test-only: print surface/grip/temperature while driving.
    local tl = ReadText("tire_log.txt")
    rv.tireLog = (tl ~= nil and tl ~= "")
    -- Test-only: steering sweep demo (assist + self-centering) and log.
    local sd = ReadText("steer_demo.txt")
    if sd ~= nil and sd ~= "" then
      rv.steerDemo = true
      rv.steerLog = true
      driveView = "chase"
    end
    local sl = ReadText("steer_log.txt")
    if sl ~= nil and sl ~= "" then rv.steerLog = true end
  end
  ensureHelpers()
  initSurvival()
  initWeather()
  initDayNight()
  if startMode == "rain" then setWeather("rain") end
  if startMode == "fog" then setWeather("fog") end

  -- 初始示例布局：车尾卧室 + 灶台/水槽/冰箱 + 车顶太阳能
  place(CATALOG[2], 2, 0, 0, true)   -- 单人床（右侧，留出 1m 走道）
  place(CATALOG[11], 3, 3, 0, true)  -- 水槽
  place(CATALOG[3], 2, 4, 0, true)   -- 灶台（右侧）
  place(CATALOG[5], 2, 6, 0, true)   -- 净水箱（右侧）
  place(CATALOG[7], 2, 8, 0, true)   -- 工作台（右侧）
  place(CATALOG[20], 3, 11, 0, true) -- 咖啡机（右侧）
  place(CATALOG[21], 3, 12, 0, true) -- 种植箱（右侧）
  local savedLayer = curLayer
  curLayer = "roof"
  place(CATALOG[17], 0, 8, 0, true)  -- 太阳能板
  place(CATALOG[19], 2, 9, 0, true)  -- 车顶行李架
  curLayer = savedLayer
  if startMode == "furnall" then
    -- Test-only: place the authored furniture set along the left wall so a
    -- single screenshot shows TV/bathroom/medicine cabinet/rack/heater.
    place(CATALOG[10], 0, 0, 0, true)   -- tv
    place(CATALOG[14], 0, 1, 0, true)   -- med_cabinet
    place(CATALOG[12], 0, 2, 0, true)   -- bathroom (2x3)
    place(CATALOG[15], 0, 5, 0, true)   -- gun_rack
    place(CATALOG[9],  0, 6, 0, true)   -- heater
    place(CATALOG[8],  2, 11, 0, true)  -- storage
    place(CATALOG[4],  2, 12, 0, true)  -- fridge
  end
  if startMode == "utilall" then
    -- Test-only: place the utility furniture along the left wall.
    place(CATALOG[5],  0, 0, 0, true)   -- water_tank (2x2)
    place(CATALOG[6],  0, 2, 0, true)   -- battery (2x1)
    place(CATALOG[13], 0, 3, 0, true)   -- generator (2x2)
    place(CATALOG[16], 0, 5, 1, true)   -- turret (2x2, barrels sideways)
    place(CATALOG[21], 0, 7, 0, true)   -- plant (1x2)
    place(CATALOG[20], 1, 7, 0, true)   -- coffee (1x1)
  end
  if startMode == "utilclose" then
    place(CATALOG[6],  0, 1, 0, true)   -- battery (2x1)
    place(CATALOG[13], 0, 3, 0, true)   -- generator (2x2)
    place(CATALOG[16], 0, 5, 1, true)   -- turret (2x2, barrels sideways)
  end
  if startMode == "utilroof" then
    -- Test-only: add a roof vent next to the default solar array.
    curLayer = "roof"
    place(CATALOG[18], 0, 3, 0, true)   -- roof_vent
    curLayer = savedLayer
  end
  computeReachability()
  recomputeStats()

  if hasSave then loadLayout() end
  applyRvTransform()
  -- RV 动态刚体：质量随载重
  rv.mass = math.max(800, 1200 + stats.weight * 0.8)
  rv.yawInertia = rv.mass * (2.4 * 2.4 + 8.6 * 8.6) / 12
  local chassisDesc = { mass = rv.mass,
                        friction = 0.15, restitution = 0.05,
                        layer = 4, mask = 0xFD }
  rvBody = PhysicsAddBox({ x = rv.x, y = 0.9, z = rv.z },
                         { x = 1.2, y = 0.64, z = 4.3 }, true, chassisDesc)

  if startMode == "rvslope" or startMode == "rvslope_in" then
    -- Test-only: park the RV on the steep part of the embankment so height
    -- following and pitch/roll tilt are visible in a screenshot.
    rv.x, rv.z = 13.0, 10.0
    rv.groundY = terrainHeight and terrainHeight(rv.x, rv.z) or 0
    rv.susp = nil
    if updateRvSuspension ~= nil then
      updateRvSuspension(1.0)
    elseif updateRvTilt ~= nil then
      updateRvTilt(1.0)
    end
    applyRvTransform()
    if rvBody ~= nil then
      PhysicsSetPosition(rvBody, { x = rv.x, y = 0.9 + rv.groundY, z = rv.z })
      if rvQuat ~= nil then PhysicsSetRotation(rvBody, rvQuat()) end
    end
  end
  if startMode == "rvhill" then
    -- Test-only: the RV on a rounded meadow knoll (four wheel contacts).
    rv.x, rv.z, rv.yaw = -26.0, -20.0, 0.7
    rv.groundY = terrainHeight and terrainHeight(rv.x, rv.z) or 0
    rv.susp = nil
    if updateRvSuspension ~= nil then
      updateRvSuspension(1.0)
    elseif updateRvTilt ~= nil then
      updateRvTilt(1.0)
    end
    applyRvTransform()
    if rvBody ~= nil then
      PhysicsSetPosition(rvBody, { x = rv.x, y = 0.9 + rv.groundY, z = rv.z })
      if rvQuat ~= nil then PhysicsSetRotation(rvBody, rvQuat()) end
    end
  end

  if startMode == "rvfar" then
    -- Test-only: the RV on the west highland of the extended 288 m terrain.
    rv.x, rv.z, rv.yaw = -95.0, 15.0, 0.7
    rv.groundY = terrainHeight and terrainHeight(rv.x, rv.z) or 0
    rv.susp = nil
    if updateRvSuspension ~= nil then
      updateRvSuspension(1.0)
    elseif updateRvTilt ~= nil then
      updateRvTilt(1.0)
    end
    applyRvTransform()
    if rvBody ~= nil then
      PhysicsSetPosition(rvBody, { x = rv.x, y = 0.9 + rv.groundY, z = rv.z })
      if rvQuat ~= nil then PhysicsSetRotation(rvBody, rvQuat()) end
    end
  end

  if startMode == "northdrive" then
    -- Test-only: start on the north meadow facing south so the far chunks
    -- first stream out and then reload while autopilot drives back to camp.
    rv.x, rv.z, rv.yaw = 0.0, 110.0, math.pi
    rv.groundY = terrainHeight and terrainHeight(rv.x, rv.z) or 0
    rv.susp = nil
    if updateRvSuspension ~= nil then
      updateRvSuspension(1.0)
    elseif updateRvTilt ~= nil then
      updateRvTilt(1.0)
    end
    applyRvTransform()
    if rvBody ~= nil then
      PhysicsSetPosition(rvBody, { x = rv.x, y = 0.9 + rv.groundY, z = rv.z })
      if rvQuat ~= nil then PhysicsSetRotation(rvBody, rvQuat()) end
    end
  end

  if startMode == "tiretest" then
    -- Test-only: start on flat open meadow (grass) so the tire model can be
    -- compared against the asphalt run.
    rv.x, rv.z, rv.yaw = -8.0, -18.0, 0.0
    rv.groundY = terrainHeight and terrainHeight(rv.x, rv.z) or 0
    rv.susp = nil
    if updateRvSuspension ~= nil then
      updateRvSuspension(1.0)
    elseif updateRvTilt ~= nil then
      updateRvTilt(1.0)
    end
    applyRvTransform()
    if rvBody ~= nil then
      PhysicsSetPosition(rvBody, { x = rv.x, y = 0.9 + rv.groundY, z = rv.z })
      if rvQuat ~= nil then PhysicsSetRotation(rvBody, rvQuat()) end
    end
  end

  if coopRole ~= nil then
    coopInitWorld(coopRole)
  end
  -- Server, single-player and clients all keep local wheel bodies: the server
  -- simulates them authoritatively, clients use them for local suspension
  -- prediction from the replicated chassis pose.
  spawnRvWheelBodies()

  if coopRole == nil then
  -- 第一人称出生：车右后侧，面朝房车（Nomad Drive 式开场）
  local px, pz, pyaw = 2.2, -8.0, dirYaw(-2.2, 8.0)
  if startMode == "look_back" then
    px, pz = -1.8, -7.5
  end
  if startMode == "inside" or startMode == "inside_night" or startMode == "inside_dark"
     or startMode == "place" or startMode == "place_night"
     or startMode == "stove" or startMode == "bench" then
    px, pz, pyaw = -0.5, -2.6, 0
  end
  if startMode == "side" or startMode == "side_night" then
    px, pz, pyaw = 6.2, 0.0, -math.pi * 0.5
  end
  if startMode == "rvfront" then
    -- Test-only front three-quarter view of the RV.
    px, pz = 2.6, 9.2
    pyaw = dirYaw(-2.6, -9.2)
  end
  if startMode == "rvrear" or startMode == "brakeview" then
    -- Test-only rear three-quarter view of the RV (curved rear cap).
    px, pz = 2.8, -9.0
    pyaw = dirYaw(-2.8, 5.4)
  end
  if startMode == "jobview" then
    -- Test-only: stand at the camp contract board.
    px, pz = 4.0, -5.8
    pyaw = dirYaw(2.6, 2.8)
  end
  if startMode == "roadside" then
    -- Test-only: look down the main road past the reflective delineators.
    px, pz = 5.6, -18.0
    pyaw = dirYaw(-1.4, 12.0)
  end
  if startMode == "nightbeam" or startMode == "nightoff" then
    -- Test-only: stand in front of the RV at night so the headlight cones
    -- point back toward the camera and the lit road is visible.
    px, pz = 7.4, 7.6
    pyaw = dirYaw(-7.4, -3.4)
  end
  if startMode == "jobmarker" or startMode == "urgentjob" then
    -- Test-only: stand at camp looking north at the gas-stop contract target.
    px, pz = 4.6, -10.5
    pyaw = dirYaw(-4.6, 70.5)
  end
  if startMode == "jobaway" then
    -- Test-only: face away from the gas-stop target so the edge arrow shows.
    px, pz = 4.6, -10.5
    pyaw = dirYaw(4.6, -70.5)
  end
  if startMode == "campview" then
    -- Test-only: look from the east side into the camp props (awning/table/lamp).
    px, pz = 8.2, -1.5
    pyaw = dirYaw(-3.7, 6.5)
  end
  if startMode == "flashlight" or startMode == "flashlight_off" then
    -- Test-only: night walk toward the RV side; the beam should light the body.
    px, pz = 6.6, -8.2
    pyaw = dirYaw(-6.6, 8.2)
  end
  if startMode == "campnight" then
    -- Test-only: the camp at night with the campfire glow and awning lamp.
    px, pz = 8.2, -1.5
    pyaw = dirYaw(-3.7, 6.5)
  end
  if startMode == "furnview" then
    -- Test-only interior view down the furniture wall.
    px, pz = -0.72, 1.30
    pyaw = math.pi
  end
  if startMode == "furnall" then
    -- Test-only view of the authored furniture showcase along the left wall.
    px, pz = -0.72, 3.20
    pyaw = math.pi
  end
  if startMode == "utilall" then
    -- Test-only view down the left wall from the middle of the aisle.
    px, pz = -0.28, 2.60
    pyaw = math.pi
  end
  if startMode == "utilclose" then
    -- Test-only close-up of the battery/generator/turret cluster.
    px, pz = -0.28, 1.90
    pyaw = math.pi
  end
  if startMode == "sinkview" then
    -- Test-only close-up of the revolved sink basin.
    px, pz = 5.0, 3.4
    pyaw = math.pi
  end
  if startMode == "texclose" then
    -- Test-only close-up of the workbench (wood/pegboard normal maps).
    px, pz = -0.35, 1.00
    pyaw = math.pi * 0.5
  end
  if startMode == "texbody" then
    -- Test-only close-up of the RV body side (rib/panel normal map).
    px, pz = 3.40, 0.00
    pyaw = -math.pi * 0.5
  end
  if startMode == "embankment" then
    -- Test-only view of the roadside dirt embankment from the road.
    px, pz = 1.50, 6.00
    pyaw = math.pi * 0.5
  end
  if startMode == "slope" then
    -- Test-only: stand on the dirt slope (camera/eye follow terrain height).
    px, pz = 13.0, 8.0
    pyaw = -math.pi * 0.5
  end
  if startMode == "glassview" or startMode == "glassbreak" then
    -- Test-only: stand in front of the ruin shop window (glassbreak also
    -- shatters it at start for the before/after fracture comparison).
    local nd = nodeById and nodeById("ruin")
    if nd ~= nil then
      local cx, cz = poiCenter(nd)
      px, pz = cx, cz + 5.4
      pyaw = math.pi
    end
  end
  if startMode == "hills" then
    -- Test-only: look across the rolling meadow knolls south-west of camp.
    px, pz = -14.0, -34.0
    pyaw = dirYaw(-12.0, 14.0)
  end
  if startMode == "hilltop" then
    -- Test-only: on the south-west knoll, looking back at the camp/RV.
    px, pz = -26.0, -20.0
    pyaw = dirYaw(26.0, 20.0)
  end
  if startMode == "farhills" then
    -- Test-only: on the extended terrain (north meadow), looking south to camp.
    px, pz = 28.0, 96.0
    pyaw = dirYaw(-28.0, -96.0)
  end
  if startMode == "rvfar" then
    -- Test-only: three-quarter view of the RV on the west highland.
    px, pz = -8.0, -8.0
    pyaw = dirYaw(8.0, 8.0)
  end
  if startMode == "rvslope" then
    -- Test-only: the RV is parked on the embankment (see rvBody block).
    px, pz = -7.0, -5.0
    pyaw = dirYaw(7.0, 5.0)
  end
  if startMode == "rvhill" then
    -- Test-only: three-quarter view of the RV resting on the meadow knoll.
    px, pz = 7.5, 7.5
    pyaw = dirYaw(-7.5, -7.5)
  end
  if startMode == "rvslope_in" then
    -- Test-only: first-person view from inside the RV on the slope; the eye
    -- follows the leaning floor (rvFloorOffset) instead of the flat ground.
    px, pz = -0.5, -2.6
    pyaw = 0
  end
  if startMode == "poi" then
    -- 路边商店门口：用于检查可进入建筑（门洞/内衬/战利品）
    local nd = nodeById("ruin")
    if nd ~= nil then
      local cx, cz = poiCenter(nd)
      px, pz, pyaw = cx, cz + 3.6, math.pi
    end
  end
  if startMode == "gas" then
    -- 加油站商店门口
    local nd = nodeById("gasstop")
    if nd ~= nil then
      local cx, cz = poiCenter(nd)
      px, pz, pyaw = cx, cz + 3.6, math.pi
    end
  end
  if startMode == "warehouse" then
    -- 物流仓库装卸门门口
    local nd = nodeById("depot")
    if nd ~= nil then
      local cx, cz = poiCenter(nd)
      px, pz, pyaw = cx, cz + 4.2, math.pi
    end
  end
  if startMode == "farm" then
    -- 旧农场谷仓门口
    local nd = nodeById("farm")
    if nd ~= nil then
      local cx, cz = poiCenter(nd)
      px, pz, pyaw = cx, cz + 3.6, math.pi
    end
  end
  if startMode == "mall" then
    -- 购物中心店门口
    local nd = nodeById("mall")
    if nd ~= nil then
      local cx, cz = poiCenter(nd)
      px, pz, pyaw = cx, cz + 4.0, math.pi
    end
  end
  if startMode == "lake" then
    -- 湖边木屋门廊
    local nd = nodeById("lakecamp")
    if nd ~= nil then
      local cx, cz = poiCenter(nd)
      px, pz, pyaw = cx, cz + 4.0, math.pi
    end
  end
  if startMode == "clinic" then
    -- 路边诊所门口
    local nd = nodeById("clinic")
    if nd ~= nil then
      local cx, cz = poiCenter(nd)
      px, pz, pyaw = cx, cz + 3.6, math.pi
    end
  end
  if startMode == "side_night" then clock = 22 * 60 end
  if startMode == "night" or startMode == "inside_night" or startMode == "inside_dark"
     or startMode == "place_night" or startMode == "night_drive" then
    clock = 22 * 60
  end
  if PlayMusic then PlayMusic("wind", 0.10) end
  local sx, sz = rotOf(px, pz)
  spawnPlayerAt(sx, sz, rv.yaw + pyaw)
  player.pitch = -0.05
  if startMode == "look_back" then player.yaw = player.yaw + math.pi
  elseif startMode == "look_left" then player.yaw = player.yaw + math.pi * 0.5
  elseif startMode == "look_right" then player.yaw = player.yaw - math.pi * 0.5
  elseif startMode == "look_down" then player.pitch = -0.9
  elseif startMode == "look_up" then player.pitch = 0.7
  elseif startMode == "utilclose" then player.pitch = -0.34
  elseif startMode == "sinkview" then player.pitch = -0.35
  elseif startMode == "texclose" then player.pitch = -0.28
  elseif startMode == "texbody" then player.pitch = 0.05
  end
  SetVar("cameraMouseLock", 1)
  updateInteraction()

  local selftestFlag = ReadText("selftest_on.txt")
  if selftestFlag and selftestFlag ~= "" then runSelfTest() end

  if startMode == "inv" then
    invOpen = true
  elseif startMode == "stove" then
    stationUI = { kind = "stove", inst = hasModule("stove") }
  elseif startMode == "bench" then
    stationUI = { kind = "workbench", inst = hasModule("workbench") }
  elseif startMode == "build" or startMode == "place" or startMode == "place_night" then
    setBuildMode(true)
  elseif startMode == "map" then
    trySpawnPoi()
    trySpawnPoi()
    mapOpen = true
  elseif startMode == "jobview" then
    jobRefreshOffers()
    jobUI = true
  elseif startMode == "jobdrive" then
    jobRefreshOffers()
    jobAccept(1)
    setPhase("driving")
    driveView = "chase"
  elseif startMode == "nighthead" then
    clock = 22 * 60
    setPhase("driving")
    driveView = "chase"
  elseif startMode == "nightbeam" then
    clock = 22 * 60
    rv.forceHeadlights = true
  elseif startMode == "nightoff" then
    clock = 22 * 60
  elseif startMode == "jobmarker" then
    jobRefreshOffers("gasstop")
    jobAccept(1)
  elseif startMode == "urgentjob" then
    jobRefreshOffers("gasstop")
    jobAccept(1)
    job.urgent = true
    job.deadlineAbs = jobNowAbs() + 420
  elseif startMode == "jobmap" then
    jobRefreshOffers("gasstop")
    jobAccept(1)
    mapOpen = true
  elseif startMode == "brakeview" then
    rv.brakeTest = true
  elseif startMode == "raindrive" then
    setWeather("rain")
    setPhase("driving")
    driveView = "chase"
  elseif startMode == "fogdrive" then
    setWeather("fog")
    setPhase("driving")
    driveView = "chase"
  elseif startMode == "inside_dark" then
    setInteriorLights(false)
  elseif startMode == "tripview" then
    tripDistance, tripFuel = 1240, 42
    rv.speed = 6.4
    setPhase("driving")
    driveView = "chase"
  elseif startMode == "flashlight" then
    clock = 22 * 60
    flashlightOn = true
  elseif startMode == "flashlight_off" then
    clock = 22 * 60
    flashlightOn = false
  elseif startMode == "campnight" then
    clock = 22 * 60
  elseif startMode == "jobresult" then
    jobRefreshOffers("gasstop")
    jobAccept(1)
    job.condition = 0.72
    jobComplete()
  elseif startMode == "jobaway" then
    jobRefreshOffers("gasstop")
    jobAccept(1)
  elseif startMode == "drive" or startMode == "night_drive" or autoPilot or rv.steerDemo then
    setPhase("driving")
  elseif startMode == "utilroof" then
    -- Roof furniture is invisible from the ground; drive in chase view.
    setPhase("driving")
    driveView = "chase"
  else
    toast.text = string.format("第 %d 天 %s — E 交互 / B 改装 / T 驾驶 / M 地图",
                               day, timeString())
    toast.t = 5.0
  end
  end
end

-- 改装模式：放置/旋转/拆除（沿用网格系统）
function updateBuildInteraction()
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
    SetVisible(gridFloorEnt, gridVisible and curLayer == "floor")
    SetVisible(gridRoofEnt, gridVisible and curLayer == "roof")
  end

  local mp = InputMousePos()
  local hitSlot = hotbarHit(mp.x, mp.y)
  local overUi = hitSlot ~= nil
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
end

-- 第一人称自由摆放：准星拾取地面，家具不再吸附网格
function updateFreePlacement()
  if ActionPressed("e") then setBuildMode(false) return end

  for i, key in ipairs(HOTKEYS) do
    if ActionPressed(key) then selected = i end
  end
  local wheel = MouseWheel()
  if wheel and wheel ~= 0 then
    local n = #CATALOG
    selected = ((selected - 1) + (wheel > 0 and 1 or -1)) % n + 1
  end
  if ActionPressed("r") then
    placeRot = (placeRot + math.pi / 12) % (math.pi * 2)
  end
  if ActionPressed("tab") then
    curLayer = (curLayer == "roof") and "floor" or "roof"
    placeRot = 0
  end

  local vp = GetViewportSize()
  local vw = (vp and vp.w) or 1280
  local vh = (vp and vp.h) or 720
  local cat = CATALOG[selected]
  local aimed = findAimedInstance(4.6)
  hover = nil

  local pick = PickGround({ x = vw * 0.5, y = vh * 0.5 })
  if pick then
    local lx, lz = worldToLocal(pick.x, pick.z)
    placeLx, placeLz = lx, lz
    local layerOk = (cat.layer == "roof") == (curLayer == "roof")
    placeValid = layerOk and freeFits(cat, lx, lz, placeRot)
                 and canAfford(cat.cost or {})
    if ghostValidLast ~= placeValid then
      setGhostColor(placeValid)
      ghostValidLast = placeValid
    end
    local baseY = (curLayer == "roof") and ROOF_Y or FLOOR_Y
    local wx, wz = rotOf(lx, lz)
    SetVisible(ghostEnt, true)
    SetPosition(ghostEnt, { x = wx, y = baseY + 0.04, z = wz })
    SetRotationY(ghostEnt, rv.yaw + placeRot)
    SetScale(ghostEnt, cat.w * CELL - 0.02, 0.05, cat.h * CELL - 0.02)
    hover = { valid = placeValid, cat = cat, lx = lx, lz = lz }
  else
    SetVisible(ghostEnt, false)
    placeValid = false
  end

  if aimed then
    local ex, ez = instanceExtents(aimed)
    local wx, wz = rotOf(aimed.lx, aimed.lz)
    local baseY = (aimed.layer == "roof") and ROOF_Y or FLOOR_Y
    SetVisible(hoverDecalEnt, true)
    SetPosition(hoverDecalEnt, { x = wx, y = baseY, z = wz })
    SetRotationY(hoverDecalEnt, rv.yaw + (aimed.lrot or 0))
    SetScale(hoverDecalEnt, ex * 2, 1, ez * 2)
    SetDecal(hoverDecalEnt, 1.0, 0.45, aimed.cat.mh + 0.25)
    if hover then hover.inst = aimed end
  else
    SetVisible(hoverDecalEnt, false)
  end

  local mp = InputMousePos()
  local hitSlot = hotbarHit(mp.x, mp.y)
  if InputMousePressed(0) then
    if hitSlot then
      selected = hitSlot
    elseif placeValid then
      local ok, why = placeFree(cat, placeLx, placeLz, placeRot)
      if ok then
        toast.text = "已放置 " .. cat.name
        toast.t = 1.6
        SetVisible(ghostEnt, false)
      elseif why == "cost" then
        toast.text = "材料不足: 需要 " .. costText(cat.cost or {})
        toast.t = 1.8
      else
        toast.text = "这里放不下 — 换个位置或旋转"
        toast.t = 1.6
      end
    end
  end
  if InputMousePressed(1) or ActionPressed("x") then
    if aimed then
      local name = aimed.cat.name
      if removeInstance(aimed) then
        toast.text = "拆下 " .. name .. "（材料已退还）"
        toast.t = 1.8
      end
    elseif InputMousePressed(1) then
      toast.text = "准星对着家具再按右键拆除"
      toast.t = 1.6
    end
  end
end

function on_update(ent, dt)
  if updateTerrainLod ~= nil then updateTerrainLod(dt) end
  if updateJobs ~= nil then updateJobs(dt) end
  if updateVehicleLights ~= nil then updateVehicleLights() end
  if updateTaillights ~= nil then updateTaillights() end
  if updateCampfireLight ~= nil then updateCampfireLight(dt) end
  if jobResult ~= nil then
    jobResult.t = jobResult.t - dt
    if jobResult.t <= 0 then jobResult = nil end
  end
  if coopRole ~= nil then
    coopUpdate(dt)
    if updatePoiWindows then updatePoiWindows(dt) end
    if updatePoiDoors then updatePoiDoors(dt) end
    if updatePoiSlidingDoors then updatePoiSlidingDoors(dt) end
    if updateMeadowWind then updateMeadowWind(dt) end
    return
  end
  if toast.t > 0 then toast.t = toast.t - dt end
  ensureHelpers()
  if ActionPressed("m") then mapOpen = not mapOpen end
  if ActionPressed("i") then invOpen = not invOpen end
  if ActionPressed("l") and setInteriorLights ~= nil then
    setInteriorLights(not interiorLightsOn)
    toast.text = interiorLightsOn and "车内灯已打开" or "车内灯已关闭"
    toast.t = 2.0
  end
  if ActionPressed("f") then
    flashlightOn = not flashlightOn
    toast.text = flashlightOn and "手电已打开（F 关闭）" or "手电已关闭（F 打开）"
    toast.t = 2.0
  end
  if updateFlashlight ~= nil then updateFlashlight() end
  if hitMarkT > 0 then hitMarkT = hitMarkT - dt end
  if recoilT > 0 then recoilT = recoilT - dt end
  if fireT > 0 then fireT = fireT - dt end
  updateGrenades(dt)
  LC.flow.setvar("turret_combat", "turret_active", hasModule("turret") and 1 or 0)
  LC.flow.tick(dt)
  updateSurvival(dt)
  updateWeather(dt)
  updateDayNight(dt)

  if ActionPressed("f5") then saveLayout() end
  if ActionPressed("f9") then loadLayout() end

  -- ======================= 行驶 =======================
  if phase == "driving" then
    -- Test-only: keep the outside chase camera for autopilot screenshots.
    if startMode == "chase" or startMode == "tiretest" or
       startMode == "northdrive" then
      driveView = "chase"
    end
    if rv.steerDemo then driveView = "chase" end
    if ActionPressed("c") then
      driveView = (driveView == "cab") and "chase" or "cab"
    end
    if ActionPressed("space") then
      setPhase("parked")
    end
    updateDrive(dt)
    updateCamera(dt)
    return
  end

  -- Parked: settle the body onto its four wheel contacts so a stop on a knoll
  -- or a slope keeps the wheels grounded. Prefer the physical spring/damper
  -- path (height/pitch/roll from Jolt), fall back to the plane fit.
  if rvBody ~= nil and updateRvPhysics ~= nil and updateRvPhysics(dt) then
    rv.steerCmd = nil
    local v = PhysicsGetVelocity(rvBody)
    if v ~= nil then
      PhysicsSetVelocity(rvBody, { x = 0, y = v.y, z = 0 })
    end
    local w = PhysicsGetAngularVelocity ~= nil and
              PhysicsGetAngularVelocity(rvBody) or nil
    if w ~= nil and PhysicsAddAngularImpulse ~= nil then
      local inertia = rv.yawInertia or 8000
      PhysicsAddAngularImpulse(rvBody, { x = 0, y = -w.y * inertia, z = 0 })
    end
    applyRvTransform()
  elseif updateRvSuspension ~= nil and rvShell ~= nil then
    updateRvSuspension(dt)
    applyRvTransform()
  end

  -- ======================= 驻车：改装 / 第一人称 =======================
  if ActionPressed("b") then
    if buildMode then
      setBuildMode(false)
    elseif nearRv() then
      setBuildMode(true)
      nearAction = nil
    else
      toast.text = "回到房车旁才能改装（B）"
      toast.t = 2.2
    end
  end
  if ActionPressed("t") then
    if nearRv() then
      if buildMode then setBuildMode(false) end
      setPhase("driving")
    else
      toast.text = "回到房车旁才能上路（T）"
      toast.t = 2.2
    end
  end

  if buildMode then
    updatePlayer(dt)
    updateFreePlacement()
    updateCamera(dt)
    return
  end

  if invOpen then
    if ActionPressed("e") then invOpen = false end
    handleQuickItem()
    updateCamera(dt)
    return
  end

  -- 灶台菜单打开时暂停移动/交互，只处理配方按键
  if stationUI then
    if ActionPressed("e") then stationUI = nil end
    if stationUI.kind == "stove" then
      for i, r in ipairs(RECIPES) do
        if ActionPressed(tostring(i)) then cookRecipe(r.id) end
      end
    elseif stationUI.kind == "workbench" then
      for i, part in ipairs(REPAIR_PARTS) do
        if ActionPressed(tostring(i)) then repairPart(part.id) end
      end
    end
    updateCamera(dt)
    return
  end

  -- 货运板菜单：1-3 接单 / 0 放弃当前合同 / E 关闭
  if jobUI then
    if ActionPressed("e") then jobUI = false end
    for i = 1, #jobOffers do
      if ActionPressed(tostring(i)) then jobAccept(i) end
    end
    if ActionPressed("0") then jobAbandon() end
    updateCamera(dt)
    return
  end

  -- 第一人称：移动 / 交互 / 射击 / 钓鱼
  updatePlayer(dt)
  if updatePoiWindows then updatePoiWindows(dt) end
  if updatePoiDoorProximity then updatePoiDoorProximity(player.x, player.z) end
  if updatePoiDoors then updatePoiDoors(dt) end
  if updatePoiBarnDoorProximity then updatePoiBarnDoorProximity(player.x, player.z) end
  if updatePoiSlidingDoors then updatePoiSlidingDoors(dt) end
  if updateMeadowWind then updateMeadowWind(dt) end
  updateFishing(dt)
  updateInteraction()
  if ActionPressed("e") then doInteract() end

  local vp = GetViewportSize()
  local cx = (vp and vp.w or 1280) * 0.5
  local cy = (vp and vp.h or 720) * 0.5
  if InputMousePressed(0) then
    if #horde > 0 and fireT <= 0 then
      fireT = RAID.fireCd
      shootAt(cx, cy)
    else
      -- Attacking inside a POI shatters its display windows.
      if breakPoiWindowsAt then breakPoiWindowsAt(player.x, player.z, "attack") end
      if nearAction and nearAction.kind == "crate" then
        doInteract()
      end
    end
  elseif InputMouseDown(0) and #horde > 0 and fireT <= 0 then
    fireT = RAID.fireCd
    shootAt(cx, cy)
  end

  updateHorde(dt)
  turretFire(dt)
  handleQuickItem()

  -- 验证钩子：自动搜箱
  if autoPilot and atNode then
    autoT = autoT + dt
    if autoT > 1.0 then
      autoT = 0
      if #crates > 0 then
        searchCrate(1)
      else
        autoPilot = false
        endScavenge(1.0)
      end
    end
  end

  updateCamera(dt)
end

-- 2D HUD 必须在 on_render 里画：draw2d 上下文只在渲染期接线
function on_render()
  if coopRole ~= nil then
    if coopRole ~= "server" then coopDrawHud() end
    return
  end
  drawWeatherOverlay()
  drawNightOverlay()
  if phase == "driving" then
    drawDriveHud()
  elseif buildMode then
    drawPlaceHud()
  else
    drawFpHud()
  end
  if invOpen then drawInventoryPanel() end
  if stationUI then drawStationMenu() end
  if jobUI then drawJobPanel() end
  if jobResult then drawJobResult() end
  if mapOpen then drawMapHud() end
end
