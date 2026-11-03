-- lc_driveevents.lua
-- 途中随机事件：零散感染者横穿/路边补给/燃油桶

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
  for _, d in ipairs(driveDeer) do Despawn(d.ent) end
  driveDeer = {}
end

-- 事件原语（流程图 game/drive-* 节点调用；权重编排放 drive_event.flow.json）
function spawnDriveHorde(n)
  local base = 25 + math.random() * 10
  local lat0 = (math.random() * 2 - 1) * 8
  for _ = 1, math.min(3, n or 2) do
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
end

function spawnDriveProp(kind)
  local prefab = (kind == "fuelcan") and "rv_fuelcan" or "rv_loot_crate"
  local ent = SpawnPrefab(prefab, { x = 0, y = -50, z = 0 })
  if ent ~= nil then
    local wx, wz = rotOf((math.random() * 2 - 1) * 6, 22 + math.random() * 10)
    SetPosition(ent, { x = wx, y = (kind == "fuelcan") and 0.35 or 0.28, z = wz })
    SetScale(ent, 0.55, 0.55, 0.55)
    driveProps[#driveProps + 1] = { ent = ent, x = wx, z = wz, kind = kind }
  end
end

-- 路面障碍：倒木/落石/废弃车堵住半幅路，必须打方向绕过；撞上掉耐久。
-- 静态碰撞盒记在 bodyIds 里，通过后一起移除，避免留下看不见的墙。
function spawnDriveRoadblock()
  local base = 24 + math.random() * 10
  local side = (math.random() < 0.5) and -1 or 1
  local group = { kind = "roadblock", x = 0, z = 0, bodyIds = {},
                  parts = {}, life = 45 }
  local layout = {
    { pre = "rv_wreck", lx = side * 3.4, lz = base,        sx = 1.0, sy = 0.9, sz = 1.0 },
    { pre = "rv_rock",  lx = side * 2.4, lz = base + 1.4,  sx = 1.2, sy = 1.0, sz = 1.2 },
    { pre = "rv_rock",  lx = side * 1.3, lz = base + 2.6,  sx = 0.9, sy = 0.8, sz = 0.9 },
  }
  for i, d in ipairs(layout) do
    local wx, wz = rotOf(d.lx, d.lz)
    local e = SpawnPrefab(d.pre, { x = wx, y = -50, z = wz })
    if e ~= nil then
      local gy = (terrainHeight and terrainHeight(wx, wz)) or 0
      SetPosition(e, { x = wx, y = gy + 0.55 * d.sy, z = wz })
      SetScale(e, d.sx, d.sy, d.sz)
      SetRotationY(e, math.random() * math.pi * 2)
      group.parts[#group.parts + 1] = e
      if PhysicsAddBox ~= nil then
        local id = PhysicsAddBox(
          { x = wx, y = gy + 0.55 * d.sy, z = wz },
          { x = 0.55 * d.sx, y = 0.55 * d.sy, z = 0.55 * d.sz }, false, {})
        if id ~= nil then group.bodyIds[#group.bodyIds + 1] = id end
      end
      if i == 1 then group.x, group.z = wx, wz end
    end
  end
  driveProps[#driveProps + 1] = group
  toast.text = "前方路面被堵——打方向绕过去"
  toast.t = 3.5
end

function despawnDriveRoadblock(p)
  for _, e in ipairs(p.parts or {}) do Despawn(e) end
  for _, id in ipairs(p.bodyIds or {}) do
    if PhysicsRemove ~= nil then PhysicsRemove(id) end
  end
  p.parts, p.bodyIds = {}, {}
end

-- 野生动物横穿：从路侧冲向另一侧，撞上掉车体耐久/货况；不追车。
function spawnDriveDeer(n)
  local base = 26 + math.random() * 12
  local side = (math.random() < 0.5) and -1 or 1
  for _ = 1, math.min(2, n or 1) do
    local ent = SpawnPrefab("rv_rabbit", { x = 0, y = -60, z = 0 })
    if ent ~= nil then
      local lx = side * 9.0
      local wx, wz = rotOf(lx, base + (math.random() * 2 - 1) * 4)
      local gy = (terrainHeight and terrainHeight(wx, wz)) or 0
      SetPosition(ent, { x = wx, y = gy + 0.55, z = wz })
      SetScale(ent, 1.10, 0.85, 1.45)
      local sp = 3.2 + math.random() * 1.6
      -- 速度方向：世界空间垂直于公路（沿 X）
      local vx = -side * sp
      driveDeer[#driveDeer + 1] = { ent = ent, x = wx, z = wz, vx = vx, vz = 0,
                                    life = 14, phase = math.random() * 6.28 }
    end
  end
end

function despawnDriveDeer(d)
  Despawn(d.ent)
end

function triggerEvent()
  -- 事件选择/权重/提示全部在 drive_event.flow.json（可视化可热调）
  LC.flow.emit("drive_event")
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
    if math.random() < 0.5 then
      local id = LOOT_ITEMS[math.random(1, #LOOT_ITEMS)].id
      addItem(id, 1)
      got[#got + 1] = itemName(id) .. "+1"
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
    if p.kind == "roadblock" then
      p.life = (p.life or 45) - dt
      if fwd < -24 or p.life <= 0 or d2 > 90 * 90 then
        despawnDriveRoadblock(p)
        table.remove(driveProps, i)
      end
    elseif d2 < 9 then
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
      if PlaySfx then PlaySfx("hurt") end
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
  for i = #driveDeer, 1, -1 do
    local d = driveDeer[i]
    d.life = d.life - dt
    d.x, d.z = d.x + d.vx * dt, d.z + d.vz * dt
    local gy = (terrainHeight and terrainHeight(d.x, d.z)) or 0
    SetPosition(d.ent, { x = d.x, y = gy + 0.55 +
                         math.abs(math.sin(raidT * 7 + d.phase)) * 0.18, z = d.z })
    SetRotationY(d.ent, dirYaw(d.vx, d.vz))
    local dx, dz = rv.x - d.x, rv.z - d.z
    local dist = math.sqrt(dx * dx + dz * dz)
    if dist < 2.6 and math.abs(rv.speed) > 1.5 then
      dur = math.max(0, dur - 5)
      if jobApplyImpact ~= nil then jobApplyImpact(3.0) end
      if PlaySfx then PlaySfx("hurt") end
      EmitParticles({ pos = { x = d.x, y = gy + 0.8, z = d.z }, count = 14,
                      speedMin = 1, speedMax = 3.0, lifeMin = 0.3, lifeMax = 0.7,
                      sizeStart = 0.18, sizeEnd = 0.05,
                      color = { r = 0.42, g = 0.20, b = 0.12, a = 0.9 },
                      colorEnd = { r = 0.42, g = 0.20, b = 0.12, a = 0.0 },
                      additive = false })
      SpawnFloatText(d.x, 1.2, d.z, "撞到野生动物 车体 -5",
                     false, 1.4, 1, 0.4, 0.3)
      Despawn(d.ent)
      table.remove(driveDeer, i)
    elseif d.life <= 0 or dist > 60 then
      Despawn(d.ent)
      table.remove(driveDeer, i)
    end
  end
end
