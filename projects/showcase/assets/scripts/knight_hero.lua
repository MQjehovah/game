-- ============================================================================
-- Knight action hero (KayKit Knight.glb) - showcase demo.
--
-- Effects use the fantasy VFX texture pack (projects/showcase/assets/sprites/
-- vfx): crescent slashes, magic-circle runes, starburst glows, embers, dust and
-- flipbook sheets (fire / aura / lightning), all resolved per-emitter via
-- EmitParticles{ texture = ... } and drawn into the HDR target so they bloom.
--
-- Controls:
--   W A S D   move (Shift = sprint)            mouse   look / turn camera
--   Left / J  melee slash       1  Fireball      2  Frost Nova
--   3  Thunder Strike           4  Dash          Space  Jump
--   Q (hold)  Block             5  Cheer (aura)  R  reset dummies
--   0         auto-demo
-- ============================================================================

local hero = nil
local dummies = {}          -- { ent=, home= } training targets

-- Effect sprites (project-relative; resolved by the runtime).
local TX = {
  slash  = "assets/sprites/vfx/slash_arc_02.png",
  slash2 = "assets/sprites/vfx/slash_arc_01.png",
  slash3 = "assets/sprites/vfx/slash_arc_04.png",
  rune   = "assets/sprites/vfx/rune_cyan.png",
  runeR  = "assets/sprites/vfx/rune_ring.png",
  runeW  = "assets/sprites/vfx/rune_white.png",
  glow   = "assets/sprites/vfx/glow_soft.png",
  burstB = "assets/sprites/vfx/burst_blue.png",
  burstG = "assets/sprites/vfx/burst_green.png",
  starR  = "assets/sprites/vfx/star_red.png",
  starW  = "assets/sprites/vfx/star_white.png",
  gold   = "assets/sprites/vfx/sparkle_gold.png",
  starB  = "assets/sprites/vfx/star_burst.png",
  smoke  = "assets/sprites/vfx/smoke_plume.png",
  dust   = "assets/sprites/vfx/dust.png",
  embers = "assets/sprites/vfx/embers.png",
  sparks = "assets/sprites/vfx/sparks.png",
  fire   = "assets/sprites/vfx/fx_fire_6x6.png",
  aura   = "assets/sprites/vfx/fx_aura_6x5.png",
  zap    = "assets/sprites/vfx/fx_zap_7x2.png",
}

-- Camera / input state
local lookYaw, lookPitch = 0.0, 0.32
local LOOK_SENS = 0.0016
local CAM_DIST = 7.5

-- Hero motion state
local facing = 0.0
local yvel = 0.0
local grounded = true
local dashT = 0.0
local dashDir = { x = 0, z = 1 }
local dashTrail = 0
local auraT = 0.0   -- cheer/buff aura timer

-- Action / one-shot state
local acting = nil
local castT = -1.0
local CAST_DUR = 0.45
local autoDemo = false -- press 0: auto-cycle skills (showcase attract mode)
local autoStep, autoT = 0, 0

-- Cooldowns
local atkCd, fireCd, novaCd, dashCd, thunCd = 0, 0, 0, 0, 0
local lastHp = nil

-- Ground decals (magic circles) with a script-driven grow + fade lifecycle.
local decals = {}
local function addDecal(tex, x, y, z, size0, size1, dur, a0, r, g, b)
  local e = SpawnDecal(tex, { x = x, y = y, z = z }, size0, a0, r or 1, g or 1, b or 1, true, 1.2)
  if e ~= nil then decals[#decals + 1] = { e = e, t = 0, dur = dur, s0 = size0, s1 = size1, a0 = a0 } end
end
local function updateDecals(dt)
  for i = #decals, 1, -1 do
    local d = decals[i]
    d.t = d.t + dt
    local u = d.t / d.dur
    if u >= 1 then
      Despawn(d.e); table.remove(decals, i)
    else
      SetDecal(d.e, d.s0 + (d.s1 - d.s0) * (1 - (1 - u) * (1 - u)), d.a0 * (1 - u * u), 1.2)
    end
  end
end

local function clamp(v, lo, hi) if v < lo then return lo elseif v > hi then return hi end return v end
local function len2(x, z) return math.sqrt(x * x + z * z) end
local function fdir() return math.sin(facing), -math.cos(facing) end

local function burst(x, y, z, n, sp, col, life, grav, sz0, tex)
  EmitParticles({
    texture = tex or TX.glow,
    pos = { x = x, y = y, z = z }, count = n,
    speedMin = sp * 0.35, speedMax = sp,
    lifeMin = (life or 0.5) * 0.6, lifeMax = (life or 0.5),
    sizeStart = sz0 or 0.55, sizeEnd = 0.04,
    color = { r = col[1], g = col[2], b = col[3], a = 1 },
    colorEnd = { r = col[1], g = col[2], b = col[3], a = 0 },
    gravity = grav or -6, additive = true,     emissive = 1.3,
  })
end

-- One large crescent billboard + a spark spray (a sword swing).
local function slashFx(px, pz, yaw, col)
  local ax, az = math.sin(yaw), -math.cos(yaw)
  local fx, fz = px + ax * 1.5, pz + az * 1.5
  EmitParticles({
    texture = TX.slash2, pos = { x = fx, y = 1.15, z = fz }, count = 1, shape = "point",
    speedMin = 0.2, speedMax = 0.6, lifeMin = 0.16, lifeMax = 0.2,
    sizeStart = 2.2, sizeEnd = 3.6,
    color = { 1.0, 0.95, 0.75, 0.95 }, colorEnd = { col[1], col[2], col[3], 0 },
    rotStart = -0.5, rotSpeed = 3.0, emissive = 1.4, additive = true,
  })
  EmitParticles({
    texture = TX.sparks, pos = { x = fx, y = 1.05, z = fz }, count = 18, spread = 70,
    direction = { x = ax, y = 0.25, z = az },
    speedMin = 5, speedMax = 13, lifeMin = 0.15, lifeMax = 0.34,
    sizeStart = 0.55, sizeEnd = 0.05,
    color = { 1.0, 0.85, 0.4, 1 }, colorEnd = { 1.0, 0.4, 0.1, 0 },
    gravity = -5,     emissive = 1.25, additive = true,
  })
end

local function meleeSlash()
  local p = GetPosition(hero); if p == nil then return end
  local clips = {
    "1H_Melee_Attack_Slice_Horizontal",
    "1H_Melee_Attack_Chop",
    "1H_Melee_Attack_Slice_Diagonal",
    "1H_Melee_Attack_Stab",
  }
  PlayAnimation(hero, clips[math.random(#clips)], false, 0.08)
  acting = "melee"
  local ax, az = fdir()
  slashFx(p.x, p.z, facing, { 1.0, 0.8, 0.35 })
  Gameplay.MeleeArc({ x = p.x, y = p.y + 1.1, z = p.z }, { x = ax, y = 0, z = az }, 2.8, 130, 55, hero)
  PlaySfx("melee")
end

local function castFireball()
  local p = GetPosition(hero); if p == nil then return end
  acting = "cast"; castT = 0.0
  PlayAnimation(hero, "Spellcast_Raise", false, 0.12)
  EmitParticles({
    texture = TX.aura, uvCols = 6, uvRows = 5, uvFps = 22, uvLoop = true,
    pos = { x = p.x, y = p.y + 1.2, z = p.z }, count = 1, shape = "point",
    speedMin = 0.0, speedMax = 0.1, lifeMin = 0.5, lifeMax = 0.5,
    sizeStart = 1.6, sizeEnd = 2.4, color = { 1.0, 0.62, 0.2, 0.85 },
    colorEnd = { 1.0, 0.35, 0.1, 0 }, rotSpeed = 2.0,     emissive = 1.3, additive = true,
  })
  for _ = 1, 10 do
    EmitParticles({
      texture = TX.gold, pos = { x = p.x, y = p.y + 1.35, z = p.z }, count = 1,
      shape = "sphere", radius = 0.5, speedMin = 0.6, speedMax = 2.2,
      lifeMin = 0.3, lifeMax = 0.55, sizeStart = 0.4, sizeEnd = 0.02,
      color = { 1.0, 0.65, 0.2, 1 }, colorEnd = { 1.0, 0.25, 0.0, 0 },
      emissive = 1.4, additive = true,
    })
  end
end

local function launchFireball()
  local p = GetPosition(hero); if p == nil then return end
  local ax, az = fdir()
  PlayAnimation(hero, "Spellcast_Shoot", false, 0.1)
  acting = "cast"
  -- Muzzle: a fire flipbook puff + embers.
  EmitParticles({
    texture = TX.fire, uvCols = 6, uvRows = 6, uvFps = 34, uvLoop = false,
    pos = { x = p.x + ax * 1.2, y = p.y + 1.3, z = p.z + az * 1.2 }, count = 1, shape = "point",
    speedMin = 0.0, speedMax = 0.1, lifeMin = 0.85, lifeMax = 0.9,
    sizeStart = 1.4, sizeEnd = 3.0, color = { 1.0, 0.8, 0.5, 0.95 },
    colorEnd = { 1.0, 0.4, 0.15, 0 },     emissive = 1.5, additive = true,
  })
  EmitParticles({
    texture = TX.embers, pos = { x = p.x + ax, y = p.y + 1.3, z = p.z + az }, count = 14, spread = 40,
    direction = { x = ax, y = 0.2, z = az }, speedMin = 3, speedMax = 8,
    lifeMin = 0.2, lifeMax = 0.45, sizeStart = 0.5, sizeEnd = 0.05,
    color = { 1.0, 0.6, 0.2, 1 }, colorEnd = { 1.0, 0.2, 0.0, 0 },     emissive = 1.3, additive = true,
  })
  Gameplay.Projectile({ x = p.x + ax, y = p.y + 1.3, z = p.z + az }, { x = ax, y = 0, z = az },
                      18, 65, 2.6, 45, 0.9, hero, { { name = "burning", duration = 3, magnitude = 4 } })
  PlaySfx("shoot")
end

local function castFrostNova()
  local p = GetPosition(hero); if p == nil then return end
  acting = "nova"
  PlayAnimation(hero, "Spellcast_Long", false, 0.15)
  addDecal(TX.rune, p.x, p.y + 0.05, p.z, 2.4, 12.0, 1.4, 0.95, 0.5, 0.85, 1.0)
  addDecal(TX.runeR, p.x, p.y + 0.07, p.z, 1.0, 9.5, 1.4, 0.8, 0.65, 0.95, 1.0)
  EmitParticles({
    texture = TX.starW, pos = { x = p.x, y = p.y + 0.15, z = p.z }, count = 140, shape = "ring",
    radius = 0.5, ringThickness = 0.9, speedMin = 6, speedMax = 12, lifeMin = 0.5, lifeMax = 0.95,
    sizeStart = 0.9, sizeEnd = 0.03, color = { 0.6, 0.85, 1.0, 1 },
    colorEnd = { 0.3, 0.6, 1.0, 0 }, emissive = 2.2, additive = true,
  })
  EmitParticles({
    texture = TX.sparks, pos = { x = p.x, y = p.y + 0.1, z = p.z }, count = 46, shape = "disc",
    radius = 4.8, vel = { x = 0, y = 1, z = 0 }, spread = 16, speedMin = 5, speedMax = 11,
    lifeMin = 0.4, lifeMax = 0.85, sizeStart = 0.5, sizeEnd = 0.03,
    color = { 0.7, 0.92, 1.0, 1 }, colorEnd = { 0.4, 0.7, 1.0, 0 },     emissive = 1.3, additive = true,
  })
  EmitParticles({
    texture = TX.burstB, pos = { x = p.x, y = p.y + 1.0, z = p.z }, count = 1, shape = "point",
    speedMin = 0, speedMax = 0.2, lifeMin = 0.32, lifeMax = 0.36, sizeStart = 3.0, sizeEnd = 8.0,
    color = { 0.6, 0.9, 1.0, 0.9 }, colorEnd = { 0.3, 0.6, 1.0, 0 },     emissive = 1.3, additive = true,
  })
  Gameplay.AoE({ x = p.x, y = p.y + 0.5, z = p.z }, 6.5, 90, hero)
  PlaySfx("nova")
end

local function castThunder()
  local p = GetPosition(hero); if p == nil then return end
  acting = "thunder"
  PlayAnimation(hero, "Spellcast_Shoot", false, 0.08)
  local ax, az = fdir()
  local tx, tz = p.x + ax * 3.6, p.z + az * 3.6
  EmitParticles({
    texture = TX.zap, uvCols = 7, uvRows = 2, uvFps = 30, uvLoop = false,
    pos = { x = tx, y = 1.7, z = tz }, count = 1, shape = "point",
    speedMin = 0, speedMax = 0.1, lifeMin = 0.34, lifeMax = 0.4,
    sizeStart = 2.0, sizeEnd = 2.3, color = { 1.0, 1.0, 1.0, 1 },
    colorEnd = { 0.6, 0.85, 1.0, 0 },     emissive = 1.6, additive = true,
  })
  addDecal(TX.runeW, tx, p.y + 0.05, tz, 2.0, 5.5, 0.8, 0.8, 0.9, 0.95, 1.0)
  EmitParticles({
    texture = TX.starB, pos = { x = tx, y = 1.0, z = tz }, count = 26, spread = 85,
    direction = { x = 0, y = 1, z = 0 }, speedMin = 7, speedMax = 16,
    lifeMin = 0.15, lifeMax = 0.4, sizeStart = 0.7, sizeEnd = 0.03,
    color = { 0.85, 0.95, 1.0, 1 }, colorEnd = { 0.5, 0.75, 1.0, 0 }, emissive = 2.2, additive = true,
  })
  Gameplay.AoE({ x = tx, y = 0.5, z = tz }, 3.2, 85, hero)
  PlaySfx("thunder")
end

local function doJump()
  if not grounded then return end
  yvel = 8.0
  grounded = false
  PlayAnimation(hero, "Jump_Start", false, 0.06)
  acting = "jump"
  local p = GetPosition(hero)
  if p then
    EmitParticles({
      texture = TX.dust, pos = { x = p.x, y = 0.15, z = p.z }, count = 18, shape = "disc",
      radius = 0.5, speedMin = 1, speedMax = 3, lifeMin = 0.4, lifeMax = 0.7,
      sizeStart = 0.7, sizeEnd = 1.4, color = { 0.85, 0.82, 0.72, 0.55 },
      colorEnd = { 0.8, 0.78, 0.7, 0 }, additive = false,
    })
  end
end

local function doDash()
  local p = GetPosition(hero)
  if p == nil or dashCd > 0 then return end
  dashT = 0.22; dashCd = 1.2
  dashDir = { x = math.sin(facing), z = -math.cos(facing) }
  PlayAnimation(hero, "Dodge_Forward", false, 0.05)
  acting = "dash"
  if type(SpawnTrail) == "function" then
    dashTrail = SpawnTrail(0.5, 0.6, 0.8, 1.0, 0.9, 0.3, 0.5, 1.0, 0.0)
  end
  EmitParticles({
    texture = TX.dust, pos = { x = p.x, y = 0.25, z = p.z }, count = 16, shape = "disc",
    radius = 0.6, speedMin = 1, speedMax = 3, lifeMin = 0.3, lifeMax = 0.6,
    sizeStart = 0.6, sizeEnd = 1.3, color = { 0.85, 0.85, 0.8, 0.5 },
    colorEnd = { 0.8, 0.8, 0.75, 0 }, additive = false,
  })
  PlaySfx("dash")
end

local function castCheer()
  local p = GetPosition(hero); if p == nil then return end
  -- Persistent golden aura for a few seconds (aura flipbook, re-emitted).
  auraT = 3.0
  PlaySfx("cheer")
end

local function refreshDummies()
  for _, d in ipairs(dummies) do
    if d.ent ~= nil then
      SetHealth(d.ent, GetMaxHealth(d.ent))
      SetPosition(d.ent, { x = d.home.x, y = d.home.y, z = d.home.z })
    end
  end
end

local function watchDummies(dt)
  for _, d in ipairs(dummies) do
    if d.ent ~= nil then
      local hp = GetHealth(d.ent)
      local mhp = GetMaxHealth(d.ent)
      if hp ~= nil and hp <= 0.0 then
        PlayAnimation(d.ent, "Death_A", false, 0.1)
        local p = GetPosition(d.ent)
        if p then
          burst(p.x, p.y + 1.0, p.z, 22, 6, { 1.0, 0.45, 0.2 }, 0.6, -8, 0.5, TX.embers)
          SpawnFloatText(p.x, p.y + 2.2, p.z, "DOWN", true, 1.0)
        end
        d.down = true
      elseif hp ~= nil and d.lastHp ~= nil and hp < d.lastHp then
        PlayAnimation(d.ent, "Hit_A", false, 0.06)
        local p = GetPosition(d.ent)
        if p then
          EmitParticles({
            texture = TX.sparks, pos = { x = p.x, y = p.y + 1.1, z = p.z }, count = 16, spread = 70,
            direction = { x = 0, y = 1, z = 0 }, speedMin = 5, speedMax = 11,
            lifeMin = 0.15, lifeMax = 0.35, sizeStart = 0.5, sizeEnd = 0.05,
            color = { 1.0, 0.85, 0.4, 1 }, colorEnd = { 1.0, 0.4, 0.1, 0 },     emissive = 1.3, additive = true,
          })
          EmitParticles({
            texture = TX.smoke, pos = { x = p.x, y = p.y + 1.2, z = p.z }, count = 5,
            speedMin = 0.5, speedMax = 1.5, lifeMin = 0.5, lifeMax = 0.9,
            sizeStart = 0.7, sizeEnd = 1.6, color = { 0.9, 0.9, 0.9, 0.5 },
            colorEnd = { 0.9, 0.9, 0.9, 0 }, gravity = 0.6, additive = false,
          })
          SpawnFloatText(p.x, p.y + 2.0, p.z, "-" .. math.floor(d.lastHp - hp), false, 0.8)
        end
      end
      d.lastHp = hp
      if d.down then
        d.revive = (d.revive or 0) + dt
        if d.revive > 2.0 then
          d.down = false; d.revive = 0; SetHealth(d.ent, mhp)
        end
      end
    end
  end
end

function on_start(e)
  hero = e
  math.randomseed(20261011)
  SetEntityPlate(e, "Knight", 1.0)
  PlayAnimation(e, "Idle", true, 0.1)
  for i = 1, 3 do
    local ent = FindNamedEntity("Dummy_" .. i)
    if ent ~= nil then
      local p = GetPosition(ent)
      if p ~= nil then
        dummies[#dummies + 1] = { ent = ent, home = { x = p.x, y = p.y, z = p.z } }
      end
    end
  end
end
function on_update(e, dt)
  if e == nil then return end
  local p = GetPosition(e)
  if p == nil then return end

  atkCd = math.max(0, atkCd - dt)
  fireCd = math.max(0, fireCd - dt)
  novaCd = math.max(0, novaCd - dt)
  dashCd = math.max(0, dashCd - dt)
  thunCd = math.max(0, thunCd - dt)

  updateDecals(dt)
  watchDummies(dt)

  -- --- camera look ---------------------------------------------------------
  local mx, my = InputMouseX(), InputMouseY()
  lookYaw = lookYaw - (mx or 0) * LOOK_SENS
  lookPitch = clamp(lookPitch + (my or 0) * LOOK_SENS, -0.15, 1.05)
  if InputKey("LEFT") > 0 then lookYaw = lookYaw + dt * 1.8 end
  if InputKey("RIGHT") > 0 then lookYaw = lookYaw - dt * 1.8 end
  local camF = { x = -math.sin(lookYaw), z = -math.cos(lookYaw) }
  local camR = { x = math.cos(lookYaw), z = -math.sin(lookYaw) }

  -- --- movement ------------------------------------------------------------
  local ix, iz = 0, 0
  if InputKey("W") > 0 then iz = iz + 1 end
  if InputKey("S") > 0 then iz = iz - 1 end
  if InputKey("D") > 0 then ix = ix + 1 end
  if InputKey("A") > 0 then ix = ix - 1 end
  local mv = len2(ix, iz)
  local sprint = InputKey("SHIFT") > 0
  local speed = sprint and 8.5 or 4.2
  local dx, dz = 0, 0
  if mv > 0.01 then
    local nx, nz = ix / mv, iz / mv
    dx = camF.x * nz + camR.x * nx
    dz = camF.z * nz + camR.z * nx
  end
  if dashT > 0 then
    dashT = dashT - dt; dx, dz = dashDir.x, dashDir.z; speed = 22
  end
  p.x = clamp(p.x + dx * speed * dt, -35, 35)
  p.z = clamp(p.z + dz * speed * dt, -35, 35)

  if not grounded then
    yvel = yvel - 20 * dt
    p.y = p.y + yvel * dt
    if p.y <= 0 then
      p.y = 0; yvel = 0; grounded = true
      PlayAnimation(e, "Jump_Land", false, 0.08)
      acting = nil
      EmitParticles({
        texture = TX.dust, pos = { x = p.x, y = 0.15, z = p.z }, count = 22, shape = "ring",
        radius = 0.7, ringThickness = 0.5, speedMin = 2, speedMax = 5, lifeMin = 0.35, lifeMax = 0.65,
        sizeStart = 0.7, sizeEnd = 1.5, color = { 0.85, 0.82, 0.72, 0.55 },
        colorEnd = { 0.8, 0.78, 0.7, 0 }, gravity = -2, additive = false,
      })
    end
  else
    p.y = 0
  end
  SetPosition(e, p)

  if mv > 0.01 then facing = math.atan(dx, -dz) end
  SetRotationY(e, facing)

  if dashTrail ~= 0 and type(TrailPoint) == "function" then
    TrailPoint(dashTrail, { x = p.x, y = p.y + 0.9, z = p.z })
    if dashT <= 0 then TrailEnd(dashTrail); dashTrail = 0 end
  end

  -- Cheer aura: re-emit a rolling aura flipbook around the hero.
  if auraT > 0 then
    auraT = auraT - dt
    EmitParticles({
      texture = TX.aura, uvCols = 6, uvRows = 5, uvFps = 18, uvLoop = true,
      pos = { x = p.x, y = p.y + 1.0, z = p.z }, count = 1, shape = "point",
      speedMin = 0, speedMax = 0.1, lifeMin = 0.5, lifeMax = 0.6,
      sizeStart = 2.2, sizeEnd = 2.9, color = { 1.0, 0.85, 0.4, 0.7 },
      colorEnd = { 1.0, 0.6, 0.2, 0 }, rotSpeed = 1.5,     emissive = 1.3, additive = true,
    })
    if InputKey("5") == 0 then
      EmitParticles({
        texture = TX.gold, pos = { x = p.x, y = p.y + 0.4, z = p.z }, count = 2, shape = "disc",
        radius = 1.2, vel = { x = 0, y = 1.4, z = 0 }, speedMin = 0.5, speedMax = 1.2,
        lifeMin = 0.6, lifeMax = 1.0, sizeStart = 0.4, sizeEnd = 0.03,
        color = { 1.0, 0.85, 0.4, 1 }, colorEnd = { 1.0, 0.6, 0.2, 0 },     emissive = 1.3, additive = true,
      })
    end
  end

  -- --- third-person camera (runs every frame, before action early-returns) --
  SetVar("cameraMouseLock", 1)
  SetVar("cameraFocus", { x = p.x, y = p.y + 1.25, z = p.z })
  SetVar("cameraYaw", lookYaw)
  SetVar("cameraPitch", lookPitch)
  SetVar("cameraDist", CAM_DIST)

  -- --- one-shot bookkeeping / auto-demo ------------------------------------
  if InputKey("0") > 0 then autoDemo = not autoDemo end
  if autoDemo and castT < 0 and acting == nil then
    autoT = autoT + dt
    if autoT > 1.1 then
      autoT = 0
      if autoStep == 0 then meleeSlash()
      elseif autoStep == 1 then castFireball()
      elseif autoStep == 2 then castFrostNova()
      elseif autoStep == 3 then castThunder()
      elseif autoStep == 4 then castCheer()
      elseif autoStep == 5 then doJump()
      elseif autoStep == 6 then doDash()
      end
      autoStep = (autoStep + 1) % 7
    end
  end

  if castT >= 0 then
    castT = castT + dt
    if castT >= CAST_DUR then castT = -1; launchFireball() end
    return
  end
  if acting ~= nil then
    if AnimationFinished(e) or AnimationProgress(e) >= 0.97 then acting = nil else return end
  end

  -- --- actions -------------------------------------------------------------
  if (InputMousePressed("left") or InputKey("J") > 0) and atkCd <= 0 then
    atkCd = 0.5; meleeSlash(); return
  end
  if InputKey("1") > 0 and fireCd <= 0 then fireCd = 1.6; castFireball(); return end
  if InputKey("2") > 0 and novaCd <= 0 then novaCd = 5.0; castFrostNova(); return end
  if InputKey("3") > 0 and thunCd <= 0 then thunCd = 2.2; castThunder(); return end
  if InputKey("4") > 0 then doDash(); return end
  if InputKey("SPACE") > 0 then doJump(); return end
  if InputKey("5") > 0 then
    PlayAnimation(e, "Cheer", false, 0.15); acting = "cheer"; castCheer(); return
  end
  if InputKey("R") > 0 then refreshDummies() end

  if InputKey("Q") > 0 then
    if acting ~= "block" then acting = "block"; PlayAnimation(e, "Blocking", true, 0.15) end
    return
  end
  if acting == "block" then acting = nil end

  local wantClip, wantLoop, wantFade = "Idle", true, 0.2
  if mv > 0.01 then
    wantClip = sprint and "Running_A" or "Walking_A"
    wantFade = 0.15
  end
  PlayAnimation(e, wantClip, wantLoop, wantFade)

  -- --- health plate + hit reaction -----------------------------------------
  local hp, mhp = GetHealth(e), GetMaxHealth(e)
  if hp ~= nil and mhp ~= nil and mhp > 0 then
    SetEntityPlate(e, "Knight", hp / mhp)
    if lastHp ~= nil and hp < lastHp then
      EmitParticles({
        texture = TX.sparks, pos = { x = p.x, y = p.y + 1.0, z = p.z }, count = 14, spread = 70,
        direction = { x = 0, y = 1, z = 0 }, speedMin = 5, speedMax = 11,
        lifeMin = 0.15, lifeMax = 0.35, sizeStart = 0.5, sizeEnd = 0.05,
        color = { 1.0, 0.35, 0.25, 1 }, colorEnd = { 1.0, 0.1, 0.05, 0 },     emissive = 1.3, additive = true,
      })
      SpawnFloatText(p.x, p.y + 2.1, p.z, "-" .. math.floor(lastHp - hp), false, 0.8)
    end
    if lastHp ~= nil and hp <= 0 then
      PlayAnimation(e, "Death_A", false, 0.1); acting = "dead"
    end
    lastHp = hp
  end
end

function on_render()
  if hero == nil then return end
  DrawRect(14, 14, 264, 116, 0.04, 0.05, 0.08, 0.5)
  DrawText("KNIGHT  -  ACTION DEMO", 26, 20, 15, 1.0, 0.88, 0.45, 1)
  DrawText("WASD move  SHIFT sprint   mouse look", 26, 42, 12, 0.85, 0.9, 0.95, 1)
  DrawText("LMB/J melee   1 fireball   2 frost nova", 26, 58, 12, 0.85, 0.9, 0.95, 1)
  DrawText("3 thunder   4 dash   SPACE jump", 26, 74, 12, 0.85, 0.9, 0.95, 1)
  DrawText("Q block   5 cheer   R reset   0 auto-demo", 26, 90, 12, 0.7, 0.8, 0.9, 1)

  local function pip(x, cd, maxcd, r, g, b)
    local f = 1.0
    if maxcd > 0 then f = 1.0 - (cd / maxcd); if f < 0 then f = 0 end end
    DrawRect(x, 108, 50, 7, 0.18, 0.18, 0.22, 0.7)
    DrawRect(x, 108, 50 * f, 7, r, g, b, 0.95)
  end
  pip(26, atkCd, 0.5, 1.0, 0.85, 0.3)
  pip(84, fireCd, 1.6, 1.0, 0.5, 0.15)
  pip(142, novaCd, 5.0, 0.5, 0.8, 1.0)
  pip(200, thunCd, 2.2, 0.85, 0.95, 1.0)
end
