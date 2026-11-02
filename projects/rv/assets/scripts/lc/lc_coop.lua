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
  Rpc("game_world", {
    seq = coop.seq,
    worldSeq = coop.worldSeq,
    furniture = buildFurnitureList(),
  })
  Rpc("game_crates", {
    seq = coop.seq,
    worldSeq = coop.worldSeq,
    crates = buildCrateList(),
  })
end

local function pushState()
  coop.seq = coop.seq + 1
  local players = {}
  local moving = math.abs(coop.rv.speed or 0) > 0.2
  for i = 1, MAX_SLOTS do
    if coop.slots[i].joined and coop.slots[i].driving then moving = true end
  end
  for i = 1, MAX_SLOTS do
    local s = coop.slots[i]
    players[i] = {
      s = i, c = s.client, x = s.x, z = s.z, y = s.yaw,
      j = s.joined and 1 or 0, d = s.driving and 1 or 0,
      a = (moving and s.joined) and 1 or 0, k = s.key,
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
  Rpc("game_state", {
    seq = coop.seq, rv = coop.rv, players = players, needs = needs,
    fuel = fuel, water = water, day = day, clock = clock,
    weather = coop.weather, lights = coop.lights and 1 or 0,
    materials = materials, inv = invList, furn = furnOn,
    crates = #crates, worldSeq = coop.worldSeq,
    event = coop.toast, eventId = coop.eventId or 0,
  })
end

local function serverDebugLog()
  print(string.format(
    "[coop-main-server] seq=%d rv=(%.2f,%.2f,yaw=%.2f speed=%.2f) p1=(%.2f,%.2f) p2=(%.2f,%.2f) p3=(%.2f,%.2f) p4=(%.2f,%.2f) joined=%d/%d fuel=%.1f water=%.1f metal=%d crates=%d meal=%d waterItem=%d stove=%d fridge=%d bed=%d",
    coop.seq, rv.x, rv.z, rv.yaw, rv.speed,
    coop.slots[1].x, coop.slots[1].z, coop.slots[2].x, coop.slots[2].z,
    coop.slots[3].x, coop.slots[3].z, coop.slots[4].x, coop.slots[4].z,
    (coop.slots[1].joined and 1 or 0) + (coop.slots[2].joined and 1 or 0) +
    (coop.slots[3].joined and 1 or 0) + (coop.slots[4].joined and 1 or 0),
    MAX_SLOTS, fuel, water, materials.metal or 0, #crates,
    inv.meal or 0, inv.water or 0,
    furnState("stove") and 1 or 0, furnState("fridge") and 1 or 0,
    furnState("bed") and 1 or 0))
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
  elseif t == "drive" then
    s.driving = not s.driving
    if s.driving then
      for i = 1, MAX_SLOTS do
        if coop.slots[i] ~= s then coop.slots[i].driving = false end
      end
      setToast(string.format("P%d 坐上了驾驶席", s.slotIndex or 0))
    else
      setToast("驾驶员离开了驾驶席")
    end
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

local function serverUpdate(dt)
  drainCommands()
  coop.autoT = coop.autoT + dt

  if coop.autoDrive and not coop.autoDriveDone and coop.autoT >= 2.0 and coop.slots[1].joined then
    coop.autoDriveDone = true
    coop.slots[1].driving = true
    setToast("P1 坐上了驾驶席")
  end
  if coop.actionsDemo and not coop.actionsDone and coop.autoT >= 6.0
     and (coop.slots[1].joined or coop.slots[2].joined) then
    coop.actionsDone = true
    if #crates > 0 then
      searchCrate(1)
      bankStopBag(1.0)
      coop.worldSeq = coop.worldSeq + 1
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
    -- Everyone aboard rides with the RV.
    for i = 1, MAX_SLOTS do
      local s = coop.slots[i]
      if s.joined then
        local offset = (s == driver) and 1.1 or (-1.4 - (i - 1) * 0.35)
        s.x = rv.x + math.sin(rv.yaw) * offset
        s.z = rv.z + math.cos(rv.yaw) * offset
        s.yaw = rv.yaw
      end
    end
  else
    rv.speed = 0
    coop.rv.speed = 0
    for i = 1, MAX_SLOTS do
      local s = coop.slots[i]
      if s.joined then
        local fx, fz = math.sin(s.yaw), math.cos(s.yaw)
        local rx, rz = math.cos(s.yaw), -math.sin(s.yaw)
        s.x = s.x + (fx * s.f + rx * s.s) * PLAYER_SPEED * dt
        s.z = s.z + (fz * s.f + rz * s.s) * PLAYER_SPEED * dt
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
  print(string.format(
    "[coop-main-client] seq=%d name=%s slot=%d id=%.0f rv=(%.2f,%.2f,yaw=%.2f) p1=(%.2f,%.2f) p2=(%.2f,%.2f) p3=(%.2f,%.2f) p4=(%.2f,%.2f) fuel=%.1f water=%.1f metal=%d crates=%d meal=%d waterItem=%d stove=%d fridge=%d bed=%d hunger=%.1f",
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
    (s.needs and s.needs.hunger) or 0))
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
        st.driving = (p.d == 1)
        st.key = p.k or st.key
      end
    end
  end

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
          coop.worldSeqApplied = seq
          applyFurnitureList(w.furniture)
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

  local mx, my = InputMouseX(), InputMouseY()
  coop.lookYaw = coop.lookYaw - mx * 0.0025
  coop.lookPitch = clamp(coop.lookPitch - my * 0.0025, -0.95, 0.65)

  local f = clamp(InputAxis("forward"), -1, 1)
  local s = clamp(InputAxis("strafe"), -1, 1)
  local driving = false
  if coop.state and coop.state.players and coop.state.players[coop.mySlot] then
    local p = coop.state.players[coop.mySlot]
    driving = (p.d == 1) or (p.a == 1)
  end

  if coop.haveSelf and not driving then
    local fx, fz = math.sin(coop.lookYaw), math.cos(coop.lookYaw)
    local rx, rz = math.cos(coop.lookYaw), -math.sin(coop.lookYaw)
    coop.localPos.x = coop.localPos.x + (fx * f + rx * s) * PLAYER_SPEED * dt
    coop.localPos.z = coop.localPos.z + (fz * f + rz * s) * PLAYER_SPEED * dt
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

  if ActionPressed("f") then Rpc("game_cmd", { type = "drive" }) end
  if ActionPressed("l") then Rpc("game_cmd", { type = "lights" }) end
  if ActionPressed("g") then Rpc("game_cmd", { type = "toggle_stove" }) end
  if ActionPressed("1") then Rpc("game_cmd", { type = "eat" }) end
  if ActionPressed("2") then Rpc("game_cmd", { type = "drink" }) end
  if ActionPressed("3") then Rpc("game_cmd", { type = "sleep" }) end
  if ActionPressed("e") then
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
      coop.toast = "附近没有物资箱（走近再按 E）"
      coop.toastT = 2.0
    end
  end

  -- Smoothly follow the authoritative RV pose, then move the main-world model.
  rv.x = rv.x + (coop.rv.x - rv.x) * math.min(1, dt * 12)
  rv.z = rv.z + (coop.rv.z - rv.z) * math.min(1, dt * 12)
  rv.yaw = coop.rv.yaw
  rv.speed = coop.rv.speed or 0
  fuel, water, day, clock = coop.fuel, coop.water, coop.day, coop.clock
  applyRvTransform()
  applyLights()
  updateDayNight(0)
  recomputeStats()

  -- Avatars: own = local prediction, remote = replicated proxy.
  for i = 1, MAX_SLOTS do
    local v = coop.visuals[i]
    if v ~= nil then
      local p = coop.state and coop.state.players and coop.state.players[i]
      local joined = p ~= nil and p.j == 1
      SetVisible(v, joined)
      if joined then
        local px, pz, py
        if i == coop.mySlot and coop.haveSelf then
          px, pz, py = coop.localPos.x, coop.localPos.z, coop.lookYaw
        else
          local proxy = coop.proxies[i]
          local pos = proxy ~= nil and GetPosition(proxy) or nil
          px = pos and pos.x or (p.x or 0)
          pz = pos and pos.z or (p.z or 0)
          py = p.y or 0
        end
        SetPosition(v, { x = px, y = 0, z = pz })
        SetRotationY(v, py + math.pi)
      end
    end
  end

  local px, pz, pyaw
  if coop.haveSelf then
    px, pz, pyaw = coop.localPos.x, coop.localPos.z, coop.lookYaw
  else
    px, pz, pyaw = COOP_SPAWN[1].lx, COOP_SPAWN[1].lz, 0
  end
  if driving then
    -- Chase view behind the moving RV, outside the shell (the third-person
    -- avatar camera would sit inside the interior while everyone is aboard).
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
    coop.autoDrive = readFlag("coop_drive.txt")
    coop.actionsDemo = readFlag("coop_actions.txt")
    coop.clampDemo = readFlag("coop_clamp.txt")
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
    coop.localPos.x, coop.localPos.z = rotOf(COOP_SPAWN[1].lx, COOP_SPAWN[1].lz)
    coop.lookYaw = rv.yaw
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
  DrawText(string.format("燃油 %.0f%%   水 %.1fL   时间 第%d天 %02d:%02d   灯 %s",
           fuel, water, day, math.floor(clock / 60) % 24, math.floor(clock % 60),
           coop.lights and "开" or "关"), 26, 91, 12, 0.80, 0.82, 0.76, 1)
  DrawText(string.format("材料 金属%d 电子%d 布料%d 木材%d",
           materials.metal or 0, materials.electronics or 0,
           materials.cloth or 0, materials.wood or 0), 26, 110, 12, 0.80, 0.82, 0.76, 1)
  DrawText(string.format("物资箱 %d   热饭 %d   净水 %d",
           #crates, coop.invMeal or 0, coop.invWater or 0),
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
      text = string.format("P%d 客户端%.0f (%.1f, %.1f)%s%s", i, s.client or 0, s.x, s.z,
                           (i == coop.mySlot) and " ←你" or "",
                           s.driving and " [驾驶]" or "")
    else
      text = string.format("P%d 等待加入", i)
    end
    DrawText(text, 26, 302 + (i - 1) * 17, 12, 0.80, 0.82, 0.76, 1)
  end
  DrawText("WASD 移动  L 车灯  F 驾驶  E 搜箱  1 吃  2 喝  3 睡  G 灶台",
           26, 378, 11, 0.66, 0.68, 0.64, 1)

  DrawText(string.format("网络 %s", (GetVar("netConnected") == 1) and "已连接" or "未连接"),
           vw - 170, 22, 12, 0.72, 0.78, 0.76, 1)
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
