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
