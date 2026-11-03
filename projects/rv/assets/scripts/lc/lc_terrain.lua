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
  { lx = -1.26, lz =  2.92, front = true },   -- front left
  { lx =  1.26, lz =  2.92, front = true },   -- front right
  { lx = -1.26, lz = -2.92, front = false },  -- rear left
  { lx =  1.26, lz = -2.92, front = false },  -- rear right
}

-- Shared wheel visuals: steering from yaw rate (works for manual driving,
-- autopilot and co-op without an extra state field) and roll from speed.
function updateWheelMotion(dt)
  local prevYaw = rv.prevYaw or rv.yaw
  local dyaw = rv.yaw - prevYaw
  rv.prevYaw = rv.yaw
  local targetSteer
  if rv.steerCmd ~= nil then
    -- Physical path: the visual/contact steering follows the driver input.
    targetSteer = rv.steerCmd
  else
    -- Fallback/co-op: derive the visual steering from the scripted yaw rate.
    local rate = dyaw / math.max(dt or 0.016, 0.016)
    targetSteer = math.max(-0.55, math.min(0.55, rate * 0.35))
  end
  local ks = math.min(1, (dt or 0.016) * 8)
  rv.steerVis = (rv.steerVis or 0) + (targetSteer - (rv.steerVis or 0)) * ks
  if rv.steerCmd == nil then
    -- Kinematic/co-op path: no Ackermann, both front wheels share the angle.
    rv.steerLeft, rv.steerRight = rv.steerVis, rv.steerVis
  end
  if rv.wheelBodies == nil then
    rv.wheelSpinI = nil
  end
  rv.wheelSpin = (rv.wheelSpin or 0) -
                 (rv.speed or 0) * (dt or 0.016) / 0.40
  if rv.wheelSpin > math.pi * 2 or rv.wheelSpin < -math.pi * 2 then
    rv.wheelSpin = rv.wheelSpin % (math.pi * 2)
  end
  if rv.steerLog then
    rv.__steerN = (rv.__steerN or 0) + 1
    if rv.__steerN % 15 == 1 then
      print(string.format("[steer] cmd=%.2f vis=%.2f max=%.2f yaw=%.2f v=%.1f",
        rv.steerCmd or 0, rv.steerVis or 0,
        steeringMaxAngle(rv.speed or 0), rv.yaw, rv.speed or 0))
    end
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

-- Full Euler extraction for R = Ry(yaw) * Rx(pitch) * Rz(roll). Used when the
-- heading itself is physical (tire forces) instead of scripted.
function rvYawPitchRollFromQuat(q)
  local w, x, y, z = q.w, q.x, q.y, q.z
  local yaw = math.atan(2 * (x * z + w * y), 1 - 2 * (x * x + y * y))
  local sp = 2 * (w * x - y * z)
  sp = math.max(-1, math.min(1, sp))
  local pitch = math.asin(sp)
  local roll = math.atan(2 * (x * y + w * z), 1 - 2 * (x * x + z * z))
  return yaw, pitch, roll
end

-- Physical suspension: the Jolt body is held up by four spring/damper impulses
-- applied at the wheel contacts. Ride height, pitch and roll therefore come out
-- of the solver; horizontal motion comes from a friction-capped traction
-- impulse plus per-wheel lateral tire grip, while the heading stays scripted.
local SUSP_K = 90000.0      -- N/m per wheel
local SUSP_C = 11000.0      -- N*s/m per wheel (slightly overdamped)
local SUSP_MAX_F = 150000.0
local SUSP_R = 0.40
local SUSP_REST = 0.06      -- free length: floats the box so springs carry the load

-- Per-wheel rigid bodies: a small dynamic box per wheel, attached to the
-- chassis by a slider joint with soft limits (spring mount, ~8 Hz). The terrain
-- spring acts on the wheel body and the chassis is supported through the joint;
-- tires also act on the wheel bodies. Present on single-player, server and
-- clients (clients use them for local suspension prediction).
local WHEEL_BODY_MASS = 300
local WHEEL_BODY_OUT = 1.0
local WHEEL_BODY_MAX_F = 12000.0
local WHEEL_SPRING_HZ = 8.0
local WHEEL_BODY_HALF = { x = 0.04, y = 0.20, z = 0.20 }

-- Friction-driven wheel spin: engine/brake torque is applied to the wheel
-- bodies and the tires' slip-ratio force (applied at the contact patch) is the
-- reaction, so wheelspin and lockup emerge from the solver instead of having
-- the angular velocity overwritten every frame.
local DRIVE_TORQUE = 1100.0    -- N*m per driven rear wheel
local BRAKE_TORQUE = 1500.0    -- N*m per braked wheel
local WHEEL_MAX_OMEGA = 90.0   -- rad/s cap (protects an airborne wheel)

function spawnRvWheelBodies()
  if rvBody == nil then return false end
  if rv.wheelBodies ~= nil then return true end
  rv.wheelBodies = {}
  rv.wheelSpinI = {}
  local gy = rv.groundY or 0
  -- All hosts use cylindrical tires with a rolling 6DOF joint. The chassis is
  -- kept in its own collision group (see garage.lua) because the tire bodies
  -- overlap the box footprint.
  local rolling = PhysicsAddCylinder ~= nil and PhysicsAddWheelJoint ~= nil
  local axx, axy, axz = rvOffset(1, 0, 0)
  local upx, upy, upz = rvOffset(0, 1, 0)
  for i, w in ipairs(WHEELS) do
    local bx = w.lx * WHEEL_BODY_OUT
    local ox, oy, oz = rvOffset(bx, 0.38, w.lz)
    local wx = rv.x + ox
    local wy = gy + oy
    local wz = rv.z + oz
    if rolling then
      local body = PhysicsAddCylinder({ x = wx, y = wy, z = wz },
                                      0.12, 0.40, true,
                                      { mass = WHEEL_BODY_MASS, friction = 0.9,
                                        restitution = 0.0, gravityScale = 0.0,
                                        layer = 2, mask = 0xFFFFFFFF })
      if body ~= nil then
        if PhysicsSetRotationEuler ~= nil then
          PhysicsSetRotationEuler(body, rv.pitch or 0,
                                  (rv.roll or 0) + math.pi * 0.5, rv.yaw)
        end
        local joint = PhysicsAddWheelJoint(rvBody, body,
                                           { x = wx, y = wy, z = wz },
                                           { x = axx, y = axy, z = axz },
                                           { x = upx, y = upy, z = upz },
                                           WHEEL_SPRING_HZ, 1.0)
        rv.wheelBodies[i] = { body = body, joint = joint, rolling = true }
      end
    else
      local body = PhysicsAddBox({ x = wx, y = wy, z = wz },
                                 WHEEL_BODY_HALF, true,
                                 { mass = WHEEL_BODY_MASS, friction = 0.9,
                                   restitution = 0.0, gravityScale = 0.0,
                                   layer = 2, mask = 0xFFFFFFFF })
      if body ~= nil then
        local joint = PhysicsAddSliderJoint(rvBody, body,
                                            { x = wx, y = wy, z = wz },
                                            { x = 0, y = 1, z = 0 },
                                            0, 0, WHEEL_SPRING_HZ, 1.0)
        rv.wheelBodies[i] = { body = body, joint = joint, rolling = false }
      end
    end
  end
  if rv.wheelBodies[1] == nil then
    rv.wheelBodies = nil
    rv.wheelSpinI = nil
    return false
  end
  return true
end

function resetRvWheelBodies()
  if rv.wheelBodies == nil then return end
  local gy = rv.groundY or 0
  for i, w in ipairs(WHEELS) do
    local wb = rv.wheelBodies[i]
    if wb ~= nil then
      local bx = w.lx * WHEEL_BODY_OUT
      local ox, oy, oz = rvOffset(bx, 0.38, w.lz)
      PhysicsSetPosition(wb.body, { x = rv.x + ox, y = gy + oy, z = rv.z + oz })
      PhysicsSetVelocity(wb.body, { x = 0, y = 0, z = 0 })
    end
  end
end

-- Friction impulse for one contact: proportional to the lateral slip, capped
-- by the friction circle mu * normalForce * dt. Exposed for the self-test.
function tireGripImpulse(latV, normalForce, mass, dt, mu)
  local step = math.min(dt or 0.016, 0.033)
  local imp = -latV * mass * math.min(1, step * 8.0)
  local cap = (mu or 0.9) * math.max(0, normalForce) * step
  if imp > cap then imp = cap end
  if imp < -cap then imp = -cap end
  return imp
end

-- Surface materials: grip multiplier, slip curve shape (peak at `peak`, then
-- settles to `sat` of the peak), and how fast the tire heats / wears on it.
-- More than the old three-value table; lookup is by world feature (road slab,
-- shoulder, embankment dirt, pond mud, POI pad, meadow).
SURFACES = {
  asphalt  = { mu = 0.98, peak = 0.12, sat = 0.78, heat = 0.9, wear = 0.8 },
  concrete = { mu = 0.92, peak = 0.13, sat = 0.76, heat = 0.9, wear = 0.9 },
  gravel   = { mu = 0.80, peak = 0.18, sat = 0.68, heat = 1.1, wear = 1.3 },
  dirt     = { mu = 0.72, peak = 0.22, sat = 0.64, heat = 1.2, wear = 1.6 },
  grass    = { mu = 0.60, peak = 0.26, sat = 0.60, heat = 1.3, wear = 2.0 },
  mud      = { mu = 0.42, peak = 0.32, sat = 0.52, heat = 1.0, wear = 2.4 },
}

-- Which material is under (x,z)? Road slab -> asphalt, its shoulder -> gravel,
-- embankment dirt -> dirt, pond rim -> mud, POI pad -> concrete, else grass.
-- 胎压随里程缓慢下降 / 工作台充气回标准值
function pressureAfterMeters(p, meters)
  return math.max(1.2, (p or 2.4) - (VEH.pressureWear or 0.00002) * (meters or 0))
end

function resetTirePressure()
  rv.tirePressureCold = { 2.4, 2.4, 2.4, 2.4 }
end

function surfaceInfo(x, z)
  if distToRoadSq ~= nil then
    local d2 = distToRoadSq(x, z)
    if d2 < 12.0 then return "asphalt", SURFACES.asphalt end
    if d2 < 30.0 then return "gravel", SURFACES.gravel end
  end
  if x > 4.0 and x < 16.0 and z > -25.0 and z < 47.0 and
     (terrainHeight(x, z) or 0) > 0.12 then
    return "dirt", SURFACES.dirt
  end
  if ponds ~= nil then
    for _, p in ipairs(ponds) do
      local dx, dz = x - p.x, z - p.z
      if dx * dx + dz * dz < 30.0 then return "mud", SURFACES.mud end
    end
  end
  if NODES ~= nil and poiCenter ~= nil then
    for _, nd in ipairs(NODES) do
      if nd.id ~= "camp" then
        local cx, cz = poiCenter(nd)
        if cx ~= nil and math.abs(x - cx) < POI_HALF_X + 2.2 and
           math.abs(z - cz) < POI_HALF_Z + 2.2 then
          return "concrete", SURFACES.concrete
        end
      end
    end
  end
  return "grass", SURFACES.grass
end

-- Backwards-compatible scalar lookup used by older call sites/tests.
function surfaceGrip(x, z)
  local _, def = surfaceInfo(x, z)
  return def.mu
end

-- 路面类型中文名（驾驶 HUD 提示用）
function surfaceLabel(name)
  local map = { asphalt = "沥青", concrete = "水泥", gravel = "碎石",
                dirt = "泥土", grass = "草地", mud = "泥泞" }
  return map[name] or "未知"
end

-- Tire temperature (deg C) modifies grip: cold tires are a bit slippery, the
-- 45-95 C window is optimal, overheating greases them. Wear also costs grip.
function tireMuScale(temp, wear)
  local t = temp or 22
  local warm
  if t < 45 then
    warm = 0.84 + 0.16 * math.max(0, (t - 10) / 35)
  elseif t <= 95 then
    warm = 1.0
  else
    warm = math.max(0.68, 1.0 - (t - 95) * 0.0045)
  end
  local w = math.max(0, math.min(100, wear or 100))
  local wearScale = 0.80 + 0.20 * (w / 100)
  return warm * wearScale
end

-- Per-wheel thermal step. Heat comes from that wheel's slip power
-- (slip speed * normal load) plus a small rolling term; airflow at speed
-- convects it back toward ambient. Clamped to 150 C. Exposed for the
-- self-test.
function tireStep(temp, speed, slipSpeed, load, dt, ambient)
  local t = temp or 22
  local amb = ambient or 22
  local v = math.abs(speed or 0)
  local n = math.max(0, load or 0)
  local power = math.min(5000, math.abs(slipSpeed or 0) * n + v * n * 0.02)
  t = t + power * 0.008 * (dt or 0.016)
  t = t - (t - amb) * (0.15 + 0.02 * v) * (dt or 0.016)
  if t < amb then t = amb end
  if t > 150 then t = 150 end
  return t
end

-- Per-wheel wear step: only that wheel's slip speed abrades the tread.
function tireWearStep(wear, slipSpeed, dt, wearRate)
  local w = wear or 100
  local d = math.abs(slipSpeed or 0) * (dt or 0.016) * 0.02 * (wearRate or 1)
  w = w - d
  if w < 0 then w = 0 end
  return w
end

-- Load sensitivity: a real tire loses a little normalized grip as the normal
-- load grows (and gains some when lightly loaded). Exposed for the self-test.
function tireLoadMuScale(load)
  local n = math.max(0, load or 0)
  local s = 1.0 - 0.000028 * (n - 3500)
  if s < 0.78 then s = 0.78 end
  if s > 1.12 then s = 1.12 end
  return s
end

-- Per-wheel tire pressure. Cold inflation 2.4 bar; the hot pressure follows
-- the ideal-gas-ish rise with tread temperature. Low/high pressure changes
-- both peak grip and where the slip curve peaks. Exposed for the self-test.
function tirePressureAt(temp, cold)
  local p = (cold or 2.4) * (1.0 + 0.0037 * ((temp or 22) - 20))
  if p < 1.2 then p = 1.2 end
  if p > 3.6 then p = 3.6 end
  return p
end

function tirePressureMuScale(p)
  local r = (p or 2.4) / 2.4
  local d = r - 1.0
  local s = 1.0 - 0.55 * d * d
  if s < 0.72 then s = 0.72 end
  if s > 1.0 then s = 1.0 end
  return s
end

function tirePressurePeakScale(p)
  local r = (p or 2.4) / 2.4
  return 1.0 + 0.35 * (1.0 - r)
end

-- Pacejka-style magic formula, normalised so the peak force equals D:
--   F = D * sin(C * atan(B*s - E*(B*s - atan(B*s))))
-- B sets the slip at which the peak occurs, C the shape, E the fall-off.
function tirePacejka(D, slip, B, C, E)
  local bs = (B or 12.0) * (slip or 0)
  return (D or 0) * math.sin((C or 1.4) *
    math.atan(bs - (E or 0.97) * (bs - math.atan(bs))))
end

-- Longitudinal magic formula: peak slip ~0.12 at nominal pressure, scaling
-- with pressure. Returns force (N) and the effective mu.
function tirePacejkaLong(mu, normalForce, slip, pressure)
  local n = math.max(0, normalForce or 0)
  local p = pressure or 2.4
  local muEff = (mu or 0.9) * tirePressureMuScale(p)
  local peakSlip = 0.12 * tirePressurePeakScale(p)
  local B = 2.08 / math.max(0.02, peakSlip)
  local f = tirePacejka(muEff * n, slip, B, 1.4, 0.0)
  -- Real tires settle a little after the peak; keep that fall-off separate so
  -- the magic-formula peak still lands exactly on `peakSlip`.
  local a = math.abs(slip or 0)
  if a > peakSlip then
    local over = math.min(1.0, (a - peakSlip) / (peakSlip * 4.0))
    f = f * (1.0 - 0.15 * over)
  end
  return f, muEff
end

-- Lateral magic formula on the slip angle (radians). Peak around 0.10 rad.
function tirePacejkaLat(muEff, normalForce, slipAngle)
  local n = math.max(0, normalForce or 0)
  local peak = 0.10
  local B = 2.62 / peak
  local f = tirePacejka((muEff or 0.9) * n * 0.95, slipAngle, B, 1.3, 0.0)
  local a = math.abs(slipAngle or 0)
  if a > peak then
    local over = math.min(1.0, (a - peak) / (peak * 4.0))
    f = f * (1.0 - 0.15 * over)
  end
  return f
end

-- Combined-slip friction ellipse. Longitudinal demand uses the Pacejka
-- magic formula, lateral demand uses the slip angle through the same model;
-- both are scaled together so the resultant never exceeds mu_eff * N.
-- Returns fx, fy (N), utilization and the effective mu.
function tireCombinedForce(mu, normalForce, slip, latV, wheelMass, dt, peak, sat,
                           pressure, vLong, muLat)
  local n = math.max(0, normalForce or 0)
  local p = pressure or 2.4
  local loadScale = tireLoadMuScale(n) * tirePressureMuScale(p)
  local muEff = (mu or 0.9) * loadScale
  local muLatEff = (muLat or mu or 0.9) * loadScale
  local cap = math.max(1.0, math.max(muEff, muLatEff) * n)
  local fx = tirePacejkaLong(mu, n, slip, p)
  local fwd = math.max(1.0, math.abs(vLong or 8.0))
  local alpha = math.atan((latV or 0) / fwd)
  local fy = -tirePacejkaLat(muLatEff, n, alpha)
  local mag = math.sqrt((fx / cap) ^ 2 + (fy / cap) ^ 2)
  local scale = 1.0
  if mag > 1.0 then scale = 1.0 / mag end
  return fx * scale, fy * scale, mag, muEff
end

-- Multi-rib tread band: four ribs across the tire (1-2 inner, 3-4 outer).
-- Longitudinal slip heats the inner ribs more, lateral slip the outer ones;
-- heat conducts sideways between neighbouring ribs before airflow cooling.
local RIB_LONG = { 0.34, 0.28, 0.22, 0.16 }
local RIB_LAT = { 0.14, 0.22, 0.30, 0.34 }
local RIB_CONDUCT = 0.6

function tireRibStep(ribs, speed, slipLong, slipLat, load, dt, ambient)
  local amb = ambient or 22
  local v = math.abs(speed or 0)
  local n = math.max(0, load or 0)
  local step = dt or 0.016
  local heated = {}
  for k = 1, 4 do
    local t = (ribs and ribs[k]) or amb
    local power = math.min(8000,
      (math.abs(slipLong or 0) * RIB_LONG[k] +
       math.abs(slipLat or 0) * RIB_LAT[k]) * n * 0.40 + v * n * 0.012)
    heated[k] = t + power * 0.008 * step
  end
  -- Explicit lateral heat conduction between neighbouring ribs.
  local cond = {}
  for k = 1, 4 do
    local left = heated[k - 1] or heated[k]
    local right = heated[k + 1] or heated[k]
    cond[k] = heated[k] + RIB_CONDUCT * (left + right - 2 * heated[k]) * step
  end
  local cool = (0.15 + 0.02 * v) * step
  local out = {}
  for k = 1, 4 do
    local t = cond[k] - (cond[k] - amb) * cool
    if t < amb then t = amb end
    if t > 150 then t = 150 end
    out[k] = t
  end
  return out
end

-- Effective mu for one side of the tread (inner ribs 1-2 for drive/brake,
-- outer ribs 3-4 for cornering), using each rib's own temperature/wear.
function tireRibMu(mu, ribs, wears, k0, k1)
  local sum, n = 0.0, 0
  for k = k0, k1 do
    sum = sum + tireMuScale((ribs and ribs[k]) or 22,
                            (wears and wears[k]) or 100)
    n = n + 1
  end
  return (mu or 0.9) * (sum / math.max(1, n))
end

-- Longitudinal tire force from a slip ratio (saturating, so it peaks at the
-- friction limit). Exposed for the self-test and kept for older callers.
function slipTraction(mu, normalForce, slip)
  local s = math.max(-1.0, math.min(1.0, (slip or 0) * 6.0))
  return (mu or 0.9) * math.max(0, normalForce or 0) * s
end

-- Slip-ratio curve with a peak and a sliding plateau: force rises to
-- mu*N at |slip| = peak, then falls to sat*mu*N at large slip. Exposed for
-- the self-test.
function tireForceCurve(mu, normalForce, slip, peak, sat)
  local p = math.max(0.02, peak or 0.14)
  local s = math.max(0.2, math.min(1.0, sat or 0.75))
  local a = math.abs(slip or 0)
  local shape
  if a <= p then
    shape = a / p
  else
    local over = math.min(1.0, (a - p) / (p * 5.0))
    shape = 1.0 - (1.0 - s) * over
  end
  if slip ~= nil and slip < 0 then shape = -shape end
  return (mu or 0.9) * math.max(0, normalForce or 0) * shape
end

-- Slip ratio from the wheel's own angular velocity: 0 when the tire rolls
-- without slip, >0 under wheelspin, <0 when the wheel is locked / braking.
function wheelSlipRatio(omega, vLong, radius)
  local surface = (omega or 0) * (radius or 0.40)
  local denom = math.max(2.0, math.abs(vLong or 0))
  return (surface - (vLong or 0)) / denom
end

-- Ackermann steering: the inner front wheel turns tighter than the outer one.
-- Returns the signed left/right wheel angles for a positive-left steer angle.
function ackermannAngles(steer, wheelbase, track)
  local s = steer or 0
  if math.abs(s) < 1e-4 then return 0, 0 end
  local L = wheelbase or WHEELBASE
  local half = (track or WHEEL_TRACK) * 0.5
  local r = L / math.tan(math.abs(s))
  if r <= half + 0.01 then r = half + 0.01 end
  local inner = math.atan(L / (r - half))
  local outer = math.atan(L / (r + half))
  if s > 0 then return inner, outer end
  return -outer, -inner
end

-- Speed-sensitive steering assist: full lock when parking, less lock at road
-- speed so the RV does not twitch. Exposed for the self-test.
function steeringMaxAngle(speed)
  local s = math.abs(speed or 0)
  local f = math.min(1.0, math.max(0.0, (s - 2.0) / 8.0))
  return 0.55 - 0.25 * f -- 0.55 rad at rest, 0.30 rad at >=10 m/s
end

-- Self-centering rate (rad/s) for a released wheel: grows with speed and with
-- the current angle, so parking is light and a fast lane change snaps back.
function steeringReturnRate(speed, angle)
  local s = math.min(1.0, math.max(0.0, (math.abs(speed or 0) - 0.3) / 5.0))
  return (0.5 + 2.5 * s) * math.abs(angle or 0)
end

-- Advance the front-wheel command: input drives it at a rate limit, releasing
-- it self-centers. Used by the single-player and co-op drivers.
function updateSteeringCommand(input, dt)
  local step = math.min(dt or 0.016, 0.05)
  local speed = rv.speed or 0
  local maxAng = steeringMaxAngle(speed)
  local cur = rv.steerCmd or 0
  local desired = (input or 0) * maxAng
  local delta = desired - cur
  local rate
  if math.abs(input or 0) > 0.01 then
    rate = 3.5 -- rad/s while the driver holds a direction
  else
    rate = steeringReturnRate(speed, cur)
  end
  local maxStep = rate * step
  if delta > maxStep then delta = maxStep end
  if delta < -maxStep then delta = -maxStep end
  rv.steerCmd = cur + delta
  return rv.steerCmd
end

-- Screen-space-error terrain LOD with hysteresis + chunk streaming.
-- Each level's geometric error (metres) is projected to pixels:
--   px = error * (screenH / (2*tan(fov/2))) / distance
-- A chunk degrades to the next coarser mesh once that mesh projects at or
-- below TARGET pixels, and only upgrades again once its current mesh projects
-- above HYST pixels -- the gap is the hysteresis band that stops flicker.
-- Chunks farther than the streaming radius hide entirely (and reload when the
-- RV comes back), which also keeps the far ring out of the vertex path.
local TERRAIN_LOD_ERR = { 0.10, 0.257, 0.449 }  -- L0 / L1 / L2 geometric error (m)
local TERRAIN_LOD_TARGET = 2.0                  -- px: switch coarser at <= this
local TERRAIN_LOD_HYST = 2.5                    -- px: switch finer above this
local TERRAIN_VIEW_LOAD = 190.0                 -- reload radius (m)
local TERRAIN_VIEW_UNLOAD = 210.0               -- unload radius (m, hysteresis)
local TERRAIN_FOV_DEG = 60.0
local terrainLodT = 0.0
local terrainLodLogged = false

function terrainLodK(screenH, fovDeg)
  local h = screenH or 720
  local half = math.rad(fovDeg or TERRAIN_FOV_DEG) * 0.5
  local t = math.tan(half)
  if t < 0.01 then t = 0.01 end
  return h / (2 * t)
end

function terrainLodLevel(dist, current, k)
  local d = math.max(dist or 0, 1.0)
  local kk = k or terrainLodK(720, TERRAIN_FOV_DEG)
  local level = math.max(1, math.min(3, current or 1))
  while level < 3 and
        TERRAIN_LOD_ERR[level + 1] * kk / d <= TERRAIN_LOD_TARGET do
    level = level + 1
  end
  while level > 1 and
        TERRAIN_LOD_ERR[level] * kk / d > TERRAIN_LOD_HYST do
    level = level - 1
  end
  return level
end

function terrainChunkStreamed(dist, loaded)
  if loaded then return dist <= TERRAIN_VIEW_UNLOAD end
  return dist <= TERRAIN_VIEW_LOAD
end

function updateTerrainLod(dt, force)
  if terrainChunks == nil or #terrainChunks == 0 then return end
  terrainLodT = terrainLodT + (dt or 0)
  if not force and terrainLodT < 0.2 then return end
  terrainLodT = 0
  local cx, cz = rv.x or 0, rv.z or 0
  if player ~= nil and player.active and player.x ~= nil then
    cx, cz = player.x, player.z
  end
  local vp = GetViewportSize and GetViewportSize() or nil
  local k = terrainLodK(vp and vp.h or 720, TERRAIN_FOV_DEG)
  local counts = { 0, 0, 0 }
  local loadedN, unloadedN, changes = 0, 0, 0
  for _, c in ipairs(terrainChunks) do
    local dx, dz = c.x - cx, c.z - cz
    local d = math.sqrt(dx * dx + dz * dz)
    local loaded = c.loaded ~= false
    if not terrainChunkStreamed(d, loaded) then
      if loaded then
        for i = 1, 3 do
          if c.levels[i] ~= nil then SetVisible(c.levels[i], false) end
        end
        c.loaded = false
        c.level = nil
        changes = changes + 1
        if changes <= 6 then
          print(string.format("[terrain-lod] unload %s d=%.0f", tostring(c.stage), d))
        end
      end
      unloadedN = unloadedN + 1
    else
      local want = terrainLodLevel(d, c.level, k)
      if (not loaded) or c.level ~= want then
        for i = 1, 3 do
          if c.levels[i] ~= nil then SetVisible(c.levels[i], i == want) end
        end
        c.level = want
        c.loaded = true
        changes = changes + 1
        if changes <= 6 then
          print(string.format("[terrain-lod] %s%s -> L%d d=%.0f px=%.1f",
                loaded and "" or "load ", tostring(c.stage), want, d,
                TERRAIN_LOD_ERR[want] * k / math.max(d, 1)))
        end
      end
      counts[want] = counts[want] + 1
      loadedN = loadedN + 1
    end
  end
  if changes > 0 or not terrainLodLogged then
    terrainLodLogged = true
    print(string.format(
      "[terrain-lod] near=%d mid=%d far=%d loaded=%d unloaded=%d changes=%d",
      counts[1], counts[2], counts[3], loadedN, unloadedN, changes))
  end
end

-- Client-side wheel prediction: the chassis pose is replicated from the server,
-- so only the four wheel bodies are advanced locally (suspension + rolling).
function updateWheelPrediction(dt)
  if rv.wheelBodies == nil then return end
  local step = math.min(dt or 0.016, 0.033)
  for i, w in ipairs(WHEELS) do
    local wb = rv.wheelBodies[i]
    if wb ~= nil then
      if PhysicsActivate ~= nil then PhysicsActivate(wb.body) end
      local bp = PhysicsGetPosition(wb.body)
      local bv = PhysicsGetVelocity(wb.body)
      if bp ~= nil then
        local ground = terrainHeight(bp.x, bp.z)
        local target = ground + SUSP_R
        local vy = bv and bv.y or 0
        local force = SUSP_K * (target - bp.y) - SUSP_C * vy
        if force > WHEEL_BODY_MAX_F then force = WHEEL_BODY_MAX_F end
        if force < -WHEEL_BODY_MAX_F then force = -WHEEL_BODY_MAX_F end
        PhysicsApplyImpulse(wb.body, { x = 0, y = force * step, z = 0 },
                            { x = bp.x, y = bp.y, z = bp.z })
        local vLong = (bv ~= nil) and
                      (bv.x * math.sin(rv.yaw) + bv.z * math.cos(rv.yaw)) or 0
        local axx, axy, axz = rvOffset(1, 0, 0)
        -- Prediction only: nudge the wheel towards the replicated rolling
        -- speed with a torque (the client never overwrites angular velocity).
        local wv = PhysicsGetAngularVelocity and
                   PhysicsGetAngularVelocity(wb.body) or nil
        local omega = wv and (wv.x * axx + wv.y * axy + wv.z * axz) or 0
        if wb.rolling and PhysicsAddAngularImpulse ~= nil then
          local err = (rv.speed or 0) / SUSP_R - omega
          local imp = math.max(-40.0, math.min(40.0, err * 4.0)) * step
          PhysicsAddAngularImpulse(wb.body,
                                   { x = axx * imp, y = axy * imp, z = axz * imp })
        end
        rv.wheelSpinI = rv.wheelSpinI or {}
        rv.wheelSpinI[i] = ((rv.wheelSpinI[i] or 0) + omega * step)
                            % (math.pi * 2)
      end
    end
  end
  updateWheelMotion(dt)
end

function updateRvPhysics(dt)
  if rvBody == nil or PhysicsApplyImpulse == nil or PhysicsGetPosition == nil
     or PhysicsGetRotation == nil then
    return false
  end
  local p = PhysicsGetPosition(rvBody)
  local q = PhysicsGetRotation(rvBody)
  if p == nil or q == nil then return false end

  local yaw, pitch, roll = rvYawPitchRollFromQuat(q)
  rv.x, rv.z = p.x, p.z
  rv.groundY = p.y - 0.9
  rv.yaw = yaw
  rv.pitch = math.max(-0.5, math.min(0.5, pitch))
  rv.roll = math.max(-0.5, math.min(0.5, roll))

  local cp, sp = math.cos(rv.pitch), math.sin(rv.pitch)
  local cr, sr = math.cos(rv.roll), math.sin(rv.roll)
  local cyw, syw = math.cos(rv.yaw), math.sin(rv.yaw)
  local step = math.min(dt or 0.016, 0.033)
  local mass = rv.mass or 1200
  local steerL, steerR = ackermannAngles(rv.steerVis or 0, WHEELBASE, WHEEL_TRACK)
  rv.steerLeft, rv.steerRight = steerL, steerR
  local fwdX, fwdZ = math.sin(rv.yaw), math.cos(rv.yaw)
  local targetSpeed = rv.speed or 0
  local axleX, axleY, axleZ = rvOffset(1, 0, 0)
  local upX, upY, upZ = rvOffset(0, 1, 0)
  local s = rv.susp
  if s == nil then
    s = { h = {}, v = {} }
    rv.susp = s
  end
  s.plane = s.plane or {}
  s.travel = s.travel or {}

  local vel = PhysicsGetVelocity ~= nil and PhysicsGetVelocity(rvBody) or nil
  local vx = vel and vel.x or 0
  local vz = vel and vel.z or 0

  -- The heading is now driven by the front-wheel steering and tire forces.
  -- A little angular damping keeps it from oscillating at the friction limit.
  if PhysicsGetAngularVelocity ~= nil and PhysicsAddAngularImpulse ~= nil then
    local w = PhysicsGetAngularVelocity(rvBody)
    if w ~= nil and math.abs(w.y) > 1e-5 then
      local inertia = rv.yawInertia or (mass * (2.4 * 2.4 + 8.6 * 8.6) / 12)
      PhysicsAddAngularImpulse(rvBody, { x = 0, y = -w.y * inertia * 0.05, z = 0 })
    end
  end

  local useBodies = rv.wheelBodies ~= nil and rv.wheelBodies[1] ~= nil
  -- Per-wheel tire state: each wheel has its own temperature/wear and surface.
  rv.tireTemps = rv.tireTemps or { 22, 22, 22, 22 }
  rv.tireWear = rv.tireWear or { 100, 100, 100, 100 }
  if rv.tireRibs == nil then
    rv.tireRibs = {}
    for i = 1, 4 do
      local t = rv.tireTreads and rv.tireTreads[i] or nil
      rv.tireRibs[i] = { t and t[1] or 22, t and t[1] or 22,
                         t and t[2] or 22, t and t[2] or 22 }
    end
  end
  if rv.tireRibWear == nil then
    rv.tireRibWear = {}
    for i = 1, 4 do
      local w = rv.tireWear[i] or 100
      rv.tireRibWear[i] = { w, w, w, w }
    end
  end
  rv.tirePressureCold = rv.tirePressureCold or { 2.4, 2.4, 2.4, 2.4 }
  rv.tirePressure = rv.tirePressure or { 2.4, 2.4, 2.4, 2.4 }
  local wheelV, wheelSlipSpd, wheelLatSpd, wheelLoad, wheelSurf = {}, {}, {}, {}, {}
  local heatPower, slipSum, slipSpeedSum, slipCount = 0, 0, 0, 0
  local muLast, surfLast = 0.9, "grass"
  if useBodies and PhysicsActivate ~= nil then
    PhysicsActivate(rvBody)
    for i = 1, #rv.wheelBodies do
      PhysicsActivate(rv.wheelBodies[i].body)
    end
  end
  for i, w in ipairs(WHEELS) do
    local steer = 0
    if w.front then
      steer = (i == 1) and steerL or steerR
    end
    local wx, wy, wz, ground, vp, nForce, mu

    if useBodies then
      local wb = rv.wheelBodies[i]
      local bp = PhysicsGetPosition(wb.body)
      local bv = PhysicsGetVelocity(wb.body)
      wx, wy, wz = bp.x, bp.y, bp.z
      vp = PhysicsGetPointVelocity and
           PhysicsGetPointVelocity(wb.body, { x = wx, y = wy, z = wz }) or bv
      ground = terrainHeight(wx, wz)
      -- Target the contact height (centre = radius): the tire then presses on
      -- the ground so friction can produce/absorb the wheel spin.
      local targetY = ground + SUSP_R
      local compression = targetY - wy
      local vy = vp and vp.y or (bv and bv.y) or 0
      local force = SUSP_K * compression - SUSP_C * vy
      if force > WHEEL_BODY_MAX_F then force = WHEEL_BODY_MAX_F end
      if force < -WHEEL_BODY_MAX_F then force = -WHEEL_BODY_MAX_F end
      PhysicsApplyImpulse(wb.body, { x = 0, y = force * step, z = 0 },
                          { x = wx, y = wy, z = wz })
      nForce = math.max(0, SUSP_K * compression)
      local surfName, surfDef = surfaceInfo(wx, wz)
      mu = surfDef.mu * tireMuScale(rv.tireTemps[i], rv.tireWear[i]) * (weatherGripScale and weatherGripScale() or 1.0)
      rv.__surfDef = surfDef
      surfLast = surfName
      s.plane[i] = ground - (p.y - 0.9)
      s.travel[i] = 0
      s.h[i] = ground
    else
      local lx, ly, lz = w.lx, 0.38 - 0.9, w.lz
      local x1 = lx * cr - ly * sr
      local y1 = lx * sr + ly * cr
      local y2 = y1 * cp - lz * sp
      local z2 = y1 * sp + lz * cp
      local ox = x1 * cyw + z2 * syw
      local oz = -x1 * syw + z2 * cyw
      wx, wy, wz = p.x + ox, p.y + y2, p.z + oz
      ground = terrainHeight(wx, wz)
      local compression = ground + SUSP_REST - (wy - SUSP_R)
      vp = PhysicsGetPointVelocity and
           PhysicsGetPointVelocity(rvBody, { x = wx, y = wy, z = wz }) or nil
      local vy = vp and vp.y or 0
      local force = SUSP_K * compression - SUSP_C * vy
      if force > SUSP_MAX_F then force = SUSP_MAX_F end
      if force < -SUSP_MAX_F then force = -SUSP_MAX_F end
      PhysicsApplyImpulse(rvBody, { x = 0, y = force * step, z = 0 },
                          { x = wx, y = wy, z = wz })
      nForce = math.max(0, SUSP_K * compression)
      local surfName, surfDef = surfaceInfo(wx, wz)
      mu = surfDef.mu * tireMuScale(rv.tireTemps[i], rv.tireWear[i]) * (weatherGripScale and weatherGripScale() or 1.0)
      rv.__surfDef = surfDef
      surfLast = surfName
      s.plane[i] = ground - (p.y - 0.9)
      s.travel[i] = 0
      s.h[i] = ground
    end

    local targetBody = useBodies and rv.wheelBodies[i].body or rvBody
    local point = { x = wx, y = wy, z = wz }
    local wyaw = rv.yaw + steer
    local lrx, lrz = -math.cos(wyaw), math.sin(wyaw)
    local latV = vp and (vp.x * lrx + vp.z * lrz) or
                 (vx * lrx + vz * lrz)

    local vLong = vp and (vp.x * fwdX + vp.z * fwdZ) or
                  (vx * fwdX + vz * fwdZ)
    local braking = math.abs(targetSpeed) < math.abs(vLong) - 0.05
    local slip, omega = 0, 0
    local applyPoint = point
    if useBodies then
      local wb = rv.wheelBodies[i]
      local wv = PhysicsGetAngularVelocity and
                 PhysicsGetAngularVelocity(wb.body) or nil
      omega = wv and (wv.x * axleX + wv.y * axleY + wv.z * axleZ) or 0
      -- Engine torque on the rear axle / brake torque on every wheel. The
      -- reaction comes from the tire force below, so a wheel can spin up
      -- (wheelspin) or lock (lockup) instead of being set kinematically.
      if wb.rolling and PhysicsAddAngularImpulse ~= nil then
        local torque = 0
        if braking then
          if omega > 0.2 then torque = -BRAKE_TORQUE
          elseif omega < -0.2 then torque = BRAKE_TORQUE end
        elseif not w.front then
          local drive = targetSpeed - vLong
          if math.abs(drive) > 0.05 then
            local amount = math.min(1.0, math.abs(drive) / 2.0)
            torque = (drive > 0 and 1 or -1) * DRIVE_TORQUE * amount
          end
        end
        if torque ~= 0 then
          PhysicsAddAngularImpulse(wb.body,
            { x = axleX * torque * step, y = axleY * torque * step,
              z = axleZ * torque * step })
        end
      end
      if math.abs(omega) > WHEEL_MAX_OMEGA then
        local excess = omega - (omega > 0 and WHEEL_MAX_OMEGA or -WHEEL_MAX_OMEGA)
        if PhysicsAddAngularImpulse ~= nil then
          PhysicsAddAngularImpulse(wb.body,
            { x = -axleX * excess * 6.0, y = -axleY * excess * 6.0,
              z = -axleZ * excess * 6.0 })
        end
      end
      slip = wheelSlipRatio(omega, vLong, SUSP_R)
      -- Tire force at the contact patch: the offset from the wheel centre
      -- turns the same impulse into the counter-torque on the wheel.
      applyPoint = { x = wx - upX * SUSP_R, y = wy - upY * SUSP_R,
                     z = wz - upZ * SUSP_R }
    else
      slip = (targetSpeed - vLong) /
             math.max(2.0, math.abs(targetSpeed))
    end

    local surfDef = rv.__surfDef or SURFACES.grass
    local slipSpeed = math.abs(slip) * math.abs(vLong)
    wheelV[i] = math.abs(vLong)
    wheelSlipSpd[i] = slipSpeed
    wheelLatSpd[i] = math.abs(latV)
    wheelLoad[i] = nForce
    wheelSurf[i] = surfDef
    if not w.front then
      heatPower = heatPower + math.abs(slip) * nForce * math.abs(vLong)
      slipSum = slipSum + math.abs(slip)
      slipSpeedSum = slipSpeedSum + slipSpeed
      slipCount = slipCount + 1
    end
    muLast = mu

    -- Every tire carries longitudinal force from its own slip (that is what
    -- spins a free-rolling front wheel up to road speed); the front axle keeps
    -- slightly less authority so the rear still leads under braking.
    local factor = 1.0
    if w.front then factor = braking and 1.0 or 0.8 end
    -- Combined slip: friction ellipse couples longitudinal and lateral force
    -- (load-sensitive mu), both applied at the contact patch.
    local press = tirePressureAt(rv.tireTemps[i] or 22,
                                 rv.tirePressureCold[i] or 2.4)
    rv.tirePressure[i] = press
    local ribs = rv.tireRibs[i]
    local ribWear = rv.tireRibWear[i]
    local muLong = tireRibMu(mu, ribs, ribWear, 1, 2)
    local muLat = tireRibMu(mu, ribs, ribWear, 3, 4)
    local fx, fy, util, muEff = tireCombinedForce(
        muLong, nForce, slip, latV, mass / 4, step, surfDef.peak, surfDef.sat,
        press, vLong, muLat)
    fx = fx * factor
    local impL = fx * step
    local impLat = fy * step
    if math.abs(impL) > 0.001 or math.abs(impLat) > 0.001 then
      PhysicsApplyImpulse(targetBody, {
        x = fwdX * impL + lrx * impLat, y = 0,
        z = fwdZ * impL + lrz * impLat,
      }, applyPoint)
    end
    muLast = muEff or mu

    if useBodies then
      rv.wheelSpinI = rv.wheelSpinI or {}
      rv.wheelSpinI[i] = ((rv.wheelSpinI[i] or 0) + omega * step)
                          % (math.pi * 2)
    end
  end

  -- Per-wheel temperature and wear: each tire is heated by its own slip power
  -- and normal load, cooled by airflow at its own speed, and abrades with its
  -- own slip speed. Clients keep the server's values (they only predict pose).
  local slipAvg = slipSum / math.max(1, slipCount)
  if useBodies and coopRole ~= "client" then
    local tempSum = 0
    for i = 1, 4 do
      local surfDef = wheelSurf[i] or SURFACES.grass
      local ribs = rv.tireRibs[i]
      local ribWear = rv.tireRibWear[i]
      local next = tireRibStep(ribs, wheelV[i] or 0,
                               wheelSlipSpd[i] or 0,
                               wheelLatSpd[i] or 0,
                               wheelLoad[i] or 0, step, 22)
      local tempSumI = 0
      local wearSumI = 0
      for k = 1, 4 do
        ribs[k] = next[k]
        tempSumI = tempSumI + ribs[k]
        local slipK = (wheelSlipSpd[i] or 0) * RIB_LONG[k] +
                      (wheelLatSpd[i] or 0) * 0.5 * RIB_LAT[k]
        ribWear[k] = tireWearStep(ribWear[k] or 100, slipK, step,
                                  surfDef.wear)
        wearSumI = wearSumI + ribWear[k]
      end
      rv.tireTemps[i] = tempSumI * 0.25
      rv.tireWear[i] = wearSumI * 0.25
      tempSum = tempSum + rv.tireTemps[i]
    end
    rv.tireTemp = tempSum / 4
    if parts ~= nil and parts.tires ~= nil then
      -- Repairs reset the whole set; otherwise the shared durability is the
      -- average of the four independent treads.
      local avgWear = 0
      for i = 1, 4 do avgWear = avgWear + (rv.tireWear[i] or 100) end
      avgWear = avgWear / 4
      if parts.tires > avgWear + 1.0 then
        for i = 1, 4 do
          rv.tireWear[i] = parts.tires
          for k = 1, 4 do rv.tireRibWear[i][k] = parts.tires end
        end
      else
        parts.tires = avgWear
      end
    end
  elseif rv.tireTemps ~= nil then
    rv.tireTemp = (rv.tireTemps[1] + rv.tireTemps[2] +
                   rv.tireTemps[3] + rv.tireTemps[4]) / 4
  end

  if rv.tireLog then
    rv.__logN = (rv.__logN or 0) + 1
    if rv.__logN % 30 == 1 then
      local wb3 = rv.wheelBodies and rv.wheelBodies[3]
      local wv3 = wb3 and PhysicsGetAngularVelocity and PhysicsGetAngularVelocity(wb3.body) or nil
      local axx3, axy3, axz3 = rvOffset(1, 0, 0)
      local om3 = wv3 and (wv3.x*axx3 + wv3.y*axy3 + wv3.z*axz3) or 0
      local vf3 = vx * fwdX + vz * fwdZ
      local temps = rv.tireTemps or { 22, 22, 22, 22 }
      local wears = rv.tireWear or { 100, 100, 100, 100 }
      local rib3 = rv.tireRibs and rv.tireRibs[3] or { 22, 22, 22, 22 }
      local pr = rv.tirePressure or { 2.4, 2.4, 2.4, 2.4 }
      print(string.format("[tire] surf=%s mu=%.2f temp=%.0f T=(%.0f/%.0f/%.0f/%.0f) P=(%.2f/%.2f/%.2f/%.2f) W=(%.1f/%.1f/%.1f/%.1f) ribsRL=(%.0f/%.0f/%.0f/%.0f) cmd=%.1f vf=%.1f omega=%.1f slip=%.2f",
        surfLast, muLast, rv.tireTemp or 22,
        temps[1], temps[2], temps[3], temps[4],
        pr[1], pr[2], pr[3], pr[4],
        wears[1], wears[2], wears[3], wears[4],
        rib3[1], rib3[2], rib3[3], rib3[4],
        targetSpeed, vf3, om3, slipAvg))
    end
  end

  updateWheelMotion(dt)
  return true
end
