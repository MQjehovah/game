-- lc_survival.lua
-- 生存循环：时间/昼夜、饥饿口渴体力卫生、物品、做饭咖啡、睡觉、种植、伤病

function initSurvival()
  needs = { health = 100, hunger = 85, thirst = 90, energy = 90, hygiene = 80 }
  inv = { water = 3, canned = 2, rawfood = 2, coffee = 2, seed = 1, repairkit = 0,
          meal = 0, brew = 0 }
  clock = SURVIVAL.startClock
  day = 1
  water = 80
  parts = { engine = 100, tires = 100, battery = 100 }
  upgrades = { engine = 0, tires = 0, cargo = 0, rod = 0, water = 0, fuel = 0 }
  lastWarn = {}
  hurtT = 0
  if dailyGoal == nil and rollDailyGoal ~= nil then rollDailyGoal() end
end

-- 真实昼夜光照：直接改场景 Sun 的方向光强度/颜色
function initDayNight()
  sunEnt = FindNamedEntity("Sun")
  lastSunClock = -999
  lastSkyBucket = nil
end

-- 太阳能出力系数：夜间 0，日出/日落线性过渡，白天 1
function solarFactor()
  local h = (clock or 0) / 60
  if h < 5 or h >= 21 then return 0 end
  if h < 7 then return (h - 5) / 2 end
  if h > 18 then return 1 - (h - 18) / 3 end
  return 1
end

function updateDayNight(dt)
  if sunEnt == nil then return end
  if math.abs(clock - lastSunClock) < 1.0 then return end
  lastSunClock = clock
  local h = clock / 60
  local f = 1.0
  if h < 5 or h >= 21 then
    f = 0.04
  elseif h < 7 then
    f = (h - 5) / 2
  elseif h >= 18 then
    f = 1.0 - (h - 18) / 3
  end
  f = math.max(0.04, math.min(1, f))
  local r, g, b
  if f < 0.28 then
    r, g, b = 0.42, 0.52, 0.78
  elseif f < 0.72 then
    local t = (f - 0.28) / 0.44
    r = 0.42 + (1.0 - 0.42) * t
    g = 0.52 + (0.62 - 0.52) * t
    b = 0.78 + (0.35 - 0.78) * t
  else
    local t = (f - 0.72) / 0.28
    r = 1.0
    g = 0.62 + (0.96 - 0.62) * t
    b = 0.35 + (0.88 - 0.35) * t
  end
  SetEntityComponentField(sunEnt, "light", "intensity", 0.06 + 1.74 * f)
  SetEntityComponentField(sunEnt, "light", "color", { r, g, b, 1 })
  SetEntityComponentField(sunEnt, "light", "ambientStrength", 0.12 + 0.43 * f)

  -- 夜空/雾色也跟随昼夜：只调太阳强度会让 22:00 仍像傍晚，车灯没有存在感。
  -- 量化到每 10 游戏分钟一次，避免 IBL 每帧重建（天空变化会触发环境贴图重算）。
  local skyBucket = math.floor(clock / 10)
  if SetEnvironmentField ~= nil and skyBucket ~= lastSkyBucket then
    lastSkyBucket = skyBucket
    local t = math.max(0, math.min(1, (f - 0.04) / 0.96))
    local warm = math.sin(t * math.pi) * (1.0 - t)
    local function mix(a, b) return a + (b - a) * t end
    SetEnvironmentField("skyTop", {
      r = mix(0.015, 0.28), g = mix(0.025, 0.38), b = mix(0.060, 0.58), a = 1 })
    SetEnvironmentField("skyHorizon", {
      r = mix(0.060, 0.55) + warm * 0.34,
      g = mix(0.090, 0.65) + warm * 0.17,
      b = mix(0.180, 0.80) + warm * 0.05, a = 1 })
    SetEnvironmentField("fogColor", {
      r = mix(0.040, 0.45), g = mix(0.060, 0.55), b = mix(0.110, 0.70), a = 1 })
    SetEnvironmentField("ambientColor", {
      r = mix(0.10, 0.52), g = mix(0.13, 0.55), b = mix(0.22, 0.62), a = 1 })
    SetEnvironmentField("ambientStrength", 0.16 + 0.42 * f)
    if recomputeStats ~= nil then recomputeStats() end  -- 太阳能随昼夜变化
  end
end

function isNight()
  return clock < SURVIVAL.nightEnd or clock >= SURVIVAL.nightStart
end

function timeString()
  local h = math.floor(clock / 60) % 24
  local m = math.floor(clock % 60)
  return string.format("%02d:%02d", h, m)
end

-- 0（白天）..1（深夜）：用于屏幕暗角/蓝色夜幕
function nightAmount()
  local t = clock / 60
  local fade = 1.5
  if t >= 19 and t < 20.5 then return (t - 19) / fade end
  if t >= 20.5 or t < 4.5 then
    if t < 4.5 then return math.min(1, (4.5 - t) / fade) end
    return 1
  end
  if t >= 4.5 and t < 6 then return math.max(0, (6 - t) / fade) end
  return 0
end

function advanceGameMinutes(mins)
  local before = day
  clock = clock + (mins or 0)
  while clock >= 1440 do
    clock = clock - 1440
    day = day + 1
  end
  if day ~= before then
    if rollDailyGoal ~= nil then rollDailyGoal() end
    if grantRelaySupply ~= nil then grantRelaySupply() end
    if LC and LC.flow then LC.flow.emit("day_advanced") end
  end
end

function addItem(id, n)
  if ITEMS[id] == nil then return end
  inv[id] = (inv[id] or 0) + (n or 1)
  if id == "rawfood" or id == "rawfish" then
    rawFoodAge = 0
    rawSpoilWarned = false
  end
end

-- 每日目标：随天数刷新，完成给燃料/材料
function rollDailyGoal()
  local g = DAILY_GOALS[math.random(1, #DAILY_GOALS)]
  if g == nil then return end
  dailyGoal = { id = g.id, name = g.name, target = g.target, progress = 0,
                done = false, reward = g.reward, day = day }
end

function addDailyProgress(kind, amount)
  if dailyGoal == nil or dailyGoal.done or dailyGoal.id ~= kind then return end
  dailyGoal.progress = (dailyGoal.progress or 0) + (amount or 1)
  if dailyGoal.progress >= dailyGoal.target then
    dailyGoal.done = true
    local got = {}
    for k, v in pairs(dailyGoal.reward or {}) do
      if k == "fuel" then
        fuel = math.min(fuelMax(), fuel + v)
        got[#got + 1] = "燃料+" .. v .. "L"
      else
        materials[k] = (materials[k] or 0) + v
        got[#got + 1] = (MAT_NAMES[k] or k) .. "+" .. v
      end
    end
    toast.text = "今日目标完成：" .. dailyGoal.name ..
                 (next(got) ~= nil and (" · " .. table.concat(got, " ")) or "")
    toast.t = 4.5
    if PlaySfx then PlaySfx("pickup") end
  end
end

-- 旅行日志：把跑单/里程碑/纪录等事件记下来，供 J 面板回看
function addJournal(text, kind)
  if text == nil or text == "" then return end
  journal[#journal + 1] = {
    text = text, kind = kind or "note",
    day = day or 1, clock = clock or 0,
  }
  while #journal > 30 do table.remove(journal, 1) end
end

-- 长期里程碑：探索地点数 / 完成货运单数达到阈值时一次性发奖
function checkMilestones(kind, value)
  local list = MILESTONES and MILESTONES[kind]
  if list == nil then return end
  for _, m in ipairs(list) do
    local key = kind .. ":" .. m.n
    if (value or 0) >= m.n and not milestones[key] then
      milestones[key] = true
      local got = {}
      for k, v in pairs(m.reward or {}) do
        if k == "fuel" then
          local before = fuel
          fuel = math.min(fuelMax(), fuel + v)
          got[#got + 1] = string.format("燃料+%dL", math.floor(fuel - before + 0.5))
        else
          materials[k] = (materials[k] or 0) + v
          got[#got + 1] = (MAT_NAMES[k] or k) .. "+" .. v
        end
      end
      local label = (kind == "explore" and "探索")
                 or (kind == "distance" and "里程") or "货运"
      local shown = (kind == "distance") and string.format("%.1fkm", m.n / 1000)
                 or tostring(m.n)
      toast.text = string.format("里程碑：%s %s · %s", label, shown,
                                 table.concat(got, " "))
      toast.t = 5.0
      if addJournal ~= nil then
        addJournal(string.format("里程碑：%s %s · %s", label, shown,
                                 table.concat(got, " ")), "goal")
      end
      if PlaySfx then PlaySfx("pickup") end
    end
  end
end

-- 下一个未领取的里程碑（用于地图长线目标提示）
function nextMilestone(kind, value)
  local list = MILESTONES and MILESTONES[kind]
  if list == nil then return nil end
  for _, m in ipairs(list) do
    if (value or 0) < m.n then return m end
  end
  return nil
end

function milestoneHint()
  local parts = {}
  local e = nextMilestone("explore", visitedCount or 0)
  if e ~= nil then
    parts[#parts + 1] = string.format("探索 %d/%d", visitedCount or 0, e.n)
  end
  local j = nextMilestone("jobs", jobDelivered or 0)
  if j ~= nil then
    parts[#parts + 1] = string.format("货运 %d/%d", jobDelivered or 0, j.n)
  end
  local d = nextMilestone("distance", tripDistance or 0)
  if d ~= nil then
    parts[#parts + 1] = string.format("里程 %.1f/%.1fkm",
                                      (tripDistance or 0) / 1000, d.n / 1000)
  end
  return table.concat(parts, "   ")
end

-- 生食材腐败速度：通电冰箱显著延缓
function rawFoodAging(gm, fridgePowered)
  local mul = fridgePowered and (1 / (SURVIVAL.fridgeSpoilMul or 3)) or 1
  if weather == "snow" then mul = mul * 0.5 end
  return (gm or 0) * mul
end

function itemCount(id)
  return inv[id] or 0
end

function hasItem(id, n)
  return (inv[id] or 0) >= (n or 1)
end

function takeItem(id, n)
  n = n or 1
  if (inv[id] or 0) < n then return false end
  inv[id] = inv[id] - n
  if inv[id] <= 0 then inv[id] = nil end
  return true
end

function itemName(id)
  local it = ITEMS[id]
  return it and it.name or tostring(id)
end

function powerOk()
  local net = (stats.powerGen or 0) - (stats.powerDraw or 0)
  return powerAvailable(net, batteryCharge or 0)
end

-- 电池容量：基础 + 电池模块
function batteryCapacity()
  return (VEH.baseBattery or 0) + (stats.battery or 0)
end

-- 电池一阶充放电：net(kW) × hours(h)，夹在 [0, cap]
function batteryStep(charge, net, hours, cap)
  return math.max(0, math.min(cap or 0,
                              (charge or 0) + (net or 0) * (hours or 0)))
end

-- 电力是否可用：发电≥负载直接可用，否则看电池余量
function powerAvailable(net, charge)
  if (net or 0) >= -0.001 then return true end
  return (charge or 0) > 0.01
end

-- 辅助用电：车内灯 / 夜间大灯（kW）
function auxPowerDraw(interiorOn, headlightsOn)
  return (interiorOn and 0.04 or 0) + (headlightsOn and 0.12 or 0)
end

-- 行驶时发动机带动发电机给电瓶充电（kW）；停车/没油不充
function alternatorPower(phaseValue, fuelValue)
  if phaseValue == "driving" and (fuelValue or 0) > 0 then
    return VEH.alternatorKw or 0.6
  end
  return 0
end

-- 电瓶低电量提示：余量 <20% 且净功率为负时给出充电建议
function batteryLowText(charge, cap, net)
  local c = charge or 0
  local m = cap or 0
  if m <= 0 then return nil end
  if c / m >= 0.2 then return nil end
  if (net or 0) >= 0 then return nil end
  return string.format("电瓶电量低（%.0f%%）——开车充电或开发电机", c / m * 100)
end

-- 每帧推进电池（游戏时间：1 现实秒 = timeScale 游戏分钟）
function updatePower(dt)
  local aux = auxPowerDraw(interiorLightsOn, (#rvHeadlights or 0) > 0)
  local alt = alternatorPower(phase, fuel)
  local net = (stats.powerGen or 0) - (stats.powerDraw or 0) - aux + alt
  local hours = (dt or 0) * SURVIVAL.timeScale / 60
  batteryCharge = batteryStep(batteryCharge or 0, net, hours, batteryCapacity())
  local lowText = batteryLowText(batteryCharge, batteryCapacity(), net)
  if lowText ~= nil and not lastWarn.battery then
    lastWarn.battery = true
    toast.text = lowText
    toast.t = 3.4
  elseif (batteryCharge or 0) / math.max(0.001, batteryCapacity()) > 0.35 then
    lastWarn.battery = false
  end
end

function useWater(amount)
  if water < amount then return false end
  water = water - amount
  return true
end

function addWater(amount)
  water = math.max(0, math.min(waterMax(), water + amount))
end

-- 水箱快空提示：低于 10% 时提醒补水（营地/雨水/风车）
function waterLowText(waterValue, maxValue)
  local w = waterValue or 0
  local m = maxValue or 0
  if m <= 0 then return nil end
  if w / m >= 0.10 then return nil end
  return string.format("水箱快空了（%.0fL）——营地免费补水、雨天接雨或风车取水", w)
end

-- 农场风车水泵：基础每 60 游戏分钟可接 30L 水；升级后冷却 30 分钟
WINDMILL_WATER_MINUTES = 60
WINDMILL_UPGRADE_MINUTES = 30
WINDMILL_WATER_AMOUNT = 30
WINDMILL_UPGRADE_COST = { metal = 3, wood = 2 }

function windmillWaterReadyMinutes()
  return windmillUpgraded and WINDMILL_UPGRADE_MINUTES or WINDMILL_WATER_MINUTES
end

function windmillWaterText()
  if (windmillWaterMin or 0) > 0 then
    return string.format("风车水泵冷却中（约 %.0f 分钟）", windmillWaterMin)
  end
  if windmillUpgraded then
    return string.format("E 接水 +%dL（升级水泵 30 分钟）", WINDMILL_WATER_AMOUNT)
  end
  return string.format("E 接水 +%dL · R 升级水泵（金属3 木材2）",
                       WINDMILL_WATER_AMOUNT)
end

-- 营地柴堆：每 60 游戏分钟可补充 1 个木材
CAMP_WOOD_MINUTES = 60

function campWoodpileText()
  if (campWoodMin or 0) > 0 then
    return string.format("柴堆补充中（约 %.0f 分钟）", campWoodMin)
  end
  return "E 取 1 个木材"
end

function gatherCampWoodpile()
  if (campWoodMin or 0) > 0 then
    toast.text = string.format("柴堆还没补充好（约 %.0f 分钟）", campWoodMin)
    toast.t = 2.2
    return false
  end
  campWoodMin = CAMP_WOOD_MINUTES
  materials.wood = (materials.wood or 0) + 1
  if PlaySfx then PlaySfx("pickup") end
  toast.text = "从营地柴堆拿了 1 个木材"
  toast.t = 2.4
  return true
end

-- 升级风车水泵：消耗金属 3 + 木材 2，冷却 60→30 分钟
function upgradeWindmill()
  if windmill == nil then return false end
  if windmillUpgraded then
    toast.text = "风车水泵已经升级过了"
    toast.t = 2.0
    return false
  end
  if not canAfford(WINDMILL_UPGRADE_COST) then
    toast.text = "升级风车需要：" .. costText(WINDMILL_UPGRADE_COST)
    toast.t = 2.8
    return false
  end
  payCost(WINDMILL_UPGRADE_COST)
  windmillUpgraded = true
  toast.text = "风车水泵升级完成——接水冷却缩短到 30 游戏分钟"
  toast.t = 3.6
  if PlaySfx then PlaySfx("repair") end
  if addJournal ~= nil then addJournal("升级了农场风车水泵", "goal") end
  if autoSave ~= nil then autoSave() end
  return true
end

function collectWindmillWater()
  if windmill == nil then return false end
  if (windmillWaterMin or 0) > 0 then
    toast.text = string.format("风车水泵还在抽水（约 %.0f 分钟）", windmillWaterMin)
    toast.t = 2.4
    return false
  end
  local before = water or 0
  addWater(WINDMILL_WATER_AMOUNT)
  local got = (water or 0) - before
  windmillWaterMin = windmillWaterReadyMinutes()
  if got <= 0.01 then
    toast.text = "水箱已经满了"
    toast.t = 2.0
    return false
  end
  if PlaySfx then PlaySfx("drink") end
  toast.text = string.format("风车水泵接了 %.0fL 水", got)
  toast.t = 2.6
  return true
end

-- 燃油罐：靠近房车按 E 加注（10L，受油箱上限约束）
function useFuelCan()
  local it = ITEMS.fuelcan
  if it == nil or not takeItem("fuelcan", 1) then return false end
  local before = fuel or 0
  fuel = math.min(fuelMax(), before + (it.amount or 10))
  local got = fuel - before
  if got <= 0.01 then
    addItem("fuelcan", 1)  -- 油箱已满，不浪费
    toast.text = "油箱已经满了"
    toast.t = 2.0
    return false
  end
  if PlaySfx then PlaySfx("pickup") end
  toast.text = string.format("加注 %0.0fL 燃料（%.0f / %d L）", got, fuel, fuelMax())
  toast.t = 2.4
  return true
end

-- 到站补给：营地免费加满；加油站按油量缺口收金属（每 15L 1 金属，至少 1）
-- 返回 ok, cost, note；金属不足时不加油、不扣材料。
function refuelAtNode(id)
  local need = fuelMax() - (fuel or 0)
  if id == "camp" then
    fuel = fuelMax()
    fuelWarned = false
    return true, 0, need > 0.5 and string.format("营地免费补给 +%.0fL", need) or "油箱已满"
  end
  if id ~= "gasstop" then return false, 0, nil end
  if need <= 0.5 then
    fuelWarned = false
    return true, 0, "油箱已满"
  end
  local cost = math.max(1, math.ceil(need / 15))
  if (materials.metal or 0) < cost then
    toast.text = string.format("加油需要 %d 金属（现有 %d）— 去搜刮或到营地免费加",
                               cost, materials.metal or 0)
    toast.t = 3.0
    return false, cost, "金属不足"
  end
  materials.metal = (materials.metal or 0) - cost
  fuel = fuelMax()
  fuelWarned = false
  if PlaySfx then PlaySfx("pickup") end
  return true, cost, string.format("加油 %dL 花费 %d 金属", math.floor(need + 0.5), cost)
end

-- 拖车：野外抛锚时花 4 金属把房车拉回营地（不修车、不补油）
TOW_COST = { metal = 4 }

function canCallTow()
  if math.sqrt((rv.x or 0) ^ 2 + (rv.z or 0) ^ 2) < 8 then
    return false, "已经在营地附近"
  end
  if phase ~= "driving" and not (nearRv ~= nil and nearRv()) then
    return false, "要靠近房车才能叫拖车"
  end
  if (materials.metal or 0) < (TOW_COST.metal or 4) then
    return false, string.format("拖车需要 %d 金属（现有 %d）",
                                TOW_COST.metal or 4, materials.metal or 0)
  end
  return true
end

function callTow()
  local ok, why = canCallTow()
  if not ok then
    toast.text = why or "现在不能叫拖车"
    toast.t = 2.6
    return false
  end
  materials.metal = (materials.metal or 0) - (TOW_COST.metal or 4)
  rv.x, rv.z, rv.yaw = 0, 0, 0
  rv.speed = 0
  rv.groundY = (terrainHeight and terrainHeight(0, 0)) or 0
  if rvBody ~= nil and PhysicsSetPosition ~= nil then
    PhysicsSetPosition(rvBody, { x = 0, y = 0.9 + rv.groundY, z = 0 })
    if PhysicsSetVelocity ~= nil then
      PhysicsSetVelocity(rvBody, { x = 0, y = 0, z = 0 })
    end
  end
  if applyRvTransform ~= nil then applyRvTransform() end
  if PlaySfx then PlaySfx("click") end
  toast.text = "拖车把房车拉回了营地（-4 金属）— 按空格停靠补给"
  toast.t = 4.0
  if addJournal ~= nil then
    addJournal("呼叫拖车回到营地（-4 金属）", "fail")
  end
  if autoSave ~= nil then autoSave() end
  return true
end

-- 最近的加油点（用于低油量提示）
function nearestFuelStop(x, z)
  local nd = (nodeById ~= nil) and nodeById("gasstop") or nil
  if nd == nil then return nil end
  local dx, dz = nd.x - (x or rv.x), nd.z - (z or rv.z)
  return { id = nd.id, name = nd.name, dist = math.sqrt(dx * dx + dz * dz) }
end

-- 低油量时罗盘是否标出最近加油站
function lowFuelCompassActive(fuelRatio)
  return (fuelRatio or 1) < 0.2
end

-- 低油量提示文案：续航 + 最近加油站距离/所需金属，金属不足时指向营地
function lowFuelHintText()
  local st = nearestFuelStop(rv.x, rv.z)
  if st == nil then return nil end
  local km = ((rangeEstimateM and rangeEstimateM()) or 0) / 1000
  local need = fuelMax() - (fuel or 0)
  local cost = math.max(1, math.ceil(need / 15))
  local tail = ""
  if (materials.metal or 0) < cost then
    local camp = (nodeById ~= nil) and nodeById("camp") or nil
    local campD = 0
    if camp ~= nil then
      campD = math.sqrt((camp.x - (rv.x or 0)) ^ 2 + (camp.z - (rv.z or 0)) ^ 2)
    end
    tail = string.format("；金属不足 %d，营地 %.0fm 免费补给", cost, campD)
  end
  return string.format("油量低（续航约 %.1fkm）— 最近加油站：%s %.0fm%s",
                       km, st.name, st.dist, tail)
end

-- 生命垂危时罗盘是否标出诊所
function lowHealthCompassActive(healthValue)
  return (healthValue or 100) < 30
end

-- 生命垂危提示：指向诊所（停靠可花布料包扎）
function lowHealthHintText(healthValue)
  if not lowHealthCompassActive(healthValue) then return nil end
  local nd = (nodeById ~= nil) and nodeById("clinic") or nil
  if nd == nil then return nil end
  local d = math.sqrt((nd.x - (rv.x or 0)) ^ 2 + (nd.z - (rv.z or 0)) ^ 2)
  return string.format("生命垂危（%.0f%%）——最近诊所 %s %.0fm，停靠可包扎",
                       healthValue or 0, nd.name, d)
end

-- 直接吃/喝背包里的东西
function useItem(id)
  local it = ITEMS[id]
  if it == nil then return false end
  if it.kind == "gear" then
    toast.text = it.name .. "：已自动生效（雨雪天提高抓地）"
    toast.t = 2.2
    return false
  end
  if it.kind == "dirty" then
    toast.text = "生水不能直接喝——到营地篝火按 E 煮沸"
    toast.t = 2.6
    return false
  end
  if not takeItem(id, 1) then return false end
  needs.hunger = math.min(100, needs.hunger + (it.hunger or 0))
  needs.thirst = math.min(100, needs.thirst + (it.thirst or 0))
  needs.energy = math.min(100, needs.energy + (it.energy or 0))
  needs.health = math.max(0, math.min(100, needs.health + (it.health or 0)))
  local warmMin = nil
  if id == "brew" then
    nightDriveMin = math.max(0, (nightDriveMin or 0) - 60)
    warmMin = 20
    warmthMin = math.max(warmthMin or 0, warmMin)
  end
  -- 热食/热汤：雪天获得一段时间的保暖
  if id == "fishsoup" or id == "meal" or id == "feast" or id == "stew" then
    local w = 30
    if id == "feast" then w = 45
    elseif id == "stew" then w = 40 end
    warmMin = w
    warmthMin = math.max(warmthMin or 0, warmMin)
  end
  if PlaySfx then PlaySfx(it.kind == "drink" and "drink" or "pickup") end
  local parts = {}
  if it.hunger then parts[#parts + 1] = string.format("饥饿 %+.0f", it.hunger) end
  if it.thirst then parts[#parts + 1] = string.format("口渴 %+.0f", it.thirst) end
  if it.energy then parts[#parts + 1] = string.format("体力 %+.0f", it.energy) end
  if warmMin ~= nil then
    parts[#parts + 1] = string.format("暖和 %d 分钟", warmMin)
  end
  toast.text = "吃下 " .. it.name .. (#parts > 0 and ("  " .. table.concat(parts, "  ")) or "")
  toast.t = 2.4
  return true
end

function damagePlayer(dmg)
  if needs.health <= 0 then return end
  needs.health = math.max(0, needs.health - dmg)
  hurtT = math.max(hurtT, 0.45)
  if PlaySfx then PlaySfx("hurt") end
  if needs.health <= 0 then faintPlayer() end
end

function healPlayer(amount)
  needs.health = math.min(100, needs.health + amount)
end

-- 倒下：丢 20% 材料，回到房车旁，推进 2 小时
function faintPlayer()
  if buildMode then
    buildMode = false
    if player.ent ~= nil then setPlayerVisible(false) end
    SetVar("cameraMouseLock", 1)
    if gridFloorEnt ~= nil then SetVisible(gridFloorEnt, false) end
    if gridRoofEnt ~= nil then SetVisible(gridRoofEnt, false) end
    if ghostEnt ~= nil then SetVisible(ghostEnt, false) end
    if hoverDecalEnt ~= nil then SetVisible(hoverDecalEnt, false) end
    hover = nil
  end
  despawnHorde()
  for k, v in pairs(materials) do materials[k] = math.floor(v * 0.8) end
  needs.health = 45
  needs.hunger = math.max(needs.hunger, 25)
  needs.thirst = math.max(needs.thirst, 25)
  needs.energy = math.max(needs.energy, 40)
  player.x, player.z = rotOf(0, -5.0)
  player.yaw = rv.yaw
  player.pitch = 0
  advanceGameMinutes(120)
  hurtT = 1.0
  toast.text = "你昏倒了……醒来时在房车旁，丢了 20% 材料"
  toast.t = 4.0
end

-- 夜里在营地篝火旁休息：慢速恢复体力
function campfireRestAvailable()
  if campfire == nil or not onFoot or not player.active or buildMode then return false end
  if atNode ~= "camp" then return false end
  if campfireOn == false then return false end
  if not (isNight ~= nil and isNight()) then return false end
  local dx, dz = player.x - campfire.x, player.z - campfire.z
  return dx * dx + dz * dz < 3.4 * 3.4
end

-- 站在点燃的篝火旁（4m 内）会持续保暖，不限时段/地点
function campfireWarmthNear(px, pz, fireX, fireZ, lit)
  if not lit or fireX == nil or fireZ == nil then return false end
  local dx, dz = (px or 0) - fireX, (pz or 0) - fireZ
  return dx * dx + dz * dz < 4.0 * 4.0
end

-- 卫生对睡眠恢复的影响：干净时 100%，脏到 0 只剩 60%
function hygieneEnergyFactor(h)
  local v = math.max(0, math.min(100, h or 0)) / 100
  return 0.6 + 0.4 * v
end

-- 车内舒适度对睡眠恢复的加成（最多 +15%）
function comfortSleepBonus(comfort)
  return math.min(0.15, math.max(0, (comfort or 0) / 300))
end

function sleepInBed()
  -- 07:00 之前睡 = 当天早上；之后睡 = 第二天早上
  if clock >= 7 * 60 then day = day + 1 end
  clock = 7 * 60
  nightDriveMin = 0
  restedMin = 240
  local atCamp = (atNode == "camp")
  local comfort = (stats ~= nil and stats.comfort) or 0
  local campfireWarm = atCamp and campfireOn ~= false
  local factor = hygieneEnergyFactor(needs.hygiene) +
                 (atCamp and 0.10 or 0) + comfortSleepBonus(comfort) +
                 (campfireWarm and 0.05 or 0)
  factor = math.min(1.0, factor)
  if campfireWarm then
    warmthMin = math.max(warmthMin or 0, 120)
  end
  needs.energy = math.min(100, needs.energy + (100 - needs.energy) * factor)
  needs.hunger = math.max(0, needs.hunger - 20)
  needs.thirst = math.max(0, needs.thirst - 32)
  needs.hygiene = math.max(0, needs.hygiene - 10)
  needs.health = math.min(100, needs.health + (atCamp and 14 or 8))
  if LC and LC.flow then LC.flow.emit("day_advanced") end
  morningReport = {
    day = day,
    camp = atCamp,
    jobs = jobDelivered or 0,
    goal = dailyGoal and dailyGoal.name or nil,
    goalDone = dailyGoal and dailyGoal.done or false,
    goalProgress = dailyGoal and dailyGoal.progress or 0,
    goalTarget = dailyGoal and dailyGoal.target or 0,
    fuel = fuel or 0,
    forecast = weatherNextId,
    oil = oilLife or 100,
    notes = loreProgressText(),
    t = 5.0,
  }
  toast.text = string.format("睡到第 %d 天 07:00 — 体力恢复", day)
  toast.t = 3.5
  if autoSave ~= nil then autoSave() end
end

-- 小睡 2 小时：不跳晨报，恢复部分体力，适合等天气/等货运截止
function napInBed()
  local before = needs.energy or 0
  advanceGameMinutes(120)
  needs.energy = math.min(100, before + 25)
  needs.hunger = math.max(0, (needs.hunger or 0) - 8)
  needs.thirst = math.max(0, (needs.thirst or 0) - 10)
  restedMin = math.max(restedMin or 0, 60)
  if PlaySfx then PlaySfx("click") end
  toast.text = string.format("小睡 2 小时 — 体力 %.0f", needs.energy)
  toast.t = 3.0
  return true
end

-- 睡到傍晚 19:00：为夜间跑单调整作息（不弹晨报）
function sleepUntilEvening()
  local before = needs.energy or 0
  if (clock or 0) < 19 * 60 then
    clock = 19 * 60
  else
    day = (day or 1) + 1
    clock = 19 * 60
  end
  needs.energy = math.min(100, before + 35)
  needs.hunger = math.max(0, (needs.hunger or 0) - 12)
  needs.thirst = math.max(0, (needs.thirst or 0) - 16)
  needs.hygiene = math.max(0, (needs.hygiene or 0) - 5)
  restedMin = math.max(restedMin or 0, 90)
  if PlaySfx then PlaySfx("click") end
  toast.text = string.format("睡到傍晚 19:00 — 体力 %.0f，适合夜间跑单",
                             needs.energy)
  toast.t = 3.2
  return true
end

-- 收音机广播：开启后按游戏分钟间隔播报路况/天气提示
-- 当前收音机广播池：修好无线电塔后优先播报“信号恢复”内容
function radioBroadcastPool()
  if relayDone and RADIO_BROADCASTS_RELAY ~= nil and
     #RADIO_BROADCASTS_RELAY > 0 then
    return RADIO_BROADCASTS_RELAY
  end
  if radioMastFixed and RADIO_BROADCASTS_FIXED ~= nil and
     #RADIO_BROADCASTS_FIXED > 0 then
    return RADIO_BROADCASTS_FIXED
  end
  return RADIO_BROADCASTS or {}
end

-- 下一段天气的收音机警告（无警告返回 nil）
function radioWeatherWarning()
  local nid = weatherNextId
  if nid == "rain" then
    return "收音机：前方有雨，路面湿滑，注意减速。"
  elseif nid == "snow" then
    return "收音机：前方有雪，路面结冰，记得装防滑链。"
  elseif nid == "fog" then
    return "收音机：前方起雾，开远光灯、放慢车速。"
  end
  return nil
end

-- 收音机按玩家进度播报不同内容（无进度时返回 nil）
function radioProgressLine(roll)
  local lines = {}
  if relayDone then
    lines[#lines + 1] = "收音机：救援队说东边的公路已经清理，感谢你恢复信号中继。"
  end
  if (jobDelivered or 0) >= 10 then
    lines[#lines + 1] = string.format(
      "收音机：货运站都在传你跑完了 %d 单，路上小心。", jobDelivered)
  end
  if (fishCaught or 0) >= 10 then
    lines[#lines + 1] = string.format(
      "收音机：湖边有人夸你钓了 %d 条鱼，说鱼汤很香。", fishCaught)
  end
  if (visitedCount or 0) >= 5 then
    lines[#lines + 1] = "收音机：地图上大半地点都被你跑遍了，注意休息。"
  end
  if allUpgradesDone then
    lines[#lines + 1] = "收音机：听说有人把整辆房车都升级满了，真是下了血本。"
  end
  if #lines == 0 then return nil end
  local r = roll or math.random()
  return lines[math.floor(r * #lines) + 1]
end

-- 信号强度：修好无线电塔前，广播有概率夹杂杂音
function radioSignalLossChance()
  if radioMastFixed or relayDone then return 0 end
  return 0.35
end

function updateRadioBroadcast(gm)
  if not radioOn then return false end
  radioBroadcastT = (radioBroadcastT or 0) + (gm or 0)
  if radioBroadcastT < (radioBroadcastNext or 120) then return false end
  radioBroadcastT = 0
  radioBroadcastNext = 90 + math.random() * 60
  local list = radioBroadcastPool()
  local warn = radioWeatherWarning()
  local progressLine = radioProgressLine and radioProgressLine() or nil
  local line
  if math.random() < (radioSignalLossChance and radioSignalLossChance() or 0) then
    line = "收音机：（沙沙……）信号很弱，听不清。修好无线电塔也许能好点。"
  elseif (radioStation or 1) == 3 then
    -- 天气台：更频繁地播报当前/下一段天气
    radioBroadcastNext = 40 + math.random() * 30
    local nowName = (WEATHER_NAMES and WEATHER_NAMES[weather]) or tostring(weather)
    local nextName = (WEATHER_NAMES and WEATHER_NAMES[weatherNextId]) or "未知"
    local hrs = (weatherHoursLeft and weatherHoursLeft()) or 0
    line = string.format("天气台：现在%s，稍后转%s（约 %.1f 小时后）。",
                         nowName, nextName, hrs)
  elseif (radioStation or 1) == 4 then
    -- 数字台：循环播报一串数字，纯氛围
    radioBroadcastNext = 50 + math.random() * 40
    local nums = { "三", "七", "九", "一", "四", "八", "二", "六" }
    local seq = {}
    for i = 1, 4 do seq[i] = nums[math.random(1, #nums)] end
    line = string.format("数字台：（电流声）……%s……重复，%s……",
                         table.concat(seq, "、"), table.concat(seq, "、"))
  elseif progressLine ~= nil and math.random() < 0.35 then
    line = progressLine
  elseif warn ~= nil and math.random() < 0.5 then
    line = warn
  elseif #list > 0 then
    line = list[math.random(#list)]
  else
    return false
  end
  toast.text = line
  toast.t = 5.0
  if addJournal ~= nil then addJournal(line, "radio") end
  return true
end

-- 夜间驾驶疲劳：连续开夜车超过 120 游戏分钟后体力消耗加快；
-- 睡饱（rested）时疲劳积累减半
function nightFatigueExtra(minutes, rested)
  local m = minutes or 0
  if m <= 120 then return 0 end
  local extra = math.min(0.020, (m - 120) * 0.0002)
  if (rested or 0) > 0 then extra = extra * 0.5 end
  return extra
end

-- 疲劳视觉强度（0–1）：连续夜驾越久越明显，用于屏幕边缘暗角
function fatigueAlpha(minutes)
  local m = minutes or 0
  if m <= 120 then return 0 end
  return math.min(1.0, (m - 120) / 180)
end

-- 夜间疲劳导致的轻微方向游移（叠加在玩家转向输入上）
function nightSteerWander(minutes, t)
  local a = fatigueAlpha(minutes)
  if a <= 0 then return 0 end
  local s = (math.sin((t or 0) * 1.7) * 0.016 +
             math.sin((t or 0) * 0.53) * 0.009) * a
  return math.max(-0.03, math.min(0.03, s))
end

-- 雪天寒冷：没开取暖器时体力消耗更快；热食保暖期间免疫
function coldEnergyExtra(weatherValue, heaterOn, warmth)
  if weatherValue == "snow" and not heaterOn and (warmth or 0) <= 0 then
    return SURVIVAL.coldExtra or 0.020
  end
  return 0
end

-- 保暖状态文案（HUD 用）：无保暖返回 nil
function warmthStatusText()
  if (warmthMin or 0) <= 0 then return nil end
  return string.format("暖和 %.0f 分钟", warmthMin)
end

-- 统一设置保暖分钟（联机客机按服务器值同步）
function setWarmth(v)
  if v == nil then return end
  warmthMin = math.max(0, tonumber(v) or 0)
end

-- 睡饱状态文案（HUD 用）：无加成返回 nil
function restedStatusText()
  if (restedMin or 0) <= 0 then return nil end
  return string.format("精神不错 %.0f 分钟", restedMin)
end

-- 统一设置睡饱分钟（联机客机按服务器值同步）
function setRested(v)
  if v == nil then return end
  restedMin = math.max(0, tonumber(v) or 0)
end

-- 篝火边休息会持续取暖（至少 20 分钟保暖）
function campfireWarmthMinutes(current)
  return math.max(current or 0, 20)
end

-- 长线收尾：修好无线电塔后，去废弃小镇恢复信号中继
RELAY_REWARD = { metal = 10, electronics = 5, fuel = 30 }

function completeSignalRelay()
  if relayDone then return false end
  relayDone = true
  local beforeFuel = fuel or 0
  for k, v in pairs(RELAY_REWARD) do
    if k == "fuel" then
      fuel = math.min(fuelMax(), (fuel or 0) + v)
    else
      materials[k] = (materials[k] or 0) + v
    end
  end
  relayCard = {
    day = day or 1, jobs = jobDelivered or 0,
    km = (tripDistance or 0) / 1000, fish = fishCaught or 0,
    explored = visitedCount or 0, total = #NODES,
    fuelGain = (fuel or 0) - beforeFuel,
    t = 8.0,
  }
  toast.text = "信号中继已恢复——你联系上了外界！"
  toast.t = 6.0
  if addJournal then addJournal("在废弃小镇恢复了信号中继", "goal") end
  if autoSave then autoSave() end
  return true
end

function updateRelay()
  if relayDone or not radioMastFixed then return false end
  if phase == "parked" and atNode == "town" then
    return completeSignalRelay()
  end
  return false
end

-- 恢复中继后的每日救援补给（每天一次）
function grantRelaySupply()
  if not relayDone then return false end
  if (relaySupplyDay or 0) == (day or 0) then return false end
  relaySupplyDay = day or 1
  materials.metal = (materials.metal or 0) + 3
  materials.electronics = (materials.electronics or 0) + 1
  local before = fuel or 0
  fuel = math.min(fuelMax(), before + 10)
  local gained = (fuel or 0) - before
  toast.text = string.format(
    "救援补给已送达营地：金属+3 电子件+1 燃料+%.0fL", gained)
  toast.t = 5.0
  if addJournal ~= nil then
    addJournal("收到救援队的每日补给", "goal")
  end
  return true
end

-- 时间/需求主循环
function updateSurvival(dt)
  if hurtT > 0 then hurtT = math.max(0, hurtT - dt) end

  local gm = dt * SURVIVAL.timeScale
  advanceGameMinutes(gm)
  if updateRadioBroadcast ~= nil then updateRadioBroadcast(gm) end
  if updatePondTrap ~= nil then updatePondTrap(gm) end
  if (warmthMin or 0) > 0 then
    warmthMin = math.max(0, warmthMin - gm)
  end
  if (restedMin or 0) > 0 then
    restedMin = math.max(0, restedMin - gm)
  end
  if (windmillWaterMin or 0) > 0 then
    windmillWaterMin = math.max(0, windmillWaterMin - gm)
  end
  if (campWoodMin or 0) > 0 then
    campWoodMin = math.max(0, campWoodMin - gm)
  end

  needs.hunger = math.max(0, needs.hunger - SURVIVAL.hungerRate * gm)
  needs.thirst = math.max(0, needs.thirst - SURVIVAL.thirstRate * gm)
  needs.hygiene = math.max(0, needs.hygiene - SURVIVAL.hygieneRate * gm)
  local sprinting = onFoot and not buildMode and ActionDown("w") and ActionDown("shift")
  local coldExtra = coldEnergyExtra(weather, (stats and stats.heaterOn) or false,
                                    warmthMin)
  local nightDriving = phase == "driving" and (isNight ~= nil and isNight()) and
                       math.abs(rv.speed or 0) > 1.0
  if nightDriving then
    nightDriveMin = (nightDriveMin or 0) + gm
  elseif phase ~= "driving" then
    nightDriveMin = math.max(0, (nightDriveMin or 0) - gm * 0.5)
  end
  local fatigue = nightFatigueExtra(nightDriveMin, restedMin)
  needs.energy = math.max(0, needs.energy -
    (SURVIVAL.energyRate + (sprinting and SURVIVAL.sprintExtra or 0) +
     coldExtra + fatigue) * gm)
  if weather == "snow" and coldExtra > 0 and needs.energy < 30 and
     not lastWarn.cold then
    lastWarn.cold = true
    toast.text = "下雪了，车里很冷——打开取暖器或回营地休息"
    toast.t = 3.4
  elseif not (weather == "snow" and coldExtra > 0) then
    lastWarn.cold = false
  end
  if nightDriveMin > 120 and not lastWarn.fatigue then
    lastWarn.fatigue = true
    toast.text = "开夜车有点累了——停车睡一觉，或喝杯热咖啡提神"
    toast.t = 3.6
  elseif nightDriveMin < 90 then
    lastWarn.fatigue = false
  end
  -- 夜里坐在营地篝火旁：缓慢回体力，并持续取暖
  if campfireRestAvailable ~= nil and campfireRestAvailable() then
    needs.energy = math.min(100, needs.energy +
      (SURVIVAL.campfireRest or 0.10) * gm)
    warmthMin = campfireWarmthMinutes(warmthMin)
  end
  -- 任何时段站在点燃的篝火旁都会持续保暖
  if onFoot and not buildMode and campfire ~= nil and
     campfireWarmthNear(player.x, player.z, campfire.x, campfire.z,
                        campfireOn ~= false) then
    warmthMin = math.max(warmthMin or 0, 20)
  end

  if needs.hunger <= 0 or needs.thirst <= 0 then
    needs.health = math.max(0, needs.health - SURVIVAL.healthDrain * gm)
  elseif needs.hunger > 45 and needs.thirst > 45 then
    needs.health = math.min(100, needs.health + SURVIVAL.healthRegen * gm)
  end
  -- 卫生太差：轻微掉血，睡醒恢复也变差
  if needs.hygiene < 15 then
    needs.health = math.max(0, needs.health - SURVIVAL.healthDrain * 0.35 * gm)
  end

  if needs.health <= 0 then faintPlayer() end

  -- 低需求提示（各只提示一次，恢复后重置）
  if needs.hunger < 25 and not lastWarn.hunger then
    lastWarn.hunger = true
    toast.text = "饿得发慌 — 到灶台做饭，或在冰箱找吃的"
    toast.t = 3.2
  elseif needs.hunger > 50 then lastWarn.hunger = false end
  if needs.thirst < 25 and not lastWarn.thirst then
    lastWarn.thirst = true
    toast.text = "口渴 — 到水槽喝水，或开一瓶瓶装水"
    toast.t = 3.2
  elseif needs.thirst > 50 then lastWarn.thirst = false end
  if needs.energy < 20 and not lastWarn.energy then
    lastWarn.energy = true
    toast.text = "体力见底 — 到床上睡一觉"
    toast.t = 3.2
  elseif needs.energy > 45 then lastWarn.energy = false end
  if needs.hygiene < 20 and not lastWarn.hygiene then
    lastWarn.hygiene = true
    toast.text = "身上太脏 — 到卫生间洗漱（卫生差会睡不好）"
    toast.t = 3.4
  elseif needs.hygiene > 40 then lastWarn.hygiene = false end
  local wLow = waterLowText(water, waterMax())
  if wLow ~= nil and not lastWarn.water then
    lastWarn.water = true
    toast.text = wLow
    toast.t = 3.6
  elseif (water or 0) > waterMax() * 0.25 then
    lastWarn.water = false
  end
  local hLow = lowHealthHintText(needs.health)
  if hLow ~= nil and not lastWarn.health then
    lastWarn.health = true
    toast.text = hLow
    toast.t = 4.0
  elseif (needs.health or 0) > 50 then
    lastWarn.health = false
  end

  -- 雨天接水
  if weather == "rain" and water < waterMax() then
    water = math.min(waterMax(), water + 0.04 * gm)
  end

  -- 生食材腐败：通电冰箱显著延缓；放坏后整批清空
  local rawCount = (inv.rawfood or 0) + (inv.rawfish or 0)
  if rawCount > 0 then
    local fridgeOn = hasModule("fridge") ~= nil and powerOk()
    rawFoodAge = (rawFoodAge or 0) + rawFoodAging(gm, fridgeOn)
    local limit = SURVIVAL.rawSpoilMinutes or 2880
    if rawFoodAge >= limit then
      inv.rawfood, inv.rawfish = nil, nil
      rawFoodAge, rawSpoilWarned = 0, false
      toast.text = "生食材放坏了——下次及时做饭，或放进通电的冰箱"
      toast.t = 4.0
    elseif rawFoodAge > limit * 0.75 and not rawSpoilWarned then
      rawSpoilWarned = true
      toast.text = "生食材快坏了——去灶台做饭，或放冰箱"
      toast.t = 3.2
    end
  else
    rawFoodAge, rawSpoilWarned = 0, false
  end

  updatePlants(dt)
end

-- 钓鱼：水塘边按 E 抛竿，等待 5-9 秒
-- 地图/UI 用的钓鱼纪录文本
function fishingRecordText()
  if (fishCaught or 0) <= 0 then return "钓鱼纪录：暂无" end
  local who = bestFishName and (bestFishName .. " ") or ""
  return string.format("钓鱼纪录 %s%.0fcm · %d 条", who, bestFish or 0,
                       fishCaught or 0)
end

-- 鱼竿升级：每级上钩率 +7%、鱼尺寸 +4cm
function rodCatchBonus()
  return (upgrades and upgrades.rod or 0) * 0.07
end

function rodSizeBonus()
  return (upgrades and upgrades.rod or 0) * 4
end

function upgradeRod()
  upgrades.rod = upgrades.rod or 0
  if upgrades.rod >= 3 then
    toast.text = "鱼竿已经满级"
    toast.t = 1.8
    return false
  end
  local lv = upgrades.rod
  local cost = { wood = 2 + lv * 2, cloth = 1 + lv }
  if not canAfford(cost) then
    toast.text = "升级材料不足：" .. costText(cost)
    toast.t = 2.2
    return false
  end
  payCost(cost)
  upgrades.rod = lv + 1
  if PlaySfx then PlaySfx("repair") end
  toast.text = string.format("鱼竿升级到 Lv%d — 上钩率 +%d%%，尺寸 +%dcm",
                             upgrades.rod, upgrades.rod * 7, upgrades.rod * 4)
  toast.t = 2.6
  if checkUpgradeMilestone ~= nil then checkUpgradeMilestone() end
  return true
end

-- 工作台制作防雨篷布：金属 2 + 布料 3
function craftTarp()
  local cost = { metal = 2, cloth = 3 }
  if not canAfford(cost) then
    toast.text = "材料不足：" .. costText(cost)
    toast.t = 2.2
    return false
  end
  payCost(cost)
  addItem("tarp", 1)
  if PlaySfx then PlaySfx("repair") end
  toast.text = "制作防雨篷布 x1（金属 -2 布料 -3）— 易碎货不再怕雨"
  toast.t = 3.0
  return true
end

-- 工作台制作修理包：金属 2 + 布料 1
function craftRepairKit()
  local cost = { metal = 2, cloth = 1 }
  if not canAfford(cost) then
    toast.text = "材料不足：" .. costText(cost)
    toast.t = 2.2
    return false
  end
  payCost(cost)
  addItem("repairkit", 1)
  if PlaySfx then PlaySfx("repair") end
  toast.text = "制作修理包 x1（金属 -2 布料 -1）"
  toast.t = 2.6
  return true
end

-- 钓鱼：按权重抽鱼种，并在该鱼种的尺寸区间内取一个尺寸
-- 鱼种偏好：雨天/夜间鲶鱼、黑鱼更活跃；晴天鲫鱼、鳊鱼略多
function fishWeight(sp, weatherValue, night)
  local w = sp.w or 1
  local name = sp.name
  if name == "鲶鱼" or name == "黑鱼" then
    if weatherValue == "rain" then w = w * 1.6 end
    if night then w = w * 1.4 end
  elseif name == "鲫鱼" or name == "鳊鱼" then
    if weatherValue == "clear" then w = w * 1.15 end
  end
  return w
end

function rollFish()
  local night = (isNight ~= nil and isNight())
  local total = 0
  for _, s in ipairs(FISH_SPECIES) do
    total = total + fishWeight(s, weather, night)
  end
  local r = math.random() * total
  local pick = FISH_SPECIES[1]
  for _, s in ipairs(FISH_SPECIES) do
    r = r - fishWeight(s, weather, night)
    if r <= 0 then pick = s break end
  end
  local size = pick.min + math.random() * (pick.max - pick.min)
  return pick.name, size, pick
end

function despawnFishingBobber()
  if fishingBobber ~= nil then Despawn(fishingBobber) end
  fishingBobber, fishingBobberBase = nil, nil
end

function spawnFishingBobber()
  despawnFishingBobber()
  if player == nil then return end
  local fx, fz = math.sin(player.yaw or 0), math.cos(player.yaw or 0)
  local x, z = player.x + fx * 2.4, player.z + fz * 2.4
  local y = ((terrainHeight and terrainHeight(x, z)) or 0) + 0.10
  local e = SpawnPrefab("rv_bobber", { x = x, y = y, z = z })
  if e ~= nil then
    SetScale(e, 0.11, 0.14, 0.11)
    fishingBobber = e
    fishingBobberBase = { x = x, y = y, z = z }
    fishingBobberT = 0
  end
end

-- 湖边鱼笼：放置后约 60 游戏分钟有收获，回来收取 2 条生鱼
POND_TRAP_COST = { metal = 2, wood = 1 }
POND_TRAP_MINUTES = 60

function pondTrapStatusText()
  if pondTrap == nil then
    return string.format("Z 放鱼笼（金属%d 木材%d）",
                         POND_TRAP_COST.metal, POND_TRAP_COST.wood)
  end
  if pondTrap.ready then return "Z 收鱼笼（生鱼 x2）" end
  return string.format("鱼笼捕鱼中（约 %.0f 分钟）", pondTrap.t or 0)
end

function togglePondTrapAt(px, pz)
  if pondTrap == nil then
    if not canAfford(POND_TRAP_COST) then
      toast.text = "鱼笼材料不足：" .. costText(POND_TRAP_COST)
      toast.t = 2.6
      return false
    end
    payCost(POND_TRAP_COST)
    pondTrap = { x = px or 0, z = pz or 0, t = POND_TRAP_MINUTES,
                 ready = false }
    -- 在岸边放一个可见的小鱼笼实体
    local ex, ez = (px or 0) + 2.0, (pz or 0) + 2.0
    local ey = (terrainHeight and terrainHeight(ex, ez)) or 0
    local ent = SpawnPrefab("rv_cargo_crate", { x = ex, y = ey + 0.16, z = ez })
    if ent ~= nil then
      SetScale(ent, 0.35, 0.22, 0.35)
      SetRotationY(ent, 0.4)
      pondTrap.ent = ent
    end
    if PlaySfx then PlaySfx("pickup") end
    toast.text = "放好鱼笼——约 60 游戏分钟后回来收"
    toast.t = 3.0
    return true
  end
  if not pondTrap.ready then
    toast.text = string.format("鱼笼还没捕到鱼（约 %.0f 分钟）", pondTrap.t or 0)
    toast.t = 2.4
    return false
  end
  if pondTrap.ent ~= nil then Despawn(pondTrap.ent) end
  pondTrap = nil
  -- 放 1 个鱼饵可多收 1 条鱼
  local extra = 0
  if hasItem ~= nil and hasItem("bait", 1) then
    takeItem("bait", 1)
    extra = 1
  end
  addItem("rawfish", 2 + extra)
  if PlaySfx then PlaySfx("pickup") end
  toast.text = string.format("收鱼笼：生鱼 x%d%s", 2 + extra,
                             extra > 0 and "（用掉 1 鱼饵）" or "")
  toast.t = 2.6
  return true
end

function updatePondTrap(gm)
  if pondTrap == nil or pondTrap.ready then return end
  pondTrap.t = (pondTrap.t or 0) - (gm or 0)
  if pondTrap.t <= 0 then
    pondTrap.t = 0
    pondTrap.ready = true
  end
end

function startFishing()
  if fishingT > 0 then return end
  fishingTotal = 5 + math.random() * 4
  fishingT = fishingTotal
  fishingBite, fishingBiteT = false, 0
  fishingBait = false
  fishingDock = (nearDock ~= nil) and nearDock(player.x, player.z) or false
  spawnFishingBobber()
  if takeItem("bait", 1) then
    fishingBait = true
    toast.text = fishingDock and "在栈桥上抛竿（鱼饵）— 别走开"
                              or "抛竿（用鱼饵）— 别走动，等咬钩"
  else
    toast.text = fishingDock and "在栈桥上抛竿 — 视野好，更容易上钩"
                              or "抛竿了——等鱼上钩（别走开）"
  end
  toast.t = 2.2
end

-- 天气/时段对钓鱼的影响：雨天、晨昏更好钓，深夜更难
function fishingConditionBonus()
  local chance, size = 0, 0
  local h = (clock or 0) / 60
  if weather == "rain" then
    chance = chance + 0.08
    size = size + 3
  elseif weather == "snow" then
    chance = chance - 0.05
  end
  if (h >= 5 and h < 7) or (h >= 17 and h < 19) then
    chance = chance + 0.06
    size = size + 2
  elseif h >= 21 or h < 5 then
    chance = chance - 0.10
  end
  return chance, size
end

-- 鱼类图鉴数据：每种鱼的纪录尺寸/累计条数/尺寸区间
function fishBookRows()
  local rows = {}
  for _, sp in ipairs(FISH_SPECIES or {}) do
    rows[#rows + 1] = {
      name = sp.name,
      record = (fishRecords and fishRecords[sp.name]) or 0,
      count = (fishCounts and fishCounts[sp.name]) or 0,
      min = sp.min, max = sp.max,
    }
  end
  return rows
end

-- 记录一次渔获：各鱼种最大尺寸 + 总纪录 + 累计条数
-- 集齐全部鱼种的一次性奖励
-- 跑遍所有固定地点的一次性奖励
function checkExplorationMilestone()
  if exploreAllDone then return false end
  local fixed = { "gasstop", "ruin", "mall", "town", "depot", "farm",
                  "lakecamp", "clinic" }
  for _, id in ipairs(fixed) do
    if not visitedPois[id] then return false end
  end
  exploreAllDone = true
  materials.metal = (materials.metal or 0) + 12
  materials.electronics = (materials.electronics or 0) + 4
  materials.cloth = (materials.cloth or 0) + 4
  local before = fuel or 0
  fuel = math.min(fuelMax(), (fuel or 0) + 20)
  local fuelGain = (fuel or 0) - before
  toast.text = string.format(
    "跑遍所有地点！奖励 金属+12 电子件+4 布料+4 燃料+%.0fL", fuelGain)
  toast.t = 5.0
  if addJournal ~= nil then
    addJournal(string.format("跑遍所有固定地点（金属+12 电子件+4 布料+4 燃料+%.0fL）",
                             fuelGain), "goal")
  end
  if autoSave ~= nil then autoSave() end
  return true
end

-- 旅行日志顶部摘要：天数/货运/里程/钓鱼/探索/笔记/连送
function journalSummaryText()
  local parts = {
    string.format("第 %d 天", day or 1),
    string.format("货运 %d 单", jobDelivered or 0),
    string.format("里程 %.1fkm", (tripDistance or 0) / 1000),
    string.format("钓鱼 %d 条", fishCaught or 0),
    string.format("探索 %d/%d", visitedCount or 0, #(NODES or {})),
  }
  if loreProgressText ~= nil then
    local lp = loreProgressText()
    if lp ~= nil then parts[#parts + 1] = lp end
  end
  if (jobStreak or 0) > 0 then
    parts[#parts + 1] = string.format("连送 %d", jobStreak)
  end
  return table.concat(parts, "  ·  ")
end

-- 笔记收集进度文案（地图/日志用）
function loreProgressText()
  local total = #(LORE_NOTES or {})
  if total <= 0 then return nil end
  local found = 0
  for _, d in ipairs(LORE_NOTES) do
    if notesFound[d.id] then found = found + 1 end
  end
  return string.format("笔记 %d/%d", found, total)
end

-- 读取路牌笔记：写入旅行日志，集齐全部后给一次性奖励
function readLoreNote(index)
  local n = loreNotes and loreNotes[index] or nil
  if n == nil or notesFound[n.id] then return false end
  notesFound[n.id] = true
  if n.ent ~= nil then SetVisible(n.ent, false) end
  local def = nil
  for _, d in ipairs(LORE_NOTES or {}) do
    if d.id == n.id then def = d end
  end
  if def ~= nil and addJournal ~= nil then
    addJournal(string.format("读笔记《%s》：%s", def.title, def.text), "note")
  end
  local found, total = 0, #(LORE_NOTES or {})
  for _, d in ipairs(LORE_NOTES or {}) do
    if notesFound[d.id] then found = found + 1 end
  end
  toast.text = string.format("读完《%s》（%d/%d）",
                             def and def.title or "笔记", found, total)
  toast.t = 4.0
  if PlaySfx then PlaySfx("pickup") end
  if found >= total and not loreRewardDone then
    loreRewardDone = true
    materials.metal = (materials.metal or 0) + 8
    materials.electronics = (materials.electronics or 0) + 2
    local before = fuel or 0
    fuel = math.min(fuelMax(), before + 15)
    local fuelGain = (fuel or 0) - before
    if addJournal ~= nil then
      addJournal(string.format(
        "集齐所有路牌笔记（金属+8 电子件+2 燃料+%.0fL）", fuelGain), "goal")
    end
    toast.text = string.format(
      "集齐所有路牌笔记！奖励 金属+8 电子件+2 燃料+%.0fL", fuelGain)
    toast.t = 5.0
    if autoSave ~= nil then autoSave() end
  end
  return true
end

-- 全车升级满级（引擎/轮胎/货架/鱼竿/水箱/油箱）：一次性奖励
function allUpgradesMaxed()
  for _, id in ipairs({ "engine", "tires", "cargo", "rod", "water", "fuel" }) do
    if (upgrades[id] or 0) < 3 then return false end
  end
  return true
end

function checkUpgradeMilestone()
  if allUpgradesDone then return false end
  if not allUpgradesMaxed() then return false end
  allUpgradesDone = true
  materials.metal = (materials.metal or 0) + 10
  materials.electronics = (materials.electronics or 0) + 5
  local before = fuel or 0
  fuel = math.min(fuelMax(), before + 20)
  local fuelGain = (fuel or 0) - before
  toast.text = string.format(
    "全车升级满级！奖励 金属+10 电子件+5 燃料+%.0fL", fuelGain)
  toast.t = 5.0
  if addJournal ~= nil then
    addJournal(string.format(
      "完成全车升级（金属+10 电子件+5 燃料+%.0fL）", fuelGain), "goal")
  end
  if autoSave ~= nil then autoSave() end
  return true
end

function checkFishCollection()
  if fishCollectionDone then return false end
  local total = #(FISH_SPECIES or {})
  if total <= 0 then return false end
  local found = 0
  for _, sp in ipairs(FISH_SPECIES) do
    if (fishCounts[sp.name] or 0) > 0 then found = found + 1 end
  end
  if found < total then return false end
  fishCollectionDone = true
  materials.metal = (materials.metal or 0) + 8
  materials.electronics = (materials.electronics or 0) + 3
  addItem("bait", 3)
  toast.text = "鱼类图鉴集齐！奖励 金属+8 电子件+3 鱼饵+3"
  toast.t = 5.0
  if addJournal ~= nil then
    addJournal("集齐全部鱼种（金属+8 电子件+3 鱼饵+3）", "goal")
  end
  if autoSave ~= nil then autoSave() end
  return true
end

function recordFish(name, size, n)
  n = n or 1
  fishCounts[name] = (fishCounts[name] or 0) + n
  local firstCatch = (fishRecords[name] or 0) <= 0
  local speciesRecord = false
  if size > (fishRecords[name] or 0) then
    fishRecords[name] = size
    speciesRecord = true
  end
  local overall = false
  if size > (bestFish or 0) then
    bestFish = size
    bestFishName = name
    overall = true
  end
  fishCaught = (fishCaught or 0) + n
  if addDailyProgress ~= nil then addDailyProgress("fish", n) end
  if addJournal ~= nil then
    if firstCatch then
      addJournal(string.format("首次钓到：%s %.0fcm", name, size), "fish")
    elseif overall then
      addJournal(string.format("钓鱼新纪录：%s %.0fcm", name, size), "fish")
    end
  end
  if (bestFish or 0) >= 60 and not bigFishRewardDone then
    bigFishRewardDone = true
    materials.metal = (materials.metal or 0) + 5
    addItem("bait", 2)
    toast.text = string.format("钓到 %.0fcm 大鱼！奖励 金属+5 鱼饵+2", bestFish)
    toast.t = 4.5
    if addJournal ~= nil then
      addJournal(string.format("钓到 %.0fcm 大鱼（金属+5 鱼饵+2）", bestFish), "goal")
    end
  end
  if checkFishCollection ~= nil then checkFishCollection() end
  return speciesRecord, overall
end

-- 钓鱼水花：在玩家前方水面溅起一簇水珠
function fishingSplash(strength)
  fishingSplashCount = (fishingSplashCount or 0) + 1
  if EmitParticles == nil or player == nil then return end
  local fx, fz = math.sin(player.yaw or 0), math.cos(player.yaw or 0)
  local x, z = player.x + fx * 2.4, player.z + fz * 2.4
  local y = (terrainHeight and terrainHeight(x, z)) or 0
  local s = strength or 0.5
  EmitParticles({
    pos = { x = x, y = y + 0.08, z = z },
    count = math.floor(8 + 10 * s), speedMin = 0.3, speedMax = 1.0 + s,
    vel = { x = 0, y = 1.0, z = 0 },
    lifeMin = 0.25, lifeMax = 0.6,
    sizeStart = 0.05, sizeEnd = 0.22,
    color = { r = 0.72, g = 0.86, b = 0.94, a = 0.65 },
    colorEnd = { r = 0.72, g = 0.86, b = 0.94, a = 0.0 },
    additive = true, gravity = 0.8,
  })
end

function finishFishing(force)
  if despawnFishingBobber ~= nil then despawnFishingBobber() end
  local usedBait = fishingBait == true
  local onDock = fishingDock == true
  fishingBait = false
  fishingDock = false
  local wChance, wSize = 0, 0
  if fishingConditionBonus ~= nil then
    wChance, wSize = fishingConditionBonus()
  end
  local chance = 0.78 + (rodCatchBonus and rodCatchBonus() or 0) +
                 (onDock and 0.10 or 0) + wChance
  if chance < 0.10 then chance = 0.10 end
  local success = force or usedBait or math.random() < chance
  fishingT = 0
  if success then
    if fishingSplash ~= nil then fishingSplash(1.0) end
    local name, size, sp = rollFish()
    local bonus = (rodSizeBonus and rodSizeBonus() or 0) +
                  (usedBait and 10 or 0) + (onDock and 4 or 0) + wSize
    if bonus > 0 then size = math.min(sp.max, size + bonus) end
    local n = 1 + ((math.random() < 0.30) and 1 or 0)
    if size >= 55 then n = n + 1 end
    if usedBait then n = n + 1 end
    addItem("rawfish", n)
    local speciesRecord, record = false, false
    if recordFish ~= nil then
      speciesRecord, record = recordFish(name, size, n)
    else
      fishCaught = (fishCaught or 0) + n
      if addDailyProgress ~= nil then addDailyProgress("fish", n) end
      if size > (bestFish or 0) then bestFish = size; record = true end
    end
    toast.text = string.format("钓到 %s %.0fcm x%d%s%s%s", name, size, n,
                               usedBait and "（鱼饵）" or "",
                               onDock and "（栈桥）" or "",
                               record and "  新纪录!" or "")
  else
    if fishingSplash ~= nil then fishingSplash(0.4) end
    toast.text = "鱼跑了……再试一次"
  end
  toast.t = 3.2
  if PlaySfx then PlaySfx("fish") end
end

-- 采摘浆果：2 份野浆果，约 12 秒后再生
function gatherBerries(index)
  local b = berryBushes and berryBushes[index] or nil
  if b == nil or not b.ready then return false end
  b.ready, b.t = false, 12.0
  for _, be in ipairs(b.berries or {}) do SetVisible(be, false) end
  addItem("berry", 2)
  if addDailyProgress ~= nil then addDailyProgress("forage", 2) end
  if PlaySfx then PlaySfx("pickup") end
  toast.text = "采到野浆果 x2（约 12 秒后再生）"
  toast.t = 2.6
  return true
end

-- 搜刮路边补给箱：少量金属/布料，概率出修理包/燃油罐；约 180 游戏分钟后再生
function lootRoadCache(index)
  local c = roadCaches and roadCaches[index] or nil
  if c == nil or not c.ready then return false end
  c.ready, c.t = false, 180.0
  SetVisible(c.ent, false)
  local got = {}
  local metal = 2 + math.random(0, 2)
  materials.metal = (materials.metal or 0) + metal
  got[#got + 1] = "金属+" .. metal
  local cloth = 1 + math.random(0, 1)
  materials.cloth = (materials.cloth or 0) + cloth
  got[#got + 1] = "布料+" .. cloth
  if math.random() < 0.25 then
    addItem("repairkit", 1)
    got[#got + 1] = "修理包+1"
  elseif math.random() < 0.20 then
    addItem("fuelcan", 1)
    got[#got + 1] = "燃油罐+1"
  end
  if PlaySfx then PlaySfx("pickup") end
  toast.text = "路边补给箱：" .. table.concat(got, " ")
  toast.t = 3.2
  if addDailyProgress ~= nil then addDailyProgress("forage", 2) end
  return true
end

-- 加油站自动售货机：花 1 金属买一份食物/水
function buyVendingSnack()
  if vendingMachine == nil then return false end
  if (materials.metal or 0) < 1 then
    toast.text = "自动售货机需要 1 金属"
    toast.t = 2.2
    return false
  end
  materials.metal = (materials.metal or 0) - 1
  local pool = { "canned", "water", "coffee" }
  local id = pool[math.random(1, #pool)]
  addItem(id, 1)
  if PlaySfx then PlaySfx("pickup") end
  toast.text = "自动售货机：" .. itemName(id) .. " x1（-1 金属）"
  toast.t = 2.8
  return true
end

-- 搜刮路边抛锚车：金属 + 可能燃油罐/燃料/修理包，一次性事件
function lootStrandedCar()
  if strandedCar == nil or strandedCar.looted then return false end
  strandedCar.looted = true
  local got = {}
  local metal = 3 + math.random(0, 2)
  materials.metal = (materials.metal or 0) + metal
  got[#got + 1] = "金属+" .. metal
  if math.random() < 0.5 then
    addItem("fuelcan", 1)
    got[#got + 1] = "燃油罐+1"
  else
    local before = fuel or 0
    fuel = math.min(fuelMax(), before + 8)
    if (fuel or 0) > before then
      got[#got + 1] = string.format("燃料+%.0fL", fuel - before)
    end
  end
  if math.random() < 0.4 then
    addItem("repairkit", 1)
    got[#got + 1] = "修理包+1"
  end
  if PlaySfx then PlaySfx("pickup") end
  toast.text = "抛锚车搜刮：" .. table.concat(got, " ")
  toast.t = 3.2
  if addJournal ~= nil then addJournal("搜刮路边抛锚车", "explore") end
  if despawnStrandedCar ~= nil then despawnStrandedCar() end
  return true
end

-- 帮助路边搭车客：给 5L 燃料，换回金属与一件随机补给
function helpHitchhiker()
  if hitchhiker == nil then return false end
  if (fuel or 0) < 5 then
    toast.text = "搭车客需要 5L 燃料才能继续赶路"
    toast.t = 3.0
    return false
  end
  fuel = math.max(0, (fuel or 0) - 5)
  local got = {}
  local metal = 3 + math.random(0, 2)
  materials.metal = (materials.metal or 0) + metal
  got[#got + 1] = "金属+" .. metal
  local r = math.random()
  if r < 0.4 then
    materials.electronics = (materials.electronics or 0) + 1
    got[#got + 1] = "电子件+1"
  elseif r < 0.7 then
    addItem("canned", 1)
    got[#got + 1] = "罐头+1"
  else
    addItem("repairkit", 1)
    got[#got + 1] = "修理包+1"
  end
  if PlaySfx then PlaySfx("pickup") end
  toast.text = "搭车客道谢：" .. table.concat(got, " ")
  toast.t = 3.4
  local hint = hitchhikerHintText()
  if hint ~= nil then
    toast.text = toast.text .. " · " .. hint
  end
  toast.t = 4.2
  if addJournal ~= nil then addJournal("帮助路边搭车客（燃料-5L）", "explore") end
  if despawnHitchhiker ~= nil then despawnHitchhiker() end
  return true
end

-- 搭车客指路：指出最近的未探索地点方向与距离（+Z 北 / +X 东）
function hitchhikerHintText(candidates)
  local list = candidates or NODES or {}
  local best, bestD = nil, math.huge
  for _, nd in ipairs(list) do
    if nd.generated and not (visitedPois and visitedPois[nd.id]) then
      local dx = (nd.x or 0) - (rv.x or 0)
      local dz = (nd.z or 0) - (rv.z or 0)
      local d = math.sqrt(dx * dx + dz * dz)
      if d < bestD then best, bestD = nd, d end
    end
  end
  if best == nil then return nil end
  local dx = (best.x or 0) - (rv.x or 0)
  local dz = (best.z or 0) - (rv.z or 0)
  local deg = math.deg(math.atan(dx, dz)) % 360
  local names = { "北", "东北", "东", "东南", "南", "西南", "西", "西北" }
  local dir = names[math.floor((deg + 22.5) / 45) % 8 + 1]
  return string.format("搭车客指路：往%s约 %.0fm 有个没去过的地方（%s）",
                       dir, bestD, best.name or "?")
end

-- 每次篝火烹饪消耗的燃料（游戏分钟）
COOK_FUEL_MIN = 10

local function spendCookFuel(minutes)
  campfireFuelMin = math.max(0, (campfireFuelMin or 0) -
                                (minutes or COOK_FUEL_MIN))
  if campfireFuelMin <= 0 and setCampfireLit ~= nil then
    setCampfireLit(false)
  end
end

-- 篝火可做的料理：是否满足材料
function campfireCan(kind)
  if kind == "fish" then return hasItem("rawfish", 1) end
  if kind == "water" then return hasItem("dirtywater", 1) end
  if kind == "stew" then
    return hasItem("rawfood", 1) and (inv.berry or 0) >= 2
  end
  if kind == "meal" then return hasItem("rawfood", 1) end
  if kind == "coffee" then
    return hasItem("coffee", 1) and (water or 0) >= 15
  end
  if kind == "smoke" then return (inv.rawfish or 0) >= 2 end
  if kind == "soup" then
    return hasItem("fish", 1) and (water or 0) >= 20
  end
  return false
end

-- 篝火烹饪菜单条目
function campfireRecipeList()
  return {
    { kind = "fish",   label = "烤鱼（生鱼 x1）" },
    { kind = "soup",   label = "鱼汤（烤鱼 x1 + 20L 水）" },
    { kind = "water",  label = "煮沸浑水（+15L 净水）" },
    { kind = "stew",   label = "浆果炖肉（生食材 + 野浆果 x2）" },
    { kind = "meal",   label = "热饭（生食材 x1）" },
    { kind = "coffee", label = "热咖啡（咖啡豆 + 15L 水）" },
    { kind = "smoke",  label = "烟熏鱼（生鱼 x2，耐存放）" },
  }
end

-- 按指定料理制作（材料不足返回 false）
function campfireCook(kind)
  if not campfireCan(kind) then
    toast.text = "材料不足——做不了这道"
    toast.t = 2.0
    return false
  end
  if kind == "fish" then
    takeItem("rawfish", 1)
    addItem("fish", 1)
    if addDailyProgress ~= nil then addDailyProgress("cook", 1) end
    toast.text = "篝火烤好一条鱼（烤鱼 +1）"
  elseif kind == "soup" then
    takeItem("fish", 1)
    useWater(20)
    addItem("fishsoup", 1)
    if addDailyProgress ~= nil then addDailyProgress("cook", 1) end
    toast.text = "篝火煮好一锅鱼汤（+1，饥饿 -35 口渴 -20）"
  elseif kind == "water" then
    takeItem("dirtywater", 1)
    addWater(15)
    toast.text = "煮沸一份浑水 — 水箱 +15L"
  elseif kind == "stew" then
    takeItem("rawfood", 1)
    takeItem("berry", 2)
    addItem("stew", 1)
    if addDailyProgress ~= nil then addDailyProgress("cook", 1) end
    toast.text = "篝火炖好一锅浆果炖肉（+1）"
  elseif kind == "meal" then
    takeItem("rawfood", 1)
    addItem("meal", 1)
    if addDailyProgress ~= nil then addDailyProgress("cook", 1) end
    toast.text = "篝火烤好一份野味（热饭 +1）"
  elseif kind == "coffee" then
    takeItem("coffee", 1)
    useWater(15)
    addItem("brew", 1)
    toast.text = "篝火煮好一壶热咖啡（+1，提神保暖）"
  elseif kind == "smoke" then
    takeItem("rawfish", 2)
    addItem("smokedfish", 1)
    if addDailyProgress ~= nil then addDailyProgress("cook", 1) end
    spendCookFuel(COOK_FUEL_MIN * 2)
    if PlaySfx then PlaySfx("cook") end
    toast.text = "篝火熏好一条鱼（烟熏鱼 +1，耐存放）"
    toast.t = 2.8
    return true
  end
  spendCookFuel()
  if PlaySfx then PlaySfx("cook") end
  toast.t = 2.6
  return true
end

-- choice 为空时沿用自动优先级（鱼→水→炖肉→热饭→咖啡）
function cookAtCampfire(choice)
  if campfireOn == false then
    toast.text = "篝火熄灭了——按 G 添柴后再做饭"
    toast.t = 2.6
    return false
  end
  if (campfireFuelMin or 0) <= 0 then
    toast.text = "篝火没有燃料了——按 G 添柴"
    toast.t = 2.6
    return false
  end
  if choice ~= nil then return campfireCook(choice) end
  for _, kind in ipairs({ "fish", "water", "stew", "meal", "coffee" }) do
    if campfireCan(kind) then return campfireCook(kind) end
  end
  toast.text = "没有生鱼/浑水/生食材 — 去水塘钓/取水或搜刮食物"
  toast.t = 2.2
  return false
end

function updateFishing(dt)
  -- 浮漂在水面上下浮动
  if fishingBobber ~= nil and fishingBobberBase ~= nil then
    fishingBobberT = (fishingBobberT or 0) + dt
    local b = fishingBobberBase
    SetPosition(fishingBobber, { x = b.x,
      y = b.y + math.sin(fishingBobberT * 3.0) * 0.03, z = b.z })
  end
  -- 咬钩窗口：1.2s 内按 E 可稳定上钩；错过则按原概率结算
  if fishingBite then
    if ActionDown("w") or ActionDown("a") or ActionDown("s") or ActionDown("d") then
      fishingBite, fishingBiteT = false, 0
      if despawnFishingBobber ~= nil then despawnFishingBobber() end
      toast.text = "你走动了，鱼跑了"
      toast.t = 2.2
      return
    end
    fishingBiteT = fishingBiteT - dt
    if fishingBiteT <= 0 then
      fishingBite, fishingBiteT = false, 0
      finishFishing()
    end
    return
  end
  if fishingT <= 0 then return end
  if ActionDown("w") or ActionDown("a") or ActionDown("s") or ActionDown("d") then
    fishingT = 0
    if despawnFishingBobber ~= nil then despawnFishingBobber() end
    toast.text = "收线了——钓鱼时别走动"
    toast.t = 2.0
    return
  end
  fishingT = fishingT - dt
  if fishingT <= 0 then
    fishingBite = true
    fishingBiteT = 1.2
    if despawnFishingBobber ~= nil then despawnFishingBobber() end
    if fishingSplash ~= nil then fishingSplash(0.5) end
    local fx, fz = math.sin(player.yaw or 0), math.cos(player.yaw or 0)
    SpawnFloatText(player.x + fx * 2.4, 0.9, player.z + fz * 2.4,
                   "咬钩!", false, 1.0, 0.92, 0.96, 1.0)
    toast.text = "咬钩了！按 E 收线"
    toast.t = 1.6
  end
end

-- 天气对作物生长的影响：雨天相当于浇水，雪天低温生长变慢
function plantGrowthScale(weatherValue)
  if weatherValue == "rain" then return 1.25 end
  if weatherValue == "snow" then return 0.50 end
  return 1.0
end

function updatePlants(dt)
  local gm = dt * SURVIVAL.timeScale
  local scale = plantGrowthScale(weather)
  for _, inst in pairs(instances) do
    if inst.cat.use == "plant" and inst.plant and not inst.plant.ready then
      inst.plant.growth = inst.plant.growth + gm * scale
      if inst.plant.growth >= SURVIVAL.plantMinutes then
        inst.plant.ready = true
      end
    end
  end
end

-- 种植箱：播种/浇水/收获
function plantPrompt(inst)
  if inst.plant == nil then
    return "播种", "种子 x1", hasItem("seed", 1)
  elseif inst.plant.ready or inst.plant.growth >= SURVIVAL.plantMinutes then
    return "收获", "生食材 x2 + 种子 x1", true
  else
    local pct = math.floor(math.min(1, inst.plant.growth / SURVIVAL.plantMinutes) * 100)
    return "浇水", string.format("生长 %d%%（水 0.5L）", pct), water >= 0.5
  end
end

function usePlant(inst)
  if inst.plant == nil then
    if takeItem("seed", 1) then
      inst.plant = { growth = 0, ready = false }
      toast.text = "种下了种子 — 需要水和时间"
      toast.t = 2.6
    else
      toast.text = "没有种子 — 搜刮路边的物资箱"
      toast.t = 2.4
    end
  elseif inst.plant.ready or inst.plant.growth >= SURVIVAL.plantMinutes then
    addItem("rawfood", 2)
    addItem("seed", 1)
    inst.plant = nil
    toast.text = "收获：生食材 x2  种子 x1"
    toast.t = 2.6
  elseif useWater(0.5) then
    inst.plant.growth = inst.plant.growth + 30
    toast.text = "浇水 — 生长加快"
    toast.t = 2.0
  else
    toast.text = "水箱没水了"
    toast.t = 2.0
  end
end

-- 车辆零件（车体 + 引擎/轮胎/电瓶）
function partValue(id)
  if id == "body" then return dur end
  return (parts and parts[id]) or 100
end

function setPartValue(id, v)
  v = math.max(0, math.min(VEH.partMax, v))
  if id == "body" then dur = v else parts[id] = v end
end

-- 现场维修：靠近房车按 R，消耗 1 个修理包修复最差的部件（野外应急）
function fieldRepair()
  -- 爆胎时优先换备胎：不需要修理包
  if rv.blowout and hasItem("sparetire", 1) then
    takeItem("sparetire", 1)
    if resetTirePressure ~= nil then resetTirePressure() end
    if clearBlowout ~= nil then clearBlowout() end
    if PlaySfx then PlaySfx("repair") end
    toast.text = "换上备胎——爆胎已处理（备胎 -1）"
    toast.t = 3.2
    if addJournal ~= nil then addJournal("换上备胎，爆胎已处理", "note") end
    return true
  end
  if not hasItem("repairkit", 1) then
    toast.text = "没有修理包——可在工作台制作，或找备胎按 R"
    toast.t = 2.8
    return false
  end
  local target, val = nil, 101
  for _, p in ipairs(REPAIR_PARTS or {}) do
    local v = partValue(p.id)
    if v < val then target, val = p.id, v end
  end
  if target == nil or val >= 99.5 then
    toast.text = "车况良好，不需要现场维修"
    toast.t = 2.0
    return false
  end
  takeItem("repairkit", 1)
  setPartValue(target, val + (ITEMS.repairkit and ITEMS.repairkit.amount or 35))
  if target == "tires" then
    if resetTirePressure ~= nil then resetTirePressure() end
    if clearBlowout ~= nil then clearBlowout() end
  end
  if PlaySfx then PlaySfx("repair") end
  local name = target
  for _, p in ipairs(REPAIR_PARTS or {}) do
    if p.id == target then name = p.name end
  end
  toast.text = string.format("现场维修 %s → %.0f%%（消耗修理包）", name,
                             partValue(target))
  toast.t = 3.0
  return true
end

function partRepairCost(id)
  if id == "engine" then return { metal = 2, electronics = 1 } end
  if id == "tires" then return { metal = 1, cloth = 1 } end
  if id == "battery" then return { electronics = 2, metal = 1 } end
  return VEH.repairCost
end

-- 货架升级：提高载重上限（每级 +300kg），最多 3 级
function upgradeCargo()
  upgrades.cargo = upgrades.cargo or 0
  if upgrades.cargo >= 3 then
    toast.text = "货架已经满级"
    toast.t = 1.8
    return false
  end
  local lv = upgrades.cargo
  local cost = { metal = 3 + lv * 2, wood = 2 + lv }
  if not canAfford(cost) then
    toast.text = "升级材料不足：" .. costText(cost)
    toast.t = 2.2
    return false
  end
  payCost(cost)
  upgrades.cargo = lv + 1
  if PlaySfx then PlaySfx("repair") end
  toast.text = string.format("货架升级到 Lv%d — 载重上限 %dkg",
                             upgrades.cargo, weightMax())
  toast.t = 2.6
  if checkUpgradeMilestone ~= nil then checkUpgradeMilestone() end
  return true
end

-- 车体装甲升级：每级减少 15% 碰撞耐久损失
function upgradeArmor()
  upgrades.armor = upgrades.armor or 0
  if upgrades.armor >= 3 then
    toast.text = "装甲已经满级"
    toast.t = 1.8
    return false
  end
  local lv = upgrades.armor
  local cost = { metal = 4 + lv * 2, cloth = 2 + lv }
  if not canAfford(cost) then
    toast.text = "升级材料不足：" .. costText(cost)
    toast.t = 2.2
    return false
  end
  payCost(cost)
  upgrades.armor = lv + 1
  if PlaySfx then PlaySfx("repair") end
  toast.text = string.format("车体装甲升级到 Lv%d — 碰撞损失 -%d%%",
                             upgrades.armor, upgrades.armor * 15)
  toast.t = 2.6
  return true
end

-- 水箱容量：基础 120L + 每级 30L
function waterMax()
  return SURVIVAL.waterMax + (upgrades.water or 0) * (SURVIVAL.waterPerLevel or 30)
end

function upgradeWater()
  upgrades.water = upgrades.water or 0
  if upgrades.water >= 3 then
    toast.text = "水箱已经满级"
    toast.t = 1.8
    return false
  end
  local lv = upgrades.water
  local cost = { metal = 2 + lv, cloth = 2 + lv }
  if not canAfford(cost) then
    toast.text = "升级材料不足：" .. costText(cost)
    toast.t = 2.2
    return false
  end
  payCost(cost)
  upgrades.water = lv + 1
  water = math.min(waterMax(), (water or 0) + 30)
  if PlaySfx then PlaySfx("repair") end
  toast.text = string.format("水箱升级到 Lv%d — 容量 %dL",
                             upgrades.water, waterMax())
  toast.t = 2.6
  if checkUpgradeMilestone ~= nil then checkUpgradeMilestone() end
  return true
end

function upgradeFuel()
  upgrades.fuel = upgrades.fuel or 0
  if upgrades.fuel >= 3 then
    toast.text = "油箱已经满级"
    toast.t = 1.8
    return false
  end
  local lv = upgrades.fuel
  local cost = { metal = 3 + lv, electronics = 1 + lv }
  if not canAfford(cost) then
    toast.text = "升级材料不足：" .. costText(cost)
    toast.t = 2.2
    return false
  end
  payCost(cost)
  upgrades.fuel = lv + 1
  fuel = math.min(fuelMax(), (fuel or 0) + 10)
  if PlaySfx then PlaySfx("repair") end
  toast.text = string.format("油箱升级到 Lv%d — 容量 %dL",
                             upgrades.fuel, fuelMax())
  toast.t = 2.6
  if checkUpgradeMilestone ~= nil then checkUpgradeMilestone() end
  return true
end

function workbenchTarget()
  local best = { id = "body", name = "车体", value = dur }
  if (parts.engine or 100) < best.value then
    best = { id = "engine", name = "引擎", value = parts.engine }
  end
  if (parts.tires or 100) < best.value then
    best = { id = "tires", name = "轮胎", value = parts.tires }
  end
  if (parts.battery or 100) < best.value then
    best = { id = "battery", name = "电瓶", value = parts.battery }
  end
  return best
end

-- 灶台配方是否可做
function recipeAvailable(r)
  if not powerOk() then return false, "没有电力" end
  if water < (r.water or 0) then return false, "水箱没水" end
  for id, n in pairs(r.cost or {}) do
    if not hasItem(id, n) then return false, "缺少" .. itemName(id) end
  end
  return true, ""
end

function partUpgradeCost(id)
  local lv = upgrades[id] or 0
  if id == "engine" then
    return { metal = 4 + lv * 2, electronics = 3 + lv }
  elseif id == "tires" then
    return { metal = 3 + lv, cloth = 2 + lv }
  end
  return nil
end

function repairPart(id)
  local value = partValue(id)
  if value >= VEH.partMax then
    local lv = upgrades[id]
    if lv ~= nil and lv < 3 then
      local cost = partUpgradeCost(id)
      if not canAfford(cost) then
        toast.text = "升级材料不足：" .. costText(cost)
        toast.t = 2.2
        return false
      end
      payCost(cost)
      upgrades[id] = lv + 1
      if id == "tires" and resetTirePressure ~= nil then resetTirePressure() end
      if id == "tires" and clearBlowout ~= nil then clearBlowout() end
      if addDailyProgress ~= nil then addDailyProgress("repair", 1) end
      if PlaySfx then PlaySfx("repair") end
      toast.text = string.format("%s 升级到 Lv%d", id == "engine" and "引擎" or "轮胎",
                                 upgrades[id]) ..
                   (id == "tires" and "（胎压 2.4bar）" or "")
      toast.t = 2.6
      if checkUpgradeMilestone ~= nil then checkUpgradeMilestone() end
      return true
    end
    toast.text = lv ~= nil and "已经满级" or "已经修好了"
    toast.t = 1.8
    return false
  end
  local partName = id
  for _, p in ipairs(REPAIR_PARTS) do if p.id == id then partName = p.name end end
  if hasItem("repairkit", 1) then
    takeItem("repairkit", 1)
    setPartValue(id, value + ITEMS.repairkit.amount)
    if id == "tires" and resetTirePressure ~= nil then resetTirePressure() end
    if id == "tires" and clearBlowout ~= nil then clearBlowout() end
    if PlaySfx then PlaySfx("repair") end
    toast.text = "修理包修好 " .. partName .. " — " .. string.format("%.0f%%", partValue(id)) ..
                 (id == "tires" and "（胎压 2.4bar）" or "")
  else
    local cost = partRepairCost(id)
    if not canAfford(cost) then
      toast.text = "材料不足：" .. costText(cost)
      toast.t = 2.0
      return false
    end
    payCost(cost)
    setPartValue(id, value + VEH.repairAmt)
    if id == "tires" and resetTirePressure ~= nil then resetTirePressure() end
    if id == "tires" and clearBlowout ~= nil then clearBlowout() end
    if PlaySfx then PlaySfx("repair") end
    toast.text = "修好 " .. partName .. " — " .. string.format("%.0f%%", partValue(id)) ..
                 (id == "tires" and "（胎压 2.4bar）" or "")
  end
  if addDailyProgress ~= nil then addDailyProgress("repair", 1) end
  toast.t = 2.4
  return true
end

-- 营地洗车：积灰 >=50% 时消耗 5L 水清洗
function washAtCampWithWater()
  if (rv.dirt or 0) < 0.5 then return false, 0, nil end
  if (water or 0) < 5 then return false, 5, "水箱水不足（需 5L）" end
  useWater(5)
  rv.dirt = 0
  if PlaySfx then PlaySfx("drink") end
  return true, 5, "用 5L 水洗车"
end

-- 诊所包扎：生命 <70% 时花 1 布料 + 2L 水，恢复 +30 生命
function clinicTreatAvailable()
  return (needs.health or 100) < 70 and (materials.cloth or 0) >= 1
end

function serviceClinicAtRv()
  if (needs.health or 100) >= 70 then
    return false, 0, "生命状态良好"
  end
  if (materials.cloth or 0) < 1 then
    return false, 1, "需要 1 布料"
  end
  if (water or 0) < 2 then
    return false, 0, "需要 2L 水"
  end
  materials.cloth = (materials.cloth or 0) - 1
  useWater(2)
  healPlayer(30)
  if addJournal ~= nil then
    addJournal("在诊所包扎伤口（布料-1 生命+30）", "explore")
  end
  return true, 1, "诊所包扎：生命 +30"
end

-- 加油站快速维修：最差部件 <70% 时修 +25%，按缺失每 25% 收 1 金属
function repairServiceCost()
  local worst, val = nil, 101
  for _, p in ipairs(REPAIR_PARTS or {}) do
    local v = partValue(p.id)
    if v < val then worst, val = p.id, v end
  end
  if worst == nil or val >= 70 then return 0 end
  return math.max(1, math.ceil((100 - val) / 25))
end

function serviceRepairAtGasstop()
  local cost = repairServiceCost()
  if cost <= 0 then return true, 0, "车况尚可" end
  if (materials.metal or 0) < cost then
    return false, cost, string.format("快速维修需要 %d 金属", cost)
  end
  local worst, val = nil, 101
  for _, p in ipairs(REPAIR_PARTS or {}) do
    local v = partValue(p.id)
    if v < val then worst, val = p.id, v end
  end
  if worst == nil then return true, 0, "车况尚可" end
  materials.metal = (materials.metal or 0) - cost
  setPartValue(worst, math.min(100, val + 25))
  if worst == "tires" then
    if resetTirePressure ~= nil then resetTirePressure() end
    if clearBlowout ~= nil then clearBlowout() end
  end
  if PlaySfx then PlaySfx("repair") end
  return true, cost, string.format("快速维修（%d 金属）", cost)
end

-- 维修当前最差的部件（联机工作台命令复用；满状态时不触发升级）
function repairWorstPart()
  local target, val = nil, 101
  for _, p in ipairs(REPAIR_PARTS or {}) do
    local v = partValue(p.id)
    if v < val then target, val = p.id, v end
  end
  if target == nil or val >= 99.5 then
    toast.text = "车况良好，不需要维修"
    toast.t = 2.0
    return false
  end
  return repairPart(target)
end

function cookRecipe(id)
  local r = nil
  for _, rec in ipairs(RECIPES) do if rec.id == id then r = rec end end
  if r == nil then return false end
  local ok, why = recipeAvailable(r)
  if not ok then
    toast.text = "做不了：" .. why
    toast.t = 2.2
    return false
  end
  for iid, n in pairs(r.cost or {}) do takeItem(iid, n) end
  useWater(r.water or 0)
  addItem(r.out, 1)
  if PlaySfx then PlaySfx("cook") end
  if addDailyProgress ~= nil then addDailyProgress("cook", 1) end
  toast.text = "做好 " .. r.name .. "（背包 +1）"
  toast.t = 2.4
  return true
end

-- 当前家具能做什么（HUD/交互提示共用）
function stationPrompt(inst)
  if inst == nil or inst.cat == nil then return nil end
  local cat = inst.cat
  local use = cat.use
  if use == nil then return nil end
  if use == "bed" then
    return "睡觉", "到第二天 07:00，恢复体力", true
  elseif use == "stove" then
    return "打开灶台", "1 热饭  2 鱼汤  3 大餐", true
  elseif use == "fridge" then
    if hasItem("feast", 1) then return "吃大餐", "饥饿 -65 生命 +10", true end
    if hasItem("fishsoup", 1) then return "喝鱼汤", "饥饿 -35 口渴 -20", true end
    if hasItem("meal", 1) then return "吃热饭", "饥饿 -42", true end
    if hasItem("fish", 1) then return "吃烤鱼", "饥饿 -30 生命 +5", true end
    if hasItem("smokedfish", 1) then return "吃烟熏鱼", "饥饿 -26 生命 +4", true end
    if hasItem("canned", 1) then return "吃罐头", "饥饿 -22", true end
    if hasItem("rawfood", 1) then return "吃生食材", "饥饿 -8，会掉血", true end
    return "找吃的", "冰箱空了", false
  elseif use == "sink" then
    if water >= 0.5 then return "喝水", "口渴 -30（水箱）", true end
    if hasItem("water", 1) then return "开瓶装水", "口渴 -28", true end
    return "喝水", "没有水了", false
  elseif use == "coffee" then
    if hasItem("brew", 1) then return "喝热咖啡", "体力 +30", true end
    if hasItem("coffee", 1) and water >= SURVIVAL.coffeeWater and powerOk() then
      return "煮咖啡", "咖啡豆 x1 + 水 0.3L", true
    end
    return "煮咖啡", hasItem("coffee", 1) and "需要电力/水" or "没有咖啡豆", false
  elseif use == "bathroom" then
    return "洗漱", "水 3L，卫生 +45", water >= SURVIVAL.bathWater
  elseif use == "workbench" then
    return "打开工作台", "1 车体  2 引擎  3 轮胎  4 电瓶", true
  elseif use == "storage" then
    return "查看物资", "随身库存", true
  elseif use == "med" then
    if needs.health >= 100 then return "包扎", "生命已满", false end
    return "包扎", "布料 x1，生命 +20", (materials.cloth or 0) >= 1
  elseif use == "tank" then
    if hasItem("water", 1) and water < waterMax() then
      return "倒入净水", "瓶装水 x1 → 水箱 +30L", true
    end
    return "净水箱", string.format("%d / %d L", math.floor(water), waterMax()), false
  elseif use == "plant" then
    local verb, detail, ok = plantPrompt(inst)
    return verb, detail, ok
  elseif use == "heater" then
    return inst.on ~= false and "关掉取暖器" or "打开取暖器", "耗电 0.8kW", true
  elseif use == "tv" then
    return inst.on ~= false and "关掉电视" or "打开电视", "耗电 0.2kW", true
  elseif use == "generator" then
    return inst.on ~= false and "关掉发电机" or "启动发电机", "发电 2.0kW，有噪音", true
  elseif use == "gun" then
    return "检查武器", "夜里有感染者时左键射击", true
  elseif use == "turret" then
    return "检查炮塔", "自动防御 16m", true
  end
  return nil
end

function useStation(inst)
  local use = inst and inst.cat and inst.cat.use
  if use == nil then return false end
  if use == "bed" then
    sleepInBed()
  elseif use == "stove" then
    cookRecipe("meal")
  elseif use == "fridge" then
    if hasItem("feast", 1) then useItem("feast")
    elseif hasItem("fishsoup", 1) then useItem("fishsoup")
    elseif hasItem("meal", 1) then useItem("meal")
    elseif hasItem("fish", 1) then useItem("fish")
    elseif hasItem("smokedfish", 1) then useItem("smokedfish")
    elseif hasItem("canned", 1) then useItem("canned")
    elseif hasItem("rawfood", 1) then useItem("rawfood")
    else toast.text = "冰箱空了"; toast.t = 2.0 end
  elseif use == "sink" then
    if water >= 0.5 then
      useWater(0.5)
      needs.thirst = math.min(100, needs.thirst + SURVIVAL.drink)
      if PlaySfx then PlaySfx("drink") end
      toast.text = "喝了一肚子水 — 口渴 -30"
    elseif hasItem("water", 1) then
      useItem("water")
    else
      toast.text = "水箱和背包都没有水"
    end
    toast.t = 2.4
  elseif use == "coffee" then
    if hasItem("brew", 1) then
      useItem("brew")
    elseif hasItem("coffee", 1) and water >= SURVIVAL.coffeeWater and powerOk() then
      takeItem("coffee", 1)
      useWater(SURVIVAL.coffeeWater)
      addItem("brew", 1)
      if PlaySfx then PlaySfx("brew") end
      toast.text = "煮好一杯热咖啡（背包 +1）"
      toast.t = 2.4
    else
      toast.text = "需要咖啡豆、水和电"
      toast.t = 2.2
    end
  elseif use == "bathroom" then
    if useWater(SURVIVAL.bathWater) then
      needs.hygiene = math.min(100, needs.hygiene + 45)
      needs.energy = math.min(100, needs.energy + 5)
      if PlaySfx then PlaySfx("drink") end
      toast.text = "洗漱完毕 — 精神多了"
    else
      toast.text = "水箱没水了"
    end
    toast.t = 2.4
  elseif use == "workbench" then
    local target = workbenchTarget()
    if target.value >= VEH.partMax then
      toast.text = "车体、引擎、轮胎、电瓶都正常"
    elseif hasItem("repairkit", 1) then
      takeItem("repairkit", 1)
      setPartValue(target.id, target.value + ITEMS.repairkit.amount)
      if PlaySfx then PlaySfx("repair") end
      toast.text = string.format("修理包修好 %s — %.0f%%",
                                 target.name, partValue(target.id))
    else
      local cost = partRepairCost(target.id)
      if canAfford(cost) then
        payCost(cost)
        setPartValue(target.id, target.value + VEH.repairAmt)
        if PlaySfx then PlaySfx("repair") end
        toast.text = string.format("修好 %s +%d — %.0f%%",
                                   target.name, VEH.repairAmt, partValue(target.id))
      else
        toast.text = "材料不足：" .. costText(cost)
      end
    end
    toast.t = 2.8
  elseif use == "storage" then
    local parts = {}
    for _, id in ipairs(ITEM_ORDER) do
      if (inv[id] or 0) > 0 then parts[#parts + 1] = itemName(id) .. "x" .. inv[id] end
    end
    toast.text = #parts > 0 and table.concat(parts, "  ") or "背包空空如也"
    toast.t = 4.0
  elseif use == "med" then
    if needs.health >= 100 then
      toast.text = "生命已满"
    elseif (materials.cloth or 0) >= 1 then
      materials.cloth = materials.cloth - 1
      healPlayer(20)
      toast.text = "用布料包扎 — 生命 +20"
    else
      toast.text = "没有布料"
    end
    toast.t = 2.4
  elseif use == "tank" then
    if hasItem("water", 1) and water < waterMax() then
      takeItem("water", 1)
      addWater(30)
      if PlaySfx then PlaySfx("drink") end
      toast.text = string.format("倒入净水 — 水箱 %d L", math.floor(water))
    else
      toast.text = string.format("水箱 %d / %d L", math.floor(water), waterMax())
    end
    toast.t = 2.4
  elseif use == "plant" then
    usePlant(inst)
  elseif use == "heater" then
    local wasOn = inst.on ~= false
    inst.on = not wasOn
    recomputeStats()
    toast.text = inst.on and "取暖器开着 — 车里暖和了" or "取暖器关了"
    toast.t = 2.0
  elseif use == "tv" then
    local wasOn = inst.on ~= false
    inst.on = not wasOn
    recomputeStats()
    toast.text = inst.on and "电视开着 — 放松一下" or "电视关了"
    toast.t = 2.0
  elseif use == "generator" then
    local wasOn = inst.on ~= false
    inst.on = not wasOn
    recomputeStats()
    toast.text = inst.on and "发电机启动 — 轰轰作响" or "发电机停了"
    toast.t = 2.0
  elseif use == "gun" then
    toast.text = "步枪就绪 — 夜里有感染者靠近时左键射击"
    toast.t = 2.6
  elseif use == "turret" then
    toast.text = "炮塔自动防御已启用（需要通电）"
    toast.t = 2.4
  end
  return true
end
