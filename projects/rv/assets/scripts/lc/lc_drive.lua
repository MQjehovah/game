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
-- 胎压对油耗的影响：2.4bar 最优，偏低明显费油，偏高略费
function tirePressureFuelScale(p)
  local v = p or 2.4
  if v < 2.4 then
    return 1.0 + math.min(0.20, (2.4 - v) * 0.25)
  end
  return 1.0 + math.min(0.06, (v - 2.4) * 0.10)
end

function avgTirePressure()
  local t = (rv and rv.tirePressureCold) or nil
  if t == nil then return 2.4 end
  local s = 0
  for i = 1, 4 do s = s + (t[i] or 2.4) end
  return s / 4
end

-- 机油寿命低于 20% 时动力下降（最低 75%），到加油站可更换
function engineOilPowerScale()
  local o = oilLife or 100
  if o >= 20 then return 1.0 end
  return 0.75 + 0.25 * math.max(0, o) / 20
end

-- 机油严重不足（<15%）时继续行驶会加速发动机磨损（%/s）
function oilEngineWearRate(oilValue)
  local o = oilValue or 100
  if o >= 15 then return 0 end
  return (15 - o) * 0.0015
end

-- 换机油费用：按缺失比例每 25% 收 1 金属（至少 1）
function oilChangeCost()
  local miss = 100 - (oilLife or 100)
  if miss <= 0.5 then return 0 end
  return math.max(1, math.ceil(miss / 25))
end

-- 统一设置机油寿命（联机客机按服务器值同步）
function setOilLife(v)
  if v == nil then return end
  oilLife = math.max(0, math.min(100, tonumber(v) or 0))
end

-- 车况同步串：dur:engine:tires:battery:temp:brake:p1..p4:t1..t4:w1..w4
-- （联机客机显示用；刹车热与四轮数据让保养面板也保持一致）
function vehicleConditionToken()
  local out = {
    tostring(math.floor(dur or 100)),
    tostring(math.floor((parts and parts.engine) or 100)),
    tostring(math.floor((parts and parts.tires) or 100)),
    tostring(math.floor((parts and parts.battery) or 100)),
    tostring(math.floor(engineTemp or 20)),
    tostring(math.floor((brakeHeat or 0) * 100)),
  }
  for i = 1, 4 do
    out[#out + 1] = string.format("%.1f",
      (rv.tirePressureCold or {})[i] or 2.4)
  end
  for i = 1, 4 do
    out[#out + 1] = string.format("%.0f", (rv.tireTemps or {})[i] or 22)
  end
  for i = 1, 4 do
    out[#out + 1] = string.format("%.0f",
      (rv.tireWear or {})[i] or ((parts and parts.tires) or 100))
  end
  return table.concat(out, ":")
end

function parseVehicleCondition(str)
  if type(str) ~= "string" then return nil end
  local out = {}
  for token in string.gmatch(str, "[^:]+") do
    out[#out + 1] = tonumber(token)
  end
  if #out < 5 or out[1] == nil then return nil end
  local v = { dur = out[1], engine = out[2], tires = out[3],
              battery = out[4], temp = out[5] }
  if out[6] ~= nil then
    v.brake = (out[6] or 0) / 100
    v.pressures, v.temps, v.wear = {}, {}, {}
    for i = 1, 4 do
      v.pressures[i] = out[6 + i] or 2.4
      v.temps[i] = out[10 + i] or 22
      v.wear[i] = out[14 + i] or 100
    end
  end
  return v
end

-- 车辆保养面板数据：每一项返回 {label, value, ok}
function maintenanceRows()
  local rows = {}
  local function row(label, value, ok)
    rows[#rows + 1] = { label = label, value = value, ok = ok }
  end
  -- 联机客机用服务器同步来的车况，避免显示本地默认值
  local veh = nil
  if coopRole == "client" and coop ~= nil then veh = coop.veh end
  local d = (veh and veh.dur) or dur or 100
  row("车体", string.format("%.0f%%", d), d >= 40)
  local e = (veh and veh.engine) or (parts and parts.engine) or 100
  row("引擎", string.format("%.0f%%", e), e >= 25)
  local t = (veh and veh.tires) or (parts and parts.tires) or 100
  row("轮胎", string.format("%.0f%%", t), t >= 18)
  local b = (veh and veh.battery) or (parts and parts.battery) or 100
  row("电瓶", string.format("%.0f%%", b), b >= 20)
  local o = oilLife or 100
  row("机油", string.format("%.0f%%", o), o >= 20)
  local c = coolant or 100
  row("冷却液", string.format("%.0f%%", c), c >= 20)
  local temp = (veh and veh.temp) or engineTemp or 20
  row("水温", string.format("%.0f°C", temp), temp < (VEH.tempOverheat or 110))
  row("刹车", string.format("%.0f%% 热", (brakeHeat or 0) * 100),
      (brakeHeat or 0) < 0.7)
  local armor = (upgrades and upgrades.armor) or 0
  row("装甲", string.format("Lv%d", armor), true)
  return rows
end

-- 是否有需要处理的保养项（HUD 警示用）
function maintenanceWarningActive()
  for _, r in ipairs(maintenanceRows()) do
    if not r.ok then return true end
  end
  return false
end

-- 每轮胎压/胎温（保养面板用）
function wheelConditionRows()
  local rows = {}
  for i = 1, 4 do
    rows[i] = {
      pressure = (rv.tirePressureCold or {})[i] or 2.4,
      temp = (rv.tireTemps or {})[i] or 22,
      wear = (rv.tireWear or {})[i] or ((parts and parts.tires) or 100),
    }
  end
  return rows
end

-- 在加油站更换机油；返回 ok, cost, note
function serviceOilAtGasstop()
  local cost = oilChangeCost()
  if cost <= 0 then return true, 0, "机油已满" end
  if (materials.metal or 0) < cost then
    return false, cost, string.format("换机油需要 %d 金属", cost)
  end
  materials.metal = (materials.metal or 0) - cost
  oilLife = 100
  if PlaySfx then PlaySfx("repair") end
  return true, cost, string.format("更换机油（%d 金属）", cost)
end

-- 加油站补充冷却液：低于 90% 时收 1 金属，补满
function coolantServiceCost()
  if (coolant or 100) >= 90 then return 0 end
  return 1
end

function serviceCoolantAtGasstop()
  local cost = coolantServiceCost()
  if cost <= 0 then return true, 0, "冷却液正常" end
  if (materials.metal or 0) < cost then
    return false, cost, "补充冷却液需要 1 金属"
  end
  materials.metal = (materials.metal or 0) - cost
  coolant = 100
  if PlaySfx then PlaySfx("repair") end
  return true, cost, "冷却液已补满（1 金属）"
end

-- 加油站充气：最低胎压低于 2.3bar 时收 1 金属，充回 2.4bar 并清除爆胎
function tireAirCost()
  local t = rv and rv.tirePressureCold
  if t == nil then return 0 end
  local minP = 2.4
  for i = 1, 4 do minP = math.min(minP, t[i] or 2.4) end
  if minP >= 2.3 then return 0 end
  return 1
end

function serviceTiresAtGasstop()
  local cost = tireAirCost()
  if cost <= 0 then return true, 0, "胎压正常" end
  if (materials.metal or 0) < cost then
    return false, cost, "充气需要 1 金属"
  end
  materials.metal = (materials.metal or 0) - cost
  if resetTirePressure ~= nil then resetTirePressure() end
  if clearBlowout ~= nil then clearBlowout() end
  return true, cost, "轮胎充气至 2.4bar（1 金属）"
end

-- 加油站下一次保养的金属花费（保养面板用；无需保养返回 nil）
function maintenanceCostText()
  local parts = {}
  local oilCost = oilChangeCost()
  if oilCost > 0 then
    parts[#parts + 1] = string.format("机油 %d", oilCost)
  end
  local coolantCost = coolantServiceCost()
  if coolantCost > 0 then
    parts[#parts + 1] = string.format("冷却液 %d", coolantCost)
  end
  local airCost = tireAirCost()
  if airCost > 0 then
    parts[#parts + 1] = string.format("充气 %d", airCost)
  end
  if #parts == 0 then return nil end
  return "加油站服务：" .. table.concat(parts, " · ") .. " 金属"
end

-- 机油劣化对油耗的影响：低于 40% 时油耗略升（最高 +8%）
function engineOilFuelScale(o)
  local v = o or 100
  if v >= 40 then return 1.0 end
  return 1.0 + math.min(0.08, (40 - v) * 0.002)
end

-- 冷车动力衰减：水温低于 60°C 时动力逐渐受限（20°C 时 ×0.85）
function engineColdPowerScale(tempValue)
  local t = tempValue or 60
  if t >= 60 then return 1.0 end
  if t <= 20 then return 0.85 end
  return 0.85 + 0.15 * (t - 20) / 40
end

-- 坡度油耗：上坡更费油、下坡更省（grade 为高度差/水平距离）
function slopeFuelScale(grade)
  local g = grade or 0
  if g > 0 then return 1.0 + math.min(0.60, g * 2.0) end
  return math.max(0.65, 1.0 + g * 1.2)
end

-- 坡度车速：上坡最高车速下降、下坡略升；加速也受影响
function slopeSpeedScale(grade)
  local g = grade or 0
  if g > 0 then return 1.0 - math.min(0.30, g * 1.5) end
  return 1.0 + math.min(0.12, -g * 0.8)
end

function slopeAccelScale(grade)
  local g = grade or 0
  if g > 0 then return 1.0 - math.min(0.40, g * 1.6) end
  return 1.0 + math.min(0.15, -g * 1.0)
end

-- 冷车油耗：暖机前燃烧效率差，20°C 时多耗 12%
function engineColdFuelScale(tempValue)
  local t = tempValue or 60
  if t >= 60 then return 1.0 end
  if t <= 20 then return 1.12 end
  return 1.12 - 0.12 * (t - 20) / 40
end

-- 非铺装路面滚动阻力更大：草地/泥土/泥泞更费油
function surfaceFuelScale(name)
  if name == "mud" then return 1.25 end
  if name == "grass" then return 1.15 end
  if name == "dirt" then return 1.10 end
  if name == "gravel" then return 1.05 end
  return 1.0
end

-- 天气对油耗的影响：雨/雪/雾增加行驶阻力与暖风负荷
function weatherFuelScale(weatherValue)
  if weatherValue == "rain" then return 1.05 end
  if weatherValue == "snow" then return 1.12 end
  if weatherValue == "fog" then return 1.03 end
  return 1.0
end

-- 防滑链在干燥铺装路上略拖慢车速（雪链抓地的权衡）
function chainsSpeedScale(surface)
  if hasItem ~= nil and hasItem("chains", 1) and chainsOn ~= false and
     (surface == "asphalt" or surface == "concrete") then
    return 0.95
  end
  return 1.0
end

function fuelBurnRate(speed, loadRatio, throttle)
  local v = math.min(1, math.abs(speed or 0) / DRIVE.maxSpeed)
  local ratio = loadRatio or 1
  local load = 1 + math.max(0, ratio - 1) * 0.60 +
               math.max(0, math.min(ratio, 1) - 0.75) * 0.20
  local idle = VEH.idleBurn or 0.010
  local surfName = "asphalt"
  if surfaceInfo ~= nil then
    surfName = surfaceInfo(rv.x or 0, rv.z or 0)
  end
  local eff = tirePressureFuelScale(avgTirePressure()) *
              engineOilFuelScale(oilLife) *
              surfaceFuelScale(surfName) *
              engineColdFuelScale(engineTemp) *
              weatherFuelScale(weather)
  if (throttle or 0) > 0 then
    return (idle + VEH.burn * (0.30 + 0.70 * v) * load) * eff
  elseif (throttle or 0) < 0 then
    return idle * 0.6 * eff
  end
  return idle * eff
end

-- 行程百公里油耗（L/100km）；里程太短时返回 0，避免怠速把数字拉爆。
function tripL100()
  if (tripDistance or 0) < 20 then return 0 end
  return (tripFuel or 0) / tripDistance * 100
end

-- 剩余续航里程（米）：有实测油耗就按实测，否则用 1.6L/100m 的基准
function rangeEstimateM()
  local per100m = tripL100()
  if per100m < 0.2 then per100m = 1.6 end
  return (fuel or 0) / per100m * 100
end

-- 载重越大，轮胎磨损越快（超载额外惩罚，最高 ×2.2）
function tireWearScale(loadRatio)
  local over = math.max(0, (loadRatio or 1) - 1.0)
  return 1.0 + math.min(1.2, over * 0.8)
end

-- 急转弯/打滑会额外磨胎（%/s）
function slipTireWear(latG, speed)
  if (latG or 0) < 0.6 or math.abs(speed or 0) < 3 then return 0 end
  return math.min(0.6, ((latG or 0) - 0.6) * 0.5)
end

-- 路面越粗糙，轮胎磨损越快
function surfaceTireWearScale(name)
  if name == "mud" then return 1.35 end
  if name == "gravel" then return 1.25 end
  if name == "dirt" then return 1.15 end
  if name == "grass" then return 1.10 end
  return 1.0
end

-- 刹车减速度：载重越重越刹不住（超载额外衰减）
-- 刹车热衰减：连续重刹后制动力最低降到 70%
function brakeFadeScale(heat)
  local h = math.max(0, math.min(1, heat or 0))
  return 1.0 - 0.30 * h
end

-- 雨雪天刹车效率：湿滑路面制动力略降
function weatherBrakeScale(weatherValue)
  if weatherValue == "rain" then return 0.92 end
  if weatherValue == "snow" then return 0.95 end
  return 1.0
end

function brakeRate(loadRatio, heat, weatherValue)
  local over = math.max(0, (loadRatio or 1) - 1.0)
  return DRIVE.brake / (1.0 + over * 0.60) * brakeFadeScale(heat) *
         weatherBrakeScale(weatherValue)
end

-- 手刹：比脚刹更强的减速度（载重同样衰减）
function handbrakeRate(loadRatio, heat, weatherValue)
  return brakeRate(loadRatio, heat, weatherValue) * 1.55
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

-- 雨后积水飞溅：雨天、车速够快且压过积水时触发
function puddleSplashActive(weatherValue, speed, dist)
  if weatherValue ~= "rain" then return false end
  if math.abs(speed or 0) < 3.0 then return false end
  return (dist or 1e9) < 3.2
end

function updatePuddleSplash(dt)
  if phase ~= "driving" then rv.splashAcc = 0 return end
  local near = false
  for _, p in ipairs(puddles or {}) do
    local dx, dz = p.x - rv.x, p.z - rv.z
    if dx * dx + dz * dz < 3.2 * 3.2 then near = true break end
  end
  if not near or not puddleSplashActive(weather, rv.speed, 0) then
    rv.splashAcc = 0
    return
  end
  local k = math.min(1, math.abs(rv.speed or 0) / 9)
  rv.splashAcc = (rv.splashAcc or 0) + (dt or 0) * (10 + 14 * k)
  while rv.splashAcc >= 1 do
    rv.splashAcc = rv.splashAcc - 1
    local side = (math.random() < 0.5) and -1.26 or 1.26
    local wx, wz = rotOf(side, -2.92)
    EmitParticles({
      pos = { x = wx, y = (rv.groundY or 0) + 0.18, z = wz },
      count = 2, speedMin = 0.5, speedMax = 1.6,
      vel = { x = 0, y = 1.1, z = 0 },
      lifeMin = 0.35, lifeMax = 0.75,
      sizeStart = 0.16, sizeEnd = 0.65,
      color = { r = 0.72, g = 0.82, b = 0.92, a = 0.55 },
      colorEnd = { r = 0.72, g = 0.82, b = 0.92, a = 0.0 },
      additive = false, gravity = -0.9,
    })
  end
end

-- 雪天行驶时车轮卷起雪雾（覆盖原本的路面尘土）
function snowSprayKind(weatherValue, speed)
  if weatherValue == "snow" and math.abs(speed or 0) > 2.5 then
    return "snow"
  end
  return nil
end

function updateWheelDust(dt)
  if phase ~= "driving" then rv.dustAcc = 0 return end
  local surfName = "grass"
  if surfaceInfo ~= nil then surfName = surfaceInfo(rv.x, rv.z) end
  local hb = ActionDown("shift") and math.abs(rv.speed or 0) > 2.5
  local rate, kind = wheelDustRate(surfName, rv.speed, hb)
  local spray = snowSprayKind(weather, rv.speed)
  if spray == "snow" then
    local k = math.min(1, math.abs(rv.speed or 0) / 9)
    rate, kind = math.floor(16 * k + 0.5), "snow"
  end
  if rate <= 0 then rv.dustAcc = 0 return end
  rv.dustAcc = (rv.dustAcc or 0) + dt * rate
  local col = {
    dirt    = { 0.55, 0.44, 0.30 },
    mud     = { 0.32, 0.25, 0.18 },
    gravel  = { 0.58, 0.56, 0.50 },
    grass   = { 0.45, 0.48, 0.30 },
    smoke   = { 0.55, 0.55, 0.56 },
    snow    = { 0.94, 0.95, 0.99 },
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

-- 启动引擎：打火声 + 计数（自检用）
function playEngineStart()
  rv.engineStarts = (rv.engineStarts or 0) + 1
  if PlaySfx then PlaySfx("engine_start") end
end

-- 自动换挡时的轻微“咔哒”声（只在行驶中换挡时触发）
function updateGearShiftFx()
  local g = rvGearInfo(rv.speed or 0)
  local shifted = rv.lastGear ~= nil and g ~= rv.lastGear and
                  math.abs(rv.speed or 0) > 0.4
  if shifted then
    if PlaySfx then PlaySfx("click") end
    rv.gearShifts = (rv.gearShifts or 0) + 1
  end
  rv.lastGear = g
  return shifted
end

-- 倒车提示：挂倒挡后每 0.8s 响一声
function updateReverseBeep(dt)
  if (rv.speed or 0) > -0.5 then
    reverseBeepT = 0
    return false
  end
  reverseBeepT = (reverseBeepT or 0) - (dt or 0)
  if reverseBeepT > 0 then return false end
  reverseBeepT = 0.8
  reverseBeeps = (reverseBeeps or 0) + 1
  if PlaySfx then PlaySfx("beep") end
  return true
end

-- 引擎水温：高速/重载升温，低速/怠速散热；过热时限制最高车速并缓慢磨损引擎
-- 冷却液不足提示：低于 20% 时提醒去加油站补充
function coolantLowText(value)
  local c = value or 100
  if c >= 20 then return nil end
  return string.format("冷却液不足（%.0f%%）——尽快到加油站补充，否则容易过热", c)
end

-- 冷却液越低，升温越快（最低时热度 ×1.5）
function coolantHeatScale(v)
  local c = v or 100
  if c >= 30 then return 1.0 end
  return 1.0 + math.min(0.5, (30 - c) / 30 * 0.5)
end

-- 环境温度对引擎散热的影响：雪天最凉、雨天次之
function weatherEngineHeatScale(weatherValue)
  if weatherValue == "snow" then return 0.85 end
  if weatherValue == "rain" then return 0.93 end
  if weatherValue == "fog" then return 0.97 end
  return 1.0
end

function engineHeatRate(speed, loadRatio)
  local v = math.min(1, math.abs(speed or 0) / DRIVE.maxSpeed)
  local load = 1 + math.max(0, (loadRatio or 1) - 1.0) * 0.8
  local heat = (VEH.tempHeat or 5.5) * v * v * load * coolantHeatScale(coolant) *
               weatherEngineHeatScale(weather)
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

-- 爆胎风险：轮胎磨损/胎压过低时逐渐升高（/s）
function blowoutRisk(tires, pressure)
  local t = tires or 100
  local p = pressure or 2.4
  local risk = 0
  if t < (VEH.blowoutTire or 18) then
    risk = risk + ((VEH.blowoutTire or 18) - t) * 0.004
  end
  if p < (VEH.blowoutPressure or 1.8) then
    risk = risk + ((VEH.blowoutPressure or 1.8) - p) * 0.08
  end
  return math.min(0.05, risk)
end

-- 车速对爆胎风险的影响：>5 m/s 起风险上升，最高 ×2.2
function speedBlowoutScale(speed)
  local v = math.abs(speed or 0)
  if v <= 5 then return 1.0 end
  return math.min(2.2, 1.0 + (v - 5) * 0.4)
end

-- 胎温对爆胎风险的影响：超过 100°C 后风险上升，最高 ×3
function tireTempBlowoutScale(temp)
  local t = temp or 22
  if t <= 100 then return 1.0 end
  return math.min(3.0, 1.0 + (t - 100) * 0.08)
end

function clearBlowout()
  rv.blowout = false
  rv.blowoutWarned = false
end

-- 真正触发爆胎（音效/提示/尘屑），便于测试与其它系统复用
function triggerBlowout()
  if rv.blowout then return false end
  rv.blowout = true
  rv.blowoutBangs = (rv.blowoutBangs or 0) + 1
  if not rv.blowoutWarned then
    rv.blowoutWarned = true
    toast.text = "爆胎了！靠边停车，到工作台修轮胎（修理包或材料）"
    toast.t = 5.0
  end
  if PlaySfx then PlaySfx("bang") end
  EmitParticles({
    pos = { x = rv.x, y = (rv.groundY or 0) + 0.4, z = rv.z },
    count = 18, speedMin = 0.8, speedMax = 2.6,
    vel = { x = 0, y = 0.8, z = 0 },
    lifeMin = 0.4, lifeMax = 0.9,
    sizeStart = 0.18, sizeEnd = 0.9,
    color = { r = 0.30, g = 0.30, b = 0.31, a = 0.55 },
    colorEnd = { r = 0.30, g = 0.30, b = 0.31, a = 0.0 },
    additive = false, gravity = 0.3,
  })
  return true
end

-- 车体装甲：每级减少 15% 碰撞耐久损失（满级 -45%）
function armorDamageScale(level)
  local lv = math.max(0, math.min(3, level or (upgrades and upgrades.armor) or 0))
  return 1.0 - 0.15 * lv
end

-- 碰撞对零部件的额外损伤：轻微剐蹭只掉车体，重撞才伤引擎/轮胎
function impactPartDamage(drop)
  local d = drop or 0
  if d <= 4 then return 0, 0 end
  return math.min(3, (d - 4) * 0.8), math.min(4, (d - 4) * 1.0)
end

function updateBlowout(dt)
  if rv.blowout then return end
  local pMin = 2.4
  for i = 1, 4 do
    pMin = math.min(pMin, (rv.tirePressureCold or {})[i] or 2.4)
  end
  local hottest = 22
  for i = 1, 4 do
    hottest = math.max(hottest, (rv.tireTemps or {})[i] or 22)
  end
  local risk = blowoutRisk(parts and parts.tires or 100, pMin) *
               speedBlowoutScale(rv.speed) *
               tireTempBlowoutScale(hottest)
  if risk > 0 and math.random() < risk * (dt or 0) then
    triggerBlowout()
  end
end

-- 房车喇叭：短促双音，吓跑附近感染者（不造成伤害）
-- 步行时能否鸣笛：需要人在房车旁且不在建造模式
function canHonkFromFoot(onFootValue, nearRvValue, buildModeValue)
  return onFootValue == true and nearRvValue == true and not buildModeValue
end

function honk()
  if (rv.honkCd or 0) > 0 then return false end
  rv.honkCd = 0.8
  rv.honks = (rv.honks or 0) + 1
  if PlaySfx then PlaySfx("horn") end
  SpawnFloatText(rv.x, 2.9, rv.z, "叭——", false, 0.9, 1.0, 0.90, 0.5)
  local scared = 0
  if horde ~= nil then
    for i = #horde, 1, -1 do
      local z = horde[i]
      local dx, dz = z.x - rv.x, z.z - rv.z
      if dx * dx + dz * dz < 26 * 26 then
        EmitParticles({ pos = { x = z.x, y = 1.0, z = z.z }, count = 8,
                        speedMin = 1.0, speedMax = 2.5, lifeMin = 0.25, lifeMax = 0.5,
                        sizeStart = 0.16, sizeEnd = 0.05,
                        color = { r = 0.5, g = 0.5, b = 0.45, a = 0.8 },
                        colorEnd = { r = 0.5, g = 0.5, b = 0.45, a = 0.0 },
                        additive = false })
        Despawn(z.ent)
        table.remove(horde, i)
        scared = scared + 1
      end
    end
  end
  rv.honkScared = scared
  -- 鸣笛也会惊走附近的野生动物（鹿）
  local scaredWild = 0
  if driveDeer ~= nil then
    for i = #driveDeer, 1, -1 do
      local d = driveDeer[i]
      local dx, dz = d.x - rv.x, d.z - rv.z
      if dx * dx + dz * dz < 24 * 24 then
        EmitParticles({ pos = { x = d.x, y = 1.0, z = d.z }, count = 8,
                        speedMin = 1.0, speedMax = 2.5, lifeMin = 0.25, lifeMax = 0.5,
                        sizeStart = 0.14, sizeEnd = 0.05,
                        color = { r = 0.45, g = 0.40, b = 0.30, a = 0.75 },
                        colorEnd = { r = 0.45, g = 0.40, b = 0.30, a = 0.0 },
                        additive = false })
        Despawn(d.ent)
        table.remove(driveDeer, i)
        scaredWild = scaredWild + 1
      end
    end
  end
  rv.honkScaredWildlife = scaredWild
  -- 鸣笛也会惊走附近的鸟和野兔
  local scaredSmall = 0
  if scareSmallGameNear ~= nil then
    scaredSmall = scareSmallGameNear(rv.x, rv.z, 24)
  end
  if scareAmbientDeer ~= nil then
    scaredSmall = scaredSmall + scareAmbientDeer(rv.x, rv.z, 24)
  end
  if scarePondDucks ~= nil then
    scaredSmall = scaredSmall + scarePondDucks(rv.x, rv.z, 24)
  end
  rv.honkScaredSmall = scaredSmall
  if scared > 0 or scaredWild > 0 or scaredSmall > 0 then
    local parts = {}
    if scared > 0 then parts[#parts + 1] = scared .. " 只感染者" end
    if scaredWild > 0 then parts[#parts + 1] = scaredWild .. " 只野生动物" end
    if scaredSmall > 0 then parts[#parts + 1] = scaredSmall .. " 只小动物" end
    toast.text = "喇叭吓跑了 " .. table.concat(parts, " / ")
    toast.t = 2.4
  end
  return true
end

-- 油箱容量：基础 60L + 每级 10L
function fuelMax()
  return VEH.tank + (upgrades.fuel or 0) * (VEH.fuelPerLevel or 10)
end

-- 胎压偏低判定（预测爆胎风险前的预警阈值）
function tirePressureLow(p)
  return (p or 2.4) < 1.95
end

-- 轮胎尖叫：横向 G 足够高且车速不低时触发
function tireSquealActive(latG, speed)
  return (latG or 0) > 0.55 and math.abs(speed or 0) > 4.0
end

-- 左右胎压不均导致轻微跑偏：低压侧滚动阻力更大，车会向低压侧偏
function tirePullSteer(pressures)
  if pressures == nil then return 0 end
  local fl, fr = pressures[1] or 2.4, pressures[2] or 2.4
  local rl, rr = pressures[3] or 2.4, pressures[4] or 2.4
  local left = (fl + rl) * 0.5
  local right = (fr + rr) * 0.5
  local delta = left - right
  return math.max(-0.06, math.min(0.06, -delta * 0.03))
end

-- 雨雪天侧风：车速够快时才有明显阵风
function windActive(weatherValue, speed)
  if weatherValue ~= "rain" and weatherValue ~= "snow" then return false end
  return math.abs(speed or 0) >= 3.0
end

-- 侧风扰动（叠加到转向输入，幅度受限）：雪天比雨天更强
function windSteerAmount(weatherValue, speed, t)
  if not windActive(weatherValue, speed) then return 0 end
  local base = (weatherValue == "snow") and 0.045 or 0.030
  local gust = math.sin((t or 0) * 0.7) * 0.6 + math.sin((t or 0) * 0.23) * 0.4
  return base * gust
end

-- 低油量警告音间隔（秒）：油量 0.5%–5% 时每 8 秒响一次
function lowFuelBeepInterval(fuelRatio)
  local f = fuelRatio or 1
  if f > 0.005 and f <= 0.05 then return 8.0 end
  return 0
end

-- 极低油量（<2%）时供油不稳，动力下降
function fuelStarvationScale(fuelRatio)
  local f = fuelRatio or 1
  if f <= 0 then return 0 end
  if f >= 0.02 then return 1.0 end
  return 0.6 + 0.4 * (f / 0.02)
end

-- 没油提示：油量归零且尚未提示过时给出文案
function fuelEmptyWarn(fuelValue, warned)
  if (fuelValue or 0) > 0 then return nil end
  if warned then return nil end
  return "没油了！只能龟速滑行——用燃油罐（E）或叫拖车（Y）"
end

-- 定速巡航油门：低于目标给油，接近/超过目标松油
function cruiseThrottle(speed, target)
  if target == nil or target <= 0 then return 0 end
  if (speed or 0) < target - 0.25 then return 1 end
  return 0
end

function updateDrive(dt)
  local throttle = (ActionDown("w") and 1 or 0) - (ActionDown("s") and 1 or 0)
  -- Positive yaw turns toward -X, which is screen-right when facing +Z, so
  -- D must subtract: A = +1 turns left, D = -1 turns right.
  local steer = (ActionDown("a") and 1 or 0) - (ActionDown("d") and 1 or 0)
  -- 定速巡航：手动油门/刹车/手刹会自动退出
  if cruiseOn then
    if ActionDown("w") or ActionDown("s") or ActionDown("shift") then
      cruiseOn, cruiseSpeed = false, 0
    else
      throttle = cruiseThrottle(rv.speed or 0, cruiseSpeed)
    end
  end
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

  -- 夜间疲劳：长时间夜驾后方向会轻微游移（自动巡航/测试钩子不受影响）
  if not autoPilot and not rv.steerDemo and nightSteerWander ~= nil then
    rv.wanderT = (rv.wanderT or 0) + (dt or 0)
    steer = steer + nightSteerWander(nightDriveMin or 0, rv.wanderT)
    -- 雨雪天侧风阵风：需要轻微反打方向
    rv.windT = (rv.windT or 0) + (dt or 0)
    steer = steer + windSteerAmount(weather, rv.speed, rv.windT)
    -- 左右胎压不均：向低压侧轻微跑偏
    steer = steer + tirePullSteer(rv.tirePressureCold)
    steer = math.max(-1, math.min(1, steer))
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
          dur = math.max(0, dur - math.min(8, drop * VEH.impactK) *
                              armorDamageScale())
          -- 重撞还会伤引擎/轮胎
          local engDmg, tireDmg = impactPartDamage(drop)
          parts.engine = math.max(0, (parts.engine or 100) - engDmg)
          parts.tires = math.max(0, (parts.tires or 100) - tireDmg)
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
  -- 机油寿命低时动力下降
  local oilF = engineOilPowerScale()
  maxSp = maxSp * oilF
  acc = acc * oilF
  -- 防滑链在干燥铺装路上略拖慢
  local surfName = "asphalt"
  if surfaceInfo ~= nil then
    surfName = surfaceInfo(rv.x, rv.z)
  end
  local chainF = chainsSpeedScale(surfName)
  maxSp = maxSp * chainF
  acc = acc * chainF
  -- 坡度：上坡降速降加速，下坡略升
  local grade = 0
  if terrainHeight ~= nil then
    local gfx, gfz = math.sin(rv.yaw), math.cos(rv.yaw)
    local hf = terrainHeight(rv.x + gfx * 2.0, rv.z + gfz * 2.0) or 0
    local hb = terrainHeight(rv.x - gfx * 2.0, rv.z - gfz * 2.0) or 0
    grade = (hf - hb) / 4.0
  end
  maxSp = maxSp * slopeSpeedScale(grade)
  acc = acc * slopeAccelScale(grade)
  -- 冷车动力受限：水温上来前别急着飙
  local coldF = engineColdPowerScale(engineTemp)
  maxSp = maxSp * coldF
  acc = acc * (0.80 + 0.20 * coldF)
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
    rate = rate * slopeFuelScale(grade)
    fuel = math.max(0, fuel - rate * dt)
    tripFuel = (tripFuel or 0) + (before - fuel)
  end
  if fuel <= 0 then
    maxSp = math.min(maxSp, VEH.limpSpeed)
    if rv.speed > VEH.limpSpeed then rv.speed = VEH.limpSpeed end
    local warnText = fuelEmptyWarn(fuel, rv.fuelEmptyWarned)
    if warnText ~= nil then
      rv.fuelEmptyWarned = true
      toast.text = warnText
      toast.t = 5.0
      if PlaySfx then PlaySfx("beep") end
    end
  elseif fuel > fuelMax() * 0.1 then
    rv.fuelEmptyWarned = false
  end
  -- 极低油量：供油不稳，动力下降（油表最后 2%）
  local starve = fuelStarvationScale((fuel or 0) / fuelMax())
  if starve < 1.0 then
    maxSp = maxSp * starve
    acc = acc * (0.7 + 0.3 * starve)
  end
  maxSp = maxSp * (0.5 + 0.5 * dur / VEH.durMax)
  if isRainy and isRainy() then maxSp = maxSp * 0.82 end  -- 雨天/雾天减速
  -- 引擎水温：过热时限制最高车速，并缓慢磨损引擎
  updateEngineTemp(dt, ratio)
  if rv.engineHot then
    maxSp = maxSp * 0.65
    parts.engine = math.max(0, (parts.engine or 100) - 0.6 * dt)
  end
  -- 爆胎：轮胎磨平/胎压过低时随机发生，恢复前只能低速挪
  updateBlowout(dt)
  if rv.blowout then
    maxSp = math.min(maxSp, 2.2)
    acc = acc * 0.45
  end
  -- 路边抛锚车：单机、非雪天白天偶发；开远后自动清理
  if coopRole == nil and strandedCar ~= nil then
    local sdx, sdz = strandedCar.x - rv.x, strandedCar.z - rv.z
    if sdx * sdx + sdz * sdz > 90 * 90 then
      if despawnStrandedCar ~= nil then despawnStrandedCar() end
    end
  end
  if coopRole == nil and strandedCar == nil and weather ~= "snow" and
     not (isNight ~= nil and isNight()) then
    rv.strandedT = (rv.strandedT or 0) + dt
    if rv.strandedT > 150 then
      rv.strandedT = 0
      if math.random() < 0.35 and spawnStrandedCar ~= nil then
        spawnStrandedCar()
      end
    end
  end
  -- 路边搭车客：白天偶发；开远后清理
  if coopRole == nil and hitchhiker ~= nil then
    local hdx, hdz = hitchhiker.x - rv.x, hitchhiker.z - rv.z
    if hdx * hdx + hdz * hdz > 90 * 90 then
      if despawnHitchhiker ~= nil then despawnHitchhiker() end
    end
  end
  if coopRole == nil and hitchhiker == nil and weather ~= "snow" and
     not (isNight ~= nil and isNight()) then
    rv.hitchT = (rv.hitchT or 0) + dt
    if rv.hitchT > 200 then
      rv.hitchT = 0
      if math.random() < 0.30 and spawnHitchhiker ~= nil then
        spawnHitchhiker()
      end
    end
  end
  if fuel <= fuelMax() * 0.2 and not fuelWarned then
    fuelWarned = true
    toast.text = "油量低！加油站加满 / 路边捡燃油桶"
    toast.t = 3.5
  elseif fuel > fuelMax() * 0.5 then
    fuelWarned = false
  end
  -- 胎压偏低：提前提示充气，避免直接爆胎
  local minP = 2.4
  for i = 1, 4 do
    minP = math.min(minP, (rv.tirePressureCold or {})[i] or 2.4)
  end
  if tirePressureLow(minP) and not rv.pressureWarned then
    rv.pressureWarned = true
    toast.text = "胎压偏低——到工作台修轮胎充回 2.4bar"
    toast.t = 3.4
  elseif minP > 2.15 then
    rv.pressureWarned = false
  end
  -- 机油偏低：提示到加油站更换
  if (oilLife or 100) < 20 and not rv.oilWarned then
    rv.oilWarned = true
    toast.text = "机油快用完了——动力下降，到加油站换机油"
    toast.t = 3.4
  elseif (oilLife or 100) > 40 then
    rv.oilWarned = false
  end
  -- 机油严重不足还继续跑：发动机额外磨损
  local oilWear = oilEngineWearRate(oilLife)
  if oilWear > 0 and math.abs(rv.speed or 0) > 1.0 then
    parts.engine = math.max(0, (parts.engine or 100) - oilWear * dt)
    if not rv.oilWearWarned then
      rv.oilWearWarned = true
      toast.text = "机油见底还在跑——发动机正在加速磨损！"
      toast.t = 3.4
    end
  elseif (oilLife or 100) > 20 then
    rv.oilWearWarned = false
  end

  -- 冷却液不足：提醒一次，冷却后可再次提醒
  local coolWarn = coolantLowText(coolant)
  if coolWarn ~= nil and not rv.coolantWarned then
    rv.coolantWarned = true
    toast.text = coolWarn
    toast.t = 3.6
  elseif (coolant or 100) >= 35 then
    rv.coolantWarned = false
  end

  -- 刹车热衰减：连续重刹累积热量，松开后缓慢冷却
  local brakingHard = (ActionDown("shift") or ActionDown("s")) and
                      math.abs(rv.speed or 0) > 2.5
  if brakingHard then
    brakeHeat = math.min(1, (brakeHeat or 0) + dt * 0.12)
  else
    brakeHeat = math.max(0, (brakeHeat or 0) - dt * 0.05)
  end
  if (brakeHeat or 0) > 0.7 and not rv.brakeWarned then
    rv.brakeWarned = true
    toast.text = "刹车过热——松一松刹车，用低挡/滑行降温"
    toast.t = 3.4
  elseif (brakeHeat or 0) < 0.4 then
    rv.brakeWarned = false
  end

  -- 手刹（驾驶时按住 Shift）：强减速 + 刹车灯亮
  local handbrake = ActionDown("shift")
  if handbrake and rv.speed > 0.05 then
    rv.braking = true
    rv.speed = math.max(0.0, rv.speed - handbrakeRate(ratio, brakeHeat, weather) * dt)
  elseif throttle > 0 then
    rv.braking = false
    rv.speed = math.min(maxSp, rv.speed + acc * dt)
  elseif throttle < 0 then
    rv.braking = true
    if rv.speed > 0.05 then
      rv.speed = math.max(0.0, rv.speed - brakeRate(ratio, brakeHeat, weather) * dt)
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

  -- 横向 G / 轮胎尖叫：用每帧航向变化估算，供甩货与音效共用
  local prevSquealYaw = rv.squealYaw
  rv.squealYaw = rv.yaw
  if prevSquealYaw ~= nil and (dt or 0) > 0 then
    local dyaw = rv.yaw - prevSquealYaw
    while dyaw > math.pi do dyaw = dyaw - 2 * math.pi end
    while dyaw < -math.pi do dyaw = dyaw + 2 * math.pi end
    rv.latG = math.abs(dyaw / dt) * math.abs(rv.speed or 0) / 9.81
    rv.latGSet = true
    -- 急转弯/打滑额外磨胎
    local slipWear = slipTireWear(rv.latG, rv.speed)
    if slipWear > 0 then
      parts.tires = math.max(0, (parts.tires or 100) - slipWear * dt)
    end
    rv.squealCd = math.max(0, (rv.squealCd or 0) - dt)
    if rv.squealCd <= 0 and tireSquealActive(rv.latG, rv.speed) then
      rv.squealCd = 0.7
      rv.squealPlays = (rv.squealPlays or 0) + 1
      if PlaySfx then PlaySfx("squeal") end
    end
  end

  -- 低油量警告音：油量 <5% 时周期性响一声
  local beepIv = lowFuelBeepInterval((fuel or 0) / fuelMax())
  if beepIv > 0 then
    rv.fuelBeepT = (rv.fuelBeepT or beepIv) - (dt or 0)
    if rv.fuelBeepT <= 0 then
      rv.fuelBeepT = beepIv
      rv.fuelBeepPlays = (rv.fuelBeepPlays or 0) + 1
      if PlaySfx then PlaySfx("beep") end
    end
  else
    rv.fuelBeepT = 0
  end

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
    if checkMilestones ~= nil then checkMilestones("distance", tripDistance) end
    if rv.tirePressureCold ~= nil then
      for i = 1, 4 do
        rv.tirePressureCold[i] = pressureAfterMeters(rv.tirePressureCold[i], meters)
      end
    end
    parts.engine = math.max(0, parts.engine - VEH.partWear.engine * meters)
    parts.tires = math.max(0, parts.tires -
      VEH.partWear.tires * meters * tireWearScale(ratio) *
      surfaceTireWearScale(surfName))
    oilLife = math.max(0, (oilLife or 100) - meters * 0.01)
    -- 冷却液缓慢消耗；过热时额外泄漏
    coolant = math.max(0, (coolant or 100) - meters * 0.002 -
                          (rv.engineHot and dt * 0.4 or 0))
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
