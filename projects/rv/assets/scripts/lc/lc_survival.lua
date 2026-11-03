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

-- 每帧推进电池（游戏时间：1 现实秒 = timeScale 游戏分钟）
function updatePower(dt)
  local aux = auxPowerDraw(interiorLightsOn, (#rvHeadlights or 0) > 0)
  local net = (stats.powerGen or 0) - (stats.powerDraw or 0) - aux
  local hours = (dt or 0) * SURVIVAL.timeScale / 60
  batteryCharge = batteryStep(batteryCharge or 0, net, hours, batteryCapacity())
end

function useWater(amount)
  if water < amount then return false end
  water = water - amount
  return true
end

function addWater(amount)
  water = math.max(0, math.min(waterMax(), water + amount))
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

-- 直接吃/喝背包里的东西
function useItem(id)
  local it = ITEMS[id]
  if it == nil then return false end
  if it.kind == "gear" then
    toast.text = it.name .. "：已自动生效（雨雪天提高抓地）"
    toast.t = 2.2
    return false
  end
  if not takeItem(id, 1) then return false end
  needs.hunger = math.min(100, needs.hunger + (it.hunger or 0))
  needs.thirst = math.min(100, needs.thirst + (it.thirst or 0))
  needs.energy = math.min(100, needs.energy + (it.energy or 0))
  needs.health = math.max(0, math.min(100, needs.health + (it.health or 0)))
  if id == "brew" then
    nightDriveMin = math.max(0, (nightDriveMin or 0) - 60)
  end
  if PlaySfx then PlaySfx(it.kind == "drink" and "drink" or "pickup") end
  local parts = {}
  if it.hunger then parts[#parts + 1] = string.format("饥饿 %+.0f", it.hunger) end
  if it.thirst then parts[#parts + 1] = string.format("口渴 %+.0f", it.thirst) end
  if it.energy then parts[#parts + 1] = string.format("体力 %+.0f", it.energy) end
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
  if not (isNight ~= nil and isNight()) then return false end
  local dx, dz = player.x - campfire.x, player.z - campfire.z
  return dx * dx + dz * dz < 3.4 * 3.4
end

-- 卫生对睡眠恢复的影响：干净时 100%，脏到 0 只剩 60%
function hygieneEnergyFactor(h)
  local v = math.max(0, math.min(100, h or 0)) / 100
  return 0.6 + 0.4 * v
end

function sleepInBed()
  -- 07:00 之前睡 = 当天早上；之后睡 = 第二天早上
  if clock >= 7 * 60 then day = day + 1 end
  clock = 7 * 60
  nightDriveMin = 0
  local atCamp = (atNode == "camp")
  local factor = hygieneEnergyFactor(needs.hygiene) + (atCamp and 0.10 or 0)
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
    t = 5.0,
  }
  toast.text = string.format("睡到第 %d 天 07:00 — 体力恢复", day)
  toast.t = 3.5
  if autoSave ~= nil then autoSave() end
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

function updateRadioBroadcast(gm)
  if not radioOn then return false end
  radioBroadcastT = (radioBroadcastT or 0) + (gm or 0)
  if radioBroadcastT < (radioBroadcastNext or 120) then return false end
  radioBroadcastT = 0
  radioBroadcastNext = 90 + math.random() * 60
  local list = radioBroadcastPool()
  local warn = radioWeatherWarning()
  local line
  if warn ~= nil and math.random() < 0.5 then
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

-- 夜间驾驶疲劳：连续开夜车超过 120 游戏分钟后体力消耗加快
function nightFatigueExtra(minutes)
  local m = minutes or 0
  if m <= 120 then return 0 end
  return math.min(0.020, (m - 120) * 0.0002)
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

-- 雪天寒冷：没开取暖器时体力消耗更快
function coldEnergyExtra(weatherValue, heaterOn)
  if weatherValue == "snow" and not heaterOn then
    return SURVIVAL.coldExtra or 0.020
  end
  return 0
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

-- 时间/需求主循环
function updateSurvival(dt)
  if hurtT > 0 then hurtT = math.max(0, hurtT - dt) end

  local gm = dt * SURVIVAL.timeScale
  advanceGameMinutes(gm)
  if updateRadioBroadcast ~= nil then updateRadioBroadcast(gm) end

  needs.hunger = math.max(0, needs.hunger - SURVIVAL.hungerRate * gm)
  needs.thirst = math.max(0, needs.thirst - SURVIVAL.thirstRate * gm)
  needs.hygiene = math.max(0, needs.hygiene - SURVIVAL.hygieneRate * gm)
  local sprinting = onFoot and not buildMode and ActionDown("w") and ActionDown("shift")
  local coldExtra = coldEnergyExtra(weather, (stats and stats.heaterOn) or false)
  local nightDriving = phase == "driving" and (isNight ~= nil and isNight()) and
                       math.abs(rv.speed or 0) > 1.0
  if nightDriving then
    nightDriveMin = (nightDriveMin or 0) + gm
  elseif phase ~= "driving" then
    nightDriveMin = math.max(0, (nightDriveMin or 0) - gm * 0.5)
  end
  local fatigue = nightFatigueExtra(nightDriveMin)
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
  -- 夜里坐在营地篝火旁：缓慢回体力
  if campfireRestAvailable ~= nil and campfireRestAvailable() then
    needs.energy = math.min(100, needs.energy +
      (SURVIVAL.campfireRest or 0.10) * gm)
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
function rollFish()
  local total = 0
  for _, s in ipairs(FISH_SPECIES) do total = total + s.w end
  local r = math.random() * total
  local pick = FISH_SPECIES[1]
  for _, s in ipairs(FISH_SPECIES) do
    r = r - s.w
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

function cookAtCampfire()
  if takeItem("rawfish", 1) then
    addItem("fish", 1)
    if PlaySfx then PlaySfx("cook") end
    if addDailyProgress ~= nil then addDailyProgress("cook", 1) end
    toast.text = "篝火烤好一条鱼（烤鱼 +1）"
    toast.t = 2.4
    return true
  end
  -- 没有生鱼就煮浑水：1 份浑水 -> +15L 净水
  if takeItem("dirtywater", 1) then
    addWater(15)
    if PlaySfx then PlaySfx("cook") end
    toast.text = "煮沸一份浑水 — 水箱 +15L"
    toast.t = 2.4
    return true
  end
  toast.text = "没有生鱼/浑水 — 去水塘钓/取水"
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

function updatePlants(dt)
  local gm = dt * SURVIVAL.timeScale
  for _, inst in pairs(instances) do
    if inst.cat.use == "plant" and inst.plant and not inst.plant.ready then
      inst.plant.growth = inst.plant.growth + gm
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
  if not hasItem("repairkit", 1) then
    toast.text = "没有修理包——在工作台用 金属2+布料1 制作"
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
