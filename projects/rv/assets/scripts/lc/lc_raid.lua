-- lc_raid.lua
-- 夜袭：尸群 AI/偷物资/射击/炮塔/手雷

function despawnHorde()
  for _, z in ipairs(horde) do Despawn(z.ent) end
  horde = {}
end

function spawnZombie()
  local ent = SpawnPrefab("rv_zombie", { x = 0, y = -50, z = 0 })
  if ent == nil then return end
  local a = math.random() * math.pi * 2
  local r = 14 + math.random() * 7
  local wx, wz = rotOf(math.sin(a) * r, math.cos(a) * r)
  local nd = nodeById(atNode or curNode)
  local danger = nd and nd.danger or 0
  SetPosition(ent, { x = wx, y = 0.8, z = wz })
  SetScale(ent, 0.5, 1.6, 0.4)
  horde[#horde + 1] = { ent = ent, x = wx, z = wz,
                        hp = RAID.hpBase + math.floor(danger / 2),
                        speed = RAID.speed * (0.85 + math.random() * 0.4)
                                + danger * 0.12,
                        phase = math.random() * 6.28, stealT = 0 }
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
  if math.random() < 0.2 then
    local mats = { "metal", "electronics", "cloth", "wood" }
    local m = mats[math.random(1, 4)]
    stopBag[m] = (stopBag[m] or 0) + 1
    SpawnFloatText(z.x, 1.4, z.z, MAT_NAMES[m] .. "+1",
                   false, 1.2, 1.0, 0.85, 0.4)
  end
end

function updateHorde(dt)
  raidT = raidT + dt
  if evacT >= 0 then
    hordeT = hordeT - dt
    if hordeT <= 0 and #horde < RAID.aliveMax then
      hordeT = RAID.spawnEvery
      spawnZombie()
    end
  end
  -- 主角下车时尸群优先扑向主角（更危险），否则奔着物资/房车去
  local tx, tz = rv.x, rv.z
  if onFoot and player.active then tx, tz = player.x, player.z end
  for i = #horde, 1, -1 do
    local z = horde[i]
    local dx, dz = tx - z.x, tz - z.z
    local dist = math.sqrt(dx * dx + dz * dz)
    if dist > 0.01 then
      z.x = z.x + dx / dist * z.speed * dt
      z.z = z.z + dz / dist * z.speed * dt
    end
    local sway = math.sin(raidT * 5.0 + z.phase)
    SetPosition(z.ent, { x = z.x, y = 0.8 + math.abs(sway) * 0.06, z = z.z })
    SetRotationY(z.ent, dirYaw(dx, dz) + sway * 0.15)
    if dist < RAID.attackDist then
      z.stealT = z.stealT - dt
      if z.stealT <= 0 then
        z.stealT = RAID.stealEvery
        local mats, n = {}, 0
        for m, vv in pairs(stopBag) do
          if vv > 0 then n = n + 1 mats[n] = m end
        end
        if n > 0 then
          local m = mats[math.random(1, n)]
          stopBag[m] = stopBag[m] - 1
          if stopBag[m] <= 0 then stopBag[m] = nil end
          SpawnFloatText(z.x, 1.5, z.z, "-" .. MAT_NAMES[m],
                         false, 1.2, 1, 0.3, 0.25)
        end
      end
    else
      z.stealT = 0
    end
  end
end

-- 射击：屏幕空间命中最近丧尸；枪口 = 主角（下车）/ 炮塔（有则）/ 车顶
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
  else
    local g = PickGround({ x = mx, y = my })
    if g then
      hx, hy, hz = g.x, 0.08, g.z
      EmitParticles({ pos = { x = hx, y = hy, z = hz }, count = 8,
                      speedMin = 0.5, speedMax = 2, lifeMin = 0.25, lifeMax = 0.5,
                      sizeStart = 0.15, sizeEnd = 0.04,
                      color = { r = 0.62, g = 0.57, b = 0.47, a = 0.7 },
                      colorEnd = { r = 0.62, g = 0.57, b = 0.47, a = 0.0 },
                      additive = false })
    end
  end
  -- 曳光：枪口到弹着点的亮粒子串
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

-- 投掷物：抛物线 + 落地范围爆炸（伤害尸群）
function updateGrenades(dt)
  for i = #grenades, 1, -1 do
    local g = grenades[i]
    g.vy = g.vy - 9.8 * dt
    g.x = g.x + g.vx * dt
    g.y = g.y + g.vy * dt
    g.z = g.z + g.vz * dt
    g.fuse = g.fuse - dt
    if g.y <= 0.12 or g.fuse <= 0 then
      EmitParticles({ pos = { x = g.x, y = 0.5, z = g.z }, count = 34,
                      shape = "sphere", radius = 0.8,
                      speedMin = 2, speedMax = 7, lifeMin = 0.35, lifeMax = 0.9,
                      sizeStart = 0.3, sizeEnd = 0.06,
                      color = { r = 1, g = 0.55, b = 0.15, a = 0.95 },
                      colorEnd = { r = 0.9, g = 0.3, b = 0.05, a = 0.0 },
                      additive = true })
      EmitParticles({ pos = { x = g.x, y = 0.6, z = g.z }, count = 16,
                      shape = "sphere", radius = 0.6,
                      speedMin = 1, speedMax = 3, lifeMin = 0.5, lifeMax = 1.2,
                      sizeStart = 0.5, sizeEnd = 0.1,
                      color = { r = 0.25, g = 0.23, b = 0.2, a = 0.8 },
                      colorEnd = { r = 0.3, g = 0.28, b = 0.25, a = 0.0 },
                      additive = false })
      for j = #horde, 1, -1 do
        local z = horde[j]
        local ddx, ddz = z.x - g.x, z.z - g.z
        if ddx * ddx + ddz * ddz < 16 then  -- 4m 爆炸半径
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
