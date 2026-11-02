-- lc_raid.lua
-- 低强度威胁：零散感染者（不强制撤离、不成波刷新）

function despawnHorde()
  for _, z in ipairs(horde) do Despawn(z.ent) end
  horde = {}
end

function spawnAmbientZombie()
  if #horde >= RAID.aliveMax then return nil end
  local ent = SpawnPrefab("rv_zombie", { x = 0, y = -50, z = 0 })
  if ent == nil then return nil end
  local a = math.random() * math.pi * 2
  local r = 10 + math.random() * 7
  local wx, wz = rotOf(math.sin(a) * r, math.cos(a) * r)
  local nd = nodeById(atNode or curNode)
  local danger = nd and nd.danger or 0
  SetPosition(ent, { x = wx, y = 0.8, z = wz })
  SetScale(ent, 0.5, 1.6, 0.4)
  local z = { ent = ent, x = wx, z = wz,
              hp = RAID.hpBase + math.floor(danger / 2),
              speed = RAID.speed * (0.85 + math.random() * 0.4),
              phase = math.random() * 6.28, atkT = 0 }
  horde[#horde + 1] = z
  SpawnFloatText(wx, 1.6, wz, "感染者", false, 1.0, 0.95, 0.45, 0.3)
  return z
end

-- 兼容流程图节点：仍叫 spawnZombie，但受低强度上限约束
function spawnZombie()
  return spawnAmbientZombie()
end

function killZombie(idx)
  local z = horde[idx]
  EmitParticles({ pos = { x = z.x, y = 1.0, z = z.z }, count = 14,
                  speedMin = 1.0, speedMax = 3.5, lifeMin = 0.3, lifeMax = 0.7,
                  sizeStart = 0.22, sizeEnd = 0.05,
                  color = { r = 0.35, g = 0.12, b = 0.1, a = 0.9 },
                  colorEnd = { r = 0.35, g = 0.12, b = 0.1, a = 0.0 },
                  additive = false })
  Despawn(z.ent)
  table.remove(horde, idx)
  killCount = killCount + 1
  LC.flow.setvar("zombie_death", "drop_x", z.x)
  LC.flow.setvar("zombie_death", "drop_z", z.z)
  LC.flow.emit("zombie_killed")
end

function updateHorde(dt)
  if #horde == 0 then return end
  local tx, tz = rv.x, rv.z
  if onFoot and player.active and not buildMode then tx, tz = player.x, player.z end
  for i = #horde, 1, -1 do
    local z = horde[i]
    if z.atkT > 0 then z.atkT = z.atkT - dt end
    local dx, dz = tx - z.x, tz - z.z
    local dist = math.sqrt(dx * dx + dz * dz)
    if dist > 0.01 then
      z.x = z.x + dx / dist * z.speed * dt
      z.z = z.z + dz / dist * z.speed * dt
    end
    local sway = math.sin(raidT * 5.0 + z.phase)
    SetPosition(z.ent, { x = z.x, y = 0.8 + math.abs(sway) * 0.06, z = z.z })
    SetRotationY(z.ent, dirYaw(dx, dz) + sway * 0.15)
    if dist < RAID.attackDist and onFoot and player.active and not buildMode then
      if z.atkT <= 0 then
        z.atkT = RAID.attackCd
        local nd = nodeById(atNode or curNode)
        damagePlayer(RAID.attackDmg + (nd and nd.danger or 0))
        SpawnFloatText(player.x, 1.7, player.z, "受伤",
                       false, 1.0, 1, 0.3, 0.25)
        EmitParticles({ pos = { x = player.x, y = 1.2, z = player.z }, count = 8,
                        speedMin = 0.6, speedMax = 2.2, lifeMin = 0.2, lifeMax = 0.5,
                        sizeStart = 0.16, sizeEnd = 0.04,
                        color = { r = 0.5, g = 0.1, b = 0.08, a = 0.9 },
                        colorEnd = { r = 0.5, g = 0.1, b = 0.08, a = 0.0 },
                        additive = false })
      end
    end
  end
end

-- 射击：屏幕中心准星命中最近感染者
function shootAt(mx, my)
  local best, bestD = nil, RAID.hitRadius * RAID.hitRadius
  for i, z in ipairs(horde) do
    local sp = WorldToScreen(z.x, 1.0, z.z)
    if sp then
      local ddx, ddz = sp.x - mx, sp.y - my
      local d2 = ddx * ddx + ddz * ddz
      if d2 < bestD then best, bestD = i, d2 end
    end
  end
  local mzx, mzy, mzz = playerMuzzle()
  EmitParticles({ pos = { x = mzx, y = mzy, z = mzz }, count = 4,
                  speedMin = 2, speedMax = 5, lifeMin = 0.05, lifeMax = 0.12,
                  sizeStart = 0.16, sizeEnd = 0.03,
                  color = { r = 1, g = 0.8, b = 0.3, a = 0.95 },
                  colorEnd = { r = 1, g = 0.6, b = 0.1, a = 0.0 },
                  additive = true })
  recoilT = 0.12
  if PlaySfx then PlaySfx("shot") end
  local hx, hy, hz
  if best then
    local z = horde[best]
    z.hp = z.hp - (hasModule("gun_rack") and 2 or 1)
    hx, hy, hz = z.x, 1.0 + math.random() * 0.4, z.z
    hitMarkT = 0.12
    EmitParticles({ pos = { x = hx, y = hy, z = hz },
                    count = 6, speedMin = 0.5, speedMax = 2,
                    lifeMin = 0.2, lifeMax = 0.45,
                    sizeStart = 0.14, sizeEnd = 0.03,
                    color = { r = 0.4, g = 0.1, b = 0.08, a = 0.9 },
                    colorEnd = { r = 0.4, g = 0.1, b = 0.08, a = 0.0 },
                    additive = false })
    if z.hp <= 0 then killZombie(best) end
  end
  -- 曳光：枪口到弹着点
  if hx then
    for k = 1, 5 do
      local t = k / 5
      EmitParticles({ pos = { x = mzx + (hx - mzx) * t, y = mzy + (hy - mzy) * t,
                             z = mzz + (hz - mzz) * t },
                      count = 1, speedMin = 0, speedMax = 0,
                      lifeMin = 0.05, lifeMax = 0.09,
                      sizeStart = 0.06, sizeEnd = 0.02,
                      color = { r = 1, g = 0.85, b = 0.4, a = 0.85 },
                      colorEnd = { r = 1, g = 0.7, b = 0.2, a = 0.0 },
                      additive = true })
    end
  end
end

function turretFire(dt)
  if turretT > 0 then
    turretT = turretT - dt
    return
  end
  local turret = hasModule("turret")
  if not turret then return end
  local best, bestD = nil, RAID.turretRange * RAID.turretRange
  for i, z in ipairs(horde) do
    local ddx, ddz = z.x - rv.x, z.z - rv.z
    local d2 = ddx * ddx + ddz * ddz
    if d2 < bestD then best, bestD = i, d2 end
  end
  if not best then return end
  turretT = RAID.turretCd
  local z = horde[best]
  local tx, tz = rotOf(turret.lx, turret.lz)
  EmitParticles({ pos = { x = tx, y = (turret.ly or 2.6) + 0.5, z = tz }, count = 4,
                  speedMin = 2, speedMax = 5, lifeMin = 0.05, lifeMax = 0.1,
                  sizeStart = 0.14, sizeEnd = 0.03,
                  color = { r = 1, g = 0.9, b = 0.4, a = 0.95 },
                  colorEnd = { r = 1, g = 0.7, b = 0.1, a = 0.0 },
                  additive = true })
  EmitParticles({ pos = { x = z.x, y = 1.0, z = z.z }, count = 8,
                  speedMin = 0.8, speedMax = 2.5, lifeMin = 0.2, lifeMax = 0.4,
                  sizeStart = 0.16, sizeEnd = 0.04,
                  color = { r = 1, g = 0.75, b = 0.25, a = 0.9 },
                  colorEnd = { r = 1, g = 0.6, b = 0.15, a = 0.0 },
                  additive = true })
  z.hp = z.hp - 1
  if z.hp <= 0 then killZombie(best) end
end

-- 投掷物逻辑保留（当前版本不绑定按键，留给后续）
function updateGrenades(dt)
  for i = #grenades, 1, -1 do
    local g = grenades[i]
    g.vy = g.vy - 9.8 * dt
    g.x = g.x + g.vx * dt
    g.y = g.y + g.vy * dt
    g.z = g.z + g.vz * dt
    g.fuse = g.fuse - dt
    if g.y <= 0.12 or g.fuse <= 0 then
      for j = #horde, 1, -1 do
        local z = horde[j]
        local ddx, ddz = z.x - g.x, z.z - g.z
        if ddx * ddx + ddz * ddz < 16 then
          z.hp = z.hp - 5
          if z.hp <= 0 then killZombie(j) end
        end
      end
      Despawn(g.ent)
      table.remove(grenades, i)
    else
      SetPosition(g.ent, { x = g.x, y = g.y, z = g.z })
      SetRotationY(g.ent, raidT * 7)
    end
  end
end
