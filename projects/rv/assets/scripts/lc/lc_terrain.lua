-- lc_terrain.lua
-- General terrain heightfield for the camp and the roadside.
--
-- tools/gen_rv_terrain.py writes assets/data/rv_terrain.json (a regular height
-- grid) and builds rv_terrain.gltf from that same grid. Sampling the JSON here
-- keeps the gameplay heights and the visible mesh in lockstep: player, RV,
-- co-op proxies, vegetation and cameras all use terrainHeight().
--
-- If the data file is missing (old build / stripped assets) we fall back to the
-- analytic roadside embankment so the game still runs.

local field = nil
local fieldTried = false

local function loadField()
  if fieldTried then return field end
  fieldTried = true
  if ReadText == nil or jsonDecode == nil then return nil end
  local text = ReadText("assets/data/rv_terrain.json")
  if text == nil or text == "" then return nil end
  local ok, data = pcall(jsonDecode, text)
  if not ok or type(data) ~= "table" then return nil end
  if type(data.h) ~= "table" or type(data.nx) ~= "number" then return nil end
  field = data
  return field
end

-- Exposed for self-tests / debugging: nil when the fallback is in use.
function terrainFieldInfo()
  local f = loadField()
  if f == nil then return nil end
  return { x0 = f.x0, z0 = f.z0, step = f.step, nx = f.nx, nz = f.nz }
end

-- Analytic fallback: the original roadside embankment (kept for safety).
local TX = { 5.0, 8.0, 11.5, 15.0, 19.0, 23.5, 28.0, 33.0, 38.0 }
local TY = { 0.0, 0.70, 1.90, 3.00, 3.50, 3.55, 3.00, 1.50, 0.0 }
local TZ0, TZ1 = -24.0, 46.0

local function embankmentHeight(x, z)
  if x <= TX[1] or x >= TX[#TX] then return 0.0 end
  local base = 0.0
  for i = 1, #TX - 1 do
    if x >= TX[i] and x <= TX[i + 1] then
      local t = (x - TX[i]) / (TX[i + 1] - TX[i])
      base = TY[i] + (TY[i + 1] - TY[i]) * t
      break
    end
  end
  base = base + 0.28 * math.sin(z * 0.23 + x * 0.35) +
         0.12 * math.sin(z * 0.61)
  local taper = math.min(1.0,
                         math.max(0.0, (z - TZ0) / 6.0),
                         math.max(0.0, (TZ1 - z) / 6.0))
  return math.max(0.0, base * taper)
end

-- Bilinear sample of the grid; 0 outside the heightfield extent.
function terrainHeight(x, z)
  local f = loadField()
  if f == nil then return embankmentHeight(x, z) end

  local fx = (x - f.x0) / f.step
  local fz = (z - f.z0) / f.step
  local nx, nz = f.nx, f.nz
  if fx < 0 or fz < 0 or fx > nx - 1 or fz > nz - 1 then return 0.0 end

  local i = math.floor(fx)
  local j = math.floor(fz)
  if i > nx - 2 then i = nx - 2 end
  if j > nz - 2 then j = nz - 2 end
  local tx, tz = fx - i, fz - j
  local a = f.h[j * nx + i + 1]
  local b = f.h[j * nx + i + 2]
  local c = f.h[(j + 1) * nx + i + 1]
  local d = f.h[(j + 1) * nx + i + 2]
  local top = a + (b - a) * tx
  local bot = c + (d - c) * tx
  return top + (bot - top) * tz
end

-- Smoothly approach the sampled terrain height (avoids stair-stepping when
-- walking or driving over the crest).
function terrainFollow(current, x, z, dt, rate)
  local target = terrainHeight(x, z)
  return current + (target - current) * math.min(1, dt * (rate or 12))
end

-- Approximate uphill direction / steepness, for slope physics and road tilt.
function terrainSlope(x, z)
  local e = 0.35
  local dx = (terrainHeight(x + e, z) - terrainHeight(x - e, z)) / (2 * e)
  local dz = (terrainHeight(x, z + e) - terrainHeight(x, z - e)) / (2 * e)
  return dx, dz
end

-- RV pitch/roll from the terrain gradient, damped so it does not jitter.
-- Pitch tilts the nose with the slope ahead, roll tilts the right side.
function updateRvTilt(dt)
  local dx, dz = terrainSlope(rv.x, rv.z)
  local fx, fz = math.sin(rv.yaw), math.cos(rv.yaw)
  local dhF = dx * fx + dz * fz
  local dhR = dx * math.cos(rv.yaw) - dz * math.sin(rv.yaw)
  local tp = -math.atan(dhF)
  local tr = math.atan(dhR)
  tp = math.max(-0.35, math.min(0.35, tp))
  tr = math.max(-0.35, math.min(0.35, tr))
  local k = math.min(1, dt * 8)
  rv.pitch = (rv.pitch or 0) + (tp - (rv.pitch or 0)) * k
  rv.roll = (rv.roll or 0) + (tr - (rv.roll or 0)) * k
end

-- Four wheel contact points in RV-local space; matches tools/gen_rv_body.py
-- (wheel centres +-1.26 m from the centreline, axles at +-2.92 m).
local WHEEL_TRACK = 2.52
local WHEELBASE = 5.84
local WHEELS = {
  { lx = -1.26, lz =  2.92 },  -- front left
  { lx =  1.26, lz =  2.92 },  -- front right
  { lx = -1.26, lz = -2.92 },  -- rear left
  { lx =  1.26, lz = -2.92 },  -- rear right
}

-- Shared wheel visuals: steering from yaw rate (works for manual driving,
-- autopilot and co-op without an extra state field) and roll from speed.
function updateWheelMotion(dt)
  local prevYaw = rv.prevYaw or rv.yaw
  local dyaw = rv.yaw - prevYaw
  rv.prevYaw = rv.yaw
  local rate = dyaw / math.max(dt or 0.016, 0.016)
  local targetSteer = math.max(-0.55, math.min(0.55, rate * 0.35))
  local ks = math.min(1, (dt or 0.016) * 6)
  rv.steerVis = (rv.steerVis or 0) + (targetSteer - (rv.steerVis or 0)) * ks
  rv.wheelSpin = (rv.wheelSpin or 0) -
                 (rv.speed or 0) * (dt or 0.016) / 0.40
  if rv.wheelSpin > math.pi * 2 or rv.wheelSpin < -math.pi * 2 then
    rv.wheelSpin = rv.wheelSpin % (math.pi * 2)
  end
end

-- Per-wheel suspension + body attitude. Each contact samples terrainHeight at
-- its own (x,z) and follows the surface through a critically damped spring, so
-- the body settles onto the wheel plane instead of the centre point: on a
-- crest the front axle lifts first, in a dip the rear drops, and the wheels no
-- longer hover over / sink into the slope.
function updateRvSuspension(dt)
  local s = rv.susp
  if s == nil then
    s = { h = {}, v = {}, init = false }
    rv.susp = s
  end
  local cy, sy = math.cos(rv.yaw), math.sin(rv.yaw)
  local step = math.min(dt or 0.016, 0.033)
  local k = 110.0                      -- spring stiffness
  local c = 2.0 * math.sqrt(k) * 0.85  -- damping (near critical)
  local hs = {}
  local sum = 0

  for i, w in ipairs(WHEELS) do
    local wx = rv.x + w.lx * cy + w.lz * sy
    local wz = rv.z - w.lx * sy + w.lz * cy
    local target = terrainHeight(wx, wz)
    local cur = s.h[i] or target
    local vel = s.v[i] or 0
    if s.init then
      local acc = (target - cur) * k - vel * c
      vel = vel + acc * step
      cur = cur + vel * step
      -- Keep the visual wheel within a sane travel of the surface.
      if cur < target - 0.30 then cur, vel = target - 0.30, 0 end
      if cur > target + 0.30 then cur, vel = target + 0.30, 0 end
    else
      cur, vel = target, 0
    end
    s.h[i], s.v[i] = cur, vel
    hs[i] = cur
    sum = sum + cur
  end
  s.init = true

  local h0 = sum / 4.0
  local front = (hs[1] + hs[2]) * 0.5
  local rear = (hs[3] + hs[4]) * 0.5
  local plusX = (hs[2] + hs[4]) * 0.5
  local minusX = (hs[1] + hs[3]) * 0.5
  local b = (front - rear) / WHEELBASE   -- dh/dlz
  local a = (plusX - minusX) / WHEEL_TRACK -- dh/dlx

  -- A rigid body can only rest on three of the four contacts when the surface
  -- is twisted. Lift the least-squares plane by the deepest penetration so no
  -- wheel sinks into the dirt; the opposite wheel then droops a little, which
  -- reads like suspension travel instead of clipping.
  local maxPen = 0
  for i, w in ipairs(WHEELS) do
    local pred = h0 + a * w.lx + b * w.lz
    local pen = hs[i] - pred
    if pen > maxPen then maxPen = pen end
  end
  if maxPen > 0.25 then maxPen = 0.25 end
  h0 = h0 + maxPen

  -- Per-wheel data for the visual articulation: the height of the fitted body
  -- plane at each wheel, plus how far that wheel droops/compresses from it.
  s.plane = s.plane or {}
  s.travel = s.travel or {}
  for i, w in ipairs(WHEELS) do
    local plane = a * w.lx + b * w.lz
    s.plane[i] = plane
    s.travel[i] = hs[i] - (h0 + plane)
  end

  -- Visual steering from the yaw rate and wheel roll from speed.
  updateWheelMotion(dt)

  local kk = math.min(1, (dt or 0.016) * 10)
  local tp = math.max(-0.35, math.min(0.35, -math.atan(b)))
  local tr = math.max(-0.35, math.min(0.35, math.atan(a)))
  rv.pitch = (rv.pitch or 0) + (tp - (rv.pitch or 0)) * kk
  rv.roll = (rv.roll or 0) + (tr - (rv.roll or 0)) * kk
  rv.groundY = (rv.groundY or 0) + (h0 - (rv.groundY or 0)) * kk
  return h0
end

-- Split a body quaternion (yaw * pitch * roll) into pitch/roll, given the
-- scripted yaw. Exposed so the self-test can round-trip it.
function rvPitchRollFromQuat(q, yaw)
  local cy, sy = math.cos(yaw * 0.5), math.sin(yaw * 0.5)
  local w = cy * q.w + sy * q.y
  local x = cy * q.x - sy * q.z
  local z = cy * q.z + sy * q.x
  if w < 0 then w, x, z = -w, -x, -z end
  return 2 * math.atan(x, w), 2 * math.atan(z, w)
end

-- Physical suspension: the Jolt body is held up by four spring/damper impulses
-- applied at the wheel contacts. Ride height, pitch and roll therefore come out
-- of the solver (the visual wheels are then snapped to their sampled ground so
-- they never float). Horizontal motion and heading stay scripted: the drive
-- code sets the horizontal velocity and re-applies the yaw each frame.
local SUSP_K = 90000.0      -- N/m per wheel
local SUSP_C = 11000.0      -- N*s/m per wheel (slightly overdamped)
local SUSP_MAX_F = 150000.0
local SUSP_R = 0.40

function updateRvPhysics(dt)
  if rvBody == nil or PhysicsApplyImpulse == nil or PhysicsGetPosition == nil
     or PhysicsGetRotation == nil then
    return false
  end
  local p = PhysicsGetPosition(rvBody)
  local q = PhysicsGetRotation(rvBody)
  if p == nil or q == nil then return false end

  local pitch, roll = rvPitchRollFromQuat(q, rv.yaw)
  rv.x, rv.z = p.x, p.z
  rv.groundY = p.y - 0.9
  rv.pitch = math.max(-0.5, math.min(0.5, pitch))
  rv.roll = math.max(-0.5, math.min(0.5, roll))

  local cp, sp = math.cos(rv.pitch), math.sin(rv.pitch)
  local cr, sr = math.cos(rv.roll), math.sin(rv.roll)
  local cyw, syw = math.cos(rv.yaw), math.sin(rv.yaw)
  local step = math.min(dt or 0.016, 0.033)
  local s = rv.susp
  if s == nil then
    s = { h = {}, v = {} }
    rv.susp = s
  end
  s.plane = s.plane or {}
  s.travel = s.travel or {}

  for i, w in ipairs(WHEELS) do
    -- Offset from the box centre: the collider centre is 0.9 above the visual
    -- origin and the wheel centres sit at 0.38.
    local lx, ly, lz = w.lx, 0.38 - 0.9, w.lz
    local x1 = lx * cr - ly * sr
    local y1 = lx * sr + ly * cr
    local y2 = y1 * cp - lz * sp
    local z2 = y1 * sp + lz * cp
    local ox = x1 * cyw + z2 * syw
    local oz = -x1 * syw + z2 * cyw
    local wx, wy, wz = p.x + ox, p.y + y2, p.z + oz

    local ground = terrainHeight(wx, wz)
    local compression = ground - (wy - SUSP_R)
    local vy = 0
    if PhysicsGetPointVelocity ~= nil then
      local vp = PhysicsGetPointVelocity(rvBody, { x = wx, y = wy, z = wz })
      if vp ~= nil then vy = vp.y end
    end
    local force = SUSP_K * compression - SUSP_C * vy
    if force > SUSP_MAX_F then force = SUSP_MAX_F end
    if force < -SUSP_MAX_F then force = -SUSP_MAX_F end
    PhysicsApplyImpulse(rvBody, { x = 0, y = force * step, z = 0 },
                        { x = wx, y = wy, z = wz })

    -- Visual wheel sits on its own contact (the body plane is the solver's
    -- job); store it relative to the body origin for applyRvTransform.
    s.plane[i] = ground - (p.y - 0.9)
    s.travel[i] = 0
    s.h[i] = ground
  end

  updateWheelMotion(dt)
  return true
end
