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
  upgrades = { engine = 0, tires = 0, cargo = 0, rod = 0 }
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
        fuel = math.min(VEH.tank, fuel + v)
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
          fuel = math.min(VEH.tank, fuel + v)
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
  water = math.max(0, math.min(SURVIVAL.waterMax, water + amount))
end

-- 燃油罐：靠近房车按 E 加注（10L，受油箱上限约束）
function useFuelCan()
  local it = ITEMS.fuelcan
  if it == nil or not takeItem("fuelcan", 1) then return false end
  local before = fuel or 0
  fuel = math.min(VEH.tank, before + (it.amount or 10))
  local got = fuel - before
  if got <= 0.01 then
    addItem("fuelcan", 1)  -- 油箱已满，不浪费
    toast.text = "油箱已经满了"
    toast.t = 2.0
    return false
  end
  if PlaySfx then PlaySfx("pickup") end
  toast.text = string.format("加注 %0.0fL 燃料（%.0f / %d L）", got, fuel, VEH.tank)
  toast.t = 2.4
  return true
end

-- 直接吃/喝背包里的东西
function useItem(id)
  local it = ITEMS[id]
  if it == nil or not takeItem(id, 1) then return false end
  needs.hunger = math.min(100, needs.hunger + (it.hunger or 0))
  needs.thirst = math.min(100, needs.thirst + (it.thirst or 0))
  needs.energy = math.min(100, needs.energy + (it.energy or 0))
  needs.health = math.max(0, math.min(100, needs.health + (it.health or 0)))
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
    t = 5.0,
  }
  toast.text = string.format("睡到第 %d 天 07:00 — 体力恢复", day)
  toast.t = 3.5
end

-- 时间/需求主循环
function updateSurvival(dt)
  if hurtT > 0 then hurtT = math.max(0, hurtT - dt) end

  local gm = dt * SURVIVAL.timeScale
  advanceGameMinutes(gm)

  needs.hunger = math.max(0, needs.hunger - SURVIVAL.hungerRate * gm)
  needs.thirst = math.max(0, needs.thirst - SURVIVAL.thirstRate * gm)
  needs.hygiene = math.max(0, needs.hygiene - SURVIVAL.hygieneRate * gm)
  local sprinting = onFoot and not buildMode and ActionDown("w") and ActionDown("shift")
  needs.energy = math.max(0, needs.energy -
    (SURVIVAL.energyRate + (sprinting and SURVIVAL.sprintExtra or 0)) * gm)
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
  if weather == "rain" and water < SURVIVAL.waterMax then
    water = math.min(SURVIVAL.waterMax, water + 0.04 * gm)
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
  return string.format("钓鱼纪录 %.0fcm · %d 条", bestFish or 0, fishCaught or 0)
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

function startFishing()
  if fishingT > 0 then return end
  fishingTotal = 5 + math.random() * 4
  fishingT = fishingTotal
  fishingBait = false
  fishingDock = (nearDock ~= nil) and nearDock(player.x, player.z) or false
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

function finishFishing(force)
  local usedBait = fishingBait == true
  local onDock = fishingDock == true
  fishingBait = false
  fishingDock = false
  local chance = 0.78 + (rodCatchBonus and rodCatchBonus() or 0) +
                 (onDock and 0.10 or 0)
  local success = force or usedBait or math.random() < chance
  fishingT = 0
  if success then
    local name, size, sp = rollFish()
    local bonus = (rodSizeBonus and rodSizeBonus() or 0) +
                  (usedBait and 10 or 0) + (onDock and 4 or 0)
    if bonus > 0 then size = math.min(sp.max, size + bonus) end
    local n = 1 + ((math.random() < 0.30) and 1 or 0)
    if size >= 55 then n = n + 1 end
    if usedBait then n = n + 1 end
    addItem("rawfish", n)
    fishCaught = (fishCaught or 0) + n
    if addDailyProgress ~= nil then addDailyProgress("fish", n) end
    local record = false
    if size > (bestFish or 0) then
      bestFish = size
      record = true
    end
    toast.text = string.format("钓到 %s %.0fcm x%d%s%s%s", name, size, n,
                               usedBait and "（鱼饵）" or "",
                               onDock and "（栈桥）" or "",
                               record and "  新纪录!" or "")
  else
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
  if fishingT <= 0 then return end
  if ActionDown("w") or ActionDown("a") or ActionDown("s") or ActionDown("d") then
    fishingT = 0
    toast.text = "收线了——钓鱼时别走动"
    toast.t = 2.0
    return
  end
  fishingT = fishingT - dt
  if fishingT <= 0 then finishFishing() end
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
    if hasItem("water", 1) and water < SURVIVAL.waterMax then
      return "倒入净水", "瓶装水 x1 → 水箱 +30L", true
    end
    return "净水箱", string.format("%d / %d L", math.floor(water), SURVIVAL.waterMax), false
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
    if hasItem("water", 1) and water < SURVIVAL.waterMax then
      takeItem("water", 1)
      addWater(30)
      if PlaySfx then PlaySfx("drink") end
      toast.text = string.format("倒入净水 — 水箱 %d L", math.floor(water))
    else
      toast.text = string.format("水箱 %d / %d L", math.floor(water), SURVIVAL.waterMax)
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
