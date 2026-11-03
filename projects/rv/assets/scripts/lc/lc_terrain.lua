-- lc_terrain.lua
-- Analytic terrain height for the roadside embankment. Mirrors
-- tools/gen_rv_embankment.py (same cross-section, noise and taper) so the
-- visible mesh, the player and the RV all agree and nothing clips.

local TX = { 5.0, 8.0, 11.5, 15.0, 19.0, 23.5, 28.0, 33.0, 38.0 }
local TY = { 0.0, 0.70, 1.90, 3.00, 3.50, 3.55, 3.00, 1.50, 0.0 }
local TZ0, TZ1 = -24.0, 46.0

-- Height of the embankment at world (x, z); 0 anywhere else.
function terrainHeight(x, z)
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

-- Smoothly approach the sampled terrain height (avoids stair-stepping when
-- walking or driving over the crest).
function terrainFollow(current, x, z, dt, rate)
  local target = terrainHeight(x, z)
  return current + (target - current) * math.min(1, dt * (rate or 12))
end

-- Approximate uphill direction / steepness, for future slope physics.
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
