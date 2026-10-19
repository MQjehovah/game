-- 仙山古道 vertical slice: third-person cultivator on a cliff path.
-- The script owns the orbit framing (cameraMode="script") so the third-person
-- distance/pitch stay stable; mouse deltas drive the orbit. Movement samples
-- GetGroundHeight, so the same script works on a flat or sculpted heightfield.

local hero = nil
local lookYaw = 0.0
local lookPitch = 0.24
local yvel = 0.0
local grounded = true
local facing = 0.0
local moveSpeed = 4.6
local runSpeed = 7.2
local tablet = { x = 0.0, z = -32.0 }
local hintCooldown = 0.0

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
  SetVar("cameraDist", 5.2)
  SetVar("cameraFocus", { x = 0.0, y = groundAt(0.0, -20.0) + 1.2, z = -20.0 })
  SpawnFloatText(0.0, groundAt(0.0, -20.0) + 2.6, -20.0,
                 "WASD 行走 · Shift 疾行 · Space 跳跃 · F 感悟", false, 3.0)
end

function on_update(ent, dt)
  if hero == nil then hero = ent end
  local p = GetPosition(hero)
  if p == nil then return end
  hintCooldown = math.max(0.0, hintCooldown - dt)

  lookYaw = lookYaw - InputMouseX() * 0.004
  lookPitch = clamp(lookPitch + InputMouseY() * 0.004, -0.30, 0.95)
  SetVar("cameraMode", "script")
  SetVar("cameraYaw", lookYaw)
  SetVar("cameraPitch", lookPitch)
  SetVar("cameraDist", 5.2)

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

  p.x = clamp(p.x + dx * speed * dt, -16.0, 16.0)
  p.z = clamp(p.z + dz * speed * dt, -38.0, 38.0)
  if grounded then p.y = groundAt(p.x, p.z) end
  SetPosition(hero, p)

  if len > 0.01 then
    facing = math.atan(dx, -dz)
    SetRotationY(hero, facing)
  end

  SetVar("cameraFocus", { x = p.x, y = p.y + 1.15, z = p.z })

  local ddx = p.x - tablet.x
  local ddz = p.z - tablet.z
  local nearTablet = (ddx * ddx + ddz * ddz) < 16.0
  if nearTablet and hintCooldown <= 0.0 and ActionPressed("interact") then
    hintCooldown = 2.0
    SpawnFloatText(tablet.x, groundAt(tablet.x, tablet.z) + 2.8, tablet.z,
                   "道法自然 · 仙途初启", false, 2.0)
  end
end
