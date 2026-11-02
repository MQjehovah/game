-- coop_test.lua
-- Minimal RV co-op vertical slice.
--
-- One scene + one script runs on the authoritative server and on every client.
-- The server owns the shared RV pose, the two player proxies, the household
-- needs and the furniture state, and broadcasts them as "game_state" RPCs.
-- Clients send input/actions as "game_cmd" RPCs and render the authoritative
-- proxies (CTransformBind snapshots) plus local prefab visuals.
--
-- The first joined client owns proxy 1, the second owns proxy 2. The server
-- publishes each proxy's EntityKey and the client claims it through the
-- "netControlKey" GameVar, so both players get real per-entity prediction.

local MAX_SLOTS = 2
local PLAYER_SPEED = 3.2
local RV_SPEED = 5.5
local ARENA = { xmin = -5.0, xmax = 5.0, zmin = -4.0, zmax = 5.0 }
local SPAWN = {
  { x = -1.8, z = 0.0, yaw = 0.0 },
  { x =  1.8, z = 0.0, yaw = 0.0 },
}

local role = nil
local roleReady = false
local isClient = false
local isServer = false
local offline = false

local slots = {}
local rv = {}
local needs = {}
local modules = {}
local water = 64.0
local meals = 3
local fuel = 82.0
local clockMin = 8 * 60
local lights = false
local lastEvent = ""
local eventId = 0
local stateSeq = 0

local playerEnts = {}
local rvEnt = nil
local avatarVisuals = {}
local rvVisual = nil
local rvView = { x = 0, z = 14, yaw = math.pi }

local mySlot = 1
local myClientId = nil
local localPos = { x = 0, z = 0, yaw = 0 }
local haveSelf = false
local lookYaw = 0.0
local lookPitch = -0.05
local sendT = 0.0
local stateT = 0.0
local logT = 0.0
local state = nil
local eventSeen = -1
local lastLogSeq = -1
local toastText = ""
local toastT = 0.0
local driveDemo = false
local driveDemoDone = false
local driveDemoT = 0.0

local function clamp(v, lo, hi)
  if v < lo then return lo end
  if v > hi then return hi end
  return v
end

local function currentRole()
  local r = GetVar("netRole")
  if type(r) == "string" then return r end
  return "offline"
end

local function resetState()
  rv = { x = 0.0, z = 14.0, yaw = math.pi * 0.5, speed = 0.0 }
  fuel = 82.0
  water = 64.0
  meals = 3
  clockMin = 8 * 60
  lights = false
  needs = { health = 100, hunger = 78, thirst = 74, energy = 90, hygiene = 76 }
  modules = { stove = true, fridge = true, bed = true, coffee = true, plant = true, workbench = true }
  slots = {}
  for i = 1, MAX_SLOTS do
    slots[i] = {
      client = 0, joined = false, key = 0, slotIndex = i,
      x = SPAWN[i].x, z = SPAWN[i].z, yaw = SPAWN[i].yaw,
      f = 0.0, s = 0.0, driving = false,
    }
  end
end

local function spawnProxies()
  playerEnts = {}
  local p1 = Spawn("player", { x = slots[1].x, y = 0.0, z = slots[1].z })
  if p1 ~= nil then SetRotationY(p1, slots[1].yaw) end
  playerEnts[1] = p1
  local p2 = Spawn("player2", { x = slots[2].x, y = 0.0, z = slots[2].z })
  if p2 ~= nil then SetRotationY(p2, slots[2].yaw) end
  playerEnts[2] = p2
  rvEnt = Spawn("rv", { x = rv.x, y = 0.0, z = rv.z })
  if rvEnt ~= nil then SetRotationY(rvEnt, rv.yaw) end
  slots[1].key = (p1 ~= nil) and EntityKey(p1) or 0
  slots[2].key = (p2 ~= nil) and EntityKey(p2) or 0
end

local function spawnVisuals()
  if rvVisual == nil then
    rvVisual = SpawnPrefab("rv_body", { x = rv.x, y = 0.0, z = rv.z })
    if rvVisual ~= nil then SetRotationY(rvVisual, rv.yaw) end
  end
  for i = 1, MAX_SLOTS do
    if avatarVisuals[i] == nil then
      local v = SpawnPrefab("rv_player", { x = SPAWN[i].x, y = 0.0, z = SPAWN[i].z })
      if v ~= nil then SetRotationY(v, SPAWN[i].yaw) end
      avatarVisuals[i] = v
    end
  end
end

local function slotOfClient(clientId)
  for i = 1, MAX_SLOTS do
    if slots[i].client == clientId then return slots[i] end
  end
  return nil
end

local function setEvent(text)
  lastEvent = text
  eventId = eventId + 1
  print(string.format("[coop-event] %s", text))
end

local function handleCommand(clientId, a)
  local s = slotOfClient(clientId)
  if s == nil or type(a) ~= "table" then return end
  local t = a.type
  if t == "input" then
    local f = tonumber(a.f) or 0.0
    local st = tonumber(a.s) or 0.0
    s.f = clamp(f, -1.0, 1.0)
    s.s = clamp(st, -1.0, 1.0)
    local yaw = tonumber(a.yaw)
    if yaw ~= nil and yaw == yaw and math.abs(yaw) < 1000.0 then s.yaw = yaw end
  elseif t == "drive" then
    s.driving = not s.driving
    if s.driving then
      for i = 1, MAX_SLOTS do
        if slots[i] ~= s then slots[i].driving = false end
      end
      setEvent(string.format("P%d 坐上了驾驶席", s.slotIndex or 0))
    else
      setEvent("驾驶员离开了驾驶席")
    end
  elseif t == "lights" then
    lights = not lights
    setEvent(lights and "车内灯已打开" or "车内灯已关闭")
  elseif t == "eat" then
    if meals > 0 then
      meals = meals - 1
      needs.hunger = math.min(100.0, needs.hunger + 30.0)
      setEvent("吃了一顿饭（饥饿 +30）")
    else
      setEvent("没有存粮了 — 去灶台做饭")
    end
  elseif t == "drink" then
    if water >= 0.5 then
      water = water - 0.5
      needs.thirst = math.min(100.0, needs.thirst + 25.0)
      setEvent("喝了一杯水（口渴 +25）")
    else
      setEvent("水箱空了")
    end
  elseif t == "cook" then
    if water >= 0.5 then
      water = water - 0.5
      meals = meals + 1
      needs.hunger = math.min(100.0, needs.hunger + 5.0)
      setEvent("灶台做好一份热饭（存粮 +1）")
    else
      setEvent("水箱没水，做不了饭")
    end
  elseif t == "sleep" then
    clockMin = clockMin + 6 * 60
    if clockMin >= 24 * 60 then clockMin = clockMin - 24 * 60 end
    needs.energy = math.min(100.0, needs.energy + 55.0)
    setEvent("在房车床上睡了 6 小时（体力 +55）")
  end
end

-- Fill slotIndex for nicer event text.
local function refreshSlotIndexes()
  for i = 1, MAX_SLOTS do slots[i].slotIndex = i end
end

local function drainCommands()
  for _ = 1, 32 do
    local cmd = NetCommand()
    if cmd == nil then break end
    if cmd.name == "game_cmd" or cmd.name == "moba_cmd" then
      local a = nil
      if type(cmd.args) == "string" and cmd.args ~= "" then a = Json.Parse(cmd.args) end
      if type(a) == "table" then handleCommand(cmd.client, a) end
    end
  end
end

local function pushState()
  stateSeq = stateSeq + 1
  local players = {}
  for i = 1, MAX_SLOTS do
    local s = slots[i]
    players[i] = {
      slot = i, client = s.client, key = s.key,
      x = s.x, z = s.z, yaw = s.yaw,
      driving = s.driving, joined = s.joined,
    }
  end
  Rpc("game_state", {
    seq = stateSeq,
    rv = { x = rv.x, z = rv.z, yaw = rv.yaw, speed = rv.speed },
    fuel = fuel, water = water, meals = meals, clock = clockMin,
    lights = lights, needs = needs, modules = modules,
    players = players, event = lastEvent, eventId = eventId,
  })
end

local function serverUpdate(dt)
  refreshSlotIndexes()
  drainCommands()

  if offline and slots[1].joined then
    slots[1].f = clamp(InputAxis("forward"), -1.0, 1.0)
    slots[1].s = clamp(InputAxis("strafe"), -1.0, 1.0)
    slots[1].yaw = lookYaw
  end

  local driver = nil
  for i = 1, MAX_SLOTS do
    if slots[i].joined and slots[i].driving then driver = slots[i] end
  end

  if driver ~= nil then
    local target = driver.f * RV_SPEED
    rv.speed = rv.speed + (target - rv.speed) * math.min(1.0, dt * 2.5)
    rv.yaw = rv.yaw - driver.s * dt * 1.15
    rv.x = rv.x + math.sin(rv.yaw) * rv.speed * dt
    rv.z = rv.z + math.cos(rv.yaw) * rv.speed * dt
    fuel = math.max(0.0, fuel - math.abs(rv.speed) * dt * 0.035)
    -- Everyone aboard rides with the RV: the driver at the wheel, passengers
    -- slightly behind so the shared authoritative pose carries the whole party.
    for i = 1, MAX_SLOTS do
      local s = slots[i]
      if s.joined then
        local offset = (s == driver) and 1.1 or -1.4
        s.x = rv.x + math.sin(rv.yaw) * offset
        s.z = rv.z + math.cos(rv.yaw) * offset
        s.yaw = rv.yaw
      end
    end
    rvView.x, rvView.z, rvView.yaw = rv.x, rv.z, rv.yaw
  else
    rv.speed = rv.speed * math.max(0.0, 1.0 - dt * 4.0)
    for i = 1, MAX_SLOTS do
      local s = slots[i]
      if s.joined then
        local fx, fz = math.sin(s.yaw), math.cos(s.yaw)
        local rx, rz = math.cos(s.yaw), -math.sin(s.yaw)
        s.x = clamp(s.x + (fx * s.f + rx * s.s) * PLAYER_SPEED * dt,
                    ARENA.xmin, ARENA.xmax)
        s.z = clamp(s.z + (fz * s.f + rz * s.s) * PLAYER_SPEED * dt,
                    ARENA.zmin, ARENA.zmax)
      end
    end
  end

  -- Shared household needs decay fast enough to be visible in a test run.
  needs.hunger = math.max(0.0, needs.hunger - 0.8 * dt)
  needs.thirst = math.max(0.0, needs.thirst - 1.0 * dt)
  needs.energy = math.max(0.0, needs.energy - 0.35 * dt)
  needs.hygiene = math.max(0.0, needs.hygiene - 0.15 * dt)
  if needs.hunger <= 0.0 or needs.thirst <= 0.0 then
    needs.health = math.max(0.0, needs.health - 2.0 * dt)
  end
  clockMin = clockMin + dt * 2.0
  if clockMin >= 24 * 60 then clockMin = clockMin - 24 * 60 end

  for i = 1, MAX_SLOTS do
    local ent = playerEnts[i]
    if ent ~= nil then
      SetPosition(ent, { x = slots[i].x, y = 0.0, z = slots[i].z })
      SetRotationY(ent, slots[i].yaw)
    end
  end
  if rvEnt ~= nil then
    SetPosition(rvEnt, { x = rv.x, y = 0.0, z = rv.z })
    SetRotationY(rvEnt, rv.yaw)
  end

  if not offline then
    stateT = stateT + dt
    if stateT >= 0.1 then
      stateT = 0.0
      pushState()
    end
    logT = logT + dt
    if logT >= 2.0 then
      logT = 0.0
      print(string.format(
        "[coop-server] rv=(%.2f,%.2f,yaw=%.2f speed=%.2f) p1=(%.2f,%.2f) p2=(%.2f,%.2f) joined=%d/%d fuel=%.1f meals=%d lights=%s stove=%d fridge=%d bed=%d",
        rv.x, rv.z, rv.yaw, rv.speed,
        slots[1].x, slots[1].z, slots[2].x, slots[2].z,
        (slots[1].joined and 1 or 0) + (slots[2].joined and 1 or 0), MAX_SLOTS,
        fuel, meals, lights and "on" or "off",
        modules.stove and 1 or 0, modules.fridge and 1 or 0, modules.bed and 1 or 0))
    end
  end
end

local function applyState(s)
  state = s
  if type(s.players) == "table" and myClientId ~= nil then
    for i, p in ipairs(s.players) do
      if p.client == myClientId then
        mySlot = i
        if not haveSelf then
          haveSelf = true
          localPos.x = p.x
          localPos.z = p.z
          localPos.yaw = p.yaw or 0.0
          lookYaw = p.yaw or 0.0
        else
          local dx, dz = p.x - localPos.x, p.z - localPos.z
          local d = math.sqrt(dx * dx + dz * dz)
          if d > 1.5 then
            localPos.x, localPos.z = p.x, p.z
          else
            localPos.x = localPos.x + dx * 0.25
            localPos.z = localPos.z + dz * 0.25
          end
        end
        if p.key ~= nil then SetVar("netControlKey", p.key) end
        break
      end
    end
  end
  if s.eventId ~= nil and s.eventId ~= eventSeen then
    eventSeen = s.eventId
    if s.event ~= nil and s.event ~= "" then
      toastText = s.event
      toastT = 3.0
    end
  end
  -- Log the same server sequence on every client so two logs can be diffed
  -- directly (10 Hz state, one line every 2 s of authoritative time).
  if s.seq ~= nil and s.seq % 20 == 0 and s.seq ~= lastLogSeq and s.rv ~= nil then
    lastLogSeq = s.seq
    local p1 = (s.players and s.players[1]) or {}
    local p2 = (s.players and s.players[2]) or {}
    print(string.format(
      "[coop-client] seq=%d name=%s slot=%d id=%.0f rv=(%.2f,%.2f,yaw=%.2f) p1=(%.2f,%.2f) p2=(%.2f,%.2f) fuel=%.1f meals=%d water=%.1f lights=%s hunger=%.1f stove=%d fridge=%d bed=%d",
      s.seq, tostring(GetVar("playerName")), mySlot, myClientId or 0,
      s.rv.x, s.rv.z, s.rv.yaw,
      p1.x or 0, p1.z or 0, p2.x or 0, p2.z or 0,
      s.fuel or 0, s.meals or 0, s.water or 0, s.lights and "on" or "off",
      (s.needs and s.needs.hunger) or 0,
      (s.modules and s.modules.stove) and 1 or 0,
      (s.modules and s.modules.fridge) and 1 or 0,
      (s.modules and s.modules.bed) and 1 or 0))
  end
end

local function drainState()
  for _ = 1, 16 do
    local cmd = NetCommand()
    if cmd == nil then break end
    if cmd.name == "game_state" then
      local s = nil
      if type(cmd.args) == "string" and cmd.args ~= "" then s = Json.Parse(cmd.args) end
      if type(s) == "table" then applyState(s) end
    end
  end
end

local function updateVisuals(dt)
  local rvpx, rvpz, rvyaw = rvView.x, rvView.z, rvView.yaw
  if isClient then
    if rvEnt ~= nil then
      local p = GetPosition(rvEnt)
      if p ~= nil then rvpx, rvpz = p.x, p.z end
    end
    if state ~= nil and state.rv ~= nil then rvyaw = state.rv.yaw end
    rvView.x = rvView.x + (rvpx - rvView.x) * math.min(1.0, dt * 12.0)
    rvView.z = rvView.z + (rvpz - rvView.z) * math.min(1.0, dt * 12.0)
    rvView.yaw = rvyaw
  else
    rvView.x, rvView.z, rvView.yaw = rv.x, rv.z, rv.yaw
  end
  if rvVisual ~= nil then
    SetPosition(rvVisual, { x = rvView.x, y = 0.0, z = rvView.z })
    SetRotationY(rvVisual, rvView.yaw)
  end

  for i = 1, MAX_SLOTS do
    local v = avatarVisuals[i]
    if v ~= nil then
      local px, pz, pyaw
      if isClient and haveSelf and i == mySlot then
        px, pz, pyaw = localPos.x, localPos.z, lookYaw
      else
        local p = (playerEnts[i] ~= nil) and GetPosition(playerEnts[i]) or nil
        if p ~= nil then px, pz = p.x, p.z
        elseif state ~= nil and state.players ~= nil and state.players[i] ~= nil then
          px, pz = state.players[i].x, state.players[i].z
        else
          px, pz = slots[i].x, slots[i].z
        end
        if state ~= nil and state.players ~= nil and state.players[i] ~= nil then
          pyaw = state.players[i].yaw or 0.0
        else
          pyaw = slots[i].yaw
        end
      end
      SetPosition(v, { x = px, y = 0.0, z = pz })
      SetRotationY(v, pyaw + math.pi)
    end
  end
end

local function updateCamera()
  local x, z, yaw
  if isClient and haveSelf then
    x, z, yaw = localPos.x, localPos.z, lookYaw
  elseif isClient then
    x, z, yaw = SPAWN[1].x, SPAWN[1].z, 0.0
  else
    x, z, yaw = slots[1].x, slots[1].z, slots[1].yaw
  end
  local fx, fz = math.sin(yaw), math.cos(yaw)
  local rx, rz = math.cos(yaw), -math.sin(yaw)
  SetVar("cameraMouseLock", 1)
  SetVar("cameraFocus", { x = x + fx * 5.0 + rx * 1.3, y = 1.55,
                          z = z + fz * 5.0 + rz * 1.3 })
  SetVar("cameraYaw", yaw + math.pi)
  SetVar("cameraPitch", -lookPitch)
  SetVar("cameraDist", 10.0)
end

local function clientUpdate(dt)
  myClientId = tonumber(GetVar("myClientId")) or myClientId
  drainState()

  local mx, my = InputMouseX(), InputMouseY()
  lookYaw = lookYaw - mx * 0.0025
  lookPitch = clamp(lookPitch - my * 0.0025, -0.95, 0.65)

  local f = clamp(InputAxis("forward"), -1.0, 1.0)
  local s = clamp(InputAxis("strafe"), -1.0, 1.0)
  local driving = state ~= nil and state.players ~= nil and
                  state.players[mySlot] ~= nil and state.players[mySlot].driving == true

  if haveSelf and not driving then
    local fx, fz = math.sin(lookYaw), math.cos(lookYaw)
    local rx, rz = math.cos(lookYaw), -math.sin(lookYaw)
    localPos.x = clamp(localPos.x + (fx * f + rx * s) * PLAYER_SPEED * dt,
                       ARENA.xmin, ARENA.xmax)
    localPos.z = clamp(localPos.z + (fz * f + rz * s) * PLAYER_SPEED * dt,
                       ARENA.zmin, ARENA.zmax)
    localPos.yaw = lookYaw
    local ent = playerEnts[mySlot]
    if ent ~= nil then
      SetPosition(ent, { x = localPos.x, y = 0.0, z = localPos.z })
      SetRotationY(ent, lookYaw)
    end
  end

  sendT = sendT + dt
  if sendT >= 0.1 and GetVar("netConnected") == 1 then
    sendT = 0.0
    Rpc("game_cmd", { type = "input", f = f, s = s, yaw = lookYaw })
  end

  if ActionPressed("l") then Rpc("game_cmd", { type = "lights" }) end
  if ActionPressed("f") then Rpc("game_cmd", { type = "drive" }) end
  if ActionPressed("e") then Rpc("game_cmd", { type = "cook" }) end
  if ActionPressed("1") then Rpc("game_cmd", { type = "eat" }) end
  if ActionPressed("2") then Rpc("game_cmd", { type = "drink" }) end
  if ActionPressed("3") then Rpc("game_cmd", { type = "sleep" }) end

  if driveDemo and not driveDemoDone then
    driveDemoT = driveDemoT + dt
    if driveDemoT >= 2.0 then
      driveDemoDone = true
      Rpc("game_cmd", { type = "drive" })
    end
  end

  updateVisuals(dt)
  updateCamera()

  toastT = math.max(0.0, toastT - dt)
end

local function drawBar(x, y, w, label, frac, r, g, b)
  DrawText(label, x, y, 11, 0.82, 0.82, 0.76, 1)
  DrawRect(x + 72, y + 1, w, 9, 0.10, 0.11, 0.13, 0.9)
  DrawRect(x + 72, y + 1, w * clamp(frac, 0, 1), 9, r, g, b, 0.95)
  DrawText(string.format("%.0f", frac * 100), x + 78 + w, y, 11, 0.85, 0.85, 0.78, 1)
end

local function drawHud()
  local vp = GetViewportSize()
  local vw = (vp and vp.w) or 1280
  local vh = (vp and vp.h) or 720

  DrawRect(14, 14, 360, 322, 0.035, 0.045, 0.055, 0.90)
  DrawRectOutline(14, 14, 360, 322, 0.52, 0.50, 0.40, 0.95)

  DrawText("房车合作 · 测试场景", 26, 24, 18, 0.98, 0.90, 0.68, 1)
  DrawText(string.format("角色 %s   槽位 %d   客户端 %.0f", tostring(role), mySlot,
           tonumber(GetVar("myClientId")) or 0), 26, 50, 11, 0.72, 0.74, 0.70, 1)

  local rx = (state and state.rv and state.rv.x) or rvView.x
  local rz = (state and state.rv and state.rv.z) or rvView.z
  local ryaw = math.deg((state and state.rv and state.rv.yaw) or rvView.yaw)
  DrawText(string.format("房车  X %.1f  Z %.1f  朝向 %.0f°", rx, rz, ryaw),
           26, 72, 12, 0.86, 0.88, 0.82, 1)
  DrawText(string.format("燃油 %.0f%%   存粮 %d   水 %.1fL   车内灯 %s",
           (state and state.fuel) or fuel, (state and state.meals) or meals,
           (state and state.water) or water,
           ((state and state.lights) and "开" or "关")),
           26, 91, 12, 0.80, 0.82, 0.76, 1)
  local mins = (state and state.clock) or clockMin
  DrawText(string.format("时间 %02d:%02d   家具 灶台/冰箱/床/咖啡/种植/工作台",
           math.floor(mins / 60) % 24, math.floor(mins % 60)),
           26, 110, 12, 0.80, 0.82, 0.76, 1)

  local nd = (state and state.needs) or needs
  drawBar(26, 138, 190, "生命", (nd.health or 0) / 100, 0.85, 0.30, 0.30)
  drawBar(26, 158, 190, "饥饿", (nd.hunger or 0) / 100, 0.88, 0.64, 0.28)
  drawBar(26, 178, 190, "口渴", (nd.thirst or 0) / 100, 0.30, 0.58, 0.88)
  drawBar(26, 198, 190, "体力", (nd.energy or 0) / 100, 0.42, 0.80, 0.42)
  drawBar(26, 218, 190, "卫生", (nd.hygiene or 0) / 100, 0.66, 0.52, 0.86)

  DrawText("玩家", 26, 246, 12, 0.95, 0.88, 0.66, 1)
  local plist = (state and state.players) or nil
  if plist == nil then
    plist = {}
    for i = 1, MAX_SLOTS do
      plist[i] = {
        joined = slots[i].joined, client = slots[i].client,
        x = slots[i].x, z = slots[i].z, driving = slots[i].driving,
      }
    end
  end
  for i = 1, MAX_SLOTS do
    local p = plist and plist[i] or nil
    local text
    if p ~= nil and p.joined then
      text = string.format("P%d  客户端 %.0f  (%.1f, %.1f)%s%s", i, p.client or 0,
                           p.x or 0, p.z or 0,
                           (i == mySlot) and "  ← 你" or "",
                           p.driving and "  [驾驶]" or "")
    else
      text = string.format("P%d  等待玩家加入", i)
    end
    DrawText(text, 26, 264 + (i - 1) * 17, 12, 0.80, 0.82, 0.76, 1)
  end
  DrawText("WASD 移动   L 车灯   F 驾驶   E 做饭   1 吃   2 喝   3 睡",
           26, 308, 11, 0.66, 0.68, 0.64, 1)

  DrawText(string.format("网络 %s   状态包 %.1fHz", (GetVar("netConnected") == 1) and "已连接" or "未连接",
           state ~= nil and 10.0 or 0.0), vw - 300, 22, 12, 0.72, 0.78, 0.76, 1)

  if toastT > 0.0 and toastText ~= "" then
    local tw = math.min(560, 18 + #toastText * 14)
    DrawRect((vw - tw) * 0.5, vh - 92, tw, 34, 0.06, 0.07, 0.08, 0.9)
    DrawRectOutline((vw - tw) * 0.5, vh - 92, tw, 34, 0.62, 0.58, 0.42, 0.9)
    DrawText(toastText, (vw - tw) * 0.5 + 14, vh - 82, 14, 0.98, 0.92, 0.74, 1)
  end
end

function on_start(e)
  resetState()
  -- Identical spawn order on server and clients keeps EntityKeys aligned.
  spawnProxies()
  print(string.format("[coop] on_start proxies p1=%.0f p2=%.0f rv=%.0f",
        slots[1].key, slots[2].key, (rvEnt ~= nil) and EntityKey(rvEnt) or 0))
end

function on_player_join(clientId)
  local slot = nil
  for i = 1, MAX_SLOTS do
    if not slots[i].joined then slot = slots[i] break end
  end
  if slot == nil then return end
  slot.joined = true
  slot.client = clientId
  slot.slotIndex = (slot == slots[1]) and 1 or 2
  if playerEnts[slot.slotIndex] ~= nil then
    BindPlayerToClient(playerEnts[slot.slotIndex], clientId)
  end
  print(string.format("[coop-server] player_join client=%.0f slot=%d key=%.0f",
        clientId, slot.slotIndex, slot.key))
  pushState()
end

function on_update(e, dt)
  if not roleReady then
    roleReady = true
    role = currentRole()
    isClient = (role == "client" or role == "spectate")
    isServer = (role == "server")
    offline = (role == "offline")
    myClientId = tonumber(GetVar("myClientId"))
    if offline then
      slots[1].joined = true
      slots[1].client = 0
    end
    if not isServer then
      spawnVisuals()
      SetVar("cameraMouseLock", 1)
      local flag = ReadText("coop_drive.txt")
      driveDemo = (flag ~= nil and flag ~= "")
    end
    print(string.format("[coop] role=%s client=%.0f visuals=%s",
          role, myClientId or 0, isServer and "no" or "yes"))
  end

  if isClient then
    clientUpdate(dt)
  else
    serverUpdate(dt)
    if not isServer then
      -- Offline single-process preview: same authoritative sim + visuals.
      updateVisuals(dt)
      updateCamera()
    end
  end
end

function on_render()
  if roleReady and not isServer then
    drawHud()
  end
end
