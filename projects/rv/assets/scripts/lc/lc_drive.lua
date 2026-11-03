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

-- 燃油消耗速率（L/s）：怠速基线 + 车速 + 载重。导出让自检能验证曲线。
function fuelBurnRate(speed, loadRatio, throttle)
  local v = math.min(1, math.abs(speed or 0) / DRIVE.maxSpeed)
  local ratio = loadRatio or 1
  local load = 1 + math.max(0, ratio - 1) * 0.60 +
               math.max(0, math.min(ratio, 1) - 0.75) * 0.20
  local idle = VEH.idleBurn or 0.010
  if (throttle or 0) > 0 then
    return idle + VEH.burn * (0.30 + 0.70 * v) * load
  elseif (throttle or 0) < 0 then
    return idle * 0.6
  end
  return idle
end

-- 行程百公里油耗（L/100km）；里程太短时返回 0，避免怠速把数字拉爆。
function tripL100()
  if (tripDistance or 0) < 20 then return 0 end
  return (tripFuel or 0) / tripDistance * 100
end

-- 刹车减速度：载重越重越刹不住（超载额外衰减）
function brakeRate(loadRatio)
  local over = math.max(0, (loadRatio or 1) - 1.0)
  return DRIVE.brake / (1.0 + over * 0.60)
end

-- 手刹：比脚刹更强的减速度（载重同样衰减）
function handbrakeRate(loadRatio)
  return brakeRate(loadRatio) * 1.55
end

-- 车轮扬尘/胎烟发射率（/s）与颜色种类：硬路面不扬尘，草地/土路/泥地明显，
-- 手刹高速时改成灰色胎烟。纯函数，便于自检。
function wheelDustRate(surface, speed, handbrake)
  local v = math.abs(speed or 0)
  if handbrake and v > 2.5 then return 26, "smoke" end
  if v < 2.5 then return 0, surface end
  local k = math.min(1, v / 9)
  if surface == "dirt" then return math.floor(18 * k + 0.5), "dirt" end
  if surface == "mud" then return math.floor(22 * k + 0.5), "mud" end
  if surface == "gravel" then return math.floor(14 * k + 0.5), "gravel" end
  if surface == "grass" then return math.floor(10 * k + 0.5), "grass" end
  return 0, surface
end

function updateWheelDust(dt)
  if phase ~= "driving" then rv.dustAcc = 0 return end
  local surfName = "grass"
  if surfaceInfo ~= nil then surfName = surfaceInfo(rv.x, rv.z) end
  local hb = ActionDown("shift") and math.abs(rv.speed or 0) > 2.5
  local rate, kind = wheelDustRate(surfName, rv.speed, hb)
  if rate <= 0 then rv.dustAcc = 0 return end
  rv.dustAcc = (rv.dustAcc or 0) + dt * rate
  local col = {
    dirt    = { 0.55, 0.44, 0.30 },
    mud     = { 0.32, 0.25, 0.18 },
    gravel  = { 0.58, 0.56, 0.50 },
    grass   = { 0.45, 0.48, 0.30 },
    smoke   = { 0.55, 0.55, 0.56 },
  }
  local c = col[kind] or col.grass
  while rv.dustAcc >= 1 do
    rv.dustAcc = rv.dustAcc - 1
    local side = (math.random() < 0.5) and -1.26 or 1.26
    local wx, wz = rotOf(side, -2.92)
    EmitParticles({
      pos = { x = wx, y = (rv.groundY or 0) + 0.16, z = wz },
      count = 1, speedMin = 0.15, speedMax = 0.55,
      vel = { x = 0, y = 0.55, z = 0 },
      lifeMin = 0.45, lifeMax = 0.95,
      sizeStart = 0.28, sizeEnd = 1.35,
      color = { r = c[1], g = c[2], b = c[3], a = (kind == "smoke") and 0.42 or 0.30 },
      colorEnd = { r = c[1], g = c[2], b = c[3], a = 0.0 },
      additive = false, gravity = -0.15,
    })
  end
end

-- 引擎音量随车速变化（总线增益，叠加在 PlayMusic 的基础音量上）
function engineAudioGain(speed)
  local v = math.min(1, math.abs(speed or 0) / DRIVE.maxSpeed)
  return 0.60 + 0.55 * v
end

-- 简易自动挡：按车速升挡，挡内把车速映射到转速区间（HUD 用）。
function rvGearInfo(speed)
  local v = math.abs(speed or 0)
  local up = VEH.gearUp or {}
  local g = 1
  for i = 1, #up do
    if v >= up[i] then g = i + 1 end
  end
  local lo = (g == 1) and 0 or up[g - 1]
  local hi = up[g] or DRIVE.maxSpeed
  local t = 0
  if hi > lo then t = math.max(0, math.min(1, (v - lo) / (hi - lo))) end
  local rpm = (VEH.gearRpmMin or 850) +
              ((VEH.gearRpmMax or 2600) - (VEH.gearRpmMin or 850)) * t
  return g, rpm
end

-- 引擎水温：高速/重载升温，低速/怠速散热；过热时限制最高车速并缓慢磨损引擎
function engineHeatRate(speed, loadRatio)
  local v = math.min(1, math.abs(speed or 0) / DRIVE.maxSpeed)
  local load = 1 + math.max(0, (loadRatio or 1) - 1.0) * 0.8
  local heat = (VEH.tempHeat or 5.5) * v * v * load
  local cool = (VEH.tempCool or 1.0) + 0.30 * v
  return heat - cool
end

function stepEngineTemp(temp, speed, loadRatio, dt)
  local t = (temp or 20) + engineHeatRate(speed, loadRatio) * (dt or 0)
  return math.max(20, math.min(VEH.tempMax or 125, t))
end

function updateEngineTemp(dt, ratio)
  engineTemp = stepEngineTemp(engineTemp or 20, rv.speed or 0, ratio, dt)
  rv.engineHot = engineTemp >= (VEH.tempOverheat or 110)
  if rv.engineHot and not rv.heatWarned then
    rv.heatWarned = true
    toast.text = "引擎过热！松油门滑行一会儿，或停车怠速散热"
    toast.t = 4.0
  elseif not rv.engineHot then
    rv.heatWarned = false
  end
end

function updateDrive(dt)
  local throttle = (ActionDown("w") and 1 or 0) - (ActionDown("s") and 1 or 0)
  -- Positive yaw turns toward -X, which is screen-right when facing +Z, so
  -- D must subtract: A = +1 turns left, D = -1 turns right.
  local steer = (ActionDown("a") and 1 or 0) - (ActionDown("d") and 1 or 0)
  if rv.steerDemo then
    -- Test hook: drive forward and sweep the wheel left / release / right /
    -- release so the assist + self-centering show up in the steering log.
    rv.steerDemoT = (rv.steerDemoT or 0) + dt
    local t = rv.steerDemoT
    throttle = 1
    if t < 1.2 then steer = 1
    elseif t < 2.4 then steer = 0
    elseif t < 3.4 then steer = -1
    else steer = 0 end
  end
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
      -- Test hook: prefer the main-road gas stop for a clean straight run.
      -- (The nearest node is the off-road ruin, which now that tire forces are
      -- physical makes the RV stall against the embankment foot.)
      autoTarget = nodeById and nodeById("gasstop") or nil
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
      -- Impact = a sudden loss of forward speed between frames. Comparing the
      -- commanded speed with the actual one would misfire while the tires are
      -- still building traction (slip-driven acceleration lags the pedal).
      local prev = rv.lastProj
      if prev ~= nil then
        local drop = math.abs(prev) - math.abs(proj)
        if drop > 1.5 and impactCd <= 0 then
          impactCd = 0.5
          dur = math.max(0, dur - math.min(8, drop * VEH.impactK))
          rv.speed = proj
          if jobApplyImpact ~= nil then jobApplyImpact(drop) end
          if rvImpactFeedback ~= nil then rvImpactFeedback(drop) end
        end
      end
      rv.lastProj = proj
    end
  end
  impactCd = impactCd - dt

  -- 超载/车况：极速与加速打折
  local ratio = stats.weight / weightMax()
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
  -- 燃油：怠速基线 + 车速/载重相关。踩油门越快越费油，载重再叠一层；
  -- 低速巡航比全速省油（每分钟的油量表变化能看出来）。没油 = 龟速挪窝。
  if fuel > 0 then
    local before = fuel
    local rate = fuelBurnRate(rv.speed or 0, ratio, throttle)
    fuel = math.max(0, fuel - rate * dt)
    tripFuel = (tripFuel or 0) + (before - fuel)
  end
  if fuel <= 0 then
    maxSp = math.min(maxSp, VEH.limpSpeed)
    if rv.speed > VEH.limpSpeed then rv.speed = VEH.limpSpeed end
  end
  maxSp = maxSp * (0.5 + 0.5 * dur / VEH.durMax)
  if isRainy and isRainy() then maxSp = maxSp * 0.82 end  -- 雨天/雾天减速
  -- 引擎水温：过热时限制最高车速，并缓慢磨损引擎
  updateEngineTemp(dt, ratio)
  if rv.engineHot then
    maxSp = maxSp * 0.65
    parts.engine = math.max(0, (parts.engine or 100) - 0.6 * dt)
  end
  if fuel <= VEH.tank * 0.2 and not fuelWarned then
    fuelWarned = true
    toast.text = "油量低！加油站加满 / 路边捡燃油桶"
    toast.t = 3.5
  elseif fuel > VEH.tank * 0.5 then
    fuelWarned = false
  end

  -- 手刹（驾驶时按住 Shift）：强减速 + 刹车灯亮
  local handbrake = ActionDown("shift")
  if handbrake and rv.speed > 0.05 then
    rv.braking = true
    rv.speed = math.max(0.0, rv.speed - handbrakeRate(ratio) * dt)
  elseif throttle > 0 then
    rv.braking = false
    rv.speed = math.min(maxSp, rv.speed + acc * dt)
  elseif throttle < 0 then
    rv.braking = true
    if rv.speed > 0.05 then
      rv.speed = math.max(0.0, rv.speed - brakeRate(ratio) * dt)
    else
      rv.speed = math.max(-DRIVE.reverseMax, rv.speed - acc * 0.6 * dt)
    end
  else
    local mag = math.abs(rv.speed) - DRIVE.drag * dt
    if mag < 0 then mag = 0 end
    rv.speed = (rv.speed >= 0) and mag or -mag
  end

  local physicalPath = rvBody ~= nil and updateRvPhysics ~= nil and
                       PhysicsApplyImpulse ~= nil and terrainHeight ~= nil
  if physicalPath then
    -- Steering is a wheel angle; yaw comes from the tire forces. The command
    -- is speed-sensitive and self-centers when the driver lets go.
    if updateSteeringCommand ~= nil then
      rv.steerCmd = updateSteeringCommand(steer, dt)
    else
      rv.steerCmd = steer * 0.50
    end
  else
    rv.steerCmd = nil
    local sf = math.min(1.0, math.abs(rv.speed) / 3.0)
    if sf > 0.01 and steer ~= 0 then
      local dir = (rv.speed < -0.05) and -1 or 1
      rv.yaw = rv.yaw + steer * DRIVE.steerRate * tireF * sf * dt * dir
    end
  end

  local fx, fz = math.sin(rv.yaw), math.cos(rv.yaw)
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
  -- hold the body up, tire friction drives and steers it, and the solver
  -- produces height/pitch/roll/yaw; otherwise fall back to the kinematic fit.
  if terrainHeight ~= nil then
    local physical = false
    if physicalPath then
      physical = updateRvPhysics(dt)
    end
    if not physical then
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

  -- 引擎音量随车速：每 0.2s 更新一次总线增益，避免每帧设置
  rv.audioT = (rv.audioT or 0) + dt
  if rv.audioT >= 0.2 then
    rv.audioT = 0
    if SetBusVolume ~= nil and engineAudioGain ~= nil then
      SetBusVolume(2, engineAudioGain(rv.speed))
    end
  end

  -- 零件磨损（按行驶里程）
  local meters = math.abs(rv.speed) * dt
  if meters > 0 then
    tripDistance = (tripDistance or 0) + meters
    if addDailyProgress ~= nil then addDailyProgress("distance", meters) end
    if rv.tirePressureCold ~= nil then
      for i = 1, 4 do
        rv.tirePressureCold[i] = pressureAfterMeters(rv.tirePressureCold[i], meters)
      end
    end
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
