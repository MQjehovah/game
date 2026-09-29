-- lc_drive.lua
-- 行驶物理：Jolt 速度驱动/燃油/撞击耐久/临近节点

function updateDrive(dt)
  local throttle = (ActionDown("w") and 1 or 0) - (ActionDown("s") and 1 or 0)
  local steer = (ActionDown("a") and 1 or 0) - (ActionDown("d") and 1 or 0)
  if autoPilot then
    driveT = driveT + dt
    if nearNode and nearNode.id ~= "camp" and driveT > 3.0 then
      setPhase("parked")  -- 靠站（autoPilot 保持 true：搜刮接管）
      spawnPlayer()       -- 验证钩子：自动下车走两步（蒙皮主角检查）
      return
    end
    if driveT > 45.0 then
      autoPilot = false
      setPhase("parked")
      return
    end
    throttle = 1
    if autoTarget == nil then
      local bestD = 1e18
      for _, nd in ipairs(NODES) do
        if nd.id ~= "camp" then
          local ddx, ddz = nd.x - rv.x, nd.z - rv.z
          local d2 = ddx * ddx + ddz * ddz
          if d2 < bestD then bestD = d2 autoTarget = nd end
        end
      end
    end
    if autoTarget then
      local dx, dz = autoTarget.x - rv.x, autoTarget.z - rv.z
      local desired = dirYaw(dx, dz)
      local err = desired - rv.yaw
      while err > math.pi do err = err - 2 * math.pi end
      while err < -math.pi do err = err + 2 * math.pi end
      steer = math.max(-1, math.min(1, err * 1.5))
    end
  end

  -- 读回上一物理步；意图速度被物理骤降 = 撞击 -> 掉耐久（冷却防研磨连扣）
  local bodyVy = 0
  if rvBody then
    local p = PhysicsGetPosition(rvBody)
    if p then rv.x, rv.z = p.x, p.z end
    local v = PhysicsGetVelocity(rvBody)
    if v then
      bodyVy = v.y
      local fxr, fzr = math.sin(rv.yaw), math.cos(rv.yaw)
      local proj = v.x * fxr + v.z * fzr
      if math.abs(proj) < math.abs(rv.speed) then
        local impact = math.abs(rv.speed) - math.abs(proj)
        if impact > 1.5 and impactCd <= 0 then
          impactCd = 0.5
          dur = math.max(0, dur - math.min(8, impact * VEH.impactK))
        end
        rv.speed = proj
      end
    end
  end
  impactCd = impactCd - dt

  -- 超载/车况：极速与加速打折
  local ratio = stats.weight / WEIGHT_MAX
  local maxSp, acc = DRIVE.maxSpeed, DRIVE.accel
  if ratio > 1.0 then
    local over = math.min(1.0, ratio - 1.0)
    maxSp = maxSp * (1.0 - over * 0.45)
    acc = acc * (1.0 - over * 0.5)
  end
  -- 燃油：踩油门燃烧（超载更费油）；没油 = 龟速挪窝
  if throttle > 0 and fuel > 0 then
    fuel = math.max(0, fuel - VEH.burn * (ratio > 1.0 and 1.5 or 1) * dt)
  end
  if fuel <= 0 then
    maxSp = math.min(maxSp, VEH.limpSpeed)
    if rv.speed > VEH.limpSpeed then rv.speed = VEH.limpSpeed end
  end
  maxSp = maxSp * (0.5 + 0.5 * dur / VEH.durMax)
  if fuel <= VEH.tank * 0.2 and not fuelWarned then
    fuelWarned = true
    toast.text = "油量低！加油站加满 / 路边捡燃油桶"
    toast.t = 3.5
  elseif fuel > VEH.tank * 0.5 then
    fuelWarned = false
  end

  if throttle > 0 then
    rv.speed = math.min(maxSp, rv.speed + acc * dt)
  elseif throttle < 0 then
    if rv.speed > 0.05 then
      rv.speed = math.max(0.0, rv.speed - DRIVE.brake * dt)
    else
      rv.speed = math.max(-DRIVE.reverseMax, rv.speed - acc * 0.6 * dt)
    end
  else
    local mag = math.abs(rv.speed) - DRIVE.drag * dt
    if mag < 0 then mag = 0 end
    rv.speed = (rv.speed >= 0) and mag or -mag
  end

  local sf = math.min(1.0, math.abs(rv.speed) / 3.0)
  if sf > 0.01 and steer ~= 0 then
    local dir = (rv.speed < -0.05) and -1 or 1
    rv.yaw = rv.yaw + steer * DRIVE.steerRate * sf * dt * dir
  end

  local fx, fz = math.sin(rv.yaw), math.cos(rv.yaw)
  if rvBody then
    PhysicsSetVelocity(rvBody, { x = fx * rv.speed, y = bodyVy, z = fz * rv.speed })
    local half = rv.yaw * 0.5
    PhysicsSetRotation(rvBody, { x = 0, y = math.sin(half), z = 0, w = math.cos(half) })
  else
    rv.x = rv.x + fx * rv.speed * dt
    rv.z = rv.z + fz * rv.speed * dt
  end

  applyRvTransform()
  if groundEnt ~= nil then
    SetPosition(groundEnt, { x = rv.x, y = -0.05, z = rv.z })
  end
  recycleScenery(fx, fz)
  updateDriveWorld(dt)

  if math.abs(rv.speed) > 2.5 then
    dustAcc = dustAcc + dt * (6.0 + math.abs(rv.speed) * 1.6)
    while dustAcc >= 1.0 do
      dustAcc = dustAcc - 1.0
      local s = (math.random() < 0.5) and -1.3 or 1.3
      local wx, wz = rotOf(s, -2.9)
      EmitParticles({
        pos = { x = wx, y = 0.12, z = wz }, count = 2,
        vel = { x = -fx * 1.2, y = 0.9, z = -fz * 1.2 },
        speedMin = 0.3, speedMax = 1.2, lifeMin = 0.5, lifeMax = 1.1,
        sizeStart = 0.35, sizeEnd = 1.5,
        color = { r = 0.58, g = 0.53, b = 0.44, a = 0.45 },
        colorEnd = { r = 0.58, g = 0.53, b = 0.44, a = 0.0 },
        additive = false, gravity = 0,
      })
    end
  else
    dustAcc = 0
  end

  -- 临近节点（12m 内可停靠）
  nearNode = nil
  local best = 144
  for _, nd in ipairs(NODES) do
    local ddx, ddz = rv.x - nd.x, rv.z - nd.z
    local d2 = ddx * ddx + ddz * ddz
    if d2 < best then best = d2 nearNode = nd end
  end
end
