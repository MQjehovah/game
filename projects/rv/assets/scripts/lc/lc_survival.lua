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
  lastWarn = {}
  hurtT = 0
end

-- 真实昼夜光照：直接改场景 Sun 的方向光强度/颜色
function initDayNight()
  sunEnt = FindNamedEntity("Sun")
  lastSunClock = -999
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
  if day ~= before and LC and LC.flow then
    LC.flow.emit("day_advanced")
  end
end

function addItem(id, n)
  if ITEMS[id] == nil then return end
  inv[id] = (inv[id] or 0) + (n or 1)
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
  local gen = (stats.powerGen or 0) - (stats.powerDraw or 0)
  return gen >= -0.001 or (stats.battery or 0) > 0
end

function useWater(amount)
  if water < amount then return false end
  water = water - amount
  return true
end

function addWater(amount)
  water = math.max(0, math.min(SURVIVAL.waterMax, water + amount))
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

function sleepInBed()
  -- 07:00 之前睡 = 当天早上；之后睡 = 第二天早上
  if clock >= 7 * 60 then day = day + 1 end
  clock = 7 * 60
  needs.energy = 100
  needs.hunger = math.max(0, needs.hunger - 20)
  needs.thirst = math.max(0, needs.thirst - 32)
  needs.hygiene = math.max(0, needs.hygiene - 10)
  needs.health = math.min(100, needs.health + 8)
  if LC and LC.flow then LC.flow.emit("day_advanced") end
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

  if needs.hunger <= 0 or needs.thirst <= 0 then
    needs.health = math.max(0, needs.health - SURVIVAL.healthDrain * gm)
  elseif needs.hunger > 45 and needs.thirst > 45 then
    needs.health = math.min(100, needs.health + SURVIVAL.healthRegen * gm)
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

  updatePlants(dt)
end

-- 钓鱼：水塘边按 E 抛竿，等待 5-9 秒
function startFishing()
  if fishingT > 0 then return end
  fishingTotal = 5 + math.random() * 4
  fishingT = fishingTotal
  toast.text = "抛竿了——等鱼上钩（别走开）"
  toast.t = 2.2
end

function finishFishing(force)
  local success = force or math.random() < 0.78
  fishingT = 0
  if success then
    local n = 1 + ((math.random() < 0.30) and 1 or 0)
    addItem("fish", n)
    toast.text = string.format("钓到鱼！烤鱼 x%d（背包里按 9 吃）", n)
  else
    toast.text = "鱼跑了……再试一次"
  end
  toast.t = 3.2
  if PlaySfx then PlaySfx("fish") end
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

-- 当前家具能做什么（HUD/交互提示共用）
function stationPrompt(inst)
  if inst == nil or inst.cat == nil then return nil end
  local cat = inst.cat
  local use = cat.use
  if use == nil then return nil end
  if use == "bed" then
    return "睡觉", "到第二天 07:00，恢复体力", true
  elseif use == "stove" then
    local ok = hasItem("rawfood", 1) and water >= SURVIVAL.cookWater and powerOk()
    local detail = "生食材 x1 + 水 0.5L"
    if not powerOk() then detail = "需要电力" end
    return "做饭", detail, ok
  elseif use == "fridge" then
    if hasItem("meal", 1) then return "吃热饭", "饥饿 -42", true end
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
    local target = workbenchTarget()
    if target.value >= VEH.partMax then return "检修", "全部正常", false end
    if hasItem("repairkit", 1) then
      return "修理" .. target.name, string.format("修理包 x1（%s %.0f%%）",
             target.name, target.value), true
    end
    local cost = partRepairCost(target.id)
    return "修理" .. target.name,
           string.format("%s %.0f%% — %s", target.name, target.value, costText(cost)),
           canAfford(cost)
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
    if not powerOk() then
      toast.text = "没有电力 — 装太阳能板或启动发电机"
    elseif not hasItem("rawfood", 1) then
      toast.text = "没有生食材 — 搜刮物资箱，或种菜收获"
    elseif not useWater(SURVIVAL.cookWater) then
      toast.text = "水箱没水了"
    else
      takeItem("rawfood", 1)
      addItem("meal", 1)
      if PlaySfx then PlaySfx("cook") end
      toast.text = "做好一份热饭（背包 +1）"
    end
    toast.t = 2.6
  elseif use == "fridge" then
    if hasItem("meal", 1) then useItem("meal")
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
