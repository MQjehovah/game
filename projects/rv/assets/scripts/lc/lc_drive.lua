-- lc_drive.lua
-- 行驶物理：Jolt 速度驱动/燃油/撞击耐久/临近节点

-- Quaternion helpers so the physics body tilts with the terrain like the
-- visual shell (roll -> pitch -> yaw, matching SetRotationEuler).
local function qAxis(x, y, z, ang)
  local s = math.sin(ang * 0.5)
  return { x = x * s, y = y * s, z = z * s, w = math.cos(ang * 0.5) }
end

local function qMul(a, b)
  return {
    x = a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y,
    y = a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x,
    z = a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w,
    w = a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z,
  }
end

function rvQuat()
  local qy = qAxis(0, 1, 0, rv.yaw)
  local qx = qAxis(1, 0, 0, rv.pitch or 0)
  local qz = qAxis(0, 0, 1, rv.roll or 0)
  return qMul(qMul(qy, qx), qz)
end

function updateDrive(dt)
  local throttle = (ActionDown("w") and 1 or 0) - (ActionDown("s") and 1 or 0)
  -- Positive yaw turns toward -X, which is screen-right when facing +Z, so
  -- D must subtract: A = +1 turns left, D = -1 turns right.
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
          -- A real collision: the body lost most of its speed at once. Small
          -- per-frame lag is expected now that traction is impulse-driven, so
          -- only a large drop clamps the commanded speed.
          impactCd = 0.5
          dur = math.max(0, dur - math.min(8, impact * VEH.impactK))
          rv.speed = proj
        end
      end
    end
  end
  impactCd = impactCd - dt

  -- 超载/车况：极速与加速打折
  local ratio = stats.weight / WEIGHT_MAX
  local maxSp, acc = DRIVE.maxSpeed, DRIVE.accel
  -- 零件磨损：引擎影响极速/加速，轮胎影响转向
  local engF = 0.60 + 0.40 * (parts.engine or 100) / 100
                       + (upgrades and upgrades.engine or 0) * 0.04
  local tireF = 0.60 + 0.40 * (parts.tires or 100) / 100
                        + (upgrades and upgrades.tires or 0) * 0.04
  maxSp = maxSp * engF
  acc = acc * (0.70 + 0.30 * (parts.engine or 100) / 100)
  -- 引擎濒临报废：只能龟速，必须先修
  if (parts.engine or 100) < 10 then
    maxSp = math.min(maxSp, 1.6)
    acc = acc * 0.25
    if not engineWarned then
      engineWarned = true
      toast.text = "引擎快报废了！只能龟速——到工作台修引擎"
      toast.t = 4.0
    end
  elseif (parts.engine or 100) > 25 then
    engineWarned = false
  end
  if (parts.tires or 100) < 10 then
    tireF = tireF * 0.35
    if not tireWarned then
      tireWarned = true
      toast.text = "轮胎几乎磨平了——转向会非常迟钝"
      toast.t = 4.0
    end
  elseif (parts.tires or 100) > 25 then
    tireWarned = false
  end
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
  if isRainy and isRainy() then maxSp = maxSp * 0.82 end  -- 雨天/雾天减速
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
    rv.yaw = rv.yaw + steer * DRIVE.steerRate * tireF * sf * dt * dir
  end

  local fx, fz = math.sin(rv.yaw), math.cos(rv.yaw)
  local physicalPath = rvBody ~= nil and updateRvPhysics ~= nil and
                       PhysicsApplyImpulse ~= nil and terrainHeight ~= nil
  if rvBody then
    if not physicalPath then
      PhysicsSetVelocity(rvBody, { x = fx * rv.speed, y = bodyVy, z = fz * rv.speed })
      PhysicsSetRotation(rvBody, rvQuat())
    end
  else
    rv.x = rv.x + fx * rv.speed * dt
    rv.z = rv.z + fz * rv.speed * dt
  end

  -- Follow the terrain. With a live physics body the four contact springs
  -- hold the body up, tire friction drives it and the solver produces
  -- height/pitch/roll; otherwise fall back to the kinematic plane fit.
  if terrainHeight ~= nil then
    local physical = false
    if physicalPath then
      physical = updateRvPhysics(dt)
    end
    if physical then
      -- Keep the scripted heading while preserving the solver's attitude.
      PhysicsSetRotation(rvBody, rvQuat())
    else
      if rvBody ~= nil and physicalPath then
        PhysicsSetVelocity(rvBody, { x = fx * rv.speed, y = bodyVy, z = fz * rv.speed })
      end
      if updateRvSuspension ~= nil then
        updateRvSuspension(dt)
      else
        rv.groundY = terrainFollow(rv.groundY or 0, rv.x, rv.z, dt, 10)
        if updateRvTilt ~= nil then updateRvTilt(dt) end
      end
      if rvBody then
        PhysicsSetPosition(rvBody, { x = rv.x, y = 0.9 + rv.groundY, z = rv.z })
        PhysicsSetRotation(rvBody, rvQuat())
      end
    end
  end

  applyRvTransform()
  if groundEnt ~= nil then
    SetPosition(groundEnt, { x = rv.x, y = -0.05, z = rv.z })
  end
  recycleScenery(fx, fz)
  updateDriveWorld(dt)

  -- 零件磨损（按行驶里程）
  local meters = math.abs(rv.speed) * dt
  if meters > 0 then
    parts.engine = math.max(0, parts.engine - VEH.partWear.engine * meters)
    parts.tires = math.max(0, parts.tires - VEH.partWear.tires * meters)
    parts.battery = math.max(0, parts.battery - VEH.partWear.battery * meters)
  end

  -- 地点在开局一次性生成；行驶中不再新增/改向公路

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
