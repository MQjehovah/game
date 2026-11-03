-- lc_coop.lua
-- Main-world 2-4 player co-op layer for the RV project.
--
-- The authoritative server owns the shared RV pose, survival needs, water,
-- inventory/materials, furniture state and loot crates. Clients send
-- "game_cmd" RPCs and render the "game_state"/"game_world"/"game_crates"
-- broadcasts on top of the real garage.json world (RV shell, POIs, furniture).
--
-- This runs only when GameVar netRole is "server" / "client" / "spectate";
-- a normal single-player launch never enters this file's update path.

coopRole = nil

local MAX_SLOTS = 4
local COOP_SPAWN = {
  { lx = -1.5, lz = -9.0 },
  { lx =  1.5, lz = -9.0 },
  { lx = -3.0, lz = -8.0 },
  { lx =  3.0, lz = -8.0 },
}
local PLAYER_SPEED = 3.2
local RV_SPEED = 7.0
local WORLD_LIMIT = 380.0

-- Co-op build palette (1-9 in build mode). Keep the list small so the whole
-- set fits on the HUD; the server validates against the full CATALOG.
local COOP_BUILD_ITEMS = {
  "storage", "heater", "tv", "med_cabinet", "gun_rack",
  "sink", "bed_single", "coffee", "solar",
}
local CAT_BY_ID = {}
for _, cat in ipairs(CATALOG) do CAT_BY_ID[cat.id] = cat end

local coop = {
  active = false, role = nil, isServer = false,
  slots = {}, proxies = {}, visuals = {},
  localPos = { x = 0, z = -8, yaw = 0 },
  mySlot = 1, myClientId = nil, haveSelf = false,
  lookYaw = 0, lookPitch = -0.05,
  state = nil, seq = 0, lastLogSeq = -1,
  sendT = 0, stateT = 0, worldT = 0,
  rv = { x = 0, z = 0, yaw = 0, speed = 0 },
  fuel = 60, water = 80, day = 1, clock = 8 * 60,
  weather = "clear", lights = true,
  needs = { health = 100, hunger = 85, thirst = 90, energy = 90, hygiene = 80 },
  materials = { metal = 0, electronics = 0, cloth = 0, wood = 0 },
  inv = {},
  worldSeq = 0, crateList = {}, furnitureList = {},
  crateSeqApplied = -1, worldSeqApplied = -1,
  autoT = 0, autoDrive = false, autoDriveDone = false,
  actionsDemo = false, actionsDone = false, clampDemo = false,
  saveT = 0, saveDirty = false, loadedSave = false,
  buildMode = false, buildIndex = 1, buildCx = 0, buildCz = 0,
  buildLayer = "floor", buildRot = 0, buildGhostValid = nil,
  buildDemo = false, buildDemoT = 0, buildDemoStage = 0,
  buildUi = false, buildUiDone = false,
  poiDemo = false, poiView = false, poiExt = false, poiNode = nil, poiIn = {}, poiT = 0,
  doorDemo = false, doorT = 0, doorNode = nil,
  doorClosedLogged = false, doorOpenLogged = false, doorCrossed = false,
  sitDemo = false, doorView = false, sitView = false,
  animDemo = nil, animDemoEnt = nil,
  doorAngle = 0, doorTarget = 0, animT = 0,
  animPhase = {},
  visualAnim = {},
  boardDemo = false, boardDemoT = 0, boardDemoStage = 0,
  carrySlot = 0, carryT = 0, carryView = false, glassView = false,
  propView = false,
  rollup = false, barnDoor = false,
  toast = "", toastT = 0, logT = 0,
}

local function clamp(v, lo, hi)
  if v < lo then return lo end
  if v > hi then return hi end
  return v
end

local function readFlag(name)
  local f = ReadText(name)
  return f ~= nil and f ~= ""
end

local function sortedInstances()
  local ids = {}
  for id, _ in pairs(instances) do ids[#ids + 1] = id end
  table.sort(ids)
  local out = {}
  for _, id in ipairs(ids) do out[#out + 1] = instances[id] end
  return out
end

local function findInstanceByCat(catId)
  if catId == "bed" then
    return findInstanceByCat("bed_single") or findInstanceByCat("bed_double")
  end
  for _, inst in pairs(instances) do
    if inst.cat and inst.cat.id == catId then return inst end
  end
  return nil
end

local function furnState(id)
  local inst = findInstanceByCat(id)
  return inst ~= nil and (inst.on ~= false)
end

local function seatLocal(seat)
  if seat == "driver" then return 0.0, 3.15 end
  return 0.60, -3.00 -- passenger sits inside the doorway bay, visible through the opening
end

local function seatWorld(seat)
  local lx, lz = seatLocal(seat)
  return rotOf(lx, lz)
end

local function doorWorld()
  return rotOf(1.23, -3.15)
end

local function setToast(text)
  coop.toast = text
  coop.toastT = 3.0
  coop.eventId = (coop.eventId or 0) + 1
  print("[coop-main-event] " .. text)
end

local function copyGlobals()
  coop.rv = { x = rv.x, z = rv.z, yaw = rv.yaw, speed = 0 }
  coop.fuel = fuel
  coop.water = water
  coop.day = day
  coop.clock = clock
  coop.weather = weather or "clear"
  coop.lights = true
  coop.needs = {}
  for k, v in pairs(needs) do coop.needs[k] = v end
  coop.materials = {}
  for k, v in pairs(materials) do coop.materials[k] = v end
  coop.inv = {}
  for k, v in pairs(inv) do coop.inv[k] = v end
end

local function applySharedGlobals()
  rv.x, rv.z, rv.yaw = coop.rv.x, coop.rv.z, coop.rv.yaw
  rv.speed = coop.rv.speed or 0
  fuel, water, day, clock = coop.fuel, coop.water, coop.day, coop.clock
  needs = coop.needs
  materials = coop.materials
  inv = coop.inv
end

local function buildFurnitureList()
  local out = {}
  for i, inst in ipairs(sortedInstances()) do
    out[i] = {
      id = inst.cat.id,
      free = inst.free and 1 or 0,
      cx = inst.cx or 0, cz = inst.cz or 0, rot = inst.rot or 0,
      lx = inst.lx or 0, lz = inst.lz or 0, lrot = inst.lrot or 0,
      layer = inst.layer or "floor",
      on = (inst.on ~= false) and 1 or 0,
    }
  end
  return out
end

local function buildCrateList()
  local out = {}
  for i, c in ipairs(crates) do out[i] = { x = c.x, z = c.z } end
  return out
end

-- Called from garage.lua saveLayout() when the authoritative server saves.
-- The standard fields (RV/needs/inventory/modules) are already in the save;
-- this adds the co-op-only state: loot crates, lights and player slots.
function coopSaveExtra()
  if not (coop.active and coop.isServer) then return nil end
  local slotsOut = {}
  for i, s in ipairs(coop.slots) do
    if s.name ~= nil and s.name ~= "" then
      slotsOut[i] = { name = s.name, x = s.x, z = s.z, yaw = s.yaw }
    end
  end
  return {
    version = 1,
    lights = coop.lights and 1 or 0,
    crates = buildCrateList(),
    slots = slotsOut,
  }
end

local function rebuildFurniture(list)
  if type(list) ~= "table" then return end
  clearAll()
  local byId = {}
  for _, cat in ipairs(CATALOG) do byId[cat.id] = cat end
  for _, e in ipairs(list) do
    local cat = byId[e.id]
    if cat then
      curLayer = e.layer or cat.layer or "floor"
      if e.free == 1 or e.free == true then
        placeFree(cat, tonumber(e.lx) or 0, tonumber(e.lz) or 0,
                  tonumber(e.lrot) or 0, true)
      else
        place(cat, math.floor(tonumber(e.cx) or 0), math.floor(tonumber(e.cz) or 0),
              math.floor(tonumber(e.rot) or 0), true)
      end
      if lastPlacedInst then
        lastPlacedInst.on = (e.on == 1 or e.on == true)
        if type(e.plant) == "table" then lastPlacedInst.plant = e.plant end
      end
    end
  end
  curLayer = "floor"
  computeReachability()
  recomputeStats()
  applyRvTransform()
end

local function applyFurnitureList(list)
  if type(list) ~= "table" then return end
  local localInst = sortedInstances()
  local same = (#localInst == #list)
  if same then
    for i, inst in ipairs(localInst) do
      if inst.cat.id ~= list[i].id then same = false break end
    end
  end
  if same then
    for i, inst in ipairs(localInst) do
      local e = list[i]
      inst.lx = tonumber(e.lx) or inst.lx
      inst.lz = tonumber(e.lz) or inst.lz
      inst.lrot = tonumber(e.lrot) or 0
      inst.layer = e.layer or inst.layer
      inst.on = (e.on == 1 or e.on == true)
      if type(e.plant) == "table" then inst.plant = e.plant end
    end
    applyRvTransform()
    recomputeStats()
    print(string.format("[coop-main] furniture synced in place (%d)", #list))
  else
    rebuildFurniture(list)
    print(string.format("[coop-main] furniture rebuilt (%d)", #list))
  end
end

local function rebuildCrates(list)
  if type(list) ~= "table" then return end
  despawnCrates()
  for _, c in ipairs(list) do
    local ent = SpawnPrefab("rv_loot_crate",
                            { x = tonumber(c.x) or 0, y = 0.28, z = tonumber(c.z) or 0 })
    if ent ~= nil then
      SetScale(ent, 0.55, 0.55, 0.55)
      crates[#crates + 1] = { ent = ent, x = tonumber(c.x) or 0, z = tonumber(c.z) or 0 }
    end
  end
  print(string.format("[coop-main] crates synced (%d)", #list))
end

local function setLampVisible(on)
  if rvInterior == nil then return end
  for _, p in ipairs(rvInterior) do
    if p.pre == "rv_lamp" then SetVisible(p.ent, on) end
  end
end

local function applyLights()
  setLampVisible(coop.lights)
end

local function furnMark(id)
  local inst = findInstanceByCat(id)
  if inst == nil then return "✗" end
  if inst.on == false then return "关" end
  return "✓"
end

local function coopBuildCat()
  local id = COOP_BUILD_ITEMS[coop.buildIndex]
  return id and CAT_BY_ID[id] or nil
end

local function setCoopBuildMode(on)
  coop.buildMode = on
  if on then
    ensureHelpers()
    coop.buildIndex = 1
    coop.buildCx = 0
    coop.buildCz = 0
    coop.buildLayer = "floor"
    coop.buildRot = 0
    coop.buildGhostValid = nil
    coop.toast = "建造模式：1-9 选家具，Tab 地板/车顶，J/L/I/K 移动，R 旋转，E 放置，X 拆除，B 退出"
    coop.toastT = 5.0
  else
    if ghostEnt ~= nil then SetVisible(ghostEnt, false) end
    coop.toast = "退出建造模式"
    coop.toastT = 1.5
  end
end

local function coopBuildUpdate()
  if ActionPressed("b") then
    setCoopBuildMode(false)
    return
  end
  if ActionPressed("tab") then
    coop.buildLayer = (coop.buildLayer == "roof") and "floor" or "roof"
  end
  if ActionPressed("r") then
    coop.buildRot = (coop.buildRot + 1) % 2
  end
  if ActionPressed("j") then coop.buildCx = math.max(0, coop.buildCx - 1) end
  if ActionPressed("l") then coop.buildCx = math.min(GW - 1, coop.buildCx + 1) end
  if ActionPressed("i") then coop.buildCz = math.max(0, coop.buildCz - 1) end
  if ActionPressed("k") then coop.buildCz = math.min(GH - 1, coop.buildCz + 1) end
  for i = 1, #COOP_BUILD_ITEMS do
    if ActionPressed(tostring(i)) then coop.buildIndex = i end
  end

  local cat = coopBuildCat()
  local cx, cz, rot = coop.buildCx, coop.buildCz, coop.buildRot
  local valid = false
  if cat ~= nil then
    if cat.layer ~= coop.buildLayer then
      valid = false
    else
      valid = fits(cat, cx, cz, rot) and canAfford(cat.cost or {})
    end
    if coop.buildGhostValid ~= valid then
      coop.buildGhostValid = valid
      setGhostColor(valid)
    end
    if ghostEnt ~= nil then
      local w, h = footprint(cat, rot)
      local baseY = (cat.layer == "roof") and ROOF_Y or FLOOR_Y
      local ox = ORIGIN_X + cx * CELL + w * CELL * 0.5
      local oz = ORIGIN_Z + cz * CELL + h * CELL * 0.5
      local wx, wz = rotOf(ox, oz)
      SetVisible(ghostEnt, true)
      SetPosition(ghostEnt, { x = wx, y = baseY + 0.05, z = wz })
      SetRotationY(ghostEnt, rv.yaw + ((rot % 2 == 1) and math.pi * 0.5 or 0))
      SetScale(ghostEnt, w * CELL - 0.04, 0.06, h * CELL - 0.04)
    end
  end

  if ActionPressed("e") then
    if cat == nil then
      coop.toast = "没有选中家具"
      coop.toastT = 2.0
    elseif not valid then
      coop.toast = "当前位置/层/材料不合法，先调整光标或按 Tab 切层"
      coop.toastT = 2.4
    elseif GetVar("netConnected") == 1 then
      Rpc("game_cmd", { type = "place", item = cat.id, cx = cx, cz = cz,
                        rot = rot, layer = coop.buildLayer })
      coop.toast = string.format("请求放置 %s (%d,%d)", cat.name, cx, cz)
      coop.toastT = 2.0
    end
  elseif ActionPressed("x") and GetVar("netConnected") == 1 then
    Rpc("game_cmd", { type = "remove", cx = cx, cz = cz, layer = coop.buildLayer })
    coop.toast = string.format("请求拆除 (%d,%d)", cx, cz)
    coop.toastT = 2.0
  end
end

local function findSlot(clientId)
  for i = 1, MAX_SLOTS do
    if coop.slots[i].client == clientId then return coop.slots[i], i end
  end
  return nil, 0
end

local function chooseFood()
  local order = { "feast", "fishsoup", "meal", "fish", "canned", "rawfood" }
  for _, id in ipairs(order) do
    if hasItem(id, 1) then return id end
  end
  return nil
end

-- ---------------------------------------------------------------------------
-- Server
-- ---------------------------------------------------------------------------

local function pushWorld()
  local all = buildFurnitureList()
  local chunkSize = 4
  local total = math.max(1, math.ceil(#all / chunkSize))
  for part = 1, total do
    local chunk = {}
    local first = (part - 1) * chunkSize + 1
    local last = math.min(#all, part * chunkSize)
    for i = first, last do chunk[#chunk + 1] = all[i] end
    Rpc("game_world", {
      seq = coop.seq,
      worldSeq = coop.worldSeq,
      part = part,
      total = total,
      furniture = chunk,
    })
  end
  Rpc("game_crates", {
    seq = coop.seq,
    worldSeq = coop.worldSeq,
    crates = buildCrateList(),
  })
end

local function pushState()
  coop.seq = coop.seq + 1
  local players = {}
  local anyInside = false
  for i = 1, MAX_SLOTS do
    local s = coop.slots[i]
    if s.joined and s.inside then anyInside = true end
  end
  for i = 1, MAX_SLOTS do
    local s = coop.slots[i]
    players[i] = {
      c = s.client, x = s.x, z = s.z, y = s.yaw,
      j = s.joined and 1 or 0,
      ins = s.inside and 1 or 0,
      st = (s.seat == "driver" and 2) or (s.seat == "passenger" and 1) or 0,
      k = s.key,
    }
  end
  local invList = {}
  for id, n in pairs(inv) do
    if (n or 0) > 0 then invList[#invList + 1] = { id = id, n = n } end
  end
  local furnOn = {}
  for _, id in ipairs({ "stove", "fridge", "bed", "coffee", "plant", "workbench" }) do
    furnOn[#furnOn + 1] = { id = id, on = furnState(id) and 1 or 0 }
  end
  local glass = "-"
  if poiGlassBrokenList ~= nil then
    local gb = poiGlassBrokenList()
    if gb ~= nil and gb ~= "" then glass = gb end
  end
  Rpc("game_state", {
    seq = coop.seq, rv = coop.rv, players = players, needs = needs,
    fuel = fuel, water = water, day = day, clock = clock,
    weather = coop.weather, lights = coop.lights and 1 or 0,
    materials = materials, inv = invList, furn = furnOn,
    crates = #crates, furnCount = #sortedInstances(), worldSeq = coop.worldSeq,
    door = anyInside and 1 or 0,
    carry = coop.carrySlot or 0,
    rollup = coop.rollup and 1 or 0,
    barndoor = coop.barnDoor and 1 or 0,
    glass = glass,
    event = coop.toast, eventId = coop.eventId or 0,
  })
end

local function serverDebugLog()
  local door = 0
  for i = 1, MAX_SLOTS do
    if coop.slots[i].joined and coop.slots[i].inside then door = 1 end
  end
  local shop = 0
  local shopNd = nodeById(coop.poiNode or "ruin")
  if shopNd ~= nil then
    local scx, scz = poiCenter(shopNd)
    for i = 1, MAX_SLOTS do
      local s = coop.slots[i]
      if s.joined and math.abs(s.x - scx) < POI_HALF_X and
         math.abs(s.z - scz) < POI_HALF_Z then shop = shop + 1 end
    end
  end
  print(string.format(
    "[coop-main-server] seq=%d rv=(%.2f,%.2f,yaw=%.2f speed=%.2f) p1=(%.2f,%.2f) p2=(%.2f,%.2f) p3=(%.2f,%.2f) p4=(%.2f,%.2f) joined=%d/%d fuel=%.1f water=%.1f metal=%d crates=%d meal=%d waterItem=%d stove=%d fridge=%d bed=%d furn=%d in1=%d in2=%d in3=%d in4=%d door=%d shop=%d rollup=%d barndoor=%d glass=%s",
    coop.seq, rv.x, rv.z, rv.yaw, rv.speed,
    coop.slots[1].x, coop.slots[1].z, coop.slots[2].x, coop.slots[2].z,
    coop.slots[3].x, coop.slots[3].z, coop.slots[4].x, coop.slots[4].z,
    (coop.slots[1].joined and 1 or 0) + (coop.slots[2].joined and 1 or 0) +
    (coop.slots[3].joined and 1 or 0) + (coop.slots[4].joined and 1 or 0),
    MAX_SLOTS, fuel, water, materials.metal or 0, #crates,
    inv.meal or 0, inv.water or 0,
    furnState("stove") and 1 or 0, furnState("fridge") and 1 or 0,
    furnState("bed") and 1 or 0, #sortedInstances(),
    coop.slots[1].inside and 1 or 0, coop.slots[2].inside and 1 or 0,
    coop.slots[3].inside and 1 or 0, coop.slots[4].inside and 1 or 0, door, shop,
    coop.rollup and 1 or 0, coop.barnDoor and 1 or 0,
    (poiGlassBrokenList and poiGlassBrokenList()) or "-"))
end

local function serverHandleCommand(clientId, a)
  local s = findSlot(clientId)
  if s == nil or type(a) ~= "table" then return end
  local t = a.type
  if t == "input" then
    s.f = clamp(tonumber(a.f) or 0, -1, 1)
    s.s = clamp(tonumber(a.s) or 0, -1, 1)
    local yaw = tonumber(a.yaw)
    if yaw ~= nil and yaw == yaw and math.abs(yaw) < 1000 then s.yaw = yaw end
  elseif t == "board" then
    if s.inside then return end
    local dx, dz = doorWorld()
    local d = math.sqrt((s.x - dx) ^ 2 + (s.z - dz) ^ 2)
    if d > 5.5 and not coop.boardDemo then
      setToast("走到车门旁再上车")
      return
    end
    s.inside = true
    s.seat = "passenger"
    s.driving = false
    s.f, s.s = 0, 0
    local sx, sz = seatWorld("passenger")
    s.x, s.z = sx, sz
    setToast(string.format("%s 上车（乘客席）", s.name or ("P" .. tostring(s.slotIndex or 0))))
    pushState()
  elseif t == "exit" then
    if not s.inside then return end
    s.inside = false
    s.seat = nil
    s.driving = false
    s.f, s.s = 0, 0
    local ex, ez = rotOf(2.2, -2.6)
    s.x, s.z = ex, ez
    s.yaw = rv.yaw
    setToast(string.format("%s 下车", s.name or ("P" .. tostring(s.slotIndex or 0))))
    pushState()
  elseif t == "place" then
    local cat = CAT_BY_ID[tostring(a.item or "")]
    local cx = math.floor(tonumber(a.cx) or -1)
    local cz = math.floor(tonumber(a.cz) or -1)
    local rot = math.floor(tonumber(a.rot) or 0)
    local layer = tostring(a.layer or "")
    local who = s.name or ("P" .. tostring(s.slotIndex or 0))
    if cat == nil then
      setToast("放置失败：未知家具")
      return
    end
    if cat.layer ~= layer then
      setToast("放置失败：地板/车顶不匹配")
      return
    end
    if not fits(cat, cx, cz, rot) then
      setToast("放置失败：位置冲突或越界")
      return
    end
    if not canAfford(cat.cost or {}) then
      setToast("放置失败：材料不足 " .. costText(cat.cost or {}))
      return
    end
    local prevLayer = curLayer
    curLayer = cat.layer
    local ok = place(cat, cx, cz, rot, false)
    curLayer = prevLayer
    if ok then
      recomputeStats()
      coop.worldSeq = coop.worldSeq + 1
      coop.saveDirty = true
      setToast(string.format("%s 放置了 %s (%d,%d)", who, cat.name, cx, cz))
      pushWorld()
      pushState()
    else
      setToast("放置失败：服务器拒绝")
    end
  elseif t == "remove" then
    local cx = math.floor(tonumber(a.cx) or -1)
    local cz = math.floor(tonumber(a.cz) or -1)
    local who = s.name or ("P" .. tostring(s.slotIndex or 0))
    if removeAt(cx, cz) then
      recomputeStats()
      coop.worldSeq = coop.worldSeq + 1
      coop.saveDirty = true
      setToast(string.format("%s 拆除了 (%d,%d)", who, cx, cz))
      pushWorld()
      pushState()
    else
      setToast("拆除失败：这里没有家具")
    end
  elseif t == "drive" then
    s.driving = not s.driving
    if s.driving then
      for i = 1, MAX_SLOTS do
        if coop.slots[i] ~= s then coop.slots[i].driving = false end
      end
      s.inside = true
      s.seat = "driver"
      s.f, s.s = 0, 0
      local sx, sz = seatWorld("driver")
      s.x, s.z = sx, sz
      setToast(string.format("%s 坐上驾驶席", s.name or ("P" .. tostring(s.slotIndex or 0))))
    else
      if s.inside then
        s.seat = "passenger"
        local sx, sz = seatWorld("passenger")
        s.x, s.z = sx, sz
      end
      setToast(string.format("%s 离开驾驶席", s.name or ("P" .. tostring(s.slotIndex or 0))))
    end
    pushState()
  elseif t == "lights" then
    coop.lights = not coop.lights
    setToast(coop.lights and "车内灯已打开" or "车内灯已关闭")
  elseif t == "eat" then
    local id = chooseFood()
    if id ~= nil and useItem(id) then
      setToast("吃了 " .. itemName(id) .. "（饥饿/口渴已恢复）")
    else
      setToast("背包里没有吃的")
    end
  elseif t == "drink" then
    if water >= 0.5 then
      water = water - 0.5
      needs.thirst = math.min(100, needs.thirst + 25)
      setToast("喝了一杯水（口渴 +25）")
    else
      setToast("水箱空了")
    end
  elseif t == "cook" then
    if not hasModule("stove") then
      setToast("没有灶台")
    elseif cookRecipe("meal") then
      setToast("灶台做好一份热饭")
    else
      setToast("做不了：缺少生食材/水/电力")
    end
  elseif t == "sleep" then
    advanceGameMinutes(360)
    needs.energy = math.min(100, needs.energy + 55)
    setToast("在房车床上睡了 6 小时（体力 +55）")
  elseif t == "search" then
    local idx = tonumber(a.index) or 0
    local c = crates[idx]
    if c == nil then
      setToast("物资箱已经不在了")
      return
    end
    local d = math.sqrt((c.x - s.x) ^ 2 + (c.z - s.z) ^ 2)
    if d > 5.5 and not coop.actionsDemo then
      setToast("离物资箱太远（先走近再搜）")
      return
    end
    searchCrate(idx)
    bankStopBag(1.0)
    coop.worldSeq = coop.worldSeq + 1
    coop.carrySlot = s.slotIndex or 0
    coop.carryT = 1.2
    setToast("搜到一个物资箱（材料/物品已进共享背包）")
  elseif t == "toggle_stove" then
    local inst = findInstanceByCat("stove")
    if inst ~= nil then
      inst.on = not (inst.on ~= false)
      coop.worldSeq = coop.worldSeq + 1
      recomputeStats()
      setToast(inst.on and "灶台已打开" or "灶台已关闭")
    end
  end
end

local function drainCommands()
  for _ = 1, 64 do
    local cmd = NetCommand()
    if cmd == nil then break end
    if cmd.name == "game_cmd" then
      local a = nil
      if type(cmd.args) == "string" and cmd.args ~= "" then a = Json.Parse(cmd.args) end
      if type(a) == "table" then serverHandleCommand(cmd.client, a) end
    end
  end
end

local function coopSave()
  if not (coop.active and coop.isServer) then return end
  local ok = saveLayout()
  if ok then
    print(string.format("[coop-main-server] saved state rv=(%.2f,%.2f) crates=%d slots=%d",
          rv.x, rv.z, #crates, #coop.slots))
  else
    print("[coop-main-server] save failed")
  end
end

local function smoothStep(t)
  t = clamp(t, 0.0, 1.0)
  return t * t * (3.0 - 2.0 * t)
end

-- Scripted verification walk for coop_ruin.txt: 3-5s enter through the front
-- door, 5-8s stand inside the store, 8-10s leave.
local function poiDemoPose(slotIndex, t)
  local nd = nodeById(coop.poiNode or "ruin")
  if nd == nil then return nil end
  -- The first cycle covers the initial join/world-sync window; loop the demo
  -- afterwards so a snapshot taken a little later still sees a full approach.
  if t >= 10.0 then t = (t - 10.0) % 10.0 end
  local cx, cz = poiCenter(nd)
  local lane = cx
  local outsideZ = cz + 5.6 + (slotIndex - 1) * 1.1
  local insideZ = cz + 0.7
  local side = (slotIndex == 1) and -1.15 or 1.15
  if t < 3.0 then
    return lane, outsideZ
  elseif t < 5.0 then
    local k = smoothStep((t - 3.0) / 2.0)
    return lane, outsideZ + (insideZ - outsideZ) * k
  elseif t < 8.0 then
    local k = smoothStep((t - 5.0) / 1.2)
    return lane + side * k, insideZ
  elseif t < 10.0 then
    local k = smoothStep((t - 8.0) / 2.0)
    local x = lane + side * (1.0 - math.min(1.0, k * 2.0))
    local z = insideZ + (outsideZ - insideZ) * math.max(0.0, (k - 0.5) * 2.0)
    return x, z
  end
  return lane, outsideZ
end

local function poiInsideCount()
  local n = 0
  local nd = nodeById(coop.poiNode or "ruin")
  if nd == nil then return 0 end
  local cx, cz = poiCenter(nd)
  for i = 1, MAX_SLOTS do
    local s = coop.slots[i]
    if s.joined and math.abs(s.x - cx) < POI_HALF_X and
       math.abs(s.z - cz) < POI_HALF_Z then
      n = n + 1
    end
  end
  return n
end

local function serverUpdate(dt)
  drainCommands()
  coop.autoT = coop.autoT + dt

  -- Test-only door collision demo: stand outside the depot doorway, walk into
  -- the closed shutter, then open it and keep walking.
  if coop.doorDemo then
    local nd = nodeById(coop.doorNode or "depot")
    if nd ~= nil then
      local cx, cz = poiCenter(nd)
      local anyJoined = false
      for i = 1, MAX_SLOTS do
        if coop.slots[i].joined then anyJoined = true end
      end
      if anyJoined then coop.doorT = coop.doorT + dt end
      for i = 1, 2 do
        local s = coop.slots[i]
        if s.joined then
          if coop.doorT < 0.3 then
            s.x, s.z = cx + ((i == 1) and -0.45 or 0.45), cz + 3.4
            s.yaw = math.pi
            s.f, s.s = 0, 0
          else
            s.yaw = math.pi
            s.f, s.s = 1, 0
          end
        end
      end
    end
  end

  if coop.carryT > 0 then
    coop.carryT = coop.carryT - dt
    if coop.carryT <= 0 then
      coop.carryT = 0
      coop.carrySlot = 0
    end
  end

  -- Server-authoritative glass: a joined player inside a POI shatters its
  -- display windows, and the broken-id list is broadcast in game_state.
  if breakPoiWindowsAt then
    for i = 1, MAX_SLOTS do
      local s = coop.slots[i]
      if s.joined and breakPoiWindowsAt(s.x, s.z, "enter") > 0 then
        print(string.format("[coop-glass] player %d broke the POI windows (server)", i))
        pushState()
      end
    end
  end

  -- Warehouse roll-up door: open while any player is near the loading bay.
  do
    local depot = nodeById("depot")
    if depot ~= nil then
      local dcx, dcz = poiCenter(depot)
      local near = false
      for i = 1, MAX_SLOTS do
        local s = coop.slots[i]
        if s.joined and math.abs(s.x - dcx) < 3.6 and
           (s.z - dcz) > 0.5 and (s.z - dcz) < 4.5 then
          near = true
        end
      end
      -- Door collision demo: hold the shutter closed for the first phase,
      -- then hold it open so the pass-through is unambiguous.
      if coop.doorDemo and coop.doorNode == "depot" then
        near = (coop.doorT >= 5.0)
      end
      if near ~= coop.rollup then
        coop.rollup = near
        print(string.format("[depot-door] %s (server)", near and "open" or "close"))
        -- Push immediately so clients animate the shutter without waiting for
        -- the next 10 Hz state tick.
        pushState()
      end
      -- Drive the authoritative door animation locally too: the server's
      -- collidePoiWalls must see the same open/closed state as the clients.
      if setPoiDoorOpen then setPoiDoorOpen("depot", near) end
    end
  end

  -- Barn sliding door: open while any player is near the barn entrance.
  do
    local farm = nodeById("farm")
    if farm ~= nil then
      local fcx, fcz = poiCenter(farm)
      local near = false
      for i = 1, MAX_SLOTS do
        local s = coop.slots[i]
        if s.joined and math.abs(s.x - fcx) < 3.6 and
           (s.z - fcz) > 0.5 and (s.z - fcz) < 4.5 then
          near = true
        end
      end
      -- Door collision demo: hold the sliding panel closed for the first phase,
      -- then hold it open for the pass-through.
      if coop.doorDemo and coop.doorNode == "farm" then
        near = (coop.doorT >= 5.0)
      end
      if near ~= coop.barnDoor then
        coop.barnDoor = near
        print(string.format("[barn-door] %s (server)", near and "open" or "close"))
        pushState()
      end
      if setPoiSlidingDoorOpen then setPoiSlidingDoorOpen("farm", near) end
    end
  end

  if coop.autoDrive and not coop.autoDriveDone and coop.autoT >= 2.0 and coop.slots[1].joined then
    coop.autoDriveDone = true
    local drv = coop.slots[1]
    drv.driving = true
    drv.inside = true
    drv.seat = "driver"
    drv.f, drv.s = 0, 0
    local sx, sz = seatWorld("driver")
    drv.x, drv.z = sx, sz
    setToast("P1 坐上了驾驶席")
  end
  if coop.actionsDemo and not coop.actionsDone and coop.autoT >= 6.0
     and (coop.slots[1].joined or coop.slots[2].joined) then
    coop.actionsDone = true
    if #crates > 0 then
      searchCrate(1)
      bankStopBag(1.0)
      coop.worldSeq = coop.worldSeq + 1
      coop.carrySlot = 1
      coop.carryT = 1.2
      setToast("自动搜刮：物资箱已进入共享背包")
    end
    local inst = findInstanceByCat("stove")
    if inst ~= nil then
      inst.on = not (inst.on ~= false)
      coop.worldSeq = coop.worldSeq + 1
      recomputeStats()
      setToast("自动切换灶台状态")
    end
  end

  local driver = nil
  for i = 1, MAX_SLOTS do
    if coop.slots[i].joined and coop.slots[i].driving then driver = coop.slots[i] end
  end

  if driver ~= nil then
    local target = driver.f * RV_SPEED
    rv.speed = rv.speed + (target - rv.speed) * math.min(1, dt * 2.5)
    rv.yaw = rv.yaw - driver.s * dt * 1.1
    rv.x = rv.x + math.sin(rv.yaw) * rv.speed * dt
    rv.z = rv.z + math.cos(rv.yaw) * rv.speed * dt
    rv.x = clamp(rv.x, -WORLD_LIMIT, WORLD_LIMIT)
    rv.z = clamp(rv.z, -WORLD_LIMIT, WORLD_LIMIT)
    coop.rv.x, coop.rv.z, coop.rv.yaw, coop.rv.speed = rv.x, rv.z, rv.yaw, rv.speed
    fuel = math.max(0, fuel - math.abs(rv.speed) * dt * 0.03)
    applyRvTransform()
    if rvBody ~= nil then
      PhysicsSetPosition(rvBody, { x = rv.x, y = 0.9, z = rv.z })
      PhysicsSetVelocity(rvBody, { x = 0, y = 0, z = 0 })
    end
    -- Only players who actually boarded ride with the RV; outside players are
    -- left where they stood (they can walk after the RV stops).
    for i = 1, MAX_SLOTS do
      local s = coop.slots[i]
      if s.joined and s.inside then
        local lx, lz = seatLocal(s.seat or "passenger")
        local wx, wz = rotOf(lx, lz)
        s.x, s.z = wx, wz
        s.yaw = rv.yaw
      end
    end
  else
    rv.speed = 0
    coop.rv.speed = 0
    for i = 1, MAX_SLOTS do
      local s = coop.slots[i]
      if s.joined and not s.inside then
        local fx, fz = math.sin(s.yaw), math.cos(s.yaw)
        local rx, rz = math.cos(s.yaw), -math.sin(s.yaw)
        local nx = s.x + (fx * s.f + rx * s.s) * PLAYER_SPEED * dt
        local nz = s.z + (fz * s.f + rz * s.s) * PLAYER_SPEED * dt
        nx, nz = collidePoiWalls(s.x, s.z, nx, nz)
        s.x, s.z = nx, nz
        if coop.clampDemo then
          s.x = clamp(s.x, -4.0, 4.0)
          s.z = clamp(s.z, -13.0, -6.5)
        else
          s.x = clamp(s.x, -WORLD_LIMIT, WORLD_LIMIT)
          s.z = clamp(s.z, -WORLD_LIMIT, WORLD_LIMIT)
        end
      end
    end
  end

  if coop.poiDemo then
    local anyJoined = false
    for i = 1, MAX_SLOTS do
      if coop.slots[i].joined then anyJoined = true end
    end
    if anyJoined then coop.poiT = coop.poiT + dt end
    for i = 1, 2 do
      local s = coop.slots[i]
      if s.joined then
        local px, pz = poiDemoPose(i, coop.poiT)
        if px ~= nil then
          s.x, s.z = px, pz
          s.yaw = (coop.poiT < 8.0) and math.pi or 0
        end
      end
    end
  end

  do
    local nd = nodeById(coop.poiNode or "ruin")
    if nd ~= nil then
      local cx, cz = poiCenter(nd)
      for i = 1, MAX_SLOTS do
        local s = coop.slots[i]
        local inside = math.abs(s.x - cx) < POI_HALF_X and
                       math.abs(s.z - cz) < POI_HALF_Z
        if inside ~= (coop.poiIn[i] == true) then
          coop.poiIn[i] = inside
          if s.joined then
            print(string.format("[coop-poi] P%d %s %s (%.1f,%.1f)",
                  i, inside and "entered" or "exited", coop.poiNode or "poi",
                  s.x, s.z))
          end
        end
      end
    end
  end

  needs.hunger = math.max(0, needs.hunger - 0.8 * dt)
  needs.thirst = math.max(0, needs.thirst - 1.0 * dt)
  needs.energy = math.max(0, needs.energy - 0.35 * dt)
  needs.hygiene = math.max(0, needs.hygiene - 0.15 * dt)
  if needs.hunger <= 0 or needs.thirst <= 0 then
    needs.health = math.max(0, needs.health - 2.0 * dt)
  end
  clock = clock + dt * 2.0
  while clock >= 1440 do
    clock = clock - 1440
    day = day + 1
  end

  for i = 1, MAX_SLOTS do
    local s = coop.slots[i]
    local ent = coop.proxies[i]
    if ent ~= nil then
      SetPosition(ent, { x = s.x, y = 0, z = s.z })
      SetRotationY(ent, s.yaw)
    end
    s.slotIndex = i
  end

  -- Door collision test: report the stopped / passed positions once per phase.
  if coop.doorDemo then
    local nd = nodeById(coop.doorNode or "depot")
    if nd ~= nil then
      local dtx, dtz = poiCenter(nd)
      local front = dtz + POI_HALF_Z
      local d = poiDoors[nd.id]
      local sb = poiBarnDoors[nd.id]
      local cur = d and d.cur or (sb and sb.cur or -1)
      if coop.doorT >= 4.6 and not coop.doorClosedLogged then
        coop.doorClosedLogged = true
        local z1 = coop.slots[1].z
        print(string.format(
          "[door-test] closed: p1 z=%.2f front=%.2f blocked=%s (door cur=%.2f)",
          z1, front, tostring(z1 > front), cur))
      end
      if coop.doorT > 0.5 and not coop.doorCrossed and
         coop.slots[1].z < front then
        coop.doorCrossed = true
        print(string.format(
          "[door-test] crossed: p1 z=%.2f front=%.2f (door cur=%.2f)",
          coop.slots[1].z, front, cur))
      end
      if coop.doorT >= 7.6 and not coop.doorOpenLogged then
        coop.doorOpenLogged = true
        local z1 = coop.slots[1].z
        print(string.format(
          "[door-test] open: p1 z=%.2f inside=%s (door cur=%.2f)",
          z1, tostring(z1 < front), cur))
      end
    end
  end

  coop.stateT = coop.stateT + dt
  if coop.stateT >= 0.1 then
    coop.stateT = 0
    pushState()
  end
  coop.worldT = coop.worldT + dt
  if coop.worldT >= 1.0 then
    coop.worldT = 0
    pushWorld()
  end

  coop.logT = coop.logT + dt
  if coop.logT >= 2.0 then
    coop.logT = 0
    serverDebugLog()
  end

  coop.saveT = coop.saveT + dt
  if coop.saveT >= 3.0 then
    coop.saveT = 0
    coop.saveDirty = false
    coopSave()
  end
end

-- ---------------------------------------------------------------------------
-- Client
-- ---------------------------------------------------------------------------

local function clientLog(s)
  local p = s.players or {}
  local shop = 0
  local shopNd = nodeById(coop.poiNode or "ruin")
  if shopNd ~= nil then
    local scx, scz = poiCenter(shopNd)
    for i = 1, MAX_SLOTS do
      local q = p[i]
      if q ~= nil and q.j == 1 and math.abs((q.x or 0) - scx) < POI_HALF_X and
         math.abs((q.z or 0) - scz) < POI_HALF_Z then
        shop = shop + 1
      end
    end
  end
  print(string.format(
    "[coop-main-client] seq=%d name=%s slot=%d id=%.0f rv=(%.2f,%.2f,yaw=%.2f) p1=(%.2f,%.2f) p2=(%.2f,%.2f) p3=(%.2f,%.2f) p4=(%.2f,%.2f) fuel=%.1f water=%.1f metal=%d crates=%d meal=%d waterItem=%d stove=%d fridge=%d bed=%d hunger=%.1f furn=%d in1=%d in2=%d in3=%d in4=%d door=%d shop=%d rollup=%s barndoor=%s glass=%s",
    s.seq, tostring(GetVar("playerName")), coop.mySlot, coop.myClientId or 0,
    s.rv.x, s.rv.z, s.rv.yaw,
    (p[1] and p[1].x) or 0, (p[1] and p[1].z) or 0,
    (p[2] and p[2].x) or 0, (p[2] and p[2].z) or 0,
    (p[3] and p[3].x) or 0, (p[3] and p[3].z) or 0,
    (p[4] and p[4].x) or 0, (p[4] and p[4].z) or 0,
    s.fuel or 0, s.water or 0, (s.materials and s.materials.metal) or 0,
    s.crates or 0, coop.invMeal or 0, coop.invWater or 0,
    (s.furn and s.furn[1] and s.furn[1].on) or 0,
    (s.furn and s.furn[2] and s.furn[2].on) or 0,
    (s.furn and s.furn[3] and s.furn[3].on) or 0,
    (s.needs and s.needs.hunger) or 0, s.furnCount or 0,
    (p[1] and p[1].ins) or 0, (p[2] and p[2].ins) or 0,
    (p[3] and p[3].ins) or 0, (p[4] and p[4].ins) or 0, s.door or 0, shop,
    tostring(s.rollup or 0), tostring(s.barndoor or 0), tostring(s.glass or "-")))
end

local function applyState(s)
  coop.state = s
  coop.seq = tonumber(s.seq) or coop.seq
  if s.rv ~= nil then
    coop.rv.x = tonumber(s.rv.x) or coop.rv.x
    coop.rv.z = tonumber(s.rv.z) or coop.rv.z
    coop.rv.yaw = tonumber(s.rv.yaw) or coop.rv.yaw
    coop.rv.speed = tonumber(s.rv.speed) or 0
  end
  coop.fuel = tonumber(s.fuel) or coop.fuel
  coop.water = tonumber(s.water) or coop.water
  coop.day = tonumber(s.day) or coop.day
  coop.clock = tonumber(s.clock) or coop.clock
  coop.lights = (s.lights == 1 or s.lights == true)
  if type(s.materials) == "table" then
    for k, v in pairs(s.materials) do materials[k] = tonumber(v) or 0 end
  end
  if type(s.needs) == "table" then
    for k, v in pairs(s.needs) do needs[k] = tonumber(v) or needs[k] end
  end
  if type(s.inv) == "table" then
    local newInv = {}
    for _, row in ipairs(s.inv) do
      if row.id ~= nil then newInv[row.id] = tonumber(row.n) or 0 end
    end
    inv = newInv
  end
  coop.invMeal = (inv.meal or 0)
  coop.invWater = (inv.water or 0)
  if s.weather ~= nil and s.weather ~= coop.weather then
    coop.weather = s.weather
    setWeather(coop.weather)
  end

  if type(s.players) == "table" then
    for i, p in ipairs(s.players) do
      local st = coop.slots[i]
      if st ~= nil then
        st.client = p.c or 0
        st.joined = (p.j == 1)
        st.x = p.x or st.x
        st.z = p.z or st.z
        st.yaw = p.y or st.yaw
        st.driving = (p.st == 2)
        st.inside = (p.ins == 1)
        st.seat = (p.st == 2) and "driver" or ((p.st == 1) and "passenger" or nil)
        st.key = p.k or st.key
      end
    end
  end
  coop.doorTarget = (s.door == 1) and (math.pi * 0.5) or 0

  if type(s.players) == "table" and coop.myClientId ~= nil then
    for i, p in ipairs(s.players) do
      if p.c == coop.myClientId then
        coop.mySlot = i
        if not coop.haveSelf then
          coop.haveSelf = true
          coop.localPos.x = p.x
          coop.localPos.z = p.z
          coop.localPos.yaw = p.y or 0
          coop.lookYaw = p.y or 0
        else
          local dx, dz = (p.x or 0) - coop.localPos.x, (p.z or 0) - coop.localPos.z
          local d = math.sqrt(dx * dx + dz * dz)
          if d > 1.5 then
            coop.localPos.x, coop.localPos.z = p.x, p.z
          else
            coop.localPos.x = coop.localPos.x + dx * 0.25
            coop.localPos.z = coop.localPos.z + dz * 0.25
          end
        end
        if p.k ~= nil then SetVar("netControlKey", p.k) end
        break
      end
    end
  end

  if s.seq ~= nil and s.seq % 20 == 0 and s.seq ~= coop.lastLogSeq and s.rv ~= nil then
    coop.lastLogSeq = s.seq
    clientLog(s)
  end
  if s.event ~= nil and s.event ~= "" and s.eventId ~= nil and s.eventId ~= coop.lastEventId then
    coop.lastEventId = s.eventId
    coop.toast = s.event
    coop.toastT = 3.0
  end
end

local function clientUpdate(dt)
  coop.myClientId = tonumber(GetVar("myClientId")) or coop.myClientId

  for _ = 1, 32 do
    local cmd = NetCommand()
    if cmd == nil then break end
    if cmd.name == "game_state" then
      local s = nil
      if type(cmd.args) == "string" and cmd.args ~= "" then s = Json.Parse(cmd.args) end
      if type(s) == "table" then applyState(s) end
    elseif cmd.name == "game_world" then
      local w = nil
      if type(cmd.args) == "string" and cmd.args ~= "" then w = Json.Parse(cmd.args) end
      if type(w) == "table" and w.furniture ~= nil then
        local seq = tonumber(w.worldSeq) or 0
        if seq ~= coop.worldSeqApplied then
          local part = math.max(1, math.floor(tonumber(w.part) or 1))
          local total = math.max(1, math.floor(tonumber(w.total) or 1))
          if total <= 1 then
            coop.worldSeqApplied = seq
            coop.worldParts = nil
            applyFurnitureList(w.furniture)
          else
            if coop.worldParts == nil or coop.worldParts.seq ~= seq then
              coop.worldParts = { seq = seq, total = total, parts = {} }
            end
            coop.worldParts.parts[part] = w.furniture
            local complete = true
            for i = 1, total do
              if coop.worldParts.parts[i] == nil then complete = false break end
            end
            if complete then
              local all = {}
              for i = 1, total do
                for _, e in ipairs(coop.worldParts.parts[i]) do all[#all + 1] = e end
              end
              coop.worldSeqApplied = seq
              coop.worldParts = nil
              applyFurnitureList(all)
            end
          end
        end
      end
    elseif cmd.name == "game_crates" then
      local w = nil
      if type(cmd.args) == "string" and cmd.args ~= "" then w = Json.Parse(cmd.args) end
      if type(w) == "table" and w.crates ~= nil then
        local seq = tonumber(w.worldSeq) or 0
        coop.crateList = w.crates
        if seq ~= coop.crateSeqApplied then
          coop.crateSeqApplied = seq
          rebuildCrates(w.crates)
        end
      end
    end
  end

  -- Shatter display windows when any replicated player steps inside a POI.
  if breakPoiWindowsAt and coop.state and coop.state.players then
    for i = 1, MAX_SLOTS do
      local p = coop.state.players[i]
      if p ~= nil and p.j == 1 then
        if breakPoiWindowsAt(p.x or 0, p.z or 0, "enter") > 0 then
          print(string.format("[coop-glass] player %d broke the POI windows", i))
        end
      end
    end
  end

  -- Server-authoritative warehouse roll-up door.
  if setPoiDoorOpen and coop.state ~= nil and coop.state.rollup ~= nil then
    setPoiDoorOpen("depot", tonumber(coop.state.rollup) == 1)
  end

  -- Server-authoritative barn sliding door.
  if setPoiSlidingDoorOpen and coop.state ~= nil and coop.state.barndoor ~= nil then
    setPoiSlidingDoorOpen("farm", tonumber(coop.state.barndoor) == 1)
  end

  -- Server-authoritative broken glass list (comma separated POI ids).
  if applyPoiGlassState and coop.state ~= nil and coop.state.glass ~= nil then
    applyPoiGlassState(tostring(coop.state.glass))
  end

  local mx, my = InputMouseX(), InputMouseY()
  coop.lookYaw = coop.lookYaw - mx * 0.0025
  coop.lookPitch = clamp(coop.lookPitch - my * 0.0025, -0.95, 0.65)

  local f = clamp(InputAxis("forward"), -1, 1)
  local s = clamp(InputAxis("strafe"), -1, 1)
  local driving = false
  local inside = false
  local me = nil
  if coop.state and coop.state.players and coop.state.players[coop.mySlot] then
    me = coop.state.players[coop.mySlot]
    driving = (me.st == 2)
    inside = (me.ins == 1)
  end

  if not coop.buildMode and ActionPressed("b") then
    setCoopBuildMode(true)
  end
  if coop.buildUi and not coop.buildUiDone and coop.haveSelf then
    coop.buildUiDone = true
    setCoopBuildMode(true)
    coop.buildLayer = "roof"
    coop.buildIndex = 9
  end
  if coop.buildMode then
    f, s = 0, 0
    coopBuildUpdate()
  end
  if inside then
    f, s = 0, 0
  end

  if coop.haveSelf and not driving and not coop.buildMode and not inside then
    local fx, fz = math.sin(coop.lookYaw), math.cos(coop.lookYaw)
    local rx, rz = math.cos(coop.lookYaw), -math.sin(coop.lookYaw)
    local nx = coop.localPos.x + (fx * f + rx * s) * PLAYER_SPEED * dt
    local nz = coop.localPos.z + (fz * f + rz * s) * PLAYER_SPEED * dt
    nx, nz = collidePoiWalls(coop.localPos.x, coop.localPos.z, nx, nz)
    coop.localPos.x, coop.localPos.z = nx, nz
    if coop.clampDemo then
      coop.localPos.x = clamp(coop.localPos.x, -4.0, 4.0)
      coop.localPos.z = clamp(coop.localPos.z, -13.0, -6.5)
    else
      coop.localPos.x = clamp(coop.localPos.x, -WORLD_LIMIT, WORLD_LIMIT)
      coop.localPos.z = clamp(coop.localPos.z, -WORLD_LIMIT, WORLD_LIMIT)
    end
    coop.localPos.yaw = coop.lookYaw
    local ent = coop.proxies[coop.mySlot]
    if ent ~= nil then
      SetPosition(ent, { x = coop.localPos.x, y = 0, z = coop.localPos.z })
      SetRotationY(ent, coop.lookYaw)
    end
  end

  coop.sendT = coop.sendT + dt
  if coop.sendT >= 0.1 and GetVar("netConnected") == 1 then
    coop.sendT = 0
    Rpc("game_cmd", { type = "input", f = f, s = s, yaw = coop.lookYaw })
  end

  if not coop.buildMode then
    if ActionPressed("f") then Rpc("game_cmd", { type = "drive" }) end
    if ActionPressed("l") then Rpc("game_cmd", { type = "lights" }) end
    if ActionPressed("g") then Rpc("game_cmd", { type = "toggle_stove" }) end
    if ActionPressed("1") then Rpc("game_cmd", { type = "eat" }) end
    if ActionPressed("2") then Rpc("game_cmd", { type = "drink" }) end
    if ActionPressed("3") then Rpc("game_cmd", { type = "sleep" }) end
    if ActionPressed("e") then
      if inside then
        Rpc("game_cmd", { type = "exit" })
      else
        local dx, dz = doorWorld()
        local dd = math.sqrt((coop.localPos.x - dx) ^ 2 + (coop.localPos.z - dz) ^ 2)
        if dd <= 3.5 then
          Rpc("game_cmd", { type = "board" })
        else
          local best, bestD = nil, 999
          local p = coop.state and coop.state.players and coop.state.players[coop.mySlot]
          local px, pz = coop.localPos.x, coop.localPos.z
          if p ~= nil then px, pz = p.x or px, p.z or pz end
          for i, c in ipairs(coop.crateList) do
            local d = math.sqrt((c.x - px) ^ 2 + (c.z - pz) ^ 2)
            if d < bestD then best, bestD = i, d end
          end
          if best ~= nil and bestD <= 5.5 then
            Rpc("game_cmd", { type = "search", index = best })
          else
            coop.toast = "附近没有车门/物资箱（走近再按 E）"
            coop.toastT = 2.0
          end
        end
      end
    end
  end

  -- Automated co-op build proof: the first client places a roof solar panel,
  -- then a storage box, then removes the storage box. Both clients receive the
  -- resulting game_world broadcasts.
  if coop.buildDemo and coop.haveSelf and coop.mySlot == 1
     and GetVar("netConnected") == 1 then
    coop.buildDemoT = coop.buildDemoT + dt
    if coop.buildDemoStage == 0 and coop.buildDemoT >= 5.0 then
      coop.buildDemoStage = 1
      Rpc("game_cmd", { type = "place", item = "solar", cx = 0, cz = 0, rot = 0,
                        layer = "roof" })
    elseif coop.buildDemoStage == 1 and coop.buildDemoT >= 6.0 then
      coop.buildDemoStage = 2
      Rpc("game_cmd", { type = "place", item = "storage", cx = 0, cz = 0, rot = 0,
                        layer = "floor" })
    elseif coop.buildDemoStage == 2 and coop.buildDemoT >= 8.0 then
      coop.buildDemoStage = 3
      Rpc("game_cmd", { type = "remove", cx = 0, cz = 0, layer = "floor" })
    end
  end

  -- Automated boarding proof: the first client boards at 5s and exits at 9s.
  if coop.boardDemo and coop.haveSelf and coop.mySlot == 1
     and GetVar("netConnected") == 1 then
    coop.boardDemoT = coop.boardDemoT + dt
    if coop.boardDemoStage == 0 and coop.boardDemoT >= 5.0 then
      coop.boardDemoStage = 1
      Rpc("game_cmd", { type = "board" })
    elseif coop.boardDemoStage == 1 and coop.boardDemoT >= 9.0 then
      coop.boardDemoStage = 2
      Rpc("game_cmd", { type = "exit" })
    end
  end

  -- Smoothly follow the authoritative RV pose, then move the main-world model.
  local targetAngle = (coop.state and coop.state.door == 1) and (math.pi * 0.5) or 0
  coop.doorAngle = coop.doorAngle + (targetAngle - coop.doorAngle) * math.min(1, dt * 6)
  rvDoorAngle = coop.doorAngle
  rv.x = rv.x + (coop.rv.x - rv.x) * math.min(1, dt * 12)
  rv.z = rv.z + (coop.rv.z - rv.z) * math.min(1, dt * 12)
  rv.yaw = coop.rv.yaw
  rv.speed = coop.rv.speed or 0
  fuel, water, day, clock = coop.fuel, coop.water, coop.day, coop.clock
  applyRvTransform()
  -- Inside the RV: hide the exterior shell so the first-person view sees the
  -- interior (the door is a separate mesh and keeps swinging).
  if rvShell ~= nil then SetVisible(rvShell, not inside) end
  applyLights()
  updateDayNight(0)
  recomputeStats()

  coop.animT = coop.animT + dt

  -- Avatars: own = local prediction / first-person, remote = replicated proxy.
  for i = 1, MAX_SLOTS do
    local v = coop.visuals[i]
    if v ~= nil then
      local p = coop.state and coop.state.players and coop.state.players[i]
      local joined = p ~= nil and p.j == 1
      local pInside = p ~= nil and p.ins == 1
      local show = joined and not (pInside and i == coop.mySlot)
                   and not (coop.poiView and i == coop.mySlot)
      SetVisible(v, show)
      if show then
        local px, pz, py
        if i == coop.mySlot and coop.haveSelf and not pInside then
          px, pz, py = coop.localPos.x, coop.localPos.z, coop.lookYaw
        elseif pInside and p ~= nil then
          -- Use the authoritative seat position: the replicated proxy can lag
          -- behind the board teleport and would draw the passenger outside.
          px, pz, py = p.x or 0, p.z or 0, p.y or 0
        else
          local proxy = coop.proxies[i]
          local pos = proxy ~= nil and GetPosition(proxy) or nil
          px = pos and pos.x or (p.x or 0)
          pz = pos and pos.z or (p.z or 0)
          py = p.y or 0
        end
        local phase = coop.animPhase[i]
        local carrying = (coop.state and coop.state.carry == i) and not pInside
        if carrying and phase == "idle" then
          coop.animPhase[i] = "carry"
          PlayAnimation(v, "Carry", true, 0.15, 1.0)
          print(string.format("[coop-anim] player %d carries a crate (Carry loop)", i))
        elseif (not carrying) and phase == "carry" then
          coop.animPhase[i] = "idle"
          PlayAnimation(v, "Idle", true, 0.15, 1.0)
          print(string.format("[coop-anim] player %d finishes carrying (Idle loop)", i))
        elseif phase == nil then
          coop.animPhase[i] = pInside and "sit" or "idle"
          PlayAnimation(v, pInside and "Sit" or "Idle", true, 0.0, 1.0)
        elseif pInside and (phase == "idle" or phase == "stand" or
                            phase == "doorclose") then
          -- Reach the door handle first; the door already swings on state.door.
          coop.animPhase[i] = "dooropen"
          local ok = PlayAnimation(v, "DoorOpen", false, 0.1, 1.0)
          print(string.format(
            "[coop-anim] player %d reaches for the door handle (DoorOpen ok=%s)",
            i, tostring(ok)))
        elseif pInside and phase == "dooropen" and AnimationFinished(v) then
          coop.animPhase[i] = "stepup"
          local ok = PlayAnimation(v, "Step", false, 0.1, 1.0)
          print(string.format(
            "[coop-anim] player %d steps up into the RV (Step ok=%s)",
            i, tostring(ok)))
        elseif pInside and phase == "stepup" and AnimationFinished(v) then
          coop.animPhase[i] = "board"
          local ok = PlayAnimation(v, "Board", false, 0.1, 1.0)
          print(string.format("[coop-anim] player %d boards (Board -> Sit, ok=%s)",
                i, tostring(ok)))
        elseif (not pInside) and (phase == "sit" or phase == "board" or
                                  phase == "dooropen" or phase == "stepup") then
          coop.animPhase[i] = "stand"
          PlayAnimation(v, "Stand", false, 0.1, 1.0)
          print(string.format(
            "[coop-anim] player %d exits (Stand -> DoorClose -> Idle)", i))
        elseif (not pInside) and phase == "stand" and AnimationFinished(v) then
          coop.animPhase[i] = "doorclose"
          local ok = PlayAnimation(v, "DoorClose", false, 0.1, 1.0)
          print(string.format(
            "[coop-anim] player %d pushes the door shut (DoorClose ok=%s)",
            i, tostring(ok)))
        elseif phase == "board" and AnimationFinished(v) then
          coop.animPhase[i] = "sit"
          PlayAnimation(v, "Sit", true, 0.1, 1.0)
          print(string.format("[coop-anim] player %d sits down (Sit loop)", i))
        elseif phase == "doorclose" and AnimationFinished(v) then
          coop.animPhase[i] = "idle"
          PlayAnimation(v, "Idle", true, 0.1, 1.0)
          print(string.format("[coop-anim] player %d stands up (Idle loop)", i))
        end
        SetPosition(v, { x = px, y = 0, z = pz })
        SetScale(v, 1.0, 1.0, 1.0)
        -- Seated passengers face out of the side door so the opening shows the
        -- torso/legs; standing players keep their walk direction.
        local face = pInside and (rv.yaw - math.pi * 0.5) or py
        SetRotationY(v, face + math.pi)
      end
    end
  end

  local px, pz, pyaw
  if coop.haveSelf then
    px, pz, pyaw = coop.localPos.x, coop.localPos.z, coop.lookYaw
  else
    px, pz, pyaw = COOP_SPAWN[1].lx, COOP_SPAWN[1].lz, 0
  end
  if coop.glassView then
    -- Test-only close-up of the active POI's front windows.
    local nd = nodeById(coop.poiNode or "ruin")
    local cx, cz = rv.x, rv.z
    if nd ~= nil then cx, cz = poiCenter(nd) end
    SetVar("cameraMouseLock", 1)
    SetVar("cameraFocus", { x = cx, y = 1.45, z = cz + 2.3 })
    SetVar("cameraYaw", 0.12)
    SetVar("cameraPitch", 0.10)
    SetVar("cameraDist", 5.0)
  elseif coop.carryView then
    -- Test-only camera following slot 1 so the wired Carry clip is visible.
    local p1 = coop.state and coop.state.players and coop.state.players[1]
    local fx, fz = p1 and (p1.x or 0) or 0, p1 and (p1.z or 0) or 0
    SetVar("cameraMouseLock", 1)
    SetVar("cameraFocus", { x = fx, y = 1.05, z = fz })
    SetVar("cameraYaw", 0.85)
    SetVar("cameraPitch", 0.05)
    SetVar("cameraDist", 3.6)
  elseif coop.animDemoEnt ~= nil then
    local p = GetPosition(coop.animDemoEnt)
    SetVar("cameraMouseLock", 1)
    SetVar("cameraFocus", { x = p and p.x or 0, y = 1.0, z = p and p.z or 0 })
    SetVar("cameraYaw", 1.55)
    SetVar("cameraPitch", 0.06)
    SetVar("cameraDist", 3.0)
  elseif coop.poiExt then
    -- Test-only exterior overview: shop + forecourt/canopy from the road side.
    local nd = nodeById(coop.poiNode or "ruin")
    local cx, cz = rv.x, rv.z
    if nd ~= nil then cx, cz = poiCenter(nd) end
    SetVar("cameraMouseLock", 1)
    SetVar("cameraFocus", { x = cx - 2.0, y = 1.4, z = cz })
    SetVar("cameraYaw", 0.30)
    SetVar("cameraPitch", 0.28)
    SetVar("cameraDist", 15.0)
  elseif coop.propView then
    -- Test-only close-up of a POI product/crate cluster.
    local nd = nodeById(coop.poiNode or "ruin")
    local cx, cz = rv.x, rv.z
    if nd ~= nil then cx, cz = poiCenter(nd) end
    local fx, fy, fz, yaw, dist = cx, 0.6, cz, 0.0, 2.2
    local kind = nd and nd.id or ""
    if kind == "farm" then
      -- Packaged crates on the back-wall shelf (centre aisle view).
      fx, fy, fz = cx + 0.55, 1.34, cz - 2.20
      yaw, dist = 0.05, 2.0
    elseif kind == "depot" then
      -- Back-left labelled pallet stack, viewed from the central aisle.
      fx, fy, fz = cx - 1.95, 0.60, cz - 1.15
      yaw, dist = 0.60, 2.4
    elseif kind == "clinic" then
      -- Top row of the glass-front medicine cabinet (best lit).
      fx, fy, fz = cx - 1.40, 1.98, cz - 2.10
      yaw, dist = 0.05, 1.9
    elseif kind == "lakecamp" then
      -- Shelves and stacked supply crates on the left wall.
      fx, fy, fz = cx - 2.45, 1.05, cz + 1.45
      yaw, dist = 1.32, 2.4
    end
    SetVar("cameraMouseLock", 1)
    SetVar("cameraFocus", { x = fx, y = fy, z = fz })
    SetVar("cameraYaw", yaw)
    SetVar("cameraPitch", 0.04)
    SetVar("cameraDist", dist)
  elseif coop.poiView then
    local nd = nodeById(coop.poiNode or "ruin")
    local cx, cz = rv.x, rv.z
    if nd ~= nil then cx, cz = poiCenter(nd) end
    SetVar("cameraMouseLock", 1)
    SetVar("cameraFocus", { x = cx - 0.5, y = 1.15, z = cz - 0.6 })
    SetVar("cameraYaw", 0.55)
    SetVar("cameraPitch", 0.04)
    SetVar("cameraDist", 3.0)
  elseif coop.sitView then
    -- Test-only interior camera: look from the cab toward the passenger seat.
    SetVar("cameraMouseLock", 1)
    SetVar("cameraFocus", { x = 0.6, y = 1.10, z = rv.z - 3.00 })
    SetVar("cameraYaw", 0.0)
    SetVar("cameraPitch", 0.04)
    SetVar("cameraDist", 5.2)
  elseif coop.doorView and coop.mySlot == 2 then
    -- Test-only three-quarter rear camera for inspecting the real door mesh,
    -- the cut opening and the seated passenger.
    SetVar("cameraMouseLock", 1)
    SetVar("cameraFocus", { x = 0.9, y = 1.10, z = rv.z - 3.05 })
    SetVar("cameraYaw", 1.57)
    SetVar("cameraPitch", 0.05)
    SetVar("cameraDist", 6.0)
  elseif inside then
    -- First-person interior view: eye at the seat, shell hidden above.
    local fx, fz = math.sin(pyaw), math.cos(pyaw)
    SetVar("cameraMouseLock", 1)
    SetVar("cameraFocus", { x = px + fx * 2.0, y = 1.55, z = pz + fz * 2.0 })
    SetVar("cameraYaw", pyaw + math.pi)
    SetVar("cameraPitch", -coop.lookPitch)
    SetVar("cameraDist", 2.0)
  elseif driving then
    -- Chase view behind the moving RV (kept for outside observers/tests).
    local fx, fz = math.sin(rv.yaw), math.cos(rv.yaw)
    SetVar("cameraMouseLock", 1)
    SetVar("cameraFocus", { x = rv.x + fx * 1.5, y = 1.5, z = rv.z + fz * 1.5 })
    SetVar("cameraYaw", rv.yaw + math.pi)
    SetVar("cameraPitch", 0.32)
    SetVar("cameraDist", 16.0)
  else
    local fx, fz = math.sin(pyaw), math.cos(pyaw)
    local rx, rz = math.cos(pyaw), -math.sin(pyaw)
    SetVar("cameraMouseLock", 1)
    SetVar("cameraFocus", { x = px + fx * 5.0 + rx * 0.5, y = 1.55,
                            z = pz + fz * 5.0 + rz * 0.5 })
    SetVar("cameraYaw", pyaw + math.pi)
    SetVar("cameraPitch", -coop.lookPitch)
    SetVar("cameraDist", 10.0)
  end

  coop.toastT = math.max(0, coop.toastT - dt)
end

-- ---------------------------------------------------------------------------
-- Public API used by garage.lua
-- ---------------------------------------------------------------------------

function coopInitWorld(role)
  coop.role = role
  coop.isServer = (role == "server")
  coop.active = true
  coopRole = role
  -- The client loads the same garage scene, so static scene entities do not
  -- need reliable snapshot spawns; only the four CTransformBind proxies do.
  SetVar("netReplicateStatic", 0)
  rv.speed = 0
  player.active = false
  if player.ent ~= nil then setPlayerVisible(false) end

  for i = 1, MAX_SLOTS do
    local wx, wz = rotOf(COOP_SPAWN[i].lx, COOP_SPAWN[i].lz)
    local kind = (i == 1) and "player" or ("player" .. i)
    local ent = Spawn(kind, { x = wx, y = 0, z = wz })
    if ent ~= nil then SetRotationY(ent, rv.yaw) end
    coop.proxies[i] = ent
    coop.slots[i] = {
      client = 0, joined = false, slotIndex = i,
      key = (ent ~= nil) and EntityKey(ent) or 0,
      x = wx, z = wz, yaw = rv.yaw, f = 0, s = 0, driving = false,
      inside = false, seat = nil,
    }
  end

  if coop.isServer then
    local text = ReadText("saves/layout.json")
    if text == nil or text == "" then text = ReadText("layout.json") end
    local data = nil
    if text ~= nil and text ~= "" then data = jsonDecode(text) end
    if type(data) == "table" and type(data.modules) == "table" then
      loadLayout() -- standard RV/needs/inventory/furniture/POIs
      if type(data.coop) == "table" then
        coop.lights = not (data.coop.lights == 0 or data.coop.lights == false)
        if type(data.coop.slots) == "table" then
          for i, row in ipairs(data.coop.slots) do
            local s = coop.slots[i]
            if s ~= nil and row.name ~= nil then
              s.name = tostring(row.name)
              s.x = tonumber(row.x) or s.x
              s.z = tonumber(row.z) or s.z
              s.yaw = tonumber(row.yaw) or s.yaw
            end
          end
        end
        if type(data.coop.crates) == "table" then
          rebuildCrates(data.coop.crates)
        else
          spawnCrates()
        end
      else
        spawnCrates()
      end
      copyGlobals()
      for i = 1, MAX_SLOTS do
        local s = coop.slots[i]
        if coop.proxies[i] ~= nil then
          SetPosition(coop.proxies[i], { x = s.x, y = 0, z = s.z })
          SetRotationY(coop.proxies[i], s.yaw)
        end
      end
      rv.x, rv.z, rv.yaw = coop.rv.x, coop.rv.z, coop.rv.yaw
      applyRvTransform()
      if rvBody ~= nil then
        PhysicsSetPosition(rvBody, { x = rv.x, y = 0.9, z = rv.z })
        PhysicsSetVelocity(rvBody, { x = 0, y = 0, z = 0 })
      end
      applyLights()
      coop.loadedSave = true
      print(string.format("[coop-main] loaded save rv=(%.2f,%.2f) crates=%d slots=%d fuel=%.1f water=%.1f",
            rv.x, rv.z, #crates, #coop.slots, fuel, water))
    else
      copyGlobals()
      spawnCrates()
      print("[coop-main] no co-op save; fresh world")
    end
    coop.worldSeq = 1
    coop.worldT = 0.99
    coop.clampDemo = readFlag("coop_clamp.txt")
    coop.autoDrive = readFlag("coop_drive.txt")
    coop.actionsDemo = readFlag("coop_actions.txt")
    coop.boardDemo = readFlag("coop_board.txt")
    local doorTest = ReadText("coop_doortest.txt")
    if doorTest ~= nil and doorTest ~= "" then
      local v = string.lower((doorTest:gsub("%s+", "")))
      coop.doorDemo = true
      coop.doorNode = (v == "farm") and "farm" or "depot"
      coop.poiNode = coop.doorNode
    end
    coop.poiDemo = readFlag("coop_ruin.txt") or readFlag("coop_gas.txt") or
                    readFlag("coop_depot.txt") or readFlag("coop_farm.txt") or
                    readFlag("coop_mall.txt") or readFlag("coop_lake.txt") or
                    readFlag("coop_clinic.txt")
    if readFlag("coop_gas.txt") then
      coop.poiNode = "gasstop"
    elseif readFlag("coop_ruin.txt") then
      coop.poiNode = "ruin"
    elseif readFlag("coop_depot.txt") then
      coop.poiNode = "depot"
    elseif readFlag("coop_farm.txt") then
      coop.poiNode = "farm"
    elseif readFlag("coop_mall.txt") then
      coop.poiNode = "mall"
    elseif readFlag("coop_lake.txt") then
      coop.poiNode = "lakecamp"
    elseif readFlag("coop_clinic.txt") then
      coop.poiNode = "clinic"
    end
    if coop.poiDemo then
      local nd = nodeById(coop.poiNode or "ruin")
      if nd ~= nil then
        local cx, cz = poiCenter(nd)
        atNode, curNode = nd.id, nd.id
        rv.x, rv.z, rv.yaw = nd.x, nd.z, 0
        coop.rv = { x = nd.x, z = nd.z, yaw = 0, speed = 0 }
        spawnCrates() -- re-roll loot inside the shop
        for i = 1, MAX_SLOTS do
          local s = coop.slots[i]
          if i <= 2 then
            s.x, s.z, s.yaw = cx, cz + 5.6 + (i - 1) * 1.1, math.pi
          end
          if coop.proxies[i] ~= nil then
            SetPosition(coop.proxies[i], { x = s.x, y = 0, z = s.z })
            SetRotationY(coop.proxies[i], s.yaw)
          end
        end
        applyRvTransform()
        print(string.format(
          "[coop-poi] demo at %s center=(%.1f,%.1f) crates=%d p1=(%.1f,%.1f) p2=(%.1f,%.1f)",
          nd.id, cx, cz, #crates, coop.slots[1].x, coop.slots[1].z,
          coop.slots[2].x, coop.slots[2].z))
      end
    end
    print(string.format("[coop-main] server init proxies=%d/%d/%d/%d crates=%d furniture=%d",
          coop.slots[1].key, coop.slots[2].key, coop.slots[3].key, coop.slots[4].key,
          #crates, #sortedInstances()))
  else
    for i = 1, MAX_SLOTS do
      local wx, wz = rotOf(COOP_SPAWN[i].lx, COOP_SPAWN[i].lz)
      local v = SpawnPrefab("rv_player", { x = wx, y = 0, z = wz })
      if v ~= nil then
        SetRotationY(v, rv.yaw + math.pi)
        SetVisible(v, false)
      end
      coop.visuals[i] = v
    end
    coop.doorAngle = 0
    coop.doorTarget = 0
    coop.animT = 0
    if readFlag("coop_sitdemo.txt") then
      local sx, sz = rotOf(1.5, -5.0)
      local v = SpawnPrefab("rv_player", { x = sx, y = 0, z = sz })
      if v ~= nil then
        SetRotationY(v, rv.yaw + math.pi)
        local ok = PlayAnimation(v, "Sit", true, 0.2, 1.0)
        print("[coop-anim] sit demo avatar=" .. tostring(ok))
      end
    end
    local ad = ReadText("coop_animdemo.txt")
    if ad ~= nil and ad ~= "" then
      local clip = string.lower((ad:gsub("%s+", "")))
      local clipName = nil
      if clip == "sit" then clipName = "Sit"
      elseif clip == "board" then clipName = "Board"
      elseif clip == "stand" then clipName = "Stand"
      elseif clip == "dooropen" then clipName = "DoorOpen"
      elseif clip == "doorclose" then clipName = "DoorClose"
      elseif clip == "step" then clipName = "Step"
      elseif clip == "carry" then clipName = "Carry"
      end
      if clipName ~= nil then
        local sx, sz = rotOf(4.0, -1.0)
        local v = SpawnPrefab("rv_player", { x = sx, y = 0, z = sz })
        if v ~= nil then
          SetRotationY(v, rv.yaw + math.pi)
          local ok = PlayAnimation(v, clipName, true, 0.0, 1.0)
          coop.animDemo = clipName
          coop.animDemoEnt = v
          print("[coop-anim] demo clip=" .. clipName .. " ok=" .. tostring(ok))
        end
      end
    end
    coop.localPos.x, coop.localPos.z = rotOf(COOP_SPAWN[1].lx, COOP_SPAWN[1].lz)
    coop.lookYaw = rv.yaw
    coop.clampDemo = readFlag("coop_clamp.txt")
    coop.buildDemo = readFlag("coop_build.txt")
    coop.buildUi = readFlag("coop_buildmode.txt")
    coop.boardDemo = readFlag("coop_board.txt")
    coop.sitDemo = readFlag("coop_sitdemo.txt")
    coop.doorView = readFlag("coop_doorview.txt")
    coop.sitView = readFlag("coop_sitview.txt")
    coop.poiView = readFlag("coop_poiview.txt")
    coop.poiExt = readFlag("coop_poiext.txt")
    coop.carryView = readFlag("coop_carryview.txt")
    coop.glassView = readFlag("coop_glassview.txt")
    coop.propView = readFlag("coop_propview.txt")
    local doorTest = ReadText("coop_doortest.txt")
    if doorTest ~= nil and doorTest ~= "" then
      local v = string.lower((doorTest:gsub("%s+", "")))
      coop.poiNode = (v == "farm") and "farm" or "depot"
    end
    if readFlag("coop_gas.txt") then
      coop.poiNode = "gasstop"
    elseif readFlag("coop_ruin.txt") then
      coop.poiNode = "ruin"
    elseif readFlag("coop_depot.txt") then
      coop.poiNode = "depot"
    elseif readFlag("coop_farm.txt") then
      coop.poiNode = "farm"
    elseif readFlag("coop_mall.txt") then
      coop.poiNode = "mall"
    elseif readFlag("coop_lake.txt") then
      coop.poiNode = "lakecamp"
    elseif readFlag("coop_clinic.txt") then
      coop.poiNode = "clinic"
    end
    -- Prefab shell cubes are spawned before networking starts; re-assert their
    -- transforms once so the co-op client's draw list uses the authored pose.
    if poiShellParts ~= nil then
      local n = 0
      for _, list in pairs(poiShellParts) do
        for _, p in ipairs(list) do
          -- Nudge once: static prefab entities keep their scene-load culling
          -- bounds unless the transform is actually dirtied.
          SetPosition(p.ent, { x = p.x + 0.0015, y = p.y, z = p.z })
          SetPosition(p.ent, { x = p.x, y = p.y, z = p.z })
          SetScale(p.ent, p.sx, p.sy, p.sz)
          SetVisible(p.ent, true)
          n = n + 1
        end
      end
      print(string.format("[coop-poi] refreshed shell parts=%d", n))
    end
    SetVar("cameraMouseLock", 1)
    applyLights()
    print(string.format("[coop-main] client init proxies=%d/%d/%d/%d",
          coop.slots[1].key, coop.slots[2].key, coop.slots[3].key, coop.slots[4].key))
  end
end

function coopUpdate(dt)
  if not coop.active then return end
  if coop.isServer then
    serverUpdate(dt)
  else
    clientUpdate(dt)
  end
end

local function drawBar(x, y, w, label, frac, r, g, b)
  DrawText(label, x, y, 11, 0.82, 0.82, 0.76, 1)
  DrawRect(x + 72, y + 1, w, 9, 0.10, 0.11, 0.13, 0.9)
  DrawRect(x + 72, y + 1, w * clamp(frac, 0, 1), 9, r, g, b, 0.95)
  DrawText(string.format("%.0f", frac * 100), x + 78 + w, y, 11, 0.85, 0.85, 0.78, 1)
end

function coopDrawHud()
  local vp = GetViewportSize()
  local vw = (vp and vp.w) or 1280
  local vh = (vp and vp.h) or 720

  DrawRect(14, 14, 390, 386, 0.035, 0.045, 0.055, 0.90)
  DrawRectOutline(14, 14, 390, 386, 0.52, 0.50, 0.40, 0.95)
  DrawText("房车合作 · 主世界", 26, 24, 18, 0.98, 0.90, 0.68, 1)
  DrawText(string.format("角色 %s   槽位 %d   客户端 %.0f", tostring(coop.role), coop.mySlot,
           coop.myClientId or 0), 26, 50, 11, 0.72, 0.74, 0.70, 1)

  DrawText(string.format("房车  X %.1f  Z %.1f  朝向 %.0f°  速度 %.1f", rv.x, rv.z,
           math.deg(rv.yaw), rv.speed or 0), 26, 72, 12, 0.86, 0.88, 0.82, 1)
  DrawText(string.format("燃油 %.0f%%   水 %.1fL   车门 %s   时间 第%d天 %02d:%02d   灯 %s",
           fuel, water, (coop.state and coop.state.door == 1) and "开" or "关",
           day, math.floor(clock / 60) % 24, math.floor(clock % 60),
           coop.lights and "开" or "关"), 26, 91, 12, 0.80, 0.82, 0.76, 1)
  DrawText(string.format("材料 金属%d 电子%d 布料%d 木材%d",
           materials.metal or 0, materials.electronics or 0,
           materials.cloth or 0, materials.wood or 0), 26, 110, 12, 0.80, 0.82, 0.76, 1)
  DrawText(string.format("物资箱 %d   家具 %d 件   商店内 %d 人   热饭 %d   净水 %d",
           #crates, #sortedInstances(), poiInsideCount(),
           coop.invMeal or 0, coop.invWater or 0),
           26, 128, 12, 0.80, 0.82, 0.76, 1)

  drawBar(26, 152, 200, "生命", (needs.health or 0) / 100, 0.85, 0.30, 0.30)
  drawBar(26, 172, 200, "饥饿", (needs.hunger or 0) / 100, 0.88, 0.64, 0.28)
  drawBar(26, 192, 200, "口渴", (needs.thirst or 0) / 100, 0.30, 0.58, 0.88)
  drawBar(26, 212, 200, "体力", (needs.energy or 0) / 100, 0.42, 0.80, 0.42)
  drawBar(26, 232, 200, "卫生", (needs.hygiene or 0) / 100, 0.66, 0.52, 0.86)

  DrawText(string.format("家具  灶台%s 冰箱%s 床%s 咖啡%s 种植%s 工作台%s",
           furnMark("stove"), furnMark("fridge"), furnMark("bed"),
           furnMark("coffee"), furnMark("plant"), furnMark("workbench")),
           26, 260, 11, 0.80, 0.82, 0.76, 1)

  DrawText("玩家", 26, 284, 12, 0.95, 0.88, 0.66, 1)
  for i = 1, MAX_SLOTS do
    local s = coop.slots[i]
    local text
    if s.joined then
      local tag = ""
      if s.seat == "driver" then tag = " [驾驶]"
      elseif s.inside then tag = " [车内]" end
      text = string.format("P%d 客户端%.0f (%.1f, %.1f)%s%s", i, s.client or 0, s.x, s.z,
                           (i == coop.mySlot) and " ←你" or "", tag)
    else
      text = string.format("P%d 等待加入", i)
    end
    DrawText(text, 26, 302 + (i - 1) * 17, 12, 0.80, 0.82, 0.76, 1)
  end
  DrawText("WASD 移动  L 车灯  F 驾驶  E 搜箱  1 吃  2 喝  3 睡  G 灶台",
           26, 378, 11, 0.66, 0.68, 0.64, 1)

  DrawText(string.format("网络 %s", (GetVar("netConnected") == 1) and "已连接" or "未连接"),
           vw - 170, 22, 12, 0.72, 0.78, 0.76, 1)

  if coop.buildMode then
    local cat = coopBuildCat()
    local bx, by, bw, bh = 14, vh - 258, 640, 144
    DrawRect(bx, by, bw, bh, 0.035, 0.045, 0.055, 0.92)
    DrawRectOutline(bx, by, bw, bh, 0.62, 0.58, 0.42, 0.95)
    DrawText("建造模式", bx + 14, by + 10, 16, 0.98, 0.90, 0.68, 1)
    DrawText("[1]储物箱 [2]电暖器 [3]娱乐柜 [4]药柜 [5]武器架",
             bx + 14, by + 38, 12, 0.90, 0.90, 0.84, 1)
    DrawText("[6]水槽 [7]单人床 [8]咖啡机 [9]太阳能板",
             bx + 14, by + 56, 12, 0.90, 0.90, 0.84, 1)
    local name = cat and cat.name or "?"
    local cost = cat and costText(cat.cost or {}) or ""
    DrawText(string.format("选中 %s   光标 (%d,%d) %s   旋转 %d°   材料 %s",
             name, coop.buildCx, coop.buildCz,
             (coop.buildLayer == "roof") and "车顶" or "地板",
             (coop.buildRot % 2) * 90, cost),
             bx + 14, by + 78, 12, 0.92, 0.94, 0.88, 1)
    DrawText("J/L 左右  I/K 前后  Tab 层  R 旋转  E 放置  X 拆除  B 退出",
             bx + 14, by + 98, 12, 0.76, 0.78, 0.72, 1)
    DrawText("材料 木" .. (materials.wood or 0) .. " 金" .. (materials.metal or 0) ..
             " 电" .. (materials.electronics or 0) .. " 布" .. (materials.cloth or 0),
             bx + 14, by + 118, 12, 0.76, 0.76, 0.70, 1)
  end

  if not coop.buildMode then
    local me = coop.state and coop.state.players and coop.state.players[coop.mySlot]
    local inside = me ~= nil and me.ins == 1
    local dx, dz = doorWorld()
    local dd = math.sqrt((coop.localPos.x - dx) ^ 2 + (coop.localPos.z - dz) ^ 2)
    local hint = nil
    if inside then hint = "E 下车"
    elseif dd <= 3.5 then hint = "E 上车（乘客席）"
    elseif #coop.crateList > 0 then hint = "E 搜物资箱"
    end
    if hint ~= nil then
      local tw = 18 + #hint * 14
      DrawRect((vw - tw) * 0.5, vh - 128, tw, 28, 0.05, 0.06, 0.07, 0.82)
      DrawRectOutline((vw - tw) * 0.5, vh - 128, tw, 28, 0.55, 0.52, 0.42, 0.9)
      DrawText(hint, (vw - tw) * 0.5 + 10, vh - 121, 13, 0.96, 0.90, 0.72, 1)
    end
  end

  if coop.toastT > 0 and coop.toast ~= "" then
    local tw = math.min(620, 18 + #coop.toast * 14)
    DrawRect((vw - tw) * 0.5, vh - 92, tw, 34, 0.06, 0.07, 0.08, 0.9)
    DrawRectOutline((vw - tw) * 0.5, vh - 92, tw, 34, 0.62, 0.58, 0.42, 0.9)
    DrawText(coop.toast, (vw - tw) * 0.5 + 14, vh - 82, 14, 0.98, 0.92, 0.74, 1)
  end
end

function on_player_join(clientId, name)
  if not coop.active or not coop.isServer then return end
  name = (type(name) == "string") and name or ""
  local slot = nil
  local reclaimed = false
  if name ~= "" then
    for i = 1, MAX_SLOTS do
      if coop.slots[i].name == name then
        slot = coop.slots[i]
        reclaimed = true
        break
      end
    end
  end
  if slot == nil then
    for i = 1, MAX_SLOTS do
      local s = coop.slots[i]
      if not s.joined and (s.name == nil or s.name == "") then slot = s break end
    end
  end
  if slot == nil then
    for i = 1, MAX_SLOTS do
      if not coop.slots[i].joined then slot = coop.slots[i] break end
    end
  end
  if slot == nil then
    print(string.format("[coop-main-server] join client=%.0f refused (all 4 slots busy)", clientId))
    return
  end
  local oldClient = slot.client
  if name ~= "" then slot.name = name end
  slot.joined = true
  slot.client = clientId
  slot.driving = false
  slot.f, slot.s = 0, 0
  if coop.proxies[slot.slotIndex] ~= nil then
    BindPlayerToClient(coop.proxies[slot.slotIndex], clientId)
  end
  print(string.format("[coop-main-server] player_join client=%.0f slot=%d key=%.0f name='%s'%s",
        clientId, slot.slotIndex, slot.key, slot.name or "",
        reclaimed and string.format(" (reconnect, old client=%.0f)", oldClient) or ""))
  coop.worldSeq = coop.worldSeq + 1
  coop.worldT = 0.99
  pushState()
  pushWorld()
  coop.saveDirty = true
end

function on_player_leave(clientId)
  if not coop.active or not coop.isServer then return end
  for i = 1, MAX_SLOTS do
    local s = coop.slots[i]
    if s.client == clientId then
      s.joined = false
      s.client = 0
      s.driving = false
      s.f, s.s = 0, 0
      print(string.format("[coop-main-server] player_leave client=%.0f slot=%d name='%s' (slot kept for reconnect)",
            clientId, s.slotIndex, s.name or ""))
      pushState()
      coop.saveDirty = true
      return
    end
  end
end

function on_server_stop()
  if coop.active and coop.isServer then
    coopSave()
    print("[coop-main-server] final save on shutdown")
  end
end
