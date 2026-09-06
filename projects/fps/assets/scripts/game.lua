-- NeonOps core game script.
-- This is the production-style FPS layer: data-driven weapons/enemies/levels,
-- wave director, enemy AI, pickups, persistent settings/scores, pause/game
-- over/victory flow, UI HUD and first-person camera.

-- Script vars (level/next_scene) are injected per-call by the engine, not at
-- load time -- so the real values are captured in on_start, never here.
local LEVEL = 1
local NEXT_SCENE = ""

local player = nil
local state = "playing" -- playing | paused | gameover | victory
local stateTimer = 0
local clock = 0

local lookYaw = 0
local lookPitch = 0
local yvel = 0
local grounded = true
local aiming = false
local spreadHeat = 0
local bobTime = 0
local bobAmount = 0
local stepAcc = 0

local settings = { mouseSensitivity = 0.004, musicVolume = 0.7, sfxVolume = 0.8 }
local bestScore = 0

local weapons = {}
local weaponsById = {}
local gunModels = {}
local weaponIdx = 1
local weapon = nil
local mag = 0
local reserve = 0
local fireCd = 0
local reloadTime = 0
local hitMarker = 0
local muzzleFlash = 0
local recoil = 0
local hitImpact = 0
local lastHitPoint = nil
local shakeT = 0
local combo = 0
local comboTimer = 0
local waveFlash = 0
local heartbeatLatch = false

local enemies = {}
local enemyKeys = {}
local enemyByKey = {}
local enemyDefs = {}
local enemiesById = {}
local pickups = {}
local covers = {}

local levelCfg = {}
local waveIndex = 0
local waveTimer = 0
local kills = 0
local score = 0

local EYE_H = 1.6
local CAM_DIST = 2.0
local MAX_HP = 100

local function clamp(v, lo, hi)
  return math.max(lo, math.min(hi, v))
end

local function dist2d(ax, az, bx, bz)
  local dx, dz = ax - bx, az - bz
  return math.sqrt(dx * dx + dz * dz)
end

local function read_json(path)
  local text = ReadText(path)
  if text == nil or text == "" then return nil end
  return Json.Parse(text)
end

local function save_game()
  local s = settings
  local text = string.format(
    '{"settings":{"mouseSensitivity":%f,"musicVolume":%f,"sfxVolume":%f},"bestScore":%d}',
    s.mouseSensitivity, s.musicVolume, s.sfxVolume, bestScore)
  WriteText("save.json", text)
end

local function load_save()
  local root = read_json("save.json")
  if root ~= nil then
    if root.settings ~= nil then
      settings.mouseSensitivity = root.settings.mouseSensitivity or settings.mouseSensitivity
      settings.musicVolume = root.settings.musicVolume or settings.musicVolume
      settings.sfxVolume = root.settings.sfxVolume or settings.sfxVolume
    end
    bestScore = root.bestScore or 0
  end
end

local function apply_audio()
  SetBusVolume(0, 1.0)
  SetBusVolume(1, settings.sfxVolume)
  SetBusVolume(2, settings.musicVolume)
end

local function load_data()
  local w = read_json("assets/data/weapons.json")
  if w ~= nil then
    weapons = w
    for _, def in ipairs(w) do weaponsById[def.id] = def end
  end
  local e = read_json("assets/data/enemies.json")
  if e ~= nil then
    enemyDefs = e
    for _, def in ipairs(e) do enemiesById[def.id] = def end
  end
  local lv = read_json("assets/data/levels.json")
  if lv ~= nil then
    for _, l in ipairs(lv) do
      if l.id == LEVEL then
        levelCfg = l
        break
      end
    end
  end
  if levelCfg.waves == nil then
    levelCfg = {
      id = LEVEL,
      scene = "assets/scenes/level_" .. string.format("%02d", LEVEL) .. ".json",
      next = NEXT_SCENE,
      title = "Training Ground",
      arenaRadius = 24,
      cover = {},
      waves = {
        { delay = 1.0, spawns = { { type = "drone", count = 6 } } },
        { delay = 20.0, spawns = { { type = "turret", count = 2 }, { type = "drone", count = 4 } } }
      }
    }
  end
end

local function update_gun_visibility()
  for id, e in pairs(gunModels) do
    SetVisible(e, weapon ~= nil and weapon.id == id)
  end
end

local function switch_weapon(idx)
  if #weapons == 0 then
    weapon = nil
    mag, reserve = 0, 0
    return
  end
  weaponIdx = clamp(idx, 1, #weapons)
  weapon = weapons[weaponIdx]
  mag = weapon.mag or 0
  reserve = weapon.reserve or 0
  fireCd = 0
  reloadTime = 0
  update_gun_visibility()
end

-- FX light pool: point lights parked underground, flashed on demand (muzzle
-- flashes, explosions, boss aura). Colors are set per event through the
-- light component; a 0 intensity parks the light visually.
local FX_COUNT = 3
local fxLights = {}
local fxTimers = {}

local function park_fx_light(e)
  if e ~= nil then
    SetEntityComponentField(e, "light", "intensity", 0)
    SetPosition(e, { x = 0, y = -100, z = 0 })
  end
end

local function flash_fx_light(pos, color, intensity, radius, life)
  local pick = 0
  for i = 1, FX_COUNT do
    if fxTimers[i] <= 0 then pick = i break end
  end
  if pick == 0 then
    pick = 1
    for i = 2, FX_COUNT do
      if fxTimers[i] > fxTimers[pick] then pick = i end
    end
  end
  local e = fxLights[pick]
  if e == nil then return end
  SetPosition(e, pos)
  SetEntityComponentField(e, "light", "color", color)
  SetEntityComponentField(e, "light", "intensity", intensity)
  SetEntityComponentField(e, "light", "radius", radius)
  fxTimers[pick] = life
end

local function update_fx_lights(dt)
  for i = 1, FX_COUNT do
    if fxTimers[i] > 0 then
      fxTimers[i] = fxTimers[i] - dt
      if fxTimers[i] <= 0 then park_fx_light(fxLights[i]) end
    end
  end
end

-- Persistent boss menace light (follows the Juggernaut, crimson).
local bossLight = nil

local function spawn_weapon_models()
  for _, w in ipairs(weapons) do
    if w.prefab ~= nil and w.prefab ~= "" then
      local e = SpawnPrefab(w.prefab, { x = 0, y = 0, z = 0 })
      if e ~= nil then
        gunModels[w.id] = e
        SetVisible(e, false)
      end
    end
  end
  update_gun_visibility()
end

local function reload_weapon()
  if weapon == nil or reloadTime > 0 then return end
  if mag >= weapon.mag or reserve <= 0 then return end
  reloadTime = 1.35
  PlaySfx("reload")
end

local function forward_dir(yaw, pitch)
  local cp = math.cos(pitch)
  return {
    x = -math.sin(yaw) * cp,
    y = -math.sin(pitch),
    z = -math.cos(yaw) * cp,
  }
end

local function emit_burst(pos, count, speed, r, g, b)
  EmitParticles({
    pos = pos,
    count = count,
    speedMin = speed * 0.5,
    speedMax = speed,
    lifeMin = 0.08,
    lifeMax = 0.22,
    sizeStart = 0.14,
    sizeEnd = 0.02,
    color = { r = r, g = g, b = b, a = 1 },
    colorEnd = { r = r, g = g, b = b, a = 0 },
    gravity = -4,
    additive = true,
  })
end

local function ray_aabb(origin, dir, min, max)
  local tmin, tmax = 0, math.huge
  local axes = { { origin.x, dir.x, min.x, max.x },
                 { origin.y, dir.y, min.y, max.y },
                 { origin.z, dir.z, min.z, max.z } }
  for _, a in ipairs(axes) do
    local o, d, lo, hi = a[1], a[2], a[3], a[4]
    if math.abs(d) < 1e-7 then
      if o < lo or o > hi then return math.huge end
    else
      local t1 = (lo - o) / d
      local t2 = (hi - o) / d
      if t1 > t2 then t1, t2 = t2, t1 end
      tmin = math.max(tmin, t1)
      tmax = math.min(tmax, t2)
      if tmin > tmax then return math.huge end
    end
  end
  return tmin
end

local function cover_aabb(c)
  return { x = c.x - c.halfX, y = c.cy - c.halfY, z = c.z - c.halfZ },
         { x = c.x + c.halfX, y = c.cy + c.halfY, z = c.z + c.halfZ }
end

local function wall_distance(origin, dir, range)
  local best = range
  -- Ground plane (the large arena floor) stops downward shots.
  if dir.y < -0.001 then
    local t = (0 - origin.y) / dir.y
    if t > 0 and t < best then
      local px = origin.x + dir.x * t
      local pz = origin.z + dir.z * t
      if math.abs(px) <= 35 and math.abs(pz) <= 35 then best = t end
    end
  end
  -- Axis-aligned cover geometry (crates + barriers, per-type AABBs).
  for _, c in ipairs(covers) do
    local lo, hi = cover_aabb(c)
    local t = ray_aabb(origin, dir, lo, hi)
    if t < best then best = t end
  end
  return best
end

local function collide_player(pos, radius)
  for _, c in ipairs(covers) do
    local dx = pos.x - c.x
    local dz = pos.z - c.z
    local minX = c.halfX + radius
    local minZ = c.halfZ + radius
    if math.abs(dx) < minX and math.abs(dz) < minZ then
      local pushX = minX - math.abs(dx)
      local pushZ = minZ - math.abs(dz)
      if pushX < pushZ then
        pos.x = pos.x + (dx >= 0 and pushX or -pushX)
      else
        pos.z = pos.z + (dz >= 0 and pushZ or -pushZ)
      end
    end
  end
  return pos
end

local function hitscan(origin, dir, range, damage)
  local wallDist = wall_distance(origin, dir, range)
  local bestEnt, bestT = nil, wallDist
  local headshot = false
  for _, t in ipairs(enemies) do
    if t.alive then
      local p = GetPosition(t.e)
      if p ~= nil then
        local to = { x = p.x - origin.x, y = p.y - origin.y, z = p.z - origin.z }
        local along = to.x * dir.x + to.y * dir.y + to.z * dir.z
        if along > 0 and along < bestT then
          local cx = origin.x + dir.x * along
          local cy = origin.y + dir.y * along
          local cz = origin.z + dir.z * along
          local ddx, ddy, ddz = p.x - cx, p.y - cy, p.z - cz
          local dist = math.sqrt(ddx * ddx + ddy * ddy + ddz * ddz)
          -- Per-type hit sphere: a 3.5m Juggernaut is not a 1m drone.
          if dist <= (t.def.hitRadius or 1.2) then
            bestEnt, bestT = t.e, along
            headshot = cy > p.y + (t.def.scale or 1) * 0.22
          end
        end
      end
    end
  end

  local point = {
    x = origin.x + dir.x * bestT,
    y = origin.y + dir.y * bestT,
    z = origin.z + dir.z * bestT,
  }
  if bestEnt ~= nil then
    local hp = GetHealth(bestEnt)
    local dmg = damage * (headshot and 2.0 or 1.0)
    if hp ~= nil then SetHealth(bestEnt, hp - dmg) end
    -- Hit reaction: scale punch so damage is FELT on the model.
    local victim = enemyByKey[string.format("%d_%d", bestEnt.id, bestEnt.gen)]
    if victim ~= nil then victim.punch = 0.12 end
    hitImpact = 0.09
    lastHitPoint = point
    emit_burst(point, headshot and 10 or 7, 7, headshot and 1 or 1, headshot and 0.8 or 0.72, headshot and 0.3 or 0.28)
    flash_fx_light(point, { r = 1, g = 0.55, b = 0.2, a = 1 }, 1.6, 5, 0.07)
    PlaySfx("hit")
    SpawnFloatText(point.x, point.y + 0.2, point.z, "-" .. math.floor(dmg), false, 0.7)
  elseif wallDist < range then
    local wallPoint = {
      x = origin.x + dir.x * wallDist,
      y = origin.y + dir.y * wallDist,
      z = origin.z + dir.z * wallDist,
    }
    hitImpact = 0.07
    lastHitPoint = wallPoint
    emit_burst(wallPoint, 5, 5, 0.9, 0.7, 0.3)
  end
  return bestEnt, point
end

local function fire_weapon()
  if weapon == nil or reloadTime > 0 or fireCd > 0 then return end
  if mag <= 0 then
    reload_weapon()
    return
  end
  local pos = GetPosition(player)
  if pos == nil then return end

  mag = mag - 1
  fireCd = weapon.fireRate or 0.15
  local eye = { x = pos.x, y = pos.y + EYE_H, z = pos.z }
  local fwd = forward_dir(lookYaw, lookPitch)
  local right = { x = math.cos(lookYaw), y = 0, z = -math.sin(lookYaw) }
  local muzzle = {
    x = eye.x + fwd.x * 0.55 + right.x * 0.18,
    y = eye.y + fwd.y * 0.55 - 0.16,
    z = eye.z + fwd.z * 0.55 + right.z * 0.18,
  }
  local pellets = weapon.pellets or 1
  local tracerEnd = nil
  for _ = 1, pellets do
    local spread = (weapon.spread or 0.008) + spreadHeat * 0.006
    local py = lookYaw + (math.random() - 0.5) * 2 * spread
    local pp = lookPitch + (math.random() - 0.5) * 2 * spread
    local _, hitPoint = hitscan(eye, forward_dir(py, pp), weapon.range or 120, weapon.damage or 20)
    if tracerEnd == nil then tracerEnd = hitPoint end
  end
  spreadHeat = math.min(1, spreadHeat + 0.22)
  muzzleFlash = 0.055
  recoil = 1
  emit_burst(muzzle, 5, 6, 1, 0.8, 0.3)
  -- Bright tracer streak downrange + muzzle point-light flash.
  if tracerEnd ~= nil then
    local dx, dy, dz = tracerEnd.x - muzzle.x, tracerEnd.y - muzzle.y, tracerEnd.z - muzzle.z
    local len = math.sqrt(dx * dx + dy * dy + dz * dz)
    if len > 0.1 then
      EmitParticles({
        pos = muzzle,
        count = 1,
        vel = { x = dx / len * 90, y = dy / len * 90, z = dz / len * 90 },
        speedMin = 0, speedMax = 0,
        lifeMin = 0.09, lifeMax = 0.09,
        sizeStart = 0.07, sizeEnd = 0.015,
        color = { r = 1, g = 0.85, b = 0.45, a = 1 },
        colorEnd = { r = 1, g = 0.5, b = 0.1, a = 0 },
        gravity = 0, additive = true,
      })
    end
  end
  -- Ejected shell casing: arcs out to the right with gravity.
  EmitParticles({
    pos = muzzle,
    count = 1,
    vel = { x = right.x * 2.2 + (math.random() - 0.5), y = 2.0, z = right.z * 2.2 + (math.random() - 0.5) },
    speedMin = 0, speedMax = 0,
    lifeMin = 0.55, lifeMax = 0.75,
    sizeStart = 0.055, sizeEnd = 0.035,
    color = { r = 0.92, g = 0.72, b = 0.25, a = 1 },
    colorEnd = { r = 0.65, g = 0.5, b = 0.18, a = 0.9 },
    gravity = -9.8, additive = false,
  })
  flash_fx_light(muzzle, { r = 1, g = 0.72, b = 0.32, a = 1 }, 3.2, 9, 0.05)
  PlaySfx(weapon.sfx or "shoot")
  lookPitch = lookPitch + 0.006
end

local function spawn_cover()
  local list = levelCfg.cover or {}
  for i, c in ipairs(list) do
    -- Alternate tech crates and jersey barriers; barriers alternate their
    -- long axis so the arena reads as a fought-over compound, not a grid.
    local barrier = (i % 2) == 0
    local e = SpawnPrefab(barrier and "cover_barrier" or "cover_crate",
                          { x = c[1] or 0, y = 1, z = c[3] or 0 })
    if e ~= nil then
      local cover
      if barrier then
        -- Long axis alternates X / Z; the model's AABB is 2.0 x 1.0 x 0.45.
        local longZ = (i % 4) >= 2
        SetRotationY(e, longZ and math.pi / 2 or 0)
        cover = {
          e = e, x = c[1] or 0, y = 1, z = c[3] or 0,
          cy = 0.52, halfX = longZ and 0.25 or 1.0, halfY = 0.55,
          halfZ = longZ and 1.0 or 0.25,
        }
      else
        cover = {
          e = e, x = c[1] or 0, y = 1, z = c[3] or 0,
          cy = 1.0, halfX = 1.1, halfY = 1.0, halfZ = 1.1,
        }
      end
      covers[#covers + 1] = cover
    end
  end
end

local function spawn_enemy(type, pos)
  local def = enemiesById[type]
  if def == nil then return nil end
  local e = SpawnPrefab(def.prefab, { x = pos.x, y = 1, z = pos.z })
  if e == nil then return nil end
  SetScale(e, def.scale or 1, def.scale or 1, def.scale or 1)
  SetHealth(e, def.hp)
  SetEntityPlate(e, def.name, 1)
  local t = {
    e = e,
    key = string.format("%d_%d", e.id, e.gen),
    def = def,
    scale = def.scale or 1,
    home = { x = pos.x, y = 1, z = pos.z },
    alive = true,
    attackCd = 0.6 + math.random(),
    phase = math.random() * 6.28,
    punch = 0,          -- hit-reaction scale punch timer
    stepPhase = 0,      -- walk-cycle phase (ground units)
    orbitDir = math.random() < 0.5 and 1 or -1,
    orbitT = 2 + math.random() * 2,
    stateT = 0,         -- heavy walk/halt cycle timer
    halted = false,
    chargeCd = 5 + math.random() * 3, -- boss charge-up timer
    charging = 0,       -- boss charge dash time remaining
    enraged = false,
    burst = nil,        -- turret burst counter
    burstT = 0,
  }
  enemyKeys[t.key] = true
  enemyByKey[t.key] = t
  enemies[#enemies + 1] = t
  return t
end

local function spawn_pickup(kind, pos)
  local name = kind == "health" and "pickup_health" or "pickup_ammo"
  local e = SpawnPrefab(name, { x = pos.x, y = 1.2, z = pos.z })
  if e ~= nil then
    pickups[#pickups + 1] = { e = e, kind = kind, phase = math.random() * 6.28 }
  end
end

local function spawn_wave(wave)
  waveFlash = 1.8
  local spawns = wave.spawns or {}
  local radius = (levelCfg.arenaRadius or 24) * 0.7
  for _, s in ipairs(spawns) do
    local count = s.count or 0
    for i = 1, count do
      local a = (i - 1) * (6.28318 / math.max(1, count)) + math.random() * 0.5
      local r = radius * (0.55 + math.random() * 0.45)
      local pp = GetPosition(player) or { x = 0, y = 0, z = 0 }
      spawn_enemy(s.type, { x = pp.x + math.cos(a) * r, y = 1, z = pp.z + math.sin(a) * r })
    end
  end
end

local function alive_count()
  local n = 0
  for _, t in ipairs(enemies) do
    if t.alive then n = n + 1 end
  end
  return n
end

local function set_hud_visible(v)
  UISetVisible("Hud", v)
end

local function enter_pause(paused)
  state = paused and "paused" or "playing"
  UISetVisible("PauseMenu", paused)
  set_hud_visible(not paused)
  SetVar("cameraMouseLock", paused and 0 or 1)
  if not paused then PlaySfx("click") end
end

local function end_game(victory)
  state = victory and "victory" or "gameover"
  UISetVisible("Hud", false)
  UISetVisible(victory and "VictoryPanel" or "GameOverPanel", true)
  UISetText(victory and "VictoryScoreLabel" or "FinalScoreLabel", "Score " .. tostring(score))
  if score > bestScore then
    bestScore = score
    save_game()
  end
  SetVar("cameraMouseLock", 0)
  PlaySfx(victory and "win" or "explosion")
end

local function restart_level()
  if levelCfg.scene ~= nil and levelCfg.scene ~= "" then
    ChangeScene(levelCfg.scene)
  end
end

function on_start(e)
  player = e
  -- Per-instance script vars are live for this call: capture the level chain.
  LEVEL = level or 1
  NEXT_SCENE = next_scene or ""
  load_save()
  apply_audio()
  load_data()
  switch_weapon(1)
  spawn_weapon_models()
  fxLights, fxTimers = {}, {}
  for i = 1, FX_COUNT do
    fxLights[i] = SpawnPrefab("muzzle_flash", { x = 0, y = -100, z = 0 })
    fxTimers[i] = 0
  end
  bossLight = SpawnPrefab("muzzle_flash", { x = 0, y = -100, z = 0 })
  park_fx_light(bossLight)
  UIShow("assets/ui/game_hud.ui.json")
  UISetVisible("Hud", true)
  UISetVisible("PauseMenu", false)
  UISetVisible("GameOverPanel", false)
  UISetVisible("VictoryPanel", false)

  SetVar("cameraMouseLock", 1)
  lookYaw = GetVar("cameraYaw") or 0
  lookPitch = 0
  SetHealth(player, MAX_HP)
  SetPosition(player, { x = 0, y = 0, z = 0 })

  spawn_cover()
  waveIndex = 0
  waveTimer = 1.2
  kills = 0
  score = 0
  stateTimer = 3.0
  PlayMusic("combat", settings.musicVolume)
end

local function update_player(dt)
  fireCd = math.max(0, fireCd - dt)
  hitMarker = math.max(0, hitMarker - dt)
  muzzleFlash = math.max(0, muzzleFlash - dt)
  recoil = math.max(0, recoil - dt * 7)
  hitImpact = math.max(0, hitImpact - dt)
  shakeT = math.max(0, shakeT - dt)
  spreadHeat = math.max(0, spreadHeat - dt * 0.35)
  update_fx_lights(dt)
  if reloadTime > 0 then
    reloadTime = reloadTime - dt
    if reloadTime <= 0 and weapon ~= nil then
      local need = weapon.mag - mag
      local take = math.min(need, reserve)
      mag = mag + take
      reserve = reserve - take
      reloadTime = 0
    end
  end

  aiming = InputMouseDown("right")
  local sens = settings.mouseSensitivity * (aiming and 0.55 or 1)
  lookYaw = lookYaw - InputMouseX() * sens
  lookPitch = clamp(lookPitch + InputMouseY() * sens, -1.25, 1.25)

  local cy = lookYaw
  local fwd = { x = -math.sin(cy), z = -math.cos(cy) }
  local right = { x = math.cos(cy), z = -math.sin(cy) }
  local ix = ActionAxis("move_strafe")
  local iz = ActionAxis("move_forward")
  local speed = aiming and 3.2 or (ActionDown("sprint") and 8.5 or 5.5)
  local dx = right.x * ix + fwd.x * iz
  local dz = right.z * ix + fwd.z * iz
  local moveMag = math.sqrt(dx * dx + dz * dz)
  if moveMag > 1 then
    dx, dz = dx / moveMag, dz / moveMag
  end
  bobAmount = math.min(1, moveMag)
  if bobAmount > 0.05 then
    bobTime = bobTime + dt * (aiming and 4.0 or 7.0)
  end

  local pos = GetPosition(player)
  if pos == nil then return end
  grounded = pos.y <= 0.001
  if ActionDown("jump") and grounded then
    yvel = 7.5
    grounded = false
  end
  if not grounded then
    yvel = yvel - 20 * dt
  end
  pos.y = pos.y + yvel * dt
  if pos.y <= 0 then
    pos.y = 0
    yvel = 0
    grounded = true
  end
  pos.x = clamp(pos.x + dx * speed * dt, -29, 29)
  pos.z = clamp(pos.z + dz * speed * dt, -29, 29)
  pos = collide_player(pos, 0.35)
  SetPosition(player, pos)
  if grounded and bobAmount > 0.05 then
    stepAcc = stepAcc + dt * (ActionDown("sprint") and 1.6 or 1.0)
    if stepAcc >= 0.34 then
      stepAcc = 0
      PlaySfx("step")
    end
  end

  if ActionPressed("weapon_1") then switch_weapon(1) end
  if ActionPressed("weapon_2") then switch_weapon(2) end
  if ActionPressed("weapon_3") then switch_weapon(3) end
  if ActionPressed("weapon_4") then switch_weapon(4) end
  if ActionPressed("next_weapon") then switch_weapon(weaponIdx % #weapons + 1) end
  if ActionPressed("reload") then reload_weapon() end
  if weapon ~= nil and weapon.auto then
    if InputMouseDown("left") then fire_weapon() end
  elseif InputMousePressed("left") then
    fire_weapon()
  end
end

local function update_camera()
  local pos = GetPosition(player)
  if pos == nil then return end
  local cd = math.cos(lookPitch)
  local eyeX, eyeY, eyeZ = pos.x, pos.y + EYE_H, pos.z
  SetVar("cameraFocus", {
    x = eyeX - math.sin(lookYaw) * cd * CAM_DIST,
    y = eyeY - math.sin(lookPitch) * CAM_DIST,
    z = eyeZ - math.cos(lookYaw) * cd * CAM_DIST,
  })
  SetVar("cameraYaw", lookYaw)
  SetVar("cameraPitch", lookPitch)
  SetVar("cameraDist", CAM_DIST)
  if shakeT > 0 then
    local f = GetVar("cameraFocus")
    if f ~= nil then
      local m = shakeT * 0.18
      SetVar("cameraFocus", {
        x = f.x + (math.random() - 0.5) * m,
        y = f.y + (math.random() - 0.5) * m * 0.5,
        z = f.z + (math.random() - 0.5) * m,
      })
    end
  end
end

local function update_viewmodel()
  local e = weapon and gunModels[weapon.id]
  if e == nil then return end
  local pos = GetPosition(player)
  if pos == nil then return end

  local fwd = forward_dir(lookYaw, lookPitch)
  local right = { x = math.cos(lookYaw), y = 0, z = -math.sin(lookYaw) }
  local eye = { x = pos.x, y = pos.y + EYE_H, z = pos.z }
  local kick = recoil * 0.12
  local dist = (aiming and 0.52 or 0.62) - kick
  local side = aiming and 0.0 or 0.22
  local drop = aiming and -0.18 or -0.22
  local bobX = math.sin(bobTime) * bobAmount * 0.012
  local bobY = math.cos(bobTime * 1.6) * bobAmount * 0.010
  if reloadTime > 0 then drop = drop - 0.12 end
  local p = {
    x = eye.x + fwd.x * dist + right.x * side + bobX,
    y = eye.y + fwd.y * dist + drop + bobY,
    z = eye.z + fwd.z * dist + right.z * side,
  }
  SetPosition(e, p)
  SetRotationY(e, lookYaw)
end

local function enemy_hitscan(origin, dir, range, damage)
  emit_burst(origin, 5, 5, 1, 0.85, 0.4)
  flash_fx_light(origin, { r = 1, g = 0.6, b = 0.25, a = 1 }, 2.2, 7, 0.06)
  EmitParticles({
    pos = origin,
    count = 6,
    speedMin = 3,
    speedMax = 6,
    lifeMin = 0.05,
    lifeMax = 0.12,
    sizeStart = 0.16,
    sizeEnd = 0.02,
    color = { r = 1, g = 0.85, b = 0.4, a = 1 },
    colorEnd = { r = 1, g = 0.25, b = 0.05, a = 0 },
    gravity = 0,
    additive = true,
  })
  EmitParticles({
    pos = origin,
    count = 1,
    vel = { x = dir.x * 50, y = dir.y * 50, z = dir.z * 50 },
    speedMin = 0,
    speedMax = 0,
    lifeMin = 0.12,
    lifeMax = 0.12,
    sizeStart = 0.08,
    sizeEnd = 0.01,
    color = { r = 1, g = 0.8, b = 0.3, a = 1 },
    colorEnd = { r = 1, g = 0.3, b = 0.05, a = 0 },
    gravity = 0,
    additive = true,
  })
  local wallDist = wall_distance(origin, dir, range)

  local pp = GetPosition(player)
  if pp ~= nil then
    local target = { x = pp.x, y = pp.y + 0.8, z = pp.z }
    local to = { x = target.x - origin.x, y = target.y - origin.y, z = target.z - origin.z }
    local along = to.x * dir.x + to.y * dir.y + to.z * dir.z
    if along > 0 and along < wallDist then
      local cx = origin.x + dir.x * along
      local cy = origin.y + dir.y * along
      local cz = origin.z + dir.z * along
      local ddx, ddy, ddz = target.x - cx, target.y - cy, target.z - cz
      local horiz = math.sqrt(ddx * ddx + ddz * ddz)
      if horiz <= 0.9 and math.abs(target.y - origin.y) <= 2.0 then
        SetHealth(player, math.max(0, (GetHealth(player) or MAX_HP) - damage))
        shakeT = math.max(shakeT, 0.15)
        emit_burst({ x = cx, y = cy, z = cz }, 5, 5, 1, 0.35, 0.25)
      end
    end
  end

  if wallDist < range then
    local wallPoint = {
      x = origin.x + dir.x * wallDist,
      y = origin.y + dir.y * wallDist,
      z = origin.z + dir.z * wallDist,
    }
    emit_burst(wallPoint, 4, 4, 0.85, 0.65, 0.3)
  end
end

local function suicide_boom(t)
  local dp = GetPosition(t.e) or { x = t.home.x, y = t.home.y, z = t.home.z }
  EmitParticles({
    pos = dp, count = 32, speedMin = 2, speedMax = 11,
    lifeMin = 0.12, lifeMax = 0.42, sizeStart = 0.22, sizeEnd = 0.02,
    color = { r = 1, g = 0.62, b = 0.08, a = 1 },
    colorEnd = { r = 1, g = 0.1, b = 0.03, a = 0 },
    gravity = -2, additive = true,
  })
  PlaySfx("explosion")
  shakeT = math.max(shakeT, 0.25)
  flash_fx_light({ x = dp.x, y = dp.y + 0.5, z = dp.z },
                 { r = 1, g = 0.45, b = 0.12, a = 1 }, 4.0, 16, 0.28)
  local pp = GetPosition(player)
  if pp ~= nil and dist2d(dp.x, dp.z, pp.x, pp.z) < 5.0 then
    SetHealth(player, math.max(0, (GetHealth(player) or MAX_HP) - (t.def.damage or 30)))
  end
  t.alive = false
  combo = combo + 1
  comboTimer = 3.0
  local gain = math.floor((t.def.score or 300) * (1 + 0.12 * (combo - 1)))
  kills = kills + 1
  score = score + gain
  SpawnFloatText(dp.x, dp.y + 0.8, dp.z, "+" .. tostring(gain), false, 0.8)
  Despawn(t.e)
end

-- Steering helpers for ground/air movement around cover.
local function inside_cover_xz(x, z, pad)
  for _, c in ipairs(covers) do
    if math.abs(x - c.x) < c.halfX + pad and math.abs(z - c.z) < c.halfZ + pad then
      return c
    end
  end
  return nil
end

-- Push the point out of any cover AABB (2D XZ), returning a corrected point.
local function resolve_cover_xz(x, z, pad)
  local c = inside_cover_xz(x, z, pad)
  if c == nil then return x, z end
  local dx = x - c.x
  local dz = z - c.z
  local pushX = (c.halfX + pad) - math.abs(dx)
  local pushZ = (c.halfZ + pad) - math.abs(dz)
  if pushX < pushZ then
    x = x + (dx >= 0 and pushX or -pushX)
  else
    z = z + (dz >= 0 and pushZ or -pushZ)
  end
  return x, z
end
local function update_enemies(dt)
  local pp = GetPosition(player)
  -- 1) Separation pass: keep the swarm readable (no stacking into one blob).
  for i, t in ipairs(enemies) do
    if t.alive and (t.def.speed or 0) > 0 then
      local pi = GetPosition(t.e)
      if pi ~= nil then
        for j = i + 1, #enemies do
          local o = enemies[j]
          if o.alive and (o.def.speed or 0) > 0 then
            local pj = GetPosition(o.e)
            if pj ~= nil then
              local dx, dz = pi.x - pj.x, pi.z - pj.z
              local d = math.sqrt(dx * dx + dz * dz)
              local minD = 1.5
              if d > 0.001 and d < minD then
                local push = (minD - d) * 0.5
                local nx, nz = dx / d, dz / d
                pi.x = pi.x + nx * push
                pi.z = pi.z + nz * push
                pj.x = pj.x - nx * push
                pj.z = pj.z - nz * push
                SetPosition(o.e, pj)
              end
            end
          end
        end
      end
    end
  end

  for _, t in ipairs(enemies) do
    if t.alive then
      local hp = GetHealth(t.e)
      if hp ~= nil and hp <= 0 then
        t.alive = false
        combo = combo + 1
        comboTimer = 3.0
        local gain = math.floor((t.def.score or 100) * (1 + 0.12 * (combo - 1)))
        kills = kills + 1
        score = score + gain
        hitMarker = 0.12
        shakeT = math.max(shakeT, 0.08)
        local dp = GetPosition(t.e) or { x = t.home.x, y = t.home.y, z = t.home.z }
        local isBoss = t.def.kind == "boss"
        EmitParticles({
          pos = dp,
          count = isBoss and 42 or 18,
          speedMin = 4,
          speedMax = 9,
          lifeMin = 0.20,
          lifeMax = 0.50,
          sizeStart = isBoss and 0.30 or 0.20,
          sizeEnd = 0.02,
          color = isBoss and { r = 0.85, g = 0.3, b = 0.95, a = 1 } or { r = 1, g = 0.52, b = 0.2, a = 1 },
          colorEnd = { r = 1, g = 0.1, b = 0.04, a = 0 },
          gravity = -7,
          additive = true,
        })
        SpawnFloatText(dp.x, dp.y + 0.8, dp.z, "+" .. tostring(gain), false, 0.8)
        PlaySfx("explosion")
        flash_fx_light({ x = dp.x, y = dp.y + 0.5, z = dp.z },
                       isBoss and { r = 0.8, g = 0.25, b = 0.95, a = 1 }
                              or { r = 1, g = 0.5, b = 0.15, a = 1 },
                       isBoss and 5.0 or 3.2, isBoss and 20 or 13,
                       isBoss and 0.4 or 0.22)
        if not isBoss then
          if math.random() < 0.22 then
            spawn_pickup("health", { x = t.home.x, y = 1.2, z = t.home.z })
          elseif math.random() < 0.30 then
            spawn_pickup("ammo", { x = t.home.x, y = 1.2, z = t.home.z })
          end
        elseif bossLight ~= nil then
          park_fx_light(bossLight)  -- aura dies with the Juggernaut
        end
        Despawn(t.e)
      else
        if hp ~= nil and t.def.hp ~= nil then
          SetEntityPlate(t.e, t.def.name, clamp(hp / t.def.hp, 0, 1))
        end
        local p = GetPosition(t.e)
        if p ~= nil and pp ~= nil then
          local d = dist2d(p.x, p.z, pp.x, pp.z)
          local nx, nz = (pp.x - p.x) / math.max(d, 0.001), (pp.z - p.z) / math.max(d, 0.001)
          local kind = t.def.kind
          -- Line of sight: never shoot through cover; use it to gate fire.
          local los = false
          if d > 0.001 then
            los = wall_distance({ x = p.x, y = p.y + 0.7, z = p.z },
                                { x = nx, y = 0, z = nz }, d) >= d - 0.8
          end

          -- Boss enrage: below 35% HP it snaps into overdrive once.
          if kind == "boss" and not t.enraged and hp < (t.def.hp or 1) * 0.35 then
            t.enraged = true
            EmitParticles({
              pos = p, count = 24, speedMin = 3, speedMax = 8,
              lifeMin = 0.2, lifeMax = 0.45, sizeStart = 0.24, sizeEnd = 0.02,
              color = { r = 0.85, g = 0.2, b = 0.9, a = 1 },
              colorEnd = { r = 0.2, g = 0.05, b = 0.1, a = 0 },
              gravity = -3, additive = true,
            })
            flash_fx_light(p, { r = 0.9, g = 0.2, b = 0.9, a = 1 }, 4.0, 18, 0.5)
            PlaySfx("wave")
          end
          local speed = t.def.speed or 0
          if t.enraged then speed = speed * 1.5 end

          -- Movement + per-archetype procedural animation.
          local yaw = math.atan(nx, nz)
          local hover = t.def.hover or 0.10
          local moving = false
          if kind == "suicide" then
            -- Sapper: spins, zigzags, pulses faster the closer it gets.
            yaw = clock * 6.0 + t.phase
            if d > 2.2 then
              moving = true
              local zig = math.sin(clock * 5.0 + t.phase) * 0.9
              local mx, mz = nx + (-nz) * zig, nz + nx * zig
              local ml = math.sqrt(mx * mx + mz * mz)
              p.x = p.x + mx / ml * speed * dt
              p.z = p.z + mz / ml * speed * dt
            end
            local pulse = 1 + math.sin(clock * (4 + math.max(0, 8 - d))) * 0.07
            SetScale(t.e, t.scale * pulse, t.scale * pulse, t.scale * pulse)
          elseif kind == "turret" then
            -- Sentry: smoothed aim, idle scan, burst recoil.
            t.recoil = math.max(0, (t.recoil or 0) - dt * 6)
            local scan = los and 0 or math.sin(clock * 0.8 + t.phase) * 0.6
            yaw = yaw + scan
            local rec = 1 + (t.recoil or 0) * 0.06
            SetScale(t.e, t.scale * rec, t.scale * rec, t.scale * rec)
          elseif kind == "boss" then
            -- Juggernaut: heavy cadence, telegraphed charge dash.
            t.stateT = t.stateT + dt
            t.chargeCd = t.chargeCd - dt
            if (t.charging or 0) > 0 then
              t.charging = t.charging - dt
              moving = true
              local cspeed = speed * 3
              p.x = p.x + nx * cspeed * dt
              p.z = p.z + nz * cspeed * dt
              EmitParticles({
                pos = { x = p.x, y = p.y - t.scale * 0.4, z = p.z },
                count = 2, speedMin = 1, speedMax = 3,
                lifeMin = 0.2, lifeMax = 0.4, sizeStart = 0.2, sizeEnd = 0.02,
                color = { r = 0.8, g = 0.3, b = 0.9, a = 1 },
                colorEnd = { r = 0.2, g = 0.05, b = 0.1, a = 0 },
                gravity = -2, additive = true,
              })
            elseif t.chargeCd <= 0 and d > 7 then
              t.chargeCd = 7 + math.random() * 3
              t.charging = 1.1
              EmitParticles({
                pos = { x = p.x, y = p.y + t.scale * 0.5, z = p.z },
                count = 16, speedMin = 2, speedMax = 6,
                lifeMin = 0.25, lifeMax = 0.5, sizeStart = 0.26, sizeEnd = 0.02,
                color = { r = 0.95, g = 0.35, b = 0.15, a = 1 },
                colorEnd = { r = 0.3, g = 0.05, b = 0.02, a = 0 },
                gravity = -1, additive = true,
              })
              PlaySfx3D("wave", p)
            elseif speed > 0 and d > math.max(1.5, (t.def.range or 15) * 0.75) then
              moving = true
              p.x = p.x + nx * speed * dt
              p.z = p.z + nz * speed * dt
            end
            t.stepPhase = t.stepPhase + dt * (moving and 7 or 1.5)
            local sway = math.sin(t.stepPhase) * (moving and 0.08 or 0.03)
            yaw = yaw + sway
            p.y = t.home.y + math.abs(math.sin(t.stepPhase)) * 0.05
            local rec = 1 + math.sin(t.stepPhase * 2) * 0.015
            SetScale(t.e, t.scale * rec, t.scale * rec, t.scale * rec)
            if bossLight ~= nil then
              SetPosition(bossLight, { x = p.x, y = p.y + t.scale * 0.4, z = p.z })
              SetEntityComponentField(bossLight, "light", "color",
                                      { r = 0.85, g = 0.2, b = 0.4, a = 1 })
              SetEntityComponentField(bossLight, "light", "intensity", 2.2)
              SetEntityComponentField(bossLight, "light", "radius", 12)
            end
          else
            -- Drones & heavies: hold a preferred band, orbit strafe inside it.
            local preferred = (t.def.range or 15) * 0.55
            if speed > 0 then
              if kind == "heavy" then
                -- Heavies march in bursts: walk a beat, halt, fire.
                t.stateT = t.stateT + dt
                if t.halted then
                  if t.stateT > 0.7 then t.stateT = 0; t.halted = false end
                else
                  if t.stateT > 1.3 then t.stateT = 0; t.halted = true end
                end
                moving = not t.halted
              else
                moving = d > preferred + 3
              end
              if moving then
                if d > preferred + 3 then
                  p.x = p.x + nx * speed * dt
                  p.z = p.z + nz * speed * dt
                else
                  -- Orbit: circle the player, flipping direction now and then.
                  t.orbitT = t.orbitT - dt
                  if t.orbitT <= 0 then
                    t.orbitT = 2 + math.random() * 2
                    t.orbitDir = -t.orbitDir
                  end
                  local sx, sz = -nz * t.orbitDir, nx * t.orbitDir
                  p.x = p.x + sx * speed * 0.65 * dt
                  p.z = p.z + sz * speed * 0.65 * dt
                end
                local cx, cz = resolve_cover_xz(p.x, p.z, 0.45)
                p.x, p.z = cx, cz
              end
            end
            if hover > 0 then
              -- Fliers bob fast, wobble, and recoil-hop on fire.
              p.y = t.home.y + math.sin(clock * 2.6 + t.phase) * hover
              yaw = yaw + math.sin(clock * 3.1 + t.phase) * 0.10
            else
              -- Ground units walk with a cadence + slight sway.
              t.stepPhase = t.stepPhase + dt * (moving and 6 or 1)
              p.y = t.home.y + math.abs(math.sin(t.stepPhase)) * 0.04
              yaw = yaw + math.sin(t.stepPhase) * (moving and 0.06 or 0.02)
            end
            t.punch = math.max(0, (t.punch or 0) - dt)
            local rec = 1 + (t.punch / 0.12) * 0.08
            SetScale(t.e, t.scale * rec, t.scale * rec, t.scale * rec)
          end

          -- Moving ground units kick up dust at footfall cadence.
          if moving and hover == 0 and kind ~= "turret" then
            local step = math.sin(t.stepPhase)
            if step > 0.9 and not t.stepped then
              t.stepped = true
              EmitParticles({
                pos = { x = p.x, y = 0.1, z = p.z },
                count = 3, speedMin = 0.5, speedMax = 1.5,
                lifeMin = 0.2, lifeMax = 0.4, sizeStart = 0.12, sizeEnd = 0.02,
                color = { r = 0.55, g = 0.55, b = 0.5, a = 0.6 },
                colorEnd = { r = 0.4, g = 0.4, b = 0.38, a = 0 },
                gravity = -1, additive = false,
              })
            elseif step < 0 then
              t.stepped = false
            end
          end

          SetRotationY(t.e, yaw)
          SetPosition(t.e, p)
          t.attackCd = math.max(0, t.attackCd - dt)
          if kind == "suicide" then
            if t.attackCd <= 0 and d < 2.6 then
              suicide_boom(t)
            end
          elseif kind == "turret" then
            -- Sentry: 3-round burst when the player is visible in range.
            if (t.burst or 0) > 0 then
              t.burstT = t.burstT - dt
              if t.burstT <= 0 then
                t.burst = t.burst - 1
                t.burstT = 0.16
                t.recoil = 1
                local dir = { x = nx, y = 0, z = nz }
                local origin = { x = p.x, y = p.y + 0.6, z = p.z }
                enemy_hitscan(origin, dir, (t.def.range or 18) + 8, t.def.damage or 8)
                PlaySfx3D("shoot", origin)
              end
            elseif d < (t.def.range or 18) and los and t.attackCd <= 0 then
              t.attackCd = (t.def.fireRate or 1.8) * (t.enraged and 0.75 or 1)
              t.burst = 3
              t.burstT = 0
            end
          elseif d < (t.def.range or 18) and los and t.attackCd <= 0 then
            t.attackCd = (t.def.fireRate or 1.8) * (t.enraged and 0.75 or 1)
            local dir = { x = nx, y = 0, z = nz }
            local origin = { x = p.x, y = p.y + 0.8, z = p.z }
            enemy_hitscan(origin, dir, (t.def.range or 18) + 8, t.def.damage or 8)
            PlaySfx3D("shoot", origin)
          end
        end
      end
    end
  end
end

local function update_waves(dt)
  if waveIndex >= #(levelCfg.waves or {}) then return end
  waveTimer = waveTimer - dt
  if waveTimer <= 0 then
    waveIndex = waveIndex + 1
    local wave = levelCfg.waves[waveIndex]
    spawn_wave(wave)
    PlaySfx("wave")
    if waveIndex < #levelCfg.waves then
      waveTimer = levelCfg.waves[waveIndex + 1].delay or 20
    else
      waveTimer = -1
    end
  end
end

local function update_pickups(dt)
  local pp = GetPosition(player)
  if pp == nil then return end
  local i = 1
  while i <= #pickups do
    local pk = pickups[i]
    local p = GetPosition(pk.e)
    if p == nil then
      table.remove(pickups, i)
    elseif dist2d(p.x, p.z, pp.x, pp.z) < 1.8 then
      if pk.kind == "health" then
        SetHealth(player, math.min(MAX_HP, (GetHealth(player) or MAX_HP) + 25))
        PlaySfx("heal")
      else
        if weapon ~= nil then
          reserve = reserve + math.floor((weapon.mag or 30) * 0.8)
        end
        PlaySfx("reload")
      end
      Despawn(pk.e)
      table.remove(pickups, i)
    else
      p.y = 1.2 + math.sin(clock * 3 + pk.phase) * 0.15
      SetPosition(pk.e, p)
      SetRotationY(pk.e, clock * 2.0)
      i = i + 1
    end
  end
end

local function update_hud()
  local hp = GetHealth(player) or 0
  UISetText("HealthLabel", "HP " .. tostring(math.floor(math.max(0, hp))) .. " / " .. MAX_HP)
  UISetFill("HealthBar", clamp(hp / MAX_HP, 0, 1))
  if weapon ~= nil then
    UISetText("WeaponLabel", weapon.name or "Weapon")
    -- Mag/reserve come from JSON as doubles; format without the ".0".
    UISetText("AmmoLabel", string.format("%.0f / %.0f", mag, reserve))
  end
  UISetText("ScoreLabel", "Score " .. tostring(score))
  UISetText("WaveLabel", "Wave " .. tostring(waveIndex) .. " / " .. tostring(#(levelCfg.waves or {})))
  local remaining = alive_count()
  UISetText("ObjectiveLabel", (levelCfg.title or "Mission") .. "  -  Hostiles: " .. tostring(remaining))
end

function on_update(e, dt)
  clock = clock + dt
  stateTimer = math.max(0, stateTimer - dt)
  comboTimer = math.max(0, comboTimer - dt)
  if comboTimer <= 0 then combo = 0 end
  waveFlash = math.max(0, waveFlash - dt)

  if state == "playing" then
    update_player(dt)
    update_enemies(dt)
    update_waves(dt)
    update_pickups(dt)
    update_camera()
    update_viewmodel()
    update_hud()

    if (GetHealth(player) or 0) <= 0 then
      end_game(false)
    elseif waveIndex >= #(levelCfg.waves or {}) and alive_count() == 0 then
      end_game(true)
    elseif ActionPressed("pause") then
      enter_pause(true)
    end
  elseif state == "paused" then
    if UIClicked("Resume") or ActionPressed("pause") then
      enter_pause(false)
    elseif UIClicked("RestartLevel") then
      restart_level()
    elseif UIClicked("QuitToMenu") then
      ChangeScene("assets/scenes/menu.json")
    end
  elseif state == "gameover" then
    if UIClicked("Retry") or ActionPressed("restart") then
      restart_level()
    elseif UIClicked("GameOverMenu") then
      ChangeScene("assets/scenes/menu.json")
    end
  elseif state == "victory" then
    if UIClicked("NextLevel") or ActionPressed("restart") then
      if NEXT_SCENE ~= "" then
        ChangeScene(NEXT_SCENE)
      else
        ChangeScene("assets/scenes/menu.json")
      end
    elseif UIClicked("VictoryMenu") then
      ChangeScene("assets/scenes/menu.json")
    end
  end
end

function on_render()
  -- Design-space viewport (height is always 720; width follows the live
  -- game-area aspect). Centering/anchoring against this keeps the HUD
  -- correct in the editor dock and any future aspect, not just 16:9.
  local vp = GetViewportSize()
  local vw, vh = (vp and vp.w) or 1280, (vp and vp.h) or 720

  if state == "playing" or state == "paused" then
    local cx, cy = vw * 0.5, vh * 0.5

    -- Reload progress arc under the crosshair (subtle, only while reloading).
    if reloadTime > 0 and weapon ~= nil then
      local total = 1.35
      local frac = 1 - clamp(reloadTime / total, 0, 1)
      local rw = 70
      DrawRect(cx - rw / 2, cy + 26, rw, 5, 0.03, 0.05, 0.09, 0.8)
      DrawRect(cx - rw / 2 + 1, cy + 27, (rw - 2) * frac, 3, 0.35, 0.85, 1, 1)
      DrawText("RELOADING", cx, cy + 44, 12, 0.6, 0.8, 0.95, 0.9, true, true)
    end
    local gap = aiming and 5 or 12
    DrawRect(cx - gap - 8, cy - 1, 8, 2, 1, 1, 1, 0.9)
    DrawRect(cx + gap, cy - 1, 8, 2, 1, 1, 1, 0.9)
    DrawRect(cx - 1, cy - gap - 8, 2, 8, 1, 1, 1, 0.9)
    DrawRect(cx - 1, cy + gap, 2, 8, 1, 1, 1, 0.9)
    DrawRect(cx - 1, cy - 1, 2, 2, 1, 1, 1, 1)

    local anchors = ScreenAnchors()
    local plates = EntityPlates()
    for i = 1, #anchors do
      local a = anchors[i]
      if a.onscreen and a.entity ~= nil then
        local key = string.format("%d_%d", a.entity.id, a.entity.gen)
        local p = plates[key]
        if enemyKeys[key] and p ~= nil and p.hp ~= nil and p.hp >= 0 then
          local w, h = 56, 5
          DrawRect(a.x - w / 2, a.y - h, w, h, 0.03, 0.04, 0.07, 0.85)
          if p.hp > 0 then
            DrawRect(a.x - w / 2 + 1, a.y - h + 1, (w - 2) * clamp(p.hp, 0, 1), h - 2,
                     0.95, 0.22, 0.2, 1)
          end
          DrawText(p.name or "Enemy", a.x, a.y - h - 16, 13, 1, 0.85, 0.85, 1, true, true)
        end
      end
    end

    -- Top-right minimap: player, enemies, pickups and cover.
    local mapS = 170
    local mapX, mapY = vw - mapS - 20, 20
    DrawRect(mapX, mapY, mapS, mapS, 0.03, 0.06, 0.09, 0.90)
    DrawRectOutline(mapX, mapY, mapS, mapS, 1.5, 0.35, 0.55, 0.85, 0.8)
    local function map_point(x, z)
      return mapX + mapS * 0.5 + x * (mapS / 48), mapY + mapS * 0.5 + z * (mapS / 48)
    end
    for _, c in ipairs(covers) do
      local mx, my = map_point(c.x, c.z)
      DrawRect(mx - 2, my - 2, 4, 4, 0.55, 0.60, 0.68, 1)
    end
    for _, t in ipairs(enemies) do
      if t.alive then
        local p = GetPosition(t.e)
        if p ~= nil then
          local mx, my = map_point(p.x, p.z)
          DrawRect(mx - 2, my - 2, 4, 4, 0.95, 0.25, 0.2, 1)
        end
      end
    end
    for _, pk in ipairs(pickups) do
      local p = GetPosition(pk.e)
      if p ~= nil then
        local mx, my = map_point(p.x, p.z)
        DrawRect(mx - 1, my - 1, 3, 3, pk.kind == "health" and 0.35 or 0.35,
                 pk.kind == "health" and 1 or 0.55, pk.kind == "health" and 0.55 or 1, 1)
      end
    end
    local pp = GetPosition(player)
    if pp ~= nil then
      local mx, my = map_point(pp.x, pp.z)
      -- View cone: 8 sectors along the facing direction, fading out.
      local fx, fz = -math.sin(lookYaw), -math.cos(lookYaw)
      for s = 1, 8 do
        local spread = (s - 1) * 0.055
        for side = -1, 1, 2 do
          local ca, sa = math.cos(spread * side), math.sin(spread * side)
          local dx = fx * ca - fz * sa
          local dz = fx * sa + fz * ca
          local sx = mx + dx * s * 7
          local sy = my + dz * s * 7
          if sx >= mapX and sx < mapX + mapS and sy >= mapY and sy < mapY + mapS then
            DrawRect(sx - 1, sy - 1, 2, 2, 0.35, 0.85, 1, 0.55 - s * 0.06)
          end
        end
      end
      DrawRect(mx - 3, my - 3, 6, 6, 0.35, 0.85, 1, 1)
      local ang = math.atan(fz, fx) * 57.2958
      if ang < 0 then ang = ang + 360 end
      local arrows = { "→", "↘", "↓", "↙", "←", "↖", "↑", "↗" }
      local idx = (math.floor((ang + 22.5) / 45) % 8) + 1
      DrawText(arrows[idx], mx, my, 18, 1, 0.85, 0.3, 1, true, true)
    end

    if hitMarker > 0 then
      DrawRect(cx - 8, cy - 8, 4, 4, 1, 0.35, 0.25, 1)
      DrawRect(cx + 4, cy - 8, 4, 4, 1, 0.35, 0.25, 1)
      DrawRect(cx - 8, cy + 4, 4, 4, 1, 0.35, 0.25, 1)
      DrawRect(cx + 4, cy + 4, 4, 4, 1, 0.35, 0.25, 1)
    end

    if hitImpact > 0 and lastHitPoint ~= nil then
      local s = WorldToScreen(lastHitPoint.x, lastHitPoint.y, lastHitPoint.z)
      if s ~= nil then
        DrawRect(s.x - 5, s.y - 5, 10, 10, 1, 0.72, 0.28, 0.95)
      end
    end

    local hp = GetHealth(player) or 0
    if hp < 35 then
      -- Heartbeat: visual pulse + sync'd thump sfx. The interval shortens as
      -- HP drops (a 30 HP player's heart beats faster than a 10 HP... no: the
      -- opposite reads better in playtests — faster when close to death).
      local beat = math.sin(clock * (5 + (35 - hp) * 0.15))
      DrawRect(0, 0, vw, vh, 0.8, 0.05, 0.05, 0.10 + beat * 0.05)
      if beat > 0.96 and not heartbeatLatch then
        heartbeatLatch = true
        PlaySfx("hit")
      elseif beat < 0 then
        heartbeatLatch = false
      end
    end

    -- Kill-streak combo banner with decay gauge.
    if combo >= 2 then
      DrawText("COMBO x" .. combo, cx, 176, 30, 1, 0.85, 0.25, 1, true, true)
      local cw = 130 * (comboTimer / 3.0)
      DrawRect(cx - cw / 2, 204, cw, 5, 1, 0.3, 0.1, 0.65)
    end

    -- Boss overhead presence bar (top-center, wide).
    for _, t in ipairs(enemies) do
      if t.alive and t.def.kind == "boss" then
        local hpB = GetHealth(t.e) or 0
        local bw = 420
        DrawRect(cx - bw / 2 - 2, 44, bw + 4, 16, 0.03, 0.04, 0.08, 0.85)
        if t.def.hp ~= nil and t.def.hp > 0 then
          DrawRect(cx - bw / 2, 46, bw * clamp(hpB / t.def.hp, 0, 1), 12, 0.85, 0.2, 0.85, 1)
        end
        DrawText(t.def.name or "BOSS", cx, 20, 20, 0.9, 0.4, 0.95, 1, true, true)
      end
    end
  end

  if waveFlash > 0 then
    local a = math.min(1, waveFlash / 0.4)
    DrawRect(vw * 0.5 - 220, 128, 440, 76, 0.04, 0.07, 0.12, 0.9 * a)
    DrawText("WAVE " .. tostring(waveIndex), vw * 0.5, 148, 42, 0.5, 0.0, 0.0, a, true, true)
  end

  if stateTimer > 0 then
    DrawRect(vw * 0.5 - 240, vh * 0.5 - 60, 480, 60, 0.03, 0.05, 0.09, 0.9)
    DrawText(levelCfg.title or "Mission", vw * 0.5, vh * 0.5 - 42, 24, 0.4, 0.85, 1, 1, true, true)
  end
end
