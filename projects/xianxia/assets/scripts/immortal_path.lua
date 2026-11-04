-- 仙山古道 vertical slice: third-person cultivator on a cliff ledge.
-- The script owns the orbit framing (cameraMode="script"); mouse deltas drive
-- the orbit. Movement samples GetGroundHeight, and a few additive motes add
-- life to the misty cliff air.

local hero = nil
local lookYaw = 0.06
local lookPitch = 0.16
local yvel = 0.0
local grounded = true
local facing = 0.0
local moveSpeed = 4.6
local runSpeed = 7.2
local tablet = { x = 0.0, z = -32.0 }
local hintCooldown = 0.0
local moteClock = 0.0
local mistClock = 0.0
local cloudClock = 0.0
local petalClock = 0.0
local clock = 0.0
local animState = ""

local function clamp(v, lo, hi)
  if v < lo then return lo end
  if v > hi then return hi end
  return v
end

local function groundAt(x, z)
  local g = GetGroundHeight(x, z)
  if g == nil then return 0.0 end
  return g
end

function on_start(e)
  hero = e
  local p = GetPosition(hero)
  if p ~= nil then
    p.y = groundAt(p.x, p.z)
    SetPosition(hero, p)
  end
  SetVar("cameraMouseLock", 0)
  SetVar("cameraMode", "script")
  SetVar("cameraYaw", lookYaw)
  SetVar("cameraPitch", lookPitch)
  SetVar("cameraDist", 3.5)
  SetVar("cameraFocus", { x = 0.0, y = groundAt(0.0, -20.0) + 1.15, z = -20.0 })
  SpawnFloatText(0.0, groundAt(0.0, -20.0) + 2.6, -20.0,
                 "WASD 行走 · Shift 疾行 · Space 跳跃 · F 感悟", false, 3.0)
  PlayAnimation(hero, "idle_in_raw", true, 0.2)
  animState = "idle_in_raw"
end

function on_update(ent, dt)
  if hero == nil then hero = ent end
  local p = GetPosition(hero)
  if p == nil then return end
  hintCooldown = math.max(0.0, hintCooldown - dt)
  clock = clock + dt

  lookYaw = lookYaw - InputMouseX() * 0.004
  lookPitch = clamp(lookPitch + InputMouseY() * 0.004, -0.30, 0.95)
  SetVar("cameraMode", "script")
  SetVar("cameraYaw", lookYaw)
  SetVar("cameraPitch", lookPitch)
  SetVar("cameraDist", 3.5)

  local cy = lookYaw
  local ix = ActionAxis("move_strafe")
  local iz = ActionAxis("move_forward")
  local fwd = { x = -math.sin(cy), z = -math.cos(cy) }
  local right = { x = math.cos(cy), z = -math.sin(cy) }
  local dx = right.x * ix + fwd.x * iz
  local dz = right.z * ix + fwd.z * iz
  local len = math.sqrt(dx * dx + dz * dz)
  if len > 1.0 then
    dx = dx / len
    dz = dz / len
    len = 1.0
  end

  local speed = moveSpeed
  if ActionDown("sprint") and len > 0.01 then speed = runSpeed end

  local ground = groundAt(p.x, p.z)
  if grounded then
    p.y = ground
    if ActionDown("jump") then
      yvel = 7.4
      grounded = false
    end
  end
  if not grounded then
    yvel = yvel - 20.0 * dt
    p.y = p.y + yvel * dt
    ground = groundAt(p.x, p.z)
    if p.y <= ground then
      p.y = ground
      yvel = 0.0
      grounded = true
    end
  end

  -- Keep the player on the ledge; the left side is a cloud-sea drop.
  p.x = clamp(p.x + dx * speed * dt, -1.9, 4.8)
  p.z = clamp(p.z + dz * speed * dt, -38.0, 38.0)
  if grounded then
    p.y = groundAt(p.x, p.z) + math.abs(math.sin(clock * 9.0)) * 0.015 * len
  end
  SetPosition(hero, p)

  if len > 0.01 then
    facing = math.atan(dx, -dz)
    SetRotationY(hero, facing)
  end

  SetVar("cameraFocus", { x = p.x, y = p.y + 1.12, z = p.z })

  local nextAnim = "idle_in_raw"
  if not grounded then
    nextAnim = "idle_in_raw"
  elseif len > 0.01 then
    nextAnim = (speed > moveSpeed) and "run2" or "run"
  end
  if nextAnim ~= animState then
    PlayAnimation(hero, nextAnim, true, 0.16)
    animState = nextAnim
  end

  -- A sparse stream of pale motes drifting over the cliff edge.
  moteClock = moteClock - dt
  if moteClock <= 0.0 then
    moteClock = 0.45
    local mx = p.x + (math.random() - 0.5) * 16.0
    local mz = p.z + (math.random() - 0.5) * 18.0
    local my = groundAt(mx, mz) + 0.8 + math.random() * 3.5
    EmitParticles({
      pos = { x = mx, y = my, z = mz },
      count = 1,
      lifeMin = 2.2,
      lifeMax = 4.5,
      sizeStart = 0.035,
      sizeEnd = 0.10,
      color = { r = 0.62, g = 0.76, b = 0.95, a = 0.32 },
      colorEnd = { r = 0.70, g = 0.82, b = 1.0, a = 0.0 },
      speedMin = 0.02,
      speedMax = 0.14,
      gravity = 0.01,
      additive = true,
      emissive = 0.8,
      texture = "assets/sprites/glow.png",
    })
  end

  -- Slow, very soft mist banks drifting up from the cloud sea on the left.
  mistClock = mistClock - dt
  if mistClock <= 0.0 then
    mistClock = 0.38
    EmitParticles({
      pos = { x = -4.0 - math.random() * 5.0,
              y = -2.0 + math.random() * 2.5,
              z = p.z - 8.0 + (math.random() - 0.5) * 24.0 },
      count = 1,
      lifeMin = 6.0,
      lifeMax = 10.0,
      sizeStart = 4.0,
      sizeEnd = 9.0,
      color = { r = 0.92, g = 0.96, b = 0.98, a = 0.12 },
      colorEnd = { r = 0.92, g = 0.96, b = 0.98, a = 0.0 },
      speedMin = 0.02,
      speedMax = 0.10,
      gravity = 0.01,
      additive = false,
      emissive = 0.0,
      texture = "assets/sprites/glow.png",
    })
  end

  -- Heavy, slow cloud banks fill the void beyond the left cliff. Big soft
  -- sprites overlapping at different depths read as a rolling cloud sea.
  cloudClock = cloudClock - dt
  if cloudClock <= 0.0 then
    cloudClock = 0.30
    EmitParticles({
      pos = { x = p.x - 10.0 + (math.random() - 0.5) * 12.0,
              y = 0.4 + math.random() * 4.0,
              z = p.z - 3.0 - math.random() * 28.0 },
      count = 1,
      lifeMin = 9.0,
      lifeMax = 16.0,
      sizeStart = 7.0,
      sizeEnd = 16.0,
      color = { r = 0.95, g = 0.98, b = 1.0, a = 0.16 },
      colorEnd = { r = 0.94, g = 0.97, b = 1.0, a = 0.0 },
      speedMin = 0.05,
      speedMax = 0.22,
      gravity = 0.0,
      additive = false,
      emissive = 0.0,
      texture = "assets/textures/cloud_soft.png",
    })
  end

  -- A few pale petals drifting down through the pines.
  petalClock = petalClock - dt
  if petalClock <= 0.0 then
    petalClock = 0.22
    EmitParticles({
      pos = { x = p.x + (math.random() - 0.5) * 10.0,
              y = p.y + 3.0 + math.random() * 3.0,
              z = p.z + (math.random() - 0.5) * 12.0 },
      count = 1,
      lifeMin = 3.0,
      lifeMax = 5.5,
      sizeStart = 0.045,
      sizeEnd = 0.018,
      color = { r = 0.98, g = 0.82, b = 0.84, a = 0.8 },
      colorEnd = { r = 0.95, g = 0.72, b = 0.76, a = 0.0 },
      speedMin = 0.05,
      speedMax = 0.25,
      gravity = -0.35,
      additive = false,
      emissive = 0.0,
      texture = "assets/sprites/glow.png",
    })
  end

  local ddx = p.x - tablet.x
  local ddz = p.z - tablet.z
  local nearTablet = (ddx * ddx + ddz * ddz) < 16.0
  if nearTablet and hintCooldown <= 0.0 and ActionPressed("interact") then
    hintCooldown = 2.0
    SpawnFloatText(tablet.x, groundAt(tablet.x, tablet.z) + 2.8, tablet.z,
                   "道法自然 · 仙途初启", false, 2.0)
  end
end
