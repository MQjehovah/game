-- lc_driveevents.lua
-- 途中随机事件：尸群横穿/路边补给/燃油桶

function pickEvent()
  local total = 0
  for _, e in ipairs(EVENTS) do total = total + e.w end
  local r = math.random() * total
  for _, e in ipairs(EVENTS) do
    r = r - e.w
    if r <= 0 then return e.id end
  end
  return "supply"
end

function despawnDriveWorld()
  for _, p in ipairs(driveProps) do Despawn(p.ent) end
  driveProps = {}
  for _, z in ipairs(driveHorde) do Despawn(z.ent) end
  driveHorde = {}
end

function triggerEvent()
  local id = pickEvent()
  if id == "horde" then
    local n = 4 + math.random(0, 2)
    local base = 25 + math.random() * 10
    local lat0 = (math.random() * 2 - 1) * 8
    for _ = 1, n do
      local ent = SpawnPrefab("rv_zombie", { x = 0, y = -50, z = 0 })
      if ent ~= nil then
        local wx, wz = rotOf(lat0 + (math.random() * 2 - 1) * 3,
                             base + (math.random() * 2 - 1) * 6)
        SetPosition(ent, { x = wx, y = 0.8, z = wz })
        SetScale(ent, 0.5, 1.6, 0.4)
        driveHorde[#driveHorde + 1] = { ent = ent, x = wx, z = wz,
            speed = 1.6 + math.random() * 0.6,
            phase = math.random() * 6.28, life = 22 }
      end
    end
    toast.text = "尸群横穿！绕开它们——撞上掉车体耐久"
  elseif id == "supply" then
    local ent = SpawnPrefab("rv_loot_crate", { x = 0, y = -50, z = 0 })
    if ent ~= nil then
      local wx, wz = rotOf((math.random() * 2 - 1) * 6, 22 + math.random() * 10)
      SetPosition(ent, { x = wx, y = 0.28, z = wz })
      SetScale(ent, 0.55, 0.55, 0.55)
      driveProps[#driveProps + 1] = { ent = ent, x = wx, z = wz, kind = "supply" }
    end
    toast.text = "发现路边补给！开过去拾取"
  else
    local ent = SpawnPrefab("rv_fuelcan", { x = 0, y = -50, z = 0 })
    if ent ~= nil then
      local wx, wz = rotOf((math.random() * 2 - 1) * 6, 22 + math.random() * 10)
      SetPosition(ent, { x = wx, y = 0.35, z = wz })
      SetScale(ent, 0.5, 0.7, 0.5)
      driveProps[#driveProps + 1] = { ent = ent, x = wx, z = wz, kind = "fuelcan" }
    end
    toast.text = "路边有燃油桶！开过去拾取"
  end
  toast.t = 3.5
end

function collectDriveProp(p)
  if p.kind == "fuelcan" then
    local v = 10 + math.random(0, 8)
    fuel = math.min(VEH.tank, fuel + v)
    fuelWarned = fuel > VEH.tank * 0.5 and false or fuelWarned
    SpawnFloatText(p.x, 1.0, p.z, "燃料 +" .. v .. "L",
                   false, 1.2, 1.0, 0.85, 0.4)
  else
    local mats = { "metal", "electronics", "cloth", "wood" }
    local got = {}
    for _ = 1, 2 + math.random(0, 2) do
      local m = mats[math.random(1, 4)]
      local n = 1 + math.random(0, 2)
      materials[m] = (materials[m] or 0) + n
      got[#got + 1] = MAT_NAMES[m] .. "+" .. n
    end
    SpawnFloatText(p.x, 1.0, p.z, table.concat(got, " "),
                   false, 1.2, 1.0, 0.85, 0.4)
  end
  Despawn(p.ent)
end

function updateDriveWorld(dt)
  raidT = raidT + dt
  nextEventT = nextEventT - dt
  if nextEventT <= 0 then
    nextEventT = VEH.eventMin + math.random() * (VEH.eventMax - VEH.eventMin)
    if math.abs(rv.speed) > 3 then triggerEvent() end
  end
  local fx, fz = math.sin(rv.yaw), math.cos(rv.yaw)
  for i = #driveProps, 1, -1 do
    local p = driveProps[i]
    local dx, dz = p.x - rv.x, p.z - rv.z
    local d2 = dx * dx + dz * dz
    local fwd = dx * fx + dz * fz
    if d2 < 9 then
      collectDriveProp(p)
      table.remove(driveProps, i)
    elseif fwd < -30 then
      Despawn(p.ent)
      table.remove(driveProps, i)
    end
  end
  for i = #driveHorde, 1, -1 do
    local z = driveHorde[i]
    z.life = z.life - dt
    local dx, dz = rv.x - z.x, rv.z - z.z
    local dist = math.sqrt(dx * dx + dz * dz)
    if dist > 0.01 then
      z.x = z.x + dx / dist * z.speed * dt
      z.z = z.z + dz / dist * z.speed * dt
    end
    SetPosition(z.ent, { x = z.x, y = 0.8 + math.abs(math.sin(raidT * 5 + z.phase)) * 0.06, z = z.z })
    SetRotationY(z.ent, dirYaw(dx, dz))
    if dist < 2.0 and math.abs(rv.speed) > 1 then
      dur = math.max(0, dur - 3)
      EmitParticles({ pos = { x = z.x, y = 1.0, z = z.z }, count = 12,
                      speedMin = 1, speedMax = 3.5, lifeMin = 0.3, lifeMax = 0.6,
                      sizeStart = 0.2, sizeEnd = 0.05,
                      color = { r = 0.35, g = 0.12, b = 0.1, a = 0.9 },
                      colorEnd = { r = 0.35, g = 0.12, b = 0.1, a = 0.0 },
                      additive = false })
      SpawnFloatText(z.x, 1.3, z.z, "车体 -3", false, 1.2, 1, 0.35, 0.3)
      Despawn(z.ent)
      table.remove(driveHorde, i)
    elseif z.life <= 0 or dist > 70 then
      Despawn(z.ent)
      table.remove(driveHorde, i)
    end
  end
end
