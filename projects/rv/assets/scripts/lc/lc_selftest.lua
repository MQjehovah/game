-- lc_selftest.lua
-- 无头验证钩子：scriptBaseDir 下放 selftest_on.txt 时执行一条完整系统自检

local function ok(results, name, cond, extra)
  results[#results + 1] = (cond and "PASS  " or "FAIL  ") .. name
                             .. (extra and ("  [" .. extra .. "]") or "")
  return cond
end

function runSelfTest()
  local r = {}
  selftestActive = true
  local stove = hasModule("stove")
  local sink = hasModule("sink")
  local bed = hasModule("bed_double") or hasModule("bed_single")
  local coffee = hasModule("coffee")
  local plant = hasModule("plant")
  local bench = hasModule("workbench")

  ok(r, "starter modules", stove ~= nil and bed ~= nil and coffee ~= nil and plant ~= nil,
     string.format("stove=%s bed=%s coffee=%s plant=%s", tostring(stove ~= nil),
                   tostring(bed ~= nil), tostring(coffee ~= nil), tostring(plant ~= nil)))

  -- 做饭：生食材 + 水 -> 热饭
  local meal0, water0 = itemCount("meal"), water
  if stove then useStation(stove) end
  ok(r, "cook at stove", itemCount("meal") == meal0 + 1 and water < water0,
     string.format("meal %d->%d water %.1f->%.1f", meal0, itemCount("meal"), water0, water))

  -- 灶台配方：鱼汤
  addItem("fish", 1)
  local soup0 = itemCount("fishsoup")
  cookRecipe("fishsoup")
  ok(r, "cook fish soup", itemCount("fishsoup") == soup0 + 1,
     string.format("soup %d->%d fish=%d", soup0, itemCount("fishsoup"), itemCount("fish")))

  -- 灶台配方：浆果酱（野浆果 x3 + 水）
  do
    local j0, b0 = itemCount("jam"), itemCount("berry")
    local w0 = water
    addItem("berry", 3)
    water = math.max(water, 5)
    local okJam = cookRecipe("jam")
    local done = itemCount("jam") == j0 + 1 and
                 itemCount("berry") == b0 and water < w0 + 5
    water = w0
    if b0 == 0 then inv.berry = nil end
    ok(r, "cook berry jam", okJam and done,
       string.format("jam %d->%d berry=%d", j0, itemCount("jam"),
                     itemCount("berry")))
  end

  -- 灶台配方：浆果炖肉（生食材 x1 + 野浆果 x2 + 水），吃下还保暖
  do
    local s0 = itemCount("stew")
    local kr, kb = inv.rawfood or 0, inv.berry or 0
    local w0 = water
    addItem("rawfood", 1)
    addItem("berry", 2)
    water = math.max(water, 5)
    local okStew = cookRecipe("stew")
    local cooked = itemCount("stew") == s0 + 1
    local used = (inv.rawfood or 0) == kr and (inv.berry or 0) == kb
    water = w0
    if kr == 0 then inv.rawfood = nil end
    if kb == 0 then inv.berry = nil end
    local keepWarm = warmthMin
    warmthMin = 0
    local ate = useItem("stew")
    local warmOk = (warmthMin or 0) >= 40
    warmthMin = keepWarm
    if ate then addItem("stew", 1) end
    ok(r, "cook berry stew", okStew and cooked and used and ate and warmOk,
       string.format("stew %d->%d rawfood=%d berry=%d warm=%s", s0,
                     itemCount("stew"), inv.rawfood or 0, inv.berry or 0,
                     tostring(warmOk)))
  end

  -- 喝水
  local thirst0 = needs.thirst
  needs.thirst = 50
  if sink then useStation(sink) end
  ok(r, "drink at sink", needs.thirst > 50,
     string.format("thirst 50->%.0f", needs.thirst))

  -- 咖啡：煮 + 喝
  local brew0 = itemCount("brew")
  if coffee then useStation(coffee) end
  local brewed = itemCount("brew") == brew0 + 1
  local energy0 = needs.energy
  needs.energy = 40
  if coffee then useStation(coffee) end
  ok(r, "coffee brew+drink", brewed and needs.energy > 40,
     string.format("brew %d->%d energy 40->%.0f", brew0, itemCount("brew"), needs.energy))

  -- 睡觉：第二天 07:00
  local day0 = day
  needs.hygiene = 100  -- 干净时睡眠回满体力
  if bed then useStation(bed) end
  ok(r, "sleep in bed", needs.energy >= 99 and day == day0 + 1 and math.floor(clock) == 420,
     string.format("day %d->%d clock=%s", day0, day, timeString()))

  -- 种植：播种 -> 成熟 -> 收获
  local seed0 = itemCount("seed")
  if plant then useStation(plant) end
  local planted = plant and plant.plant ~= nil
  local raw0 = itemCount("rawfood")
  if planted then plant.plant.growth = SURVIVAL.plantMinutes; useStation(plant) end
  ok(r, "plant grow+harvest", planted and itemCount("rawfood") >= raw0 + 2,
     string.format("seed %d->%d rawfood %d->%d", seed0, itemCount("seed"), raw0, itemCount("rawfood")))

  -- 天气影响作物生长：雨天 1.25x、雪天 0.5x
  do
    local rain = plantGrowthScale("rain")
    local snow = plantGrowthScale("snow")
    local clear = plantGrowthScale("clear")
    ok(r, "weather plant growth",
       math.abs(rain - 1.25) < 1e-6 and math.abs(snow - 0.50) < 1e-6 and
       math.abs(clear - 1.0) < 1e-6,
       string.format("rain=%.2f snow=%.2f clear=%.2f", rain, snow, clear))
  end

  -- 钓鱼 -> 生鱼 -> 篝火烤鱼
  local raw0 = itemCount("rawfish")
  startFishing()
  finishFishing(true)
  local fished = itemCount("rawfish") > raw0
  local fish0 = itemCount("fish")
  cookAtCampfire()
  ok(r, "fishing + campfire", fished and itemCount("fish") > fish0,
     string.format("raw %d->%d fish %d->%d", raw0, itemCount("rawfish"),
                   fish0, itemCount("fish")))

  -- 修车
  dur = 50
  if bench then useStation(bench) end
  ok(r, "repair at workbench", dur > 50, string.format("dur 50->%.0f", dur))

  -- 工作台菜单：单独修引擎 + 引擎升级
  parts.engine = 40
  repairPart("engine")
  local repaired = parts.engine > 40
  parts.engine = 100
  materials.metal = 20; materials.electronics = 10
  local lv0 = upgrades.engine
  repairPart("engine")
  ok(r, "repair/upgrade engine", repaired and upgrades.engine == lv0 + 1,
     string.format("engine 40->100 lv %d->%d", lv0, upgrades.engine))

  -- 货架升级：载重上限 +300kg/级
  do
    local clv = upgrades.cargo or 0
    local max0 = weightMax()
    materials.metal = (materials.metal or 0) + 12
    materials.wood = (materials.wood or 0) + 12
    if clv < 3 then upgradeCargo() end
    local max1 = weightMax()
    local newLv = upgrades.cargo or 0
    local upgraded = newLv == clv + 1 and max1 > max0
    upgrades.cargo = clv
    recomputeStats()
    ok(r, "cargo rack upgrade", upgraded,
       string.format("max %d->%d lv %d->%d", max0, max1, clv, newLv))
  end

  -- 鱼竿升级：上钩率 +7%/级、尺寸 +4cm/级
  do
    local lv0 = upgrades.rod or 0
    materials.wood = (materials.wood or 0) + 12
    materials.cloth = (materials.cloth or 0) + 12
    if lv0 < 3 then upgradeRod() end
    local newLv = upgrades.rod or 0
    local bonus = rodCatchBonus()
    local size = rodSizeBonus()
    local upOk = newLv == lv0 + 1 and bonus > 0 and size > 0
    upgrades.rod = lv0
    ok(r, "fishing rod upgrade", upOk,
       string.format("lv %d->%d bonus=%.2f size=%dcm", lv0, newLv, bonus, size))
  end

  -- 水箱升级：容量 +30L/级
  do
    local lv0 = upgrades.water or 0
    local max0 = waterMax()
    local w0 = water
    materials.metal = (materials.metal or 0) + 8
    materials.cloth = (materials.cloth or 0) + 8
    if lv0 < 3 then upgradeWater() end
    local max1 = waterMax()
    local newLv = upgrades.water or 0
    local upOk = newLv == lv0 + 1 and max1 == max0 + 30
    upgrades.water = lv0
    water = w0
    ok(r, "water tank upgrade", upOk,
       string.format("capacity %d->%d lv %d->%d", max0, max1, lv0, newLv))
  end

  -- 油箱升级：容量 +10L/级
  do
    local lv0 = upgrades.fuel or 0
    local max0 = fuelMax()
    local f0 = fuel
    materials.metal = (materials.metal or 0) + 12
    materials.electronics = (materials.electronics or 0) + 8
    if lv0 < 3 then upgradeFuel() end
    local max1 = fuelMax()
    local newLv = upgrades.fuel or 0
    local upOk = newLv == lv0 + 1 and max1 == max0 + 10
    upgrades.fuel = lv0
    fuel = f0
    ok(r, "fuel tank upgrade", upOk,
       string.format("capacity %d->%d lv %d->%d", max0, max1, lv0, newLv))
  end

  -- 工作台制作修理包：金属 2 + 布料 1
  do
    local m0, c0, k0 = materials.metal or 0, materials.cloth or 0,
                       itemCount("repairkit")
    materials.metal, materials.cloth = 5, 3
    local crafted = craftRepairKit()
    local gotMetal = materials.metal or 0
    local gotCloth = materials.cloth or 0
    local gotKits = itemCount("repairkit")
    local okCraft = crafted and gotMetal == 3 and gotCloth == 2 and
                    gotKits == k0 + 1
    materials.metal, materials.cloth = m0, c0
    inv.repairkit = k0 > 0 and k0 or nil
    ok(r, "craft repair kit", okCraft,
       string.format("metal 5->%d cloth 3->%d kits %d->%d",
                     gotMetal, gotCloth, k0, gotKits))
  end

  -- 现场维修：消耗修理包修复最差部件；无包/满状态失败
  do
    local sb, se, st, sbat = dur, parts.engine, parts.tires, parts.battery
    local k0 = inv.repairkit or 0
    inv.repairkit = 1
    dur, parts.engine, parts.tires, parts.battery = 100, 40, 100, 100
    local fixed = fieldRepair()
    local engineUp = (parts.engine or 0) == 75
    local consumed = (inv.repairkit or 0) == 0
    local again = fieldRepair()
    local noKit = (again == false)
    inv.repairkit = k0 > 0 and k0 or nil
    dur, parts.engine, parts.tires, parts.battery = sb, se, st, sbat
    ok(r, "field repair", fixed and engineUp and consumed and noKit,
       string.format("fixed=%s engine=%d kitLeft=%d", tostring(fixed),
                     parts.engine or 0, inv.repairkit or 0))
  end

  -- 备胎：爆胎时按 R 直接换上，不消耗修理包
  do
    local keepBlow = rv.blowout
    local kp = rv.tirePressureCold
    local ks, kk = inv.sparetire or 0, inv.repairkit
    rv.blowout = true
    inv.sparetire = 1
    inv.repairkit = nil
    local fixed = fieldRepair()
    local cleared = rv.blowout == false
    local used = (inv.sparetire or 0) == 0
    rv.blowout = keepBlow
    rv.tirePressureCold = kp
    inv.sparetire = ks > 0 and ks or nil
    inv.repairkit = kk
    ok(r, "spare tire", fixed and cleared and used,
       string.format("fixed=%s cleared=%s left=%d", tostring(fixed),
                     tostring(cleared), inv.sparetire or 0))
  end

  -- 维修最差部件（联机工作台命令复用）
  do
    local sb, se, st, sbat = dur, parts.engine, parts.tires, parts.battery
    local sm, sel = materials.metal or 0, materials.electronics or 0
    local k0 = inv.repairkit
    inv.repairkit = nil
    dur, parts.engine, parts.tires, parts.battery = 100, 40, 100, 100
    materials.metal, materials.electronics = 6, 4
    local fixed = repairWorstPart()
    local engineUp = (parts.engine or 0) == 65
    local spent = (materials.metal or 0) < 6
    dur, parts.engine, parts.tires, parts.battery = sb, se, st, sbat
    materials.metal, materials.electronics = sm, sel
    inv.repairkit = k0
    ok(r, "repair worst part", fixed and engineUp and spent,
       string.format("fixed=%s engine=%d metal=%d", tostring(fixed),
                     parts.engine or 0, materials.metal or 0))
  end

  -- 游荡威胁（离开营地安全区才能刷）
  do
    local h0 = #horde
    local sx, sz = rv.x, rv.z
    rv.x, rv.z = 150, 150
    spawnAmbientZombie()
    local spawned = #horde == h0 + 1
    if spawned then killZombie(#horde) end
    rv.x, rv.z = sx, sz
    ok(r, "ambient threat spawn/kill", spawned and #horde == h0,
       string.format("horde %d->%d", h0, #horde))
  end

  -- 营地安全区：营地内不刷游荡感染者
  do
    local h0 = #horde
    local sx, sz, sa = rv.x, rv.z, atNode
    rv.x, rv.z, atNode = 0, 0, "camp"
    local z = spawnAmbientZombie()
    local blocked = (z == nil) and #horde == h0
    rv.x, rv.z, atNode = sx, sz, sa
    ok(r, "camp safe zone", blocked,
       string.format("spawned=%s horde=%d", tostring(z ~= nil), #horde))
  end

  -- 交互检测：站到灶台前应识别到"做饭"
  if stove then
    player.x, player.z = rotOf(stove.lx, stove.lz - 1.1)
    player.yaw = rv.yaw
    updateInteraction()
    ok(r, "interaction detection", nearAction ~= nil and nearAction.kind == "station",
       nearAction and nearAction.verb or "nil")
  end

  -- 第一人称自由摆放
  local storageCat = nil
  for _, c in ipairs(CATALOG) do if c.id == "storage" then storageCat = c end end
  local freeOk = false
  if storageCat then
    local before = 0
    for _ in pairs(instances) do before = before + 1 end
    freeOk = placeFree(storageCat, -0.5, 2.6, math.pi / 6, true)
    local after = 0
    for _ in pairs(instances) do after = after + 1 end
    freeOk = freeOk and after == before + 1 and lastPlacedInst.free == true
  end
  ok(r, "free placement", freeOk, "no grid snap")

  -- 程序化地点（已到 15 个上限时 trySpawnPoi 应保持不变）
  local n0 = #NODES
  trySpawnPoi()
  local grew = (#NODES == n0 + 1)
  local atCap = (n0 >= 15) and (#NODES == n0)
  ok(r, "procedural POI", grew or atCap,
     string.format("nodes %d->%d", n0, #NODES))

  -- 程序化地点种类：至少 10 种，且都有名称/危险度/战利品
  do
    local okKinds = #POI_KINDS >= 10
    local names = {}
    for _, k in ipairs(POI_KINDS) do
      if k.name == nil or k.danger == nil or type(k.loot) ~= "table" then
        okKinds = false
      end
      names[k.name] = true
    end
    ok(r, "poi kinds",
       okKinds and names["林间木屋"] == true and names["废弃邮局"] == true,
       string.format("kinds=%d woodcabin=%s post=%s", #POI_KINDS,
                     tostring(names["林间木屋"]), tostring(names["废弃邮局"])))
  end

  -- 类型化外景装饰：木屋/邮局/护林站可识别，普通地点不带装饰
  do
    local cabin = poiDecorStyle("林间木屋 #7")
    local post = poiDecorStyle("废弃邮局 #9")
    local ranger = poiDecorStyle("林间护林站 #3")
    local plain = poiDecorStyle("路边废宅 #3")
    ok(r, "poi decor style",
       cabin == "woodcabin" and post == "postoffice" and
       ranger == "ranger" and plain == nil,
       string.format("cabin=%s post=%s ranger=%s plain=%s",
                     tostring(cabin), tostring(post), tostring(ranger),
                     tostring(plain)))
  end

  -- 护林站地图奖励：只揭示 90m 内未探索的程序化地点
  do
    local base = visitedCount
    local a = { id = "selftest_ranger_a", x = 0, z = 0, generated = true }
    local b = { id = "selftest_ranger_b", x = 50, z = 0, generated = true }
    local c = { id = "selftest_ranger_c", x = 200, z = 0, generated = true }
    local d = { id = "selftest_ranger_d", x = 10, z = 0, generated = false }
    local got = revealNearbyPoisAt(
      { id = "selftest_ranger", x = 0, z = 0, generated = true }, 90,
      { a, b, c, d })
    local aKnown = visitedPois.selftest_ranger_a == true
    local bKnown = visitedPois.selftest_ranger_b == true
    local cKnown = visitedPois.selftest_ranger_c
    local dKnown = visitedPois.selftest_ranger_d
    visitedPois.selftest_ranger_a, visitedPois.selftest_ranger_b = nil, nil
    visitedCount = base
    ok(r, "ranger map reveal",
       #got == 2 and aKnown and bKnown and not cKnown and not dKnown,
       string.format("revealed=%d a=%s b=%s c=%s d=%s", #got,
                     tostring(aKnown), tostring(bKnown), tostring(cKnown),
                     tostring(dKnown)))
  end

  -- 昼夜
  clock = 22 * 60
  local nightOn = nightAmount() > 0.5
  clock = 12 * 60
  local dayOff = nightAmount() < 0.01
  ok(r, "day/night cycle", nightOn and dayOff)

  -- 天气
  setWeather("rain")
  local rainOn = isRainy()
  setWeather("clear")
  ok(r, "weather", rainOn and not isRainy())

  -- 天气持续 1.5–3 游戏小时，HUD 预报可读
  do
    setWeather("clear")
    local dur = weatherNext or 0
    local hours = weatherHoursLeft()
    ok(r, "weather duration", dur >= 90 and dur <= 180 and
       hours > 1.4 and hours <= 3.01,
       string.format("next=%.0fmin left=%.2fh", dur, hours))
  end

  -- 真实昼夜光照
  clock = 22 * 60; lastSunClock = -999; updateDayNight(0)
  local nightInt = EntityComponentField(sunEnt, "light", "intensity")
  clock = 12 * 60; lastSunClock = -999; updateDayNight(0)
  local dayInt = EntityComponentField(sunEnt, "light", "intensity")
  ok(r, "day/night sun light", nightInt and dayInt and dayInt > nightInt,
     string.format("night=%.2f day=%.2f", tonumber(nightInt) or -1, tonumber(dayInt) or -1))

  -- Terrain relief: the embankment profile rises away from the road and the
  -- follow helper converges on the sampled height (player/RV ground following).
  if terrainHeight ~= nil and terrainFollow ~= nil then
    if WORLD_PROCEDURAL then
      -- Procedural world: deterministic, finite far away, flat camp, and the
      -- alpine uplift to the north. Old authored-height constants no longer
      -- apply.
      local h0 = terrainHeight(0, 0)
      local hNorth = terrainHeight(0, 800)
      local hFar = terrainHeight(100000, -100000)
      local h0b = terrainHeight(0, 0)
      ok(r, "terrain world",
         h0 == h0b and math.abs(h0) < 0.6 and hNorth > 30 and
         hFar == hFar and math.abs(hFar) < 10000,
         string.format("camp=%.2f north=%.2f far=%.2f", h0, hNorth, hFar))
      -- Rendering budget guard: 1m cells at a 512m view drag the RTX 2060 to
      -- ~12 FPS. 320m (fog ends at 220m) with 3m cells keeps the ground detailed
      -- and the frame rate above ~50 FPS in the camp/terrain-heavy view.
      local scene = ReadText and ReadText("assets/scenes/garage.json") or nil
      local vd = scene and tonumber(scene:match(
        '"terrainViewDistance"%s*:%s*([%d%.]+)')) or 0
      local cs = scene and tonumber(scene:match(
        '"terrainCellSize"%s*:%s*([%d%.]+)')) or 0
      ok(r, "terrain render budget", vd > 0 and vd <= 512 and cs >= 2.0,
         string.format("view=%.0f cell=%.1f", vd, cs))
    else
    local hFlat = terrainHeight(0, 0)
    local hMid = terrainHeight(16, -2)
    local hTop = terrainHeight(22, -2)
    ok(r, "terrain height profile",
       hFlat < 0.01 and hMid > 1.0 and hTop > hMid,
       string.format("flat=%.2f mid=%.2f top=%.2f", hFlat, hMid, hTop))
    local followed = terrainFollow(0, 22, -2, 1.0, 10)
    ok(r, "terrain ground follow",
       math.abs(followed - hTop) < 0.001,
       string.format("follow=%.2f top=%.2f", followed, hTop))
    -- General heightfield: the grid is loaded, the camp/roads stay level and
    -- the meadow knolls roll.
    if terrainFieldInfo ~= nil then
      local fi = terrainFieldInfo()
      ok(r, "terrain heightfield",
         fi ~= nil and fi.nx >= 73 and fi.nz >= 73,
         fi and string.format("%dx%d step=%.0f", fi.nx, fi.nz, fi.step)
            or "analytic fallback")
      local knoll = terrainHeight(-26, -20)
      ok(r, "terrain knoll",
         knoll > 1.5,
         string.format("knoll=%.2f", knoll))
      local west = terrainHeight(-95, 15)
      local east = terrainHeight(112, 24)
      local north = terrainHeight(28, 105)
      local south = terrainHeight(72, -116)
      local northEast = terrainHeight(96, 86)
      local outside = terrainHeight(150, 0)
      ok(r, "terrain extended grid",
         west > 2.0 and east > 2.0 and north > 2.0 and south > 1.5 and
         northEast > 1.5 and outside == 0.0,
         string.format("west=%.2f east=%.2f north=%.2f south=%.2f ne=%.2f out=%.2f",
                       west, east, north, south, northEast, outside))
      local flatOk = terrainHeight(0, 0) < 0.1 and
                     terrainHeight(0, 60) < 0.1 and
                     terrainHeight(38, 27) < 0.1 and
                     terrainHeight(24, -40) < 0.1 and
                     terrainHeight(12, -20) < 0.1 and
                     terrainHeight(72, 0) < 0.1 and
                     terrainHeight(106, -66) < 0.1 and
                     terrainHeight(137, -2) < 0.1
      ok(r, "terrain flat corridor", flatOk,
         string.format("camp=%.2f gas=%.2f ruin=%.2f clinic=%.2f road=%.2f edge=%.2f mall=%.2f town=%.2f",
                       terrainHeight(0, 0), terrainHeight(0, 60),
                       terrainHeight(38, 27), terrainHeight(24, -40),
                       terrainHeight(12, -20), terrainHeight(72, 0),
                       terrainHeight(106, -66), terrainHeight(137, -2)))
    end
    end -- WORLD_PROCEDURAL
    -- Authored POIs sit on the camp plateau in the new world; this pins the
    -- assumption so a future terrain change cannot quietly bury them.
    if NODES ~= nil and poiCenter ~= nil then
      local worst, worstId = 0, ""
      for _, nd in ipairs(NODES) do
        local cx, cz = poiCenter(nd)
        if cx ~= nil then
          local d = math.abs(terrainHeight(cx, cz))
          if d > worst then worst, worstId = d, tostring(nd.id) end
        end
      end
      ok(r, "pois grounded", worst < 1.5,
         string.format("worst=%s %.2fm", worstId, worst))
    end
    if safePoiGround ~= nil then
      local sx, sz = safePoiGround(0, 500)
      ok(r, "generated poi relocation",
         sx ~= nil and math.abs(terrainHeight(sx, sz)) <= 0.8,
         sx and string.format("spot=(%.1f,%.1f) h=%.2f", sx, sz,
                              terrainHeight(sx, sz)) or "none")
    end
    if slopeFactorFromGradient ~= nil then
      local f0 = slopeFactorFromGradient(0)
      local f30 = slopeFactorFromGradient(math.tan(math.rad(30)))
      local f50 = slopeFactorFromGradient(math.tan(math.rad(50)))
      ok(r, "slope traction",
         f0 == 1.0 and f30 < 0.80 and f30 > 0.55 and f50 == 0.0,
         string.format("flat=%.2f d30=%.2f d50=%.2f", f0, f30, f50))
    end
    -- Wheels must never be buried by the visible terrain: the physics wheel
    -- body rests slightly compressed, so the visual tire is clamped to
    -- ground + WHEEL_RADIUS in lc_vehicle.
    if rvWheels ~= nil and applyRvTransform ~= nil and GetPosition ~= nil then
      applyRvTransform()
      local sunk = nil
      for i, w in ipairs(rvWheels) do
        local p = GetPosition(w.ent)
        if p ~= nil then
          -- Road slab included: the tire bottom must sit on the asphalt, not
          -- on the terrain 9 cm below it.
          local g = driveGroundAt(p.x, p.z)
          if p.y < g + 0.38 then sunk = i end
        end
      end
      local wp = GetPosition(rvWheels[1].ent)
      local wg = wp and driveGroundAt(wp.x, wp.z) or 0
      ok(r, "rv wheels grounded", sunk == nil,
         sunk and ("wheel " .. sunk .. " sunk")
              or string.format("tire-bottom offset=%.3fm", (wp and wp.y or 0) - (wg + 0.40)))
    end
    if ReadText ~= nil and Json ~= nil then
      -- Sculpted geometry: the RV shell uses swept wheel-arch lips plus a
      -- three-strip lofted roof shoulder instead of chamfered boxes, and the
      -- sink basin is a revolved U-profile (two U loops + rim).
      local bodyStats = Json.Parse(ReadText("assets/data/rv_body_stats.json") or "")
      local furnStats = Json.Parse(ReadText("assets/data/rv_furniture_stats.json") or "")
      local sink = type(furnStats) == "table" and furnStats["rv_furn_sink"] or nil
      ok(r, "rv sculpted geometry",
         type(bodyStats) == "table" and
         (bodyStats.archLipSegments or 0) >= 12 and
         (bodyStats.archArcs or 0) == 4 and
         (bodyStats.roofShoulderStrips or 0) >= 3 and
         (bodyStats.wallQuads or 0) >= 300 and
         (bodyStats.wallInnerQuads or 0) == (bodyStats.wallQuads or -1) and
         (bodyStats.wallThickness or 0) >= 0.05 and
         (bodyStats.wallJambs or 0) >= 56 and
         (bodyStats.jambChamfer or 0) >= 0.02 and
         (bodyStats.glassThickness or 0) >= 0.02 and
         (bodyStats.windowPanes or 0) >= 14 and
         (bodyStats.windowCurvature or 0) >= 0.005 and
         (bodyStats.curvedGlassPanels or 0) >= 100 and
         (bodyStats.glassEdgeStrips or 0) >= 60 and
         (bodyStats.gasketStrips or 0) >= 24 and
         (bodyStats.wallArchReveals or 0) >= 16 and
         (bodyStats.wallHoles or 0) == 12 and
         (bodyStats.wallCurve or 0) > 0.01 and
         (bodyStats.curvedPanels or 0) >= 24 and
         (bodyStats.fasciaChamfers or 0) >= 14 and
         (bodyStats.fasciaCurve or 0) >= 0.05 and
         (bodyStats.rearPanels or 0) >= 36 and
         (bodyStats.rearChamfers or 0) >= 24 and
         (bodyStats.rearReveals or 0) >= 4 and
         (bodyStats.rearCurve or 0) >= 0.03 and
         (bodyStats.verts or 0) > 10000 and
         type(sink) == "table" and (sink.latheSegments or 0) >= 24 and
         (sink.verts or 0) > 1500,
         string.format("wall=%d/%d t=%.2f jambs=%d(chamfer %.2f) panes=%d glass=%d(curve %.3f) gaskets=%d fascia=%d/%d rear=%d/%d reveals=%d bodyV=%d lathe=%d sinkV=%d",
                       bodyStats and (bodyStats.wallQuads or 0) or 0,
                       bodyStats and (bodyStats.wallInnerQuads or 0) or 0,
                       bodyStats and (bodyStats.wallThickness or 0) or 0,
                       bodyStats and (bodyStats.wallJambs or 0) or 0,
                       bodyStats and (bodyStats.jambChamfer or 0) or 0,
                       bodyStats and (bodyStats.windowPanes or 0) or 0,
                       bodyStats and (bodyStats.curvedGlassPanels or 0) or 0,
                       bodyStats and (bodyStats.windowCurvature or 0) or 0,
                       bodyStats and (bodyStats.gasketStrips or 0) or 0,
                       bodyStats and (bodyStats.curvedPanels or 0) or 0,
                       bodyStats and (bodyStats.fasciaChamfers or 0) or 0,
                       bodyStats and (bodyStats.rearPanels or 0) or 0,
                       bodyStats and (bodyStats.rearChamfers or 0) or 0,
                       bodyStats and (bodyStats.wallArchReveals or 0) or 0,
                       bodyStats and (bodyStats.verts or 0) or 0,
                       sink and (sink.latheSegments or 0) or 0,
                       sink and (sink.verts or 0) or 0))
    end
    if ReadText ~= nil and Json ~= nil and not WORLD_PROCEDURAL then
      -- Visible terrain is 16 chunk groups, each with three prefabs (4 m,
      -- 8 m, 12 m) and exactly one visible level chosen by distance.
      local function gltfVerts(txt)
        if txt == nil or txt == "" then return 0 end
        local g = Json.Parse(txt)
        if type(g) ~= "table" or type(g.meshes) ~= "table" or
           type(g.accessors) ~= "table" then return 0 end
        local total = 0
        for _, mesh in ipairs(g.meshes) do
          for _, prim in ipairs(mesh.primitives or {}) do
            local pi = prim.attributes and prim.attributes.POSITION
            if pi ~= nil and g.accessors[pi + 1] ~= nil then
              total = total + (g.accessors[pi + 1].count or 0)
            end
          end
        end
        return total
      end
      local chunks, okLod, fineSum, farSum = 0, true, 0, 0
      local worst = 0.0
      for iz = 0, 3 do
        for ix = 0, 3 do
          local stage = string.format("%d%d", ix, iz)
          local base = "assets/models/rv/rv_terrain_" .. stage
          for _, suffix in ipairs({ "", "_lod2", "_lod3" }) do
            local pf = ReadText("assets/prefabs/rv_terrain_" .. stage .. suffix .. ".json")
            if pf == nil or #pf < 20 then okLod = false end
          end
          local v0 = gltfVerts(ReadText(base .. ".gltf"))
          local v2 = gltfVerts(ReadText(base .. "_lod2.gltf"))
          local v3 = gltfVerts(ReadText(base .. "_lod3.gltf"))
          if v0 <= 0 or v2 <= 0 or v3 <= 0 or
             v2 >= v0 * 0.5 or v3 >= v0 * 0.3 then
            okLod = false
          end
          if v0 > 0 and v3 > 0 then
            local ratio = v3 / v0
            if ratio > worst then worst = ratio end
          end
          fineSum = fineSum + v0
          farSum = farSum + v3
          chunks = chunks + 1
        end
      end
      local nearN, midN, farN, haveAll, loadedN = 0, 0, 0, 0, 0
      for _, c in ipairs(terrainChunks or {}) do
        if c.level == 1 then nearN = nearN + 1
        elseif c.level == 2 then midN = midN + 1
        elseif c.level == 3 then farN = farN + 1 end
        if c.loaded ~= false then loadedN = loadedN + 1 end
        local n = 0
        for i = 1, 3 do
          if c.levels[i] ~= nil then n = n + 1 end
        end
        if n == 3 then haveAll = haveAll + 1 end
      end
      ok(r, "rv terrain lod",
         okLod and chunks == 16 and farSum > 0 and farSum < fineSum * 0.3 and
         haveAll == 16 and nearN + midN + farN == 16 and
         nearN >= 1 and farN >= 1 and
         loadedN == 16,
         string.format("chunks=%d verts=%d far=%d worst=%.2f levels=%d/%d/%d loaded=%d",
                       chunks, fineSum, farSum, worst, nearN, midN, farN, loadedN))
      -- Screen-space error: the ring distances must land on 4 m / 8 m / 12 m,
      -- and the hysteresis band must keep a chunk from flapping.
      local k = terrainLodK(720, 60)
      local l0 = terrainLodLevel(51, 1, k)
      local l1 = terrainLodLevel(114, 1, k)
      local l2 = terrainLodLevel(152.7, 1, k)
      local band1 = terrainLodLevel(70, 1, k)
      local band2 = terrainLodLevel(70, 2, k)
      local up = terrainLodLevel(110, 3, k)
      ok(r, "rv terrain lod sse",
         l0 == 1 and l1 == 2 and l2 == 3 and
         band1 == 1 and band2 == 2 and up == 2,
         string.format("d51=L%d d114=L%d d152=L%d band=1/%d up=L%d",
                       l0, l1, l2, band2, up))
      -- Streaming hysteresis: loaded chunks survive to the unload radius,
      -- unloaded ones only come back inside the (smaller) load radius.
      ok(r, "rv terrain lod stream",
         terrainChunkStreamed(200, true) and not terrainChunkStreamed(215, true) and
         terrainChunkStreamed(185, false) and not terrainChunkStreamed(200, false),
         "load=190 unload=210")
    end
    -- Tilt/suspension math is world-independent: drive it on a synthetic 30%
    -- east-west slope so these assertions do not depend on authored terrain.
    local __realTerrainHeight = terrainHeight
    -- A linear slope fits a plane exactly (zero articulation), so add a small
    -- bump that only some wheels see to exercise the travel residual.
    terrainHeight = function(x, z)
      return x * 0.30 + 0.12 * math.sin(x * 1.7) + (z or 0) * 0.02
    end
    if updateRvTilt ~= nil then
      local kx, kz, ky = rv.x, rv.z, rv.yaw
      local kp, kr = rv.pitch, rv.roll
      rv.x, rv.z, rv.yaw = 16.0, -2.0, 0.0
      rv.pitch, rv.roll = 0.0, 0.0
      updateRvTilt(1.0)
      ok(r, "rv slope tilt",
         math.abs(rv.roll) > 0.15 and math.abs(rv.pitch) < 0.2,
         string.format("pitch=%.2f roll=%.2f", rv.pitch, rv.roll))
      -- The interior floor must lean with the body: the nose (lz=+3) drops
      -- with positive pitch and the right side (lx=+3) rises with roll.
      if rvFloorOffset ~= nil then
        rv.pitch, rv.roll = 0.20, 0.00
        local nose = rvFloorOffset(0, 3.0)
        rv.pitch, rv.roll = 0.00, 0.20
        local right = rvFloorOffset(3.0, 0)
        ok(r, "rv tilted interior",
           nose < -0.5 and right > 0.5,
           string.format("nose=%.2f right=%.2f", nose, right))
      end
      rv.x, rv.z, rv.yaw = kx, kz, ky
      rv.pitch, rv.roll = kp, kr
    end
    -- Four wheel contacts: the body height and attitude must come from the
    -- four sampled contact heights, not from the single centre gradient.
    if updateRvSuspension ~= nil then
      local kx, kz, ky = rv.x, rv.z, rv.yaw
      local kp, kr, kg, ks = rv.pitch, rv.roll, rv.groundY, rv.susp
      local yaw = 0.6
      rv.x, rv.z, rv.yaw = 10.0, -6.0, yaw
      rv.pitch, rv.roll, rv.groundY = 0.0, 0.0, 0.0
      rv.susp = nil
      updateRvSuspension(1.0)
      local c, s = math.cos(yaw), math.sin(yaw)
      local function wheelH(lx, lz)
        return terrainHeight(rv.x + lx * c + lz * s,
                             rv.z - lx * s + lz * c)
      end
      local fl, fr = wheelH(-1.26, 2.92), wheelH(1.26, 2.92)
      local rl, rr = wheelH(-1.26, -2.92), wheelH(1.26, -2.92)
      local hs = { fl, fr, rl, rr }
      local lx = { -1.26, 1.26, -1.26, 1.26 }
      local lz = { 2.92, 2.92, -2.92, -2.92 }
      local avg = (fl + fr + rl + rr) / 4
      local b = ((fl + fr) / 2 - (rl + rr) / 2) / 5.84
      local a = ((fr + rr) / 2 - (fl + rl) / 2) / 2.52
      local maxPen = 0
      for i = 1, 4 do
        local pen = hs[i] - (avg + a * lx[i] + b * lz[i])
        if pen > maxPen then maxPen = pen end
      end
      if maxPen > 0.25 then maxPen = 0.25 end
      local h0 = avg + maxPen
      local maxClear = -1e9
      for i = 1, 4 do
        local clear = hs[i] - (h0 + a * lx[i] + b * lz[i])
        if clear > maxClear then maxClear = clear end
      end
      local spread = math.abs(fl - rl) + math.abs(fr - rr) +
                     math.abs(fl - fr)
      ok(r, "rv four wheel contact",
         math.abs(rv.groundY - h0) < 0.02 and
         math.abs(rv.pitch + math.atan(b)) < 0.02 and
         math.abs(rv.roll - math.atan(a)) < 0.02 and
         maxClear < 0.02 and
         spread > 0.05,
         string.format("gy=%.2f h0=%.2f clearance=%.2f pitch=%.2f roll=%.2f spread=%.2f",
                       rv.groundY, h0, maxClear, rv.pitch, rv.roll, spread))
      local tr = (rv.susp and rv.susp.travel) or {}
      local tmin, tmax = 1e9, -1e9
      for i = 1, #tr do
        if tr[i] < tmin then tmin = tr[i] end
        if tr[i] > tmax then tmax = tr[i] end
      end
      ok(r, "rv wheel articulation",
         #tr == 4 and (tmax - tmin) > 0.05 and
         math.abs(tmax) < 0.6 and math.abs(tmin) < 0.6,
         string.format("travel=%.2f..%.2f", tmin, tmax))
      ok(r, "rv wheel entities",
         rvWheels ~= nil and #rvWheels == 4 and rvWheels[1].ent ~= nil and
         rvWheels[4].ent ~= nil,
         string.format("wheels=%d", rvWheels and #rvWheels or 0))
      -- Front wheels steer from the yaw rate (no extra network field).
      rv.prevYaw = rv.yaw - 0.05
      updateRvSuspension(0.1)
      ok(r, "rv wheel steer",
         math.abs(rv.steerVis or 0) > 0.01,
         string.format("steer=%.2f spin=%.1f",
                       rv.steerVis or 0, rv.wheelSpin or 0))
      rv.x, rv.z, rv.yaw = kx, kz, ky
      rv.pitch, rv.roll, rv.groundY = kp, kr, kg
      rv.susp = ks
    end
    terrainHeight = __realTerrainHeight
    -- Physical suspension bridge + attitude round-trip through Jolt.
    if PhysicsApplyImpulse ~= nil and PhysicsGetPointVelocity ~= nil then
      ok(r, "rv physics suspension binding", true,
         "PhysicsApplyImpulse + PhysicsGetPointVelocity")
    end
    if PhysicsGetAngularVelocity ~= nil and PhysicsAddAngularImpulse ~= nil then
      ok(r, "rv tire grip binding", true,
         "PhysicsGetAngularVelocity + PhysicsAddAngularImpulse")
    end
    if PhysicsAddWheelJoint ~= nil and PhysicsAddCylinder ~= nil and
       PhysicsActivate ~= nil then
      ok(r, "rv wheel body binding", true,
         "PhysicsAddWheelJoint + PhysicsAddCylinder + PhysicsActivate")
    end
    if rv.wheelBodies ~= nil then
      ok(r, "rv wheel bodies", #rv.wheelBodies == 4 and
         rv.wheelBodies[1].body ~= nil and rv.wheelBodies[4].body ~= nil,
         string.format("bodies=%d", #rv.wheelBodies))
      ok(r, "rv wheel cylinder rolling",
         rv.wheelBodies[1].rolling == true and rv.wheelBodies[4].rolling == true,
         string.format("rolling=%s", tostring(rv.wheelBodies[1].rolling)))
    end
    if updateWheelPrediction ~= nil and rv.wheelBodies ~= nil then
      updateWheelPrediction(1 / 60)
      local okW = true
      for i = 1, #rv.wheelBodies do
        local bp = PhysicsGetPosition(rv.wheelBodies[i].body)
        if bp == nil or bp.y ~= bp.y then okW = false end
      end
      ok(r, "rv wheel prediction", okW, "local suspension step ok")
    end
    if coopPredictPose ~= nil and coopAngleDelta ~= nil then
      local p = coopPredictPose({ x = 0, z = 0, yaw = 0.5 },
                                { x = 4, z = 2, yawRate = 0.5 }, 0.25)
      local d = coopAngleDelta(3.2, -3.2)
      ok(r, "rv pose prediction",
         math.abs(p.x - 1.0) < 1e-6 and math.abs(p.z - 0.5) < 1e-6 and
         math.abs(p.yaw - 0.625) < 1e-6 and
         math.abs(d - (6.4 - 2 * math.pi)) < 1e-6,
         string.format("x=%.2f z=%.2f yaw=%.2f wrap=%.2f",
                       p.x, p.z, p.yaw, d))
    end
    if PhysicsSetAngularVelocity ~= nil and PhysicsGetAngularVelocity ~= nil
       and rv.wheelBodies ~= nil and rv.wheelBodies[1] ~= nil then
      PhysicsSetAngularVelocity(rv.wheelBodies[1].body, { x = 2.0, y = 0, z = 0 })
      local wv = PhysicsGetAngularVelocity(rv.wheelBodies[1].body)
      ok(r, "rv wheel rolling",
         wv ~= nil and math.abs(wv.x - 2.0) < 0.2,
         string.format("wx=%.2f", wv and wv.x or -1))
    end
    if tireGripImpulse ~= nil then
      local dt = 1 / 60
      local grip = tireGripImpulse(3.0, 3000, 1200, dt)
      local capped = tireGripImpulse(100.0, 3000, 1200, dt)
      local empty = tireGripImpulse(3.0, 0, 1200, dt)
      ok(r, "rv tire grip model",
         grip < 0 and math.abs(capped + 0.9 * 3000 * dt) < 0.5 and
         math.abs(empty) < 0.001,
         string.format("grip=%.1f capped=%.1f empty=%.1f",
                       grip, capped, empty))
    end
    if surfaceInfo ~= nil then
      local muRoad = surfaceGrip(0, -20)
      local muGrass = surfaceGrip(-20, -28)
      local muDirt = surfaceGrip(14, 20)
      local muGravel = surfaceGrip(5, 120)
      local muMud = surfaceGrip(-18, -12)
      local roads, grass = surfaceInfo(0, -20), surfaceInfo(-20, -28)
      ok(r, "rv surface grip",
         math.abs(muRoad - 0.98) < 0.01 and
         math.abs(muDirt - 0.72) < 0.01 and
         math.abs(muGrass - 0.60) < 0.01 and
         math.abs(muGravel - 0.80) < 0.01 and
         math.abs(muMud - 0.42) < 0.01 and
         roads == "asphalt" and grass == "grass",
         string.format("road=%.2f gravel=%.2f dirt=%.2f grass=%.2f mud=%.2f",
                       muRoad, muGravel, muDirt, muGrass, muMud))
      local seen, count = {}, 0
      for _, d in pairs(SURFACES) do
        if not seen[d.mu] then seen[d.mu] = true count = count + 1 end
      end
      ok(r, "rv surface table", count >= 5,
         string.format("distinctMu=%d", count))
      ok(r, "surface labels",
         surfaceLabel("asphalt") == "沥青" and
         surfaceLabel("grass") == "草地" and
         surfaceLabel("mud") == "泥泞" and
         surfaceLabel("???") == "未知",
         string.format("asphalt=%s grass=%s mud=%s",
                       surfaceLabel("asphalt"), surfaceLabel("grass"),
                       surfaceLabel("mud")))
    end
    if tireForceCurve ~= nil then
      local peak = tireForceCurve(0.9, 3000, 0.12, 0.12, 0.75)
      local high = tireForceCurve(0.9, 3000, 0.60, 0.12, 0.75)
      local zero = tireForceCurve(0.9, 3000, 0.0, 0.12, 0.75)
      local back = tireForceCurve(0.9, 3000, -0.12, 0.12, 0.75)
      ok(r, "rv tire force curve",
         math.abs(peak - 0.9 * 3000) < 1 and high < peak and high > 0.5 * peak and
         math.abs(zero) < 0.001 and math.abs(back + peak) < 1,
         string.format("peak=%.0f slide=%.0f zero=%.1f back=%.0f",
                       peak, high, zero, back))
    end
    if ReadText ~= nil then
      local veg = { "rv_grass_tuft", "rv_grass_tuft_b", "rv_fern",
                    "rv_flower_white", "rv_bush", "rv_meadow_a", "rv_meadow_c" }
      local okWind = true
      for _, name in ipairs(veg) do
        local txt = ReadText("assets/prefabs/" .. name .. ".json")
        if txt == nil or not string.find(txt, "\"windSway\"", 1, true) then
          okWind = false
        end
      end
      ok(r, "rv vegetation wind", okWind,
         string.format("per-vertex windSway on %d prefabs", #veg))
    end
    if tireMuScale ~= nil then
      local cold = tireMuScale(20, 100)
      local warm = tireMuScale(70, 100)
      local hot = tireMuScale(130, 100)
      local worn = tireMuScale(70, 10)
      ok(r, "rv tire temperature",
         cold < warm and hot < warm and warm > 0.99 and worn < warm,
         string.format("cold=%.2f warm=%.2f hot=%.2f worn=%.2f",
                       cold, warm, hot, worn))
    end
    if tireStep ~= nil and tireWearStep ~= nil then
      -- Front tire: rolling fast, no drive slip. Rear tire: same speed but
      -- wheelspin (slip speed 9 m/s) on a similar load. Run ~2 s at 60 Hz.
      local front, rear = 22.0, 22.0
      local frontW, rearW = 100.0, 100.0
      for _ = 1, 120 do
        front = tireStep(front, 8.0, 0.2, 4000, 1 / 60, 22)
        rear = tireStep(rear, 7.0, 9.0, 3800, 1 / 60, 22)
        frontW = tireWearStep(frontW, 0.2, 1 / 60, 0.8)
        rearW = tireWearStep(rearW, 9.0, 1 / 60, 2.0)
      end
      local coldFront = tireStep(22, 0, 0, 0, 1 / 60, 22)
      ok(r, "rv tire per wheel",
         rear > front and front > 22 and rear <= 150 and
         rearW < frontW and frontW > 99 and
         math.abs(coldFront - 22) < 1e-6,
         string.format("front=%.0fC rear=%.0fC fWear=%.1f rWear=%.1f",
                       front, rear, frontW, rearW))
    end
    if glassVoronoiCells ~= nil and glassVoronoiStats ~= nil then
      -- Impact-stress Voronoi: the cells must tile the pane, be denser near
      -- the impact, and rebuild identically from the same server seed.
      local cells, iu, iv = glassVoronoiCells(981231)
      local n, total, nearAvg, farAvg, nearN, farN =
        glassVoronoiStats(cells, iu, iv)
      local cells2, iu2, iv2 = glassVoronoiCells(981231)
      local n2, total2 = glassVoronoiStats(cells2, iu2, iv2)
      local same = n == n2 and math.abs(total - total2) < 1e-9 and
                   #cells > 0 and #cells2 > 0 and
                   math.abs(cells[1].cu - cells2[1].cu) < 1e-9 and
                   math.abs(cells[1].cv - cells2[1].cv) < 1e-9
      ok(r, "glass voronoi cells",
         n >= 12 and total > 0.90 and total < 1.05 and
         nearN >= 3 and farN >= 3 and nearAvg < farAvg and same,
         string.format("cells=%d area=%.3f near=%.4f far=%.4f n=%d/%d",
                       n, total, nearAvg, farAvg, nearN, farN))
      local tokId, tokSeed, tokCount = glassParseToken(glassToken("ruin", 981231, n))
      local oldId, oldSeed, oldCount = glassParseToken("gasstop:42")
      ok(r, "glass seed token",
         tokId == "ruin" and tokSeed == 981231 and tokCount == n and
         oldId == "gasstop" and oldSeed == 42 and oldCount == 0,
         string.format("%s:%d:%d legacy=%s:%d:%d",
                       tostring(tokId), tokSeed, tokCount,
                       tostring(oldId), oldSeed, oldCount))
    end
    if tireCombinedForce ~= nil and tireLoadMuScale ~= nil then
      -- Friction ellipse: pure longitudinal demand stays under the circle;
      -- adding lateral demand scales both axes so the resultant is capped.
      local fx, fy, util, muEff = tireCombinedForce(
          0.9, 4000, 0.5, 0.0, 300, 1 / 60, 0.12, 0.75)
      local fx2, fy2 = tireCombinedForce(
          0.9, 4000, 0.5, 6.0, 300, 1 / 60, 0.12, 0.75)
      local cap = muEff * 4000
      local res = math.sqrt(fx2 * fx2 + fy2 * fy2)
      local muLight = tireLoadMuScale(1500)
      local muHeavy = tireLoadMuScale(6000)
      ok(r, "rv tire combined slip",
         math.abs(fy) < 0.01 and fx > 0 and util <= 1.0001 and
         math.abs(fx2) < math.abs(fx) and math.abs(fy2) > 500 and
         res <= cap + 0.5 and muLight > muHeavy,
         string.format("fx=%.0f fx2=%.0f fy2=%.0f res=%.0f cap=%.0f mu=%.2f/%.2f",
                       fx, math.abs(fx2), math.abs(fy2), res, cap,
                       muLight, muHeavy))
    end
    if tirePacejkaLong ~= nil then
      -- Pacejka magic formula: peak at ~0.12 slip, falling plateau afterwards,
      -- odd symmetry, and pressure scales both peak grip and peak-slip point.
      local zero = tirePacejkaLong(0.9, 4000, 0.0, 2.4)
      local peak = tirePacejkaLong(0.9, 4000, 0.12, 2.4)
      local high = tirePacejkaLong(0.9, 4000, 0.50, 2.4)
      local back = tirePacejkaLong(0.9, 4000, -0.12, 2.4)
      local lowP = tirePacejkaLong(0.9, 4000, 0.12, 1.4)
      ok(r, "rv tire pacejka",
         math.abs(zero) < 1 and peak > 3000 and high < peak and
         high > 0.4 * peak and math.abs(back + peak) < 1 and lowP < peak,
         string.format("zero=%.0f peak=%.0f high=%.0f lowP=%.0f",
                       zero, peak, high, lowP))
    end
    if tirePressureMuScale ~= nil and tirePressureAt ~= nil then
      local nom = tirePressureMuScale(2.4)
      local low = tirePressureMuScale(1.4)
      local hot = tirePressureAt(90, 2.4)
      local cold = tirePressureAt(10, 2.4)
      ok(r, "rv tire pressure",
         nom > low and nom > 0.99 and hot > 2.4 and cold < 2.4 and
         tirePressureMuScale(1.0) >= 0.72 and tirePressureMuScale(4.0) >= 0.72,
         string.format("muNom=%.2f muLow=%.2f pCold=%.2f pHot=%.2f",
                       nom, low, cold, hot))
    end
    if tireRibStep ~= nil and tireRibMu ~= nil then
      -- Four-rib tread band: cornering heats the outer ribs (3-4), braking
      -- heats the inner ones (1-2); heat conducts sideways between ribs.
      local cor = { 22.0, 22.0, 22.0, 22.0 }
      local brk = { 22.0, 22.0, 22.0, 22.0 }
      for _ = 1, 120 do
        cor = tireRibStep(cor, 8.0, 0.3, 6.0, 4000, 1 / 60, 22)
        brk = tireRibStep(brk, 8.0, 6.0, 0.1, 4000, 1 / 60, 22)
      end
      local corOuter = (cor[3] + cor[4]) * 0.5
      local corInner = (cor[1] + cor[2]) * 0.5
      local brkInner = (brk[1] + brk[2]) * 0.5
      local brkOuter = (brk[3] + brk[4]) * 0.5
      ok(r, "rv tire rib tread",
         corOuter > corInner + 1.0 and brkInner > brkOuter + 1.0 and
         cor[4] >= cor[1] and brk[1] >= brk[4],
         string.format("corner in/out=%.0f/%.0f brake in/out=%.0f/%.0f",
                       corInner, corOuter, brkInner, brkOuter))
      -- Conduction: a hot rib bleeds into a cold neighbour even with no slip.
      local hot = { 100.0, 30.0, 22.0, 22.0 }
      for _ = 1, 60 do hot = tireRibStep(hot, 0.0, 0.0, 0.0, 0.0, 1 / 60, 22) end
      ok(r, "rv tire rib conduction",
         hot[1] < 100.0 and hot[2] > 30.0 + 2.0 and hot[3] > 22.0,
         string.format("ribs=%.0f/%.0f/%.0f/%.0f", hot[1], hot[2], hot[3], hot[4]))
      -- Per-side mu: overheated inner ribs reduce drive grip while the cool
      -- outer side keeps cornering grip.
      local ribs = { 130.0, 130.0, 30.0, 30.0 }
      local wear = { 100.0, 100.0, 100.0, 100.0 }
      local mLong = tireRibMu(0.9, ribs, wear, 1, 2)
      local mLat = tireRibMu(0.9, ribs, wear, 3, 4)
      ok(r, "rv tire rib mu",
         mLong < mLat - 0.05 and mLat > 0.80 and mLong > 0.5,
         string.format("muLong=%.2f muLat=%.2f", mLong, mLat))
    end
    if ackermannAngles ~= nil then
      local l, rr = ackermannAngles(0.5, 5.84, 2.52)
      local l2, r2 = ackermannAngles(-0.5, 5.84, 2.52)
      ok(r, "rv ackermann steering",
         l > rr and rr > 0.1 and (l - rr) > 0.05 and
         math.abs(l2 + rr) < 1e-6 and math.abs(r2 + l) < 1e-6 and
         math.abs(r2) > math.abs(l2) and r2 < -0.1,
         string.format("L/R=%.2f/%.2f mirror=%.2f/%.2f", l, rr, l2, r2))
    end
    if steeringMaxAngle ~= nil then
      local park = steeringMaxAngle(0)
      local road = steeringMaxAngle(10)
      ok(r, "rv steering assist",
         park > 0.5 and road < 0.35 and road < park,
         string.format("park=%.2f road=%.2f", park, road))
    end
    if updateSteeringCommand ~= nil and steeringReturnRate ~= nil then
      local kCmd, kSpeed = rv.steerCmd, rv.speed
      rv.speed = 9.0
      rv.steerCmd = 0.4
      updateSteeringCommand(0, 0.1)
      local fast = rv.steerCmd
      rv.speed = 0.5
      rv.steerCmd = 0.4
      updateSteeringCommand(0, 0.1)
      local slow = rv.steerCmd
      rv.speed = 3.0
      rv.steerCmd = 0
      for _ = 1, 40 do updateSteeringCommand(1, 0.05) end
      local held = rv.steerCmd
      ok(r, "rv steering self-center",
         fast < slow and held > 0.4,
         string.format("fast=%.2f slow=%.2f held=%.2f", fast, slow, held))
      rv.steerCmd, rv.speed = kCmd, kSpeed
    end
    if slipTraction ~= nil then
      local fwd = slipTraction(0.6, 3000, 0.5)
      local brake = slipTraction(0.6, 3000, -0.1)
      local empty = slipTraction(0.6, 0, 0.5)
      ok(r, "rv slip traction",
         math.abs(fwd - 0.6 * 3000) < 1 and brake < 0 and
         math.abs(empty) < 0.001,
         string.format("fwd=%.0f brake=%.0f empty=%.0f", fwd, brake, empty))
    end
    if wheelSlipRatio ~= nil then
      local roll = wheelSlipRatio(20.0, 8.0, 0.40)      -- 20*0.4 = 8 m/s
      local spin = wheelSlipRatio(40.0, 8.0, 0.40)      -- wheel spinning up
      local lock = wheelSlipRatio(0.0, 8.0, 0.40)       -- locked wheel
      ok(r, "rv wheel slip ratio",
         math.abs(roll) < 0.001 and spin > 0.2 and lock < -0.5,
         string.format("roll=%.2f spin=%.2f lock=%.2f", roll, spin, lock))
    end
    if absBrakeScale ~= nil and tractionControlScale ~= nil then
      local noSlip = absBrakeScale(0.0, 10.0, 0.12)
      local locked = absBrakeScale(0.8, 10.0, 0.12)
      local crawl = absBrakeScale(0.8, 1.0, 0.12)
      local tcFree = tractionControlScale(0.0, 10.0, 0.12)
      local tcSpin = tractionControlScale(0.5, 10.0, 0.12)
      local tcMud = tractionControlScale(0.5, 10.0, 0.32)
      ok(r, "rv abs traction control",
         noSlip == 1.0 and locked < 0.9 and crawl == 1.0 and
         tcFree == 1.0 and tcSpin < 1.0 and tcMud > tcSpin,
         string.format("abs=%.2f/%.2f/%.2f tc=%.2f/%.2f/%.2f",
                       noSlip, locked, crawl, tcFree, tcSpin, tcMud))
    end
    if rvBody ~= nil and PhysicsGetRotation ~= nil and rvQuat ~= nil
       and rvPitchRollFromQuat ~= nil then
      local kp, kr, ky = rv.pitch, rv.roll, rv.yaw
      rv.pitch, rv.roll, rv.yaw = 0.15, -0.22, 0.8
      PhysicsSetRotation(rvBody, rvQuat())
      local q = PhysicsGetRotation(rvBody)
      local p2, r2 = rvPitchRollFromQuat(q, rv.yaw)
      ok(r, "rv attitude roundtrip",
         math.abs(p2 - 0.15) < 0.02 and math.abs(r2 + 0.22) < 0.02,
         string.format("pitch=%.2f roll=%.2f", p2, r2))
      rv.pitch, rv.roll, rv.yaw = kp, kr, ky
      PhysicsSetRotation(rvBody, rvQuat())
    end
    if rvBody ~= nil and PhysicsGetRotation ~= nil and rvQuat ~= nil
       and rvYawPitchRollFromQuat ~= nil then
      local kp, kr, ky = rv.pitch, rv.roll, rv.yaw
      rv.pitch, rv.roll, rv.yaw = 0.15, -0.22, 0.8
      PhysicsSetRotation(rvBody, rvQuat())
      local q2 = PhysicsGetRotation(rvBody)
      local y2, p2, r2 = rvYawPitchRollFromQuat(q2)
      ok(r, "rv yaw attitude roundtrip",
         math.abs(y2 - 0.8) < 0.02 and math.abs(p2 - 0.15) < 0.02 and
         math.abs(r2 + 0.22) < 0.02,
         string.format("yaw=%.2f pitch=%.2f roll=%.2f", y2, p2, r2))
      rv.pitch, rv.roll, rv.yaw = kp, kr, ky
      PhysicsSetRotation(rvBody, rvQuat())
    end
  end

  -- 地点物资箱刷在建筑内部，玩家需要从正门进入
  atNode = "ruin"
  spawnCrates()
  local ruin = nodeById("ruin")
  local scx, scz = poiCenter(ruin)
  local insideCrates = 0
  for _, c in ipairs(crates) do
    local dx, dz = c.x - scx, c.z - scz
    if math.abs(dx) < POI_HALF_X and math.abs(dz) < POI_HALF_Z then
      insideCrates = insideCrates + 1
    end
  end
  ok(r, "crates inside POI store", insideCrates >= 4,
     string.format("%d/%d interior", insideCrates, #crates))
  despawnCrates()

  -- 加油站商店：物资箱同样在店内
  atNode = "gasstop"
  spawnCrates()
  local gas = nodeById("gasstop")
  local gx, gz = poiCenter(gas)
  local gasInside = 0
  for _, c in ipairs(crates) do
    if math.abs(c.x - gx) < POI_HALF_X and math.abs(c.z - gz) < POI_HALF_Z then
      gasInside = gasInside + 1
    end
  end
  ok(r, "crates inside gas shop", gasInside >= 4,
     string.format("%d/%d interior", gasInside, #crates))
  despawnCrates()

  -- 物流仓库：店内货架之间同样有物资箱
  atNode = "depot"
  spawnCrates()
  local dep = nodeById("depot")
  local dx, dz = poiCenter(dep)
  local depInside = 0
  for _, c in ipairs(crates) do
    if math.abs(c.x - dx) < POI_HALF_X and math.abs(c.z - dz) < POI_HALF_Z then
      depInside = depInside + 1
    end
  end
  ok(r, "crates inside warehouse", depInside >= 4,
     string.format("%d/%d interior", depInside, #crates))
  despawnCrates()

  -- 旧农场：谷仓内畜栏/干草之间也有物资箱
  atNode = "farm"
  spawnCrates()
  local farm = nodeById("farm")
  local fx, fz = poiCenter(farm)
  local farmInside = 0
  for _, c in ipairs(crates) do
    if math.abs(c.x - fx) < POI_HALF_X and math.abs(c.z - fz) < POI_HALF_Z then
      farmInside = farmInside + 1
    end
  end
  ok(r, "crates inside barn", farmInside >= 4,
     string.format("%d/%d interior", farmInside, #crates))
  despawnCrates()

  -- 购物中心：店内货架/柜台旁也有物资箱
  atNode = "mall"
  spawnCrates()
  local mall = nodeById("mall")
  local mx, mz = poiCenter(mall)
  local mallInside = 0
  for _, c in ipairs(crates) do
    if math.abs(c.x - mx) < POI_HALF_X and math.abs(c.z - mz) < POI_HALF_Z then
      mallInside = mallInside + 1
    end
  end
  ok(r, "crates inside mall", mallInside >= 4,
     string.format("%d/%d interior", mallInside, #crates))
  despawnCrates()

  -- 湖边营地：木屋内也有物资箱
  atNode = "lakecamp"
  spawnCrates()
  local lake = nodeById("lakecamp")
  local lx, lz = poiCenter(lake)
  local lakeInside = 0
  for _, c in ipairs(crates) do
    if math.abs(c.x - lx) < POI_HALF_X and math.abs(c.z - lz) < POI_HALF_Z then
      lakeInside = lakeInside + 1
    end
  end
  ok(r, "crates inside lake cabin", lakeInside >= 4,
     string.format("%d/%d interior", lakeInside, #crates))
  despawnCrates()

  -- 路边诊所：药柜/诊床旁也有物资箱
  atNode = "clinic"
  spawnCrates()
  local clinic = nodeById("clinic")
  local clx, clz = poiCenter(clinic)
  local clinicInside = 0
  for _, c in ipairs(crates) do
    if math.abs(c.x - clx) < POI_HALF_X and math.abs(c.z - clz) < POI_HALF_Z then
      clinicInside = clinicInside + 1
    end
  end
  ok(r, "crates inside clinic", clinicInside >= 4,
     string.format("%d/%d interior", clinicInside, #crates))
  despawnCrates()

  -- POI 墙体碰撞：侧墙挡住，门洞可以通过
  do
    local cx, cz = poiCenter(ruin)
    local wx = collidePoiWalls(cx, cz, cx + 6.0, cz)
    local dx, dz = collidePoiWalls(cx, cz + 2.0, cx, cz + 3.2)
    local wallOk = math.abs(wx - (cx + POI_HALF_X - 0.14)) < 1e-6
    local doorOk = math.abs(dx - cx) < 1e-6 and dz > cz + 2.6
    ok(r, "poi wall collision", wallOk and doorOk,
       string.format("wall x=%.2f door z=%.2f", wx, dz))
  end
  atNode = nil

  -- ======================= 顺路货运合同 =======================
  local dailyKeep = dailyGoal
  dailyGoal = nil  -- 货运测试期间关闭每日目标奖励，避免污染收益断言
  local milestonesKeep = milestones
  milestones = {}  -- 同时关闭长期里程碑奖励
  -- 货运板：营地生成一块可交互的牌子
  jobBoard = nil
  spawnJobBoard()
  ok(r, "job board at camp", jobBoard ~= nil,
     jobBoard and string.format("board=(%.1f,%.1f)", jobBoard.x, jobBoard.z) or "missing")

  -- 接单：货重进入 stats.weight，目的地为 45m 外的节点
  local cargo0 = jobCargoWeight()
  jobRefreshOffers("gasstop")
  local preferOk = #jobOffers > 0 and jobOffers[1].destId == "gasstop"
  ok(r, "job board target offer", preferOk,
     preferOk and jobOffers[1].destName or "no offer")
  local offerOk = #jobOffers > 0 and jobOffers[1].distance >= JOBS.minDistance and
                  jobOffers[1].crates >= 1 and (jobOffers[1].weight or 0) > 0
  local payOk = next(jobOffers[1] and jobOffers[1].pay or {}) ~= nil
  local usedOffer = jobOffers[1]
  local accepted = jobAccept(1)
  if job ~= nil then job.stops = nil; job.stopIndex = 1 end
  local weightWithCargo = stats.weight
  ok(r, "job accept loads cargo",
     offerOk and payOk and accepted and job ~= nil and
     weightWithCargo >= (job.weight or 0) and jobCargoWeight() > cargo0 and
     #jobCargoEnts > 0 and (rv.loadFxT or 0) > 0,
     string.format("offers=%d dest=%s cargo=%s x%d %.0fkg total=%.0fkg fx=%.1f",
                   #jobOffers + 1, usedOffer and usedOffer.destId or "?",
                   usedOffer and usedOffer.cargo or "?",
                   usedOffer and usedOffer.crates or 0,
                   job and job.weight or 0, weightWithCargo, rv.loadFxT or 0))

  -- 到达目的地：停靠 -> 自动结算材料/燃料 -> 货重卸下
  local dest = job and job.destId
  local matsBefore, fuelBefore, doneBefore = {}, fuel, jobDelivered
  for k, v in pairs(materials) do matsBefore[k] = v end
  atNode = dest
  phase = "parked"
  updateJobs(1 / 60)
  local paidOut = false
  for k, v in pairs(job and job.pay or usedOffer.pay or {}) do
    if (materials[k] or 0) > (matsBefore[k] or 0) then paidOut = true end
  end
  ok(r, "job delivered at destination",
     job == nil and paidOut and jobDelivered == doneBefore + 1 and
     stats.weight < weightWithCargo and #jobCargoEnts == 0 and fuel >= fuelBefore and
     (rv.unloadFxT or 0) > 0,
     string.format("dest=%s delivered=%d->%d weight=%.0f->%.0f fuel=%.0f->%.0f fx=%.1f",
                   tostring(dest), doneBefore, jobDelivered, weightWithCargo,
                   stats.weight, fuelBefore, fuel, rv.unloadFxT or 0))
  ok(r, "job result card",
     jobResult ~= nil and jobResult.ok == true and (jobResult.crates or 0) > 0,
     jobResult and string.format("cargo=%s cond=%.2f jobs=%d",
                                 tostring(jobResult.cargo), jobResult.cond or 0,
                                 jobResult.jobs or 0) or "missing")
  -- 货运往返油耗估算：随距离增加
  do
    local a = jobFuelEstimate(50)
    local b = jobFuelEstimate(200)
    ok(r, "job fuel estimate", a > 0 and b > a and
       math.abs(a - 1.6) < 1e-6 and math.abs(b - 6.4) < 1e-6,
       string.format("50m=%.1fL 200m=%.1fL", a, b))
  end

  -- 部分合同额外奖励物品：结算时进背包并写入结算卡
  do
    local k0 = inv.repairkit or 0
    jobRefreshOffers("gasstop")
    jobAccept(1)
    job.stops, job.stopIndex = nil, 1
    job.bonusItem = "repairkit"
    job.condition = 1.0
    job.pay = { metal = 1 }
    jobComplete()
    local gained = (inv.repairkit or 0) == k0 + 1
    local shown = jobResult ~= nil and jobResult.item == "repairkit"
    inv.repairkit = k0 > 0 and k0 or nil
    ok(r, "job bonus item", gained and shown,
       string.format("repairkit %d->%d shown=%s", k0,
                     k0 + (gained and 1 or 0), tostring(shown)))
  end

  -- 刷新合同：有金属才刷新，消耗 1 金属并重新抽 3 张
  do
    local km = materials.metal or 0
    jobRefreshOffers("gasstop")
    local before = #jobOffers
    materials.metal = 1
    local okReroll = jobReroll()
    local spent = (materials.metal or 0) == 0
    local again = false
    if okReroll then again = jobReroll() end
    local poor = (materials.metal or 0) == 0 and again == false
    materials.metal = km
    ok(r, "job reroll",
       okReroll and spent and poor and #jobOffers > 0,
       string.format("offers=%d spent=%s poor=%s", #jobOffers,
                     tostring(spent), tostring(poor)))
  end

  -- 货物种类：新增冷藏药品，且有对应车顶箱 prefab
  do
    local found = nil
    for _, c in ipairs(JOBS.cargo or {}) do
      if c.id == "coldmed" then found = c end
    end
    local prefab = (jobCargoPrefab and jobCargoPrefab("coldmed")) or nil
    ok(r, "job cargo variety",
       found ~= nil and found.name == "冷藏药品" and prefab ~= nil,
       string.format("cargo=%s prefab=%s",
                     found and found.name or "missing", tostring(prefab)))
  end
  ok(r, "job history",
     jobHistory ~= nil and #jobHistory >= 1 and
     tostring(jobHistory[#jobHistory]):find("→", 1, true) ~= nil,
     string.format("entries=%d last=%s", #(jobHistory or {}),
                   tostring(jobHistory and jobHistory[#jobHistory] or "-")))

  -- 放弃合同：卸货、重量还原
  jobRefreshOffers()
  jobAccept(1)
  local carried = stats.weight
  jobAbandon()
  ok(r, "job abandon unloads cargo",
     job == nil and stats.weight < carried and #jobCargoEnts == 0,
     string.format("weight %.0f->%.0f", carried, stats.weight))

  -- 货损：撞击掉货况，结算按货况打折（0.55 + 0.45*货况）
  jobRefreshOffers("gasstop")
  jobAccept(1)
  job.stops = nil
  job.stopIndex = 1
  local fullMetal = job and job.pay and job.pay.metal or 0
  jobApplyImpact(8)
  local cond = job and job.condition or 1
  local metalBefore = materials.metal or 0
  atNode = job and job.destId or nil
  updateJobs(1 / 60)
  local gained = (materials.metal or 0) - metalBefore
  ok(r, "job cargo damage",
     job == nil and cond < 0.7 and gained >= 1 and gained < fullMetal,
     string.format("cond=%.2f metal=%d/%d", cond, gained, fullMetal))

  -- 双站路线：第一站推进、第二站结算
  do
    jobRefreshOffers("gasstop")
    jobAccept(1)
    local a = nodeById("gasstop")
    local b = nodeById("ruin")
    job.stops = { { id = a.id, name = a.name }, { id = b.id, name = b.name } }
    job.stopIndex = 1
    job.destId, job.destName = a.id, a.name
    job.pay = { metal = 5, wood = 2 }
    job.condition = 1.0
    atNode, phase = a.id, "parked"
    updateJobs(1 / 60)
    local advanced = job ~= nil and (job.stopIndex or 0) == 2 and job.destId == b.id
    local m0 = materials.metal or 0
    atNode = b.id
    updateJobs(1 / 60)
    local completed = job == nil and (materials.metal or 0) > m0
    ok(r, "two-stop delivery", advanced and completed,
       string.format("advanced=%s completed=%s", tostring(advanced),
                     tostring(completed)))
  end

  -- 夜间送达 +10%（同一单、同一货况下对比白天）
  do
    local sd, sc, slast = day, clock, lastJobDay
    local jd = jobDelivered
    local function deliverAt(hour)
      day, clock, lastJobDay = 9, hour * 60, 9
      jobDelivered = 0
      jobRefreshOffers("gasstop")
      jobAccept(1)
      job.urgent, job.deadlineAbs, job.acceptedAbs = false, nil, nil
      job.stops, job.stopIndex = nil, 1
      job.pay = { metal = 10 }
      job.condition = 1.0
      local m0 = materials.metal or 0
      jobComplete()
      return (materials.metal or 0) - m0
    end
    local nightGain = deliverAt(22)
    local dayGain = deliverAt(12)
    day, clock, lastJobDay, jobDelivered = sd, sc, slast, jd
    ok(r, "night delivery bonus", nightGain == 12 and dayGain == 11,
       string.format("night=%d day=%d", nightGain, dayGain))
  end

  -- 雨天送达 +8%（与晴天同一单对比）
  do
    local sw, sc, sd, slast = weather, clock, day, lastJobDay
    local jd, js = jobDelivered, jobStreak
    local function deliverWith(weatherId)
      weather = weatherId
      day, clock, lastJobDay = 9, 12 * 60, 9
      jobDelivered, jobStreak = 0, 0
      jobRefreshOffers("gasstop")
      jobAccept(1)
      job.urgent, job.deadlineAbs, job.acceptedAbs = false, nil, nil
      job.stops, job.stopIndex = nil, 1
      job.pay = { metal = 100 }
      job.condition = 1.0
      local m0 = materials.metal or 0
      jobComplete()
      return (materials.metal or 0) - m0
    end
    local clearGain = deliverWith("clear")
    local rainGain = deliverWith("rain")
    weather, clock, day, lastJobDay = sw, sc, sd, slast
    jobDelivered, jobStreak = jd, js
    ok(r, "rain delivery bonus", rainGain == 113 and clearGain == 105,
       string.format("clear=%d rain=%d", clearGain, rainGain))
  end

  -- 雪天送达 +12%（用 20 金属的单对比：22 vs 20）
  do
    local sw, sc, sd, slast = weather, clock, day, lastJobDay
    local jd, js = jobDelivered, jobStreak
    local function deliverWith(weatherId)
      weather = weatherId
      day, clock, lastJobDay = 9, 12 * 60, 9
      jobDelivered, jobStreak = 0, 0
      jobRefreshOffers("gasstop")
      jobAccept(1)
      job.urgent, job.deadlineAbs, job.acceptedAbs = false, nil, nil
      job.stops, job.stopIndex = nil, 1
      job.pay = { metal = 20 }
      job.condition = 1.0
      local m0 = materials.metal or 0
      jobComplete()
      return (materials.metal or 0) - m0
    end
    local clearGain = deliverWith("clear")
    local snowGain = deliverWith("snow")
    weather, clock, day, lastJobDay = sw, sc, sd, slast
    jobDelivered, jobStreak = jd, js
    ok(r, "snow delivery bonus", snowGain == 24 and clearGain == 21,
       string.format("clear=%d snow=%d", clearGain, snowGain))
  end

  -- 雾天送达 +6%（用 100 金属的单对比，避免取整吃掉差异）
  do
    local sw, sc, sd, slast = weather, clock, day, lastJobDay
    local jd, js = jobDelivered, jobStreak
    local function deliverWith(weatherId)
      weather = weatherId
      day, clock, lastJobDay = 9, 12 * 60, 9
      jobDelivered, jobStreak = 0, 0
      jobRefreshOffers("gasstop")
      jobAccept(1)
      job.urgent, job.deadlineAbs, job.acceptedAbs = false, nil, nil
      job.stops, job.stopIndex = nil, 1
      job.pay = { metal = 100 }
      job.condition = 1.0
      local m0 = materials.metal or 0
      jobComplete()
      return (materials.metal or 0) - m0
    end
    local clearGain = deliverWith("clear")
    local fogGain = deliverWith("fog")
    weather, clock, day, lastJobDay = sw, sc, sd, slast
    jobDelivered, jobStreak = jd, js
    ok(r, "fog delivery bonus", fogGain == 111 and clearGain == 105,
       string.format("clear=%d fog=%d", clearGain, fogGain))
  end

  -- 送达环境加成标签：雨/雪/夜间分别出现在追踪提示里
  do
    local sw, sc = weather, clock
    weather, clock = "rain", 12 * 60
    local rain = jobDeliveryBonusTags()
    weather, clock = "snow", 12 * 60
    local snow = jobDeliveryBonusTags()
    weather, clock = "clear", 22 * 60
    local night = jobDeliveryBonusTags()
    weather, clock = "clear", 12 * 60
    local none = jobDeliveryBonusTags()
    weather, clock = sw, sc
    ok(r, "delivery bonus tags",
       rain:find("雨天", 1, true) ~= nil and
       snow:find("雪天", 1, true) ~= nil and
       night:find("夜间", 1, true) ~= nil and none == "",
       string.format("rain=%s snow=%s night=%s none=%s",
                     rain, snow, night, none))
  end

  -- 预计到达时间：按车速估算，最低 4 m/s，向上取整
  do
    local slow = jobEtaMinutes(1200, 0)
    local fast = jobEtaMinutes(1200, 10)
    local zero = jobEtaMinutes(0, 6)
    ok(r, "job eta",
       slow == 5 and fast == 2 and zero == 0,
       string.format("slow=%d fast=%d zero=%d", slow, fast, zero))
  end

  -- 完好送达奖励：货况 ≥95% 时 +5%
  do
    local full = cargoPristineBonus(1.0)
    local edge = cargoPristineBonus(0.95)
    local damaged = cargoPristineBonus(0.90)
    ok(r, "cargo pristine bonus",
       math.abs(full - 1.05) < 1e-6 and math.abs(edge - 1.05) < 1e-6 and
       math.abs(damaged - 1.0) < 1e-6,
       string.format("1.0=%.2f 0.95=%.2f 0.90=%.2f",
                     full, edge, damaged))
  end

  -- 急单提前送达奖励：剩余 ≥ 一半 +10%
  do
    local early = urgentEarlyBonus(0, 100, 10)
    local edge = urgentEarlyBonus(0, 100, 50)
    local late = urgentEarlyBonus(0, 100, 80)
    local none = urgentEarlyBonus(nil, nil, 10)
    ok(r, "urgent early bonus",
       math.abs(early - 1.10) < 1e-6 and math.abs(edge - 1.10) < 1e-6 and
       math.abs(late - 1.0) < 1e-6 and math.abs(none - 1.0) < 1e-6,
       string.format("early=%.2f edge=%.2f late=%.2f none=%.2f",
                     early, edge, late, none))
  end

  -- 省油驾驶奖励：本单油耗 ≤1.4L/100m 时报酬 +5%
  do
    local sw, sd, sc, slast = weather, day, clock, lastJobDay
    local jd, js = jobDelivered, jobStreak
    local std, stf = tripDistance, tripFuel
    local function deliverWithEco(eco)
      weather, day, clock, lastJobDay = "clear", 9, 12 * 60, 9
      jobDelivered, jobStreak = 0, 0
      jobRefreshOffers("gasstop")
      jobAccept(1)
      job.urgent, job.deadlineAbs, job.acceptedAbs = false, nil, nil
      job.stops, job.stopIndex = nil, 1
      job.pay = { metal = 20 }
      job.condition = 1.0
      if eco then
        job.startTripDist = (tripDistance or 0) - 200
        job.startTripFuel = (tripFuel or 0) - 2.0
      else
        job.startTripDist = tripDistance or 0
        job.startTripFuel = tripFuel or 0
      end
      local m0 = materials.metal or 0
      jobComplete()
      return (materials.metal or 0) - m0, jobResult.eco == true
    end
    local keepGoal = dailyGoal
    local normal, normalEco = deliverWithEco(false)
    dailyGoal = { id = "eco", name = "省油测试", target = 1, progress = 0,
                  done = false, reward = { fuel = 8 } }
    local eco, ecoGot = deliverWithEco(true)
    local ecoGoal = dailyGoal ~= nil and dailyGoal.done == true
    dailyGoal = keepGoal
    weather, day, clock, lastJobDay = sw, sd, sc, slast
    jobDelivered, jobStreak = jd, js
    tripDistance, tripFuel = std, stf
    ok(r, "eco driving bonus",
       normal == 21 and eco == 22 and normalEco == false and ecoGot == true and
       ecoGoal,
       string.format("normal=%d eco=%d flags=%s/%s goal=%s", normal, eco,
                     tostring(normalEco), tostring(ecoGot), tostring(ecoGoal)))
  end

  -- 雨天行驶：易碎货缓慢受潮掉货况
  do
    local sw, sp = weather, phase
    jobRefreshOffers("gasstop")
    jobAccept(1)
    job.stops, job.stopIndex = nil, 1
    job.fragile = true
    job.condition = 1.0
    atNode = nil
    weather, phase = "rain", "driving"
    updateJobs(10)
    local wet = job.condition or 1
    weather, phase = sw, sp
    jobAbandon()
    ok(r, "rain damages fragile cargo", wet < 1.0 and wet > 0.90,
       string.format("condition=%.3f", wet))
  end

  -- 急转弯甩货：横向加速度超阈值才掉货况，易碎货更敏感
  do
    local calm = cornerCargoLoss(0.3, false, 1.0)
    local hard = cornerCargoLoss(1.2, false, 1.0)
    local fragile = cornerCargoLoss(1.2, true, 1.0)
    local capped = cornerCargoLoss(20.0, true, 1.0)
    ok(r, "corner cargo loss",
       calm == 0 and hard > 0 and fragile > hard and capped <= 0.30,
       string.format("calm=%.3f hard=%.3f fragile=%.3f cap=%.3f",
                     calm, hard, fragile, capped))
  end

  -- 防雨篷布：带在身上时易碎货不再被雨水打湿
  do
    local sw, sp = weather, phase
    local k0 = inv.tarp or 0
    inv.tarp = (inv.tarp or 0) + 1
    jobRefreshOffers("gasstop")
    jobAccept(1)
    job.stops, job.stopIndex = nil, 1
    job.fragile = true
    job.perishable = false
    job.livestock = false
    job.condition = 1.0
    atNode = nil
    weather, phase = "rain", "driving"
    updateJobs(10)
    local dry = job.condition or 0
    weather, phase = sw, sp
    jobAbandon()
    inv.tarp = k0 > 0 and k0 or nil
    ok(r, "tarp protects cargo", dry >= 0.999,
       string.format("tarp=%d condition=%.3f", k0 + 1, dry))
  end

  -- 易腐货：行驶时货况随时间下降
  do
    local sw, sp = weather, phase
    jobRefreshOffers("gasstop")
    jobAccept(1)
    job.stops, job.stopIndex = nil, 1
    job.fragile = false
    job.perishable = true
    job.condition = 1.0
    atNode = nil
    weather, phase = "clear", "driving"
    updateJobs(10)
    local rot = job.condition or 1
    job.perishable = false
    job.condition = 1.0
    updateJobs(10)
    local fresh = job.condition or 1
    weather, phase = sw, sp
    jobAbandon()
    ok(r, "perishable cargo", rot < 1.0 and rot > 0.9 and fresh == 1.0,
       string.format("rot=%.3f fresh=%.3f", rot, fresh))
  end

  -- 通电冰箱延缓易腐货变质：速度减半
  do
    local cold = perishableDecayRate(true)
    local warm = perishableDecayRate(false)
    ok(r, "perishable cold storage",
       cold > 0 and cold < warm and math.abs(cold - 0.001) < 1e-9 and
       math.abs(warm - 0.002) < 1e-9,
       string.format("cold=%.4f warm=%.4f", cold, warm))
  end

  -- 易碎货：撞击掉货况翻倍（0.10 vs 0.05 per m/s）
  do
    jobRefreshOffers("gasstop")
    jobAccept(1)
    job.fragile = true
    local c0 = job.condition
    jobApplyImpact(2.0)
    local fragileDrop = c0 - job.condition
    jobAbandon()
    jobRefreshOffers("gasstop")
    jobAccept(1)
    job.fragile = false
    c0 = job.condition
    jobApplyImpact(2.0)
    local normalDrop = c0 - job.condition
    jobAbandon()
    ok(r, "fragile cargo damage",
       math.abs(fragileDrop - 0.20) < 1e-6 and math.abs(normalDrop - 0.10) < 1e-6,
       string.format("fragile=%.2f normal=%.2f", fragileDrop, normalDrop))
  end

  -- 活畜：撞击掉货况比普通货更多（0.12 vs 0.05 per m/s）
  do
    jobRefreshOffers("gasstop")
    jobAccept(1)
    job.livestock = true
    job.fragile = false
    local c0 = job.condition
    jobApplyImpact(2.0)
    local liveDrop = c0 - job.condition
    jobAbandon()
    ok(r, "livestock cargo damage",
       math.abs(liveDrop - 0.24) < 1e-6,
       string.format("livestock=%.2f", liveDrop))
  end

  -- 危险品：重撞泄漏并伤驾驶员（drop > 4）
  do
    local light = hazmatLeakDamage(3.0)
    local hit = hazmatLeakDamage(7.0)
    local cap = hazmatLeakDamage(100)
    local kh = needs.health
    jobRefreshOffers("gasstop")
    jobAccept(1)
    job.hazmat, job.fragile, job.livestock = true, false, false
    needs.health = 100
    jobApplyImpact(7.0)
    local hurt = (needs.health or 100) < 100
    local drop = 1.0 - (job.condition or 1)
    needs.health = kh
    jobAbandon()
    ok(r, "hazmat cargo",
       light == 0 and hit > 0 and cap <= 12 and hurt and drop >= 0.5,
       string.format("light=%.0f hit=%.0f cap=%.0f hurt=%s drop=%.2f",
                     light, hit, cap, tostring(hurt), drop))
  end

  -- 限时急单：超时作废，不结算报酬
  jobRefreshOffers("gasstop")
  jobAccept(1)
  job.urgent = true
  job.deadlineAbs = jobNowAbs() - 1
  local metalExpire0 = materials.metal or 0
  updateJobs(1 / 60)
  ok(r, "job urgent expiry",
     job == nil and (materials.metal or 0) == metalExpire0,
     string.format("job=%s metal=%d", tostring(job), materials.metal or 0))
  atNode = nil

  -- 公路轮廓桩：沿路肩生成、避开营地/建筑
  do
    local n = #roadMarkers
    local bad = 0
    for _, m in ipairs(roadMarkers) do
      if m.x * m.x + m.z * m.z < 12 * 12 then bad = bad + 1 end
    end
    ok(r, "roadside delineators", n >= 16 and bad == 0,
       string.format("posts=%d nearCamp=%d", n, bad))
  end

  -- 夜间前大灯：夜里开车生成两盏聚光灯，白天/驻车移除
  do
    local savedClock, savedPhase, savedWeather = clock, phase, weather
    phase = "driving"
    weather = "clear"
    clock = 22 * 60
    updateVehicleLights()
    local onCount = #rvHeadlights
    clock = 12 * 60
    updateVehicleLights()
    local offCount = #rvHeadlights
    clock, phase, weather = savedClock, savedPhase, savedWeather
    ok(r, "rv night headlights", onCount == 2 and offCount == 0,
       string.format("night=%d day=%d parts=%d", onCount, offCount, #rvParts))
  end

  -- 雾天/雨天行驶也自动开灯
  do
    local dark = needHeadlights(true, true, "clear")
    local fog = needHeadlights(true, false, "fog")
    local rain = needHeadlights(true, false, "rain")
    local clear = needHeadlights(true, false, "clear")
    local parked = needHeadlights(false, true, "clear")
    ok(r, "headlights in weather",
       dark and fog and rain and not clear and not parked,
       string.format("night=%s fog=%s rain=%s clear=%s parked=%s",
                     tostring(dark), tostring(fog), tostring(rain),
                     tostring(clear), tostring(parked)))
  end

  -- 夜空/雾色：新引擎绑定把昼夜曲线写进 SceneEnvironment
  do
    local okApi = SetEnvironmentField ~= nil
    local applied = okApi and SetEnvironmentField("skyTop", { r = 0.02, g = 0.03, b = 0.07, a = 1 })
    local applied2 = okApi and SetEnvironmentField("ambientStrength", 0.16) == true
    ok(r, "rv night sky environment", applied == true and applied2,
       "SetEnvironmentField skyTop/ambientStrength")
  end

  -- 营地陈设：遮阳棚/木桌凳/物资箱/营地灯
  do
    local n = #campProps
    local awning = 0
    for _, p in ipairs(campProps) do
      if p.x > 3.0 and p.x < 6.0 and p.z > 3.8 and p.z < 6.4 then
        awning = awning + 1
      end
    end
    ok(r, "camp props", n >= 12 and awning >= 8,
       string.format("props=%d awning=%d", n, awning))
  end

  -- 刹车/倒车尾灯：自发光网格显隐切换
  do
    rv.brakeTest = true
    updateTaillights()
    local onLit = rv.brakeLit
    rv.brakeTest = nil
    rv.braking = false
    local savedSpeed = rv.speed or 0
    rv.speed = 1.0
    updateTaillights()
    local offLit = rv.brakeLit
    rv.speed = -1.0
    updateTaillights()
    local revLit = rv.reverseLit == true and rv.brakeLit == true
    rv.speed = savedSpeed
    updateTaillights()
    ok(r, "rv brake lights",
       onLit == true and offLit == false and revLit and
       #rvTaillights == 2 and #rvReverselights == 2,
       string.format("lamps=%d/%d on=%s off=%s rev=%s", #rvTaillights,
                     #rvReverselights, tostring(onLit), tostring(offLit),
                     tostring(revLit)))
  end

  -- 首次抵达奖励：每个地点只发一次
  do
    local nd = nodeById("clinic")
    local metal0, cloth0 = materials.metal or 0, materials.cloth or 0
    local t1 = firstVisitBonus(nd)
    local metal1, cloth1 = materials.metal or 0, materials.cloth or 0
    local t2 = firstVisitBonus(nd)
    local metal2 = materials.metal or 0
    ok(r, "poi first-visit bonus",
       t1 ~= nil and t2 == nil and metal1 > metal0 and cloth1 > cloth0 and metal2 == metal1,
       string.format("metal %d->%d->%d cloth %d->%d",
                     metal0, metal1, metal2, cloth0, cloth1))
  end

  -- 燃油曲线：低速巡航比全速省油，载重再加成，怠速有基线
  do
    local rIdle = fuelBurnRate(0.0, 1.0, 0)
    local rLow  = fuelBurnRate(2.0, 1.0, 1)
    local rHigh = fuelBurnRate(DRIVE.maxSpeed, 1.0, 1)
    local rLoad = fuelBurnRate(DRIVE.maxSpeed, 1.3, 1)
    ok(r, "rv fuel burn curve",
       rIdle > 0 and rIdle < rLow and rLow < rHigh and rHigh < rLoad,
       string.format("idle=%.3f low=%.3f high=%.3f loaded=%.3f L/s",
                     rIdle, rLow, rHigh, rLoad))
  end

  -- 非铺装路面更费油：草地/泥土/泥泞依次升高
  do
    local road = surfaceFuelScale("asphalt")
    local gravel = surfaceFuelScale("gravel")
    local grass = surfaceFuelScale("grass")
    local mud = surfaceFuelScale("mud")
    ok(r, "surface fuel scale",
       road == 1.0 and gravel > road and grass > gravel and mud > grass,
       string.format("road=%.2f gravel=%.2f grass=%.2f mud=%.2f",
                     road, gravel, grass, mud))
  end

  -- 非铺装路面压低极速/加速：泥泞最慢
  do
    local road = surfaceSpeedScale("asphalt")
    local gravel = surfaceSpeedScale("gravel")
    local grass = surfaceSpeedScale("grass")
    local mud = surfaceSpeedScale("mud")
    ok(r, "surface speed scale",
       road == 1.0 and gravel < road and grass < gravel and mud < grass,
       string.format("road=%.2f gravel=%.2f grass=%.2f mud=%.2f",
                     road, gravel, grass, mud))
  end

  -- 坡度油耗：上坡更费、下坡更省，且有上下限
  do
    local up = slopeFuelScale(0.15)
    local flat = slopeFuelScale(0)
    local down = slopeFuelScale(-0.15)
    local steep = slopeFuelScale(2.0)
    ok(r, "slope fuel scale",
       up > flat and flat > down and down >= 0.65 and steep <= 1.60,
       string.format("up=%.2f flat=%.2f down=%.2f steep=%.2f",
                     up, flat, down, steep))
  end

  -- 天气油耗：雪天最费、雨天次之、晴天基准
  do
    local clear = weatherFuelScale("clear")
    local rain = weatherFuelScale("rain")
    local snow = weatherFuelScale("snow")
    ok(r, "weather fuel scale",
       clear == 1.0 and rain > clear and snow > rain,
       string.format("clear=%.2f rain=%.2f snow=%.2f", clear, rain, snow))
  end

  -- 坡度车速/加速：上坡慢、下坡略快
  do
    local up = slopeSpeedScale(0.15)
    local flat = slopeSpeedScale(0)
    local down = slopeSpeedScale(-0.15)
    local upAcc = slopeAccelScale(0.15)
    local flatAcc = slopeAccelScale(0)
    ok(r, "slope speed scale",
       up < flat and down > flat and up >= 0.70 and down <= 1.15 and
       upAcc < flatAcc,
       string.format("up=%.2f flat=%.2f down=%.2f acc=%.2f/%.2f",
                     up, flat, down, upAcc, flatAcc))
  end

  -- 车内灯开关：Despawn 真正熄灯，再开时重新生成
  do
    setInteriorLights(false)
    local off = 0
    for _, p in ipairs(rvInterior or {}) do
      if p.pre == "rv_lamp" and p.ent == nil then off = off + 1 end
    end
    setInteriorLights(true)
    local on = 0
    for _, p in ipairs(rvInterior or {}) do
      if p.pre == "rv_lamp" and p.ent ~= nil then on = on + 1 end
    end
    ok(r, "rv interior light toggle", off >= 1 and on >= 1 and interiorLightsOn == true,
       string.format("off=%d on=%d state=%s", off, on, tostring(interiorLightsOn)))
  end

  -- 行程电脑：百公里油耗换算
  do
    local sd, sf = tripDistance, tripFuel
    tripDistance, tripFuel = 1000, 30
    local e1 = tripL100()
    tripDistance, tripFuel = 10, 5
    local e2 = tripL100()
    tripDistance, tripFuel = sd, sf
    ok(r, "rv trip economy", math.abs(e1 - 3.0) < 1e-6 and e2 == 0,
       string.format("30L/1000m=%.2f L/100km short=%s", e1, tostring(e2)))
  end

  -- 胎压不足增加油耗（2.4bar 为基准，不改变原曲线）
  do
    local base = tirePressureFuelScale(2.4)
    local low = tirePressureFuelScale(1.6)
    local high = tirePressureFuelScale(2.8)
    ok(r, "tire pressure fuel",
       math.abs(base - 1.0) < 1e-6 and low > 1.1 and high > 1.0,
       string.format("2.4=%.2f 1.6=%.2f 2.8=%.2f", base, low, high))
  end

  -- 胎压不足降低极速：2.2bar 以上无影响，越低越慢且有下限
  do
    local base = tirePressureSpeedScale(2.4)
    local edge = tirePressureSpeedScale(2.2)
    local low = tirePressureSpeedScale(1.6)
    local floorV = tirePressureSpeedScale(1.0)
    ok(r, "tire pressure speed",
       base == 1.0 and edge == 1.0 and low < 1.0 and low > 0.9 and
       floorV >= 0.90,
       string.format("2.4=%.2f 2.2=%.2f 1.6=%.3f 1.0=%.2f",
                     base, edge, low, floorV))
  end

  -- 电瓶老化：低于 40% 动力略降，0% 时 ×0.85
  do
    local full = batteryPowerScale(100)
    local mid = batteryPowerScale(40)
    local low = batteryPowerScale(20)
    local empty = batteryPowerScale(0)
    ok(r, "battery power scale",
       full == 1.0 and mid == 1.0 and low < 1.0 and empty < low and
       math.abs(empty - 0.85) < 1e-6,
       string.format("100=%.2f 40=%.2f 20=%.3f 0=%.2f",
                     full, mid, low, empty))
  end

  -- 胎压偏低预警阈值（1.95bar 以下）
  do
    ok(r, "tire pressure warning",
       tirePressureLow(1.8) and not tirePressureLow(2.4) and
       not tirePressureLow(2.0),
       string.format("1.8=%s 2.0=%s 2.4=%s",
                     tostring(tirePressureLow(1.8)),
                     tostring(tirePressureLow(2.0)),
                     tostring(tirePressureLow(2.4))))
  end

  -- 胎压影响抓地：2.3bar 以上满抓地，偏低逐渐下降且有下限
  do
    local low = tirePressureGripScale(1.6)
    local mid = tirePressureGripScale(1.8)
    local okp = tirePressureGripScale(2.0)
    local full = tirePressureGripScale(2.4)
    ok(r, "tire pressure grip",
       math.abs(low - 0.85) < 1e-6 and mid > low and okp > mid and
       full == 1.0,
       string.format("1.6=%.2f 1.8=%.2f 2.0=%.2f 2.4=%.2f",
                     low, mid, okp, full))
  end

  -- 机油寿命：低于 20% 动力下降；加油站按缺失比例收金属更换
  do
    local keepOil = oilLife
    local sm = materials.metal or 0
    oilLife = 100
    local fullScale = engineOilPowerScale()
    local fullCost = oilChangeCost()
    oilLife = 10
    local lowScale = engineOilPowerScale()
    local lowCost = oilChangeCost()
    materials.metal = 10
    local okOil, cost = serviceOilAtGasstop()
    local refilled = oilLife == 100 and
                     (materials.metal or 0) == 10 - lowCost
    oilLife = keepOil
    materials.metal = sm
    ok(r, "engine oil service",
       fullScale == 1.0 and lowScale < 1.0 and fullCost == 0 and
       lowCost > 0 and okOil and refilled and cost == lowCost,
       string.format("full=%.2f low=%.2f cost=%d refilled=%s",
                     fullScale, lowScale, lowCost, tostring(refilled)))
  end

  -- 冷却液：过低时升温更快，加油站 1 金属补满
  do
    local keepCool, keepMetal = coolant, materials.metal or 0
    coolant = 100
    local full = coolantHeatScale(coolant)
    local low = coolantHeatScale(10)
    local cheap = coolantServiceCost()
    coolant = 50
    local need = coolantServiceCost()
    materials.metal = 5
    local okCool = serviceCoolantAtGasstop()
    local refilled = (coolant or 0) == 100 and
                     (materials.metal or 0) == 4
    coolant, materials.metal = keepCool, keepMetal
    ok(r, "coolant service",
       full == 1.0 and low > 1.0 and low <= 1.5 and cheap == 0 and
       need == 1 and okCool and refilled,
       string.format("full=%.2f low=%.2f need=%d refilled=%s",
                     full, low, need, tostring(refilled)))
  end

  -- 加油站快速维修：最差部件 <70% 时修 +25%，按缺失收金属
  do
    local kb, ke, kt = dur, parts.engine, parts.tires
    local kbat, km = parts.battery, materials.metal or 0
    local sj = jobDelivered
    jobDelivered = 0
    dur, parts.engine, parts.tires, parts.battery = 100, 40, 100, 100
    local cost = repairServiceCost()
    materials.metal = 5
    local repaired, spent, note = serviceRepairAtGasstop()
    local gotEngine = parts.engine or 0
    local gotMetal = materials.metal or 0
    dur, parts.engine, parts.tires = kb, ke, kt
    parts.battery, materials.metal = kbat, km
    jobDelivered = sj
    ok(r, "gas station repair",
       cost == 3 and repaired and gotEngine == 65 and
       gotMetal == 5 - cost,
       string.format("cost=%d engine=%.0f metal=%d", cost, gotEngine,
                     gotMetal))
  end

  -- 诊所包扎：生命 <70% 时花 1 布料 + 2L 水，恢复 +30
  do
    local kh, kc, kw = needs.health, materials.cloth, water
    needs.health, materials.cloth, water = 40, 1, 10
    local healed, healCost, healNote = serviceClinicAtRv()
    local gotHealth = needs.health
    local leftCloth = materials.cloth
    local leftWater = water
    needs.health, materials.cloth, water = kh, kc, kw
    ok(r, "clinic treatment",
       healed and healCost == 1 and gotHealth == 70 and leftCloth == 0 and
       math.abs(leftWater - 8) < 1e-6,
       string.format("health=%.0f cloth=%d water=%.0f", gotHealth,
                     leftCloth, leftWater))
  end

  -- 冷却液不足提示：低于 20% 才提示
  do
    local low = coolantLowText(10)
    local edge = coolantLowText(20)
    local okCool = coolantLowText(60)
    ok(r, "coolant low hint",
       low ~= nil and edge == nil and okCool == nil,
       string.format("low=%s edge=%s ok=%s", tostring(low),
                     tostring(edge), tostring(okCool)))
  end

  -- 水箱快空提示：低于 10% 才提示
  do
    local low = waterLowText(5, 120)
    local edge = waterLowText(12, 120)
    local okW = waterLowText(60, 120)
    ok(r, "low water hint",
       low ~= nil and edge == nil and okW == nil,
       string.format("low=%s edge=%s ok=%s", tostring(low),
                     tostring(edge), tostring(okW)))
  end

  -- 机油劣化增加油耗（最高 +8%）
  do
    local full = engineOilFuelScale(100)
    local mid = engineOilFuelScale(20)
    local low = engineOilFuelScale(0)
    ok(r, "oil fuel penalty",
       full == 1.0 and mid > full and low > mid and low <= 1.08,
       string.format("100=%.3f 20=%.3f 0=%.3f", full, mid, low))
  end

  -- 引擎升级优化燃烧：Lv0 1.0，逐级降到 Lv3 0.94
  do
    local l0 = engineUpgradeFuelScale(0)
    local l1 = engineUpgradeFuelScale(1)
    local l3 = engineUpgradeFuelScale(3)
    ok(r, "engine upgrade fuel",
       l0 == 1.0 and l1 < l0 and l3 < l1 and
       math.abs(l3 - 0.94) < 1e-6,
       string.format("0=%.2f 1=%.2f 3=%.2f", l0, l1, l3))
  end

  -- 机油严重不足时继续行驶会加速发动机磨损
  do
    local none = oilEngineWearRate(100)
    local okOil = oilEngineWearRate(20)
    local low = oilEngineWearRate(10)
    local empty = oilEngineWearRate(0)
    ok(r, "oil engine wear",
       none == 0 and okOil == 0 and low > 0 and empty > low,
       string.format("100=%.4f 20=%.4f 10=%.4f 0=%.4f",
                     none, okOil, low, empty))
  end

  -- 加油站充气：低胎压收 1 金属并充回 2.4bar、清除爆胎
  do
    local keep = rv.tirePressureCold
    local keepBlow = rv.blowout
    local sm = materials.metal or 0
    resetTirePressure()
    local normalCost = tireAirCost()
    rv.tirePressureCold = { 1.8, 1.8, 1.8, 1.8 }
    rv.blowout = true
    local lowCost = tireAirCost()
    materials.metal = 5
    local okAir, cost = serviceTiresAtGasstop()
    local full = rv.tirePressureCold[1] == 2.4 and rv.blowout == false
    local charged = (materials.metal or 0) == 5 - lowCost
    rv.tirePressureCold = keep
    rv.blowout = keepBlow
    materials.metal = sm
    ok(r, "tire air service",
       normalCost == 0 and lowCost == 1 and okAir and full and
       charged and cost == lowCost,
       string.format("normal=%d low=%d full=%s charged=%s",
                     normalCost, lowCost, tostring(full), tostring(charged)))
  end

  -- 联机机油同步：按服务器值设置并夹紧到 0–100
  do
    local keep = oilLife
    setOilLife(55)
    local mid = math.abs((oilLife or 0) - 55) < 1e-6
    setOilLife(200)
    local high = (oilLife or 0) == 100
    setOilLife(-10)
    local low = (oilLife or -1) == 0
    oilLife = keep
    ok(r, "oil sync", mid and high and low,
       string.format("mid=%s high=%s low=%s", tostring(mid),
                     tostring(high), tostring(low)))
  end

  -- 车况同步串：dur:engine:tires:battery:temp:brake + 四轮压/温/磨损
  do
    local kd, ke, kt = dur, parts.engine, parts.tires
    local kb, kt2 = parts.battery, engineTemp
    local kheat = brakeHeat
    local kp, ktt, kw = rv.tirePressureCold, rv.tireTemps, rv.tireWear
    local kbw, kbb = rv.blowoutWheel, rv.blowout
    local kdirt = rv.dirt
    local karmor = upgrades.armor
    dur, parts.engine, parts.tires, parts.battery = 80, 70, 60, 55
    engineTemp, brakeHeat = 90, 0.4
    rv.tirePressureCold = { 2.4, 2.2, 2.0, 1.8 }
    rv.tireTemps = { 22, 40, 60, 80 }
    rv.tireWear = { 90, 80, 70, 60 }
    rv.blowout, rv.blowoutWheel = true, 4
    rv.dirt, upgrades.armor = 0.4, 2
    local token = vehicleConditionToken()
    local veh = parseVehicleCondition(token)
    local bad = parseVehicleCondition("nope")
    local wheelOk = veh ~= nil and veh.pressures ~= nil and
                    math.abs(veh.pressures[4] - 1.8) < 1e-6 and
                    veh.temps[4] == 80 and veh.wear[4] == 60 and
                    math.abs((veh.brake or 0) - 0.4) < 1e-6 and
                    veh.blowout == true and veh.blowoutWheel == 4 and
                    math.abs((veh.dirt or 0) - 0.4) < 1e-6 and
                    veh.armor == 2
    dur, parts.engine, parts.tires = kd, ke, kt
    parts.battery, engineTemp, brakeHeat = kb, kt2, kheat
    rv.tirePressureCold, rv.tireTemps, rv.tireWear = kp, ktt, kw
    rv.blowout, rv.blowoutWheel = kbb, kbw
    rv.dirt, upgrades.armor = kdirt, karmor
    ok(r, "vehicle condition sync",
       veh ~= nil and veh.dur == 80 and veh.engine == 70 and
       veh.tires == 60 and veh.battery == 55 and veh.temp == 90 and
       wheelOk and bad == nil,
       string.format("token=%s", tostring(token)))
  end

  -- 保养面板数据：10 项、良好时全部 ok、低值时标红、四轮数据
  do
    local kd, ke, kt = dur, parts.engine, parts.tires
    local kb, ko, kc = parts.battery, oilLife, coolant
    local ktemp, kheat = engineTemp, brakeHeat
    local kdirt = rv.dirt
    dur, parts.engine, parts.tires, parts.battery = 100, 100, 100, 100
    oilLife, coolant, engineTemp, brakeHeat = 100, 100, 20, 0
    rv.dirt = 0
    local rows = maintenanceRows()
    local allOk = #rows == 10
    for _, r in ipairs(rows) do
      if not r.ok then allOk = false end
    end
    dur, oilLife, coolant = 10, 5, 5
    local flagged = false
    for _, r in ipairs(maintenanceRows()) do
      if not r.ok then flagged = true end
    end
    local wheels = wheelConditionRows()
    dur, parts.engine, parts.tires = kd, ke, kt
    parts.battery, oilLife, coolant = kb, ko, kc
    engineTemp, brakeHeat = ktemp, kheat
    rv.dirt = kdirt
    ok(r, "maintenance panel",
       allOk and flagged and #wheels == 4 and wheels[1].pressure ~= nil,
       string.format("rows=%d ok=%s flagged=%s wheels=%d",
                     #rows, tostring(allOk), tostring(flagged), #wheels))
  end

  -- 车身积灰：泥泞/雨天更脏，脏车略费油；加油站/营地洗车
  do
    local road = dirtGainPerMeter("asphalt", "clear")
    local mud = dirtGainPerMeter("mud", "clear")
    local rain = dirtGainPerMeter("asphalt", "rain")
    local clean = dirtFuelScale(0)
    local dirty = dirtFuelScale(1)
    local kd, km, kw = rv.dirt, materials.metal or 0, water
    rv.dirt, materials.metal = 0.8, 5
    local okWash, washCost = serviceWashAtGasstop()
    local washed = (rv.dirt or 0) == 0 and (materials.metal or 0) == 4
    rv.dirt, materials.metal = 0.1, 5
    local okSkip, skipCost = serviceWashAtGasstop()
    rv.dirt, water = 0.7, 20
    local okCamp = washAtCampWithWater()
    local campWashed = (rv.dirt or 0) == 0 and math.abs((water or 0) - 15) < 1e-6
    rv.dirt, water = 0.7, 2
    local poorCamp = washAtCampWithWater()
    rv.dirt, materials.metal, water = kd, km, kw
    ok(r, "vehicle dirt",
       road > 0 and mud > road and rain > road and clean == 1.0 and
       dirty > clean and okWash and washCost == 1 and washed and
       okSkip and skipCost == 0 and okCamp and campWashed and poorCamp == false,
       string.format("mud/road=%.2f rain/road=%.2f dirtyFuel=%.3f wash=%s/%d",
                     mud / road, rain / road, dirty, tostring(okWash), washCost))
  end

  -- 车身太脏提示：>=80% 才提示，干净时返回 nil
  do
    local dirty = dirtWarningText(0.85)
    local clean = dirtWarningText(0.4)
    ok(r, "dirt warning",
       dirty ~= nil and dirty:find("洗车", 1, true) ~= nil and clean == nil,
       string.format("dirty=%s clean=%s", tostring(dirty), tostring(clean)))
  end

  -- 驾驶中喷玻璃水：消耗 1L 水，脏污 -0.3；太干净或没水不生效
  do
    local kd, kw = rv.dirt, water
    local kt, kv = toast.text, toast.t
    rv.dirt, water = 0.8, 10
    local okWash = useWindshieldWasher()
    local gotDirt, gotWater = rv.dirt, water
    rv.dirt, water = 0.1, 10
    local clean = useWindshieldWasher()
    rv.dirt, water = 0.8, 0.2
    local dry = useWindshieldWasher()
    rv.dirt, water = kd, kw
    toast.text, toast.t = kt, kv
    ok(r, "windshield washer",
       okWash and math.abs(gotDirt - 0.5) < 1e-6 and
       math.abs(gotWater - 9) < 1e-6 and clean == false and dry == false,
       string.format("ok=%s dirt=%.2f water=%.1f clean=%s dry=%s",
                     tostring(okWash), gotDirt, gotWater,
                     tostring(clean), tostring(dry)))
  end

  -- 太阳能板积灰：干净 100%，脏满最多损失 25%
  do
    local clean = solarDirtScale(0)
    local mid = solarDirtScale(0.5)
    local dirty = solarDirtScale(1)
    ok(r, "solar dirt scale",
       clean == 1.0 and mid < clean and dirty < mid and dirty >= 0.75,
       string.format("0=%.2f 0.5=%.2f 1=%.2f", clean, mid, dirty))
  end

  -- 夜间远光招感染者：只在“远光 + 夜晚”成立
  do
    local both = highBeamAttracts(true, true)
    local day = highBeamAttracts(true, false)
    local off = highBeamAttracts(false, true)
    ok(r, "high beam risk",
       both == true and day == false and off == false,
       string.format("night=%s day=%s off=%s", tostring(both),
                     tostring(day), tostring(off)))
  end

  -- 夜间远光惊走鹿群：只在高光 + 夜晚成立
  do
    local both = deerAvoidsHighBeam(true, true)
    local day = deerAvoidsHighBeam(true, false)
    local off = deerAvoidsHighBeam(false, true)
    ok(r, "deer avoid high beam",
       both == true and day == false and off == false,
       string.format("night=%s day=%s off=%s", tostring(both),
                     tostring(day), tostring(off)))
  end

  -- 保养警示：全部良好为 false，任意一项低则 true
  do
    local ko, kc = oilLife, coolant
    oilLife, coolant = 100, 100
    local durKeep = dur
    dur = 100
    local okAll = maintenanceWarningActive()
    oilLife = 10
    local warned = maintenanceWarningActive()
    oilLife, coolant = ko, kc
    dur = durKeep
    ok(r, "maintenance warning",
       okAll == false and warned == true,
       string.format("ok=%s warned=%s", tostring(okAll), tostring(warned)))
  end

  -- 保养花费提示：需要保养时列出费用（含洗车），全部良好时返回 nil
  do
    local ko, kc, kp = oilLife, coolant, rv.tirePressureCold
    local kdirt = rv.dirt
    oilLife, coolant = 50, 40
    rv.tirePressureCold = { 1.8, 1.8, 1.8, 1.8 }
    rv.dirt = 0.9
    local need = maintenanceCostText()
    oilLife, coolant = 100, 100
    rv.tirePressureCold = { 2.4, 2.4, 2.4, 2.4 }
    rv.dirt = 0
    local none = maintenanceCostText()
    oilLife, coolant, rv.tirePressureCold = ko, kc, kp
    rv.dirt = kdirt
    ok(r, "maintenance cost",
       need ~= nil and need:find("机油", 1, true) ~= nil and
       need:find("冷却液", 1, true) ~= nil and
       need:find("充气", 1, true) ~= nil and
       need:find("洗车", 1, true) ~= nil and none == nil,
       string.format("need=%s none=%s", tostring(need), tostring(none)))
  end

  -- 续航估算：无实测时用 1.6L/100m 基准，有实测按实测油耗
  do
    local sf, sd, sfu = fuel, tripDistance, tripFuel
    fuel, tripDistance, tripFuel = 60, 0, 0
    local base = rangeEstimateM()
    fuel, tripDistance, tripFuel = 60, 1000, 30
    local measured = rangeEstimateM()
    fuel, tripDistance, tripFuel = sf, sd, sfu
    ok(r, "range estimate",
       math.abs(base - 3750) < 1 and math.abs(measured - 2000) < 1,
       string.format("base=%.0fm measured=%.0fm", base, measured))
  end

  -- 地图探索迷雾：未去过的程序化地点在地图上只显示“?”
  do
    local gen = nil
    for _, nd in ipairs(NODES) do
      if nd.generated and gen == nil then gen = nd end
    end
    local knownBefore = gen ~= nil and mapNodeKnown(gen)
    if gen ~= nil then visitedPois[gen.id] = true end
    local knownAfter = gen ~= nil and mapNodeKnown(gen)
    local baseKnown = mapNodeKnown(nodeById("camp"))
    ok(r, "map discovery fog",
       gen ~= nil and knownBefore == false and knownAfter == true and baseKnown == true,
       string.format("gen=%s before=%s after=%s base=%s",
                     gen and gen.id or "?", tostring(knownBefore),
                     tostring(knownAfter), tostring(baseKnown)))
  end

  -- 修好无线电塔后地图全解锁（不改变已探索地点集合）
  do
    local gen = nil
    for _, nd in ipairs(NODES) do
      if nd.generated and gen == nil then gen = nd end
    end
    local keepFixed = radioMastFixed
    local keepVisited = gen and visitedPois[gen.id] or nil
    if gen ~= nil then visitedPois[gen.id] = nil end
    radioMastFixed = false
    local hidden = gen ~= nil and not mapNodeKnown(gen)
    radioMastFixed = true
    local revealed = gen ~= nil and mapNodeKnown(gen)
    radioMastFixed = keepFixed
    if gen ~= nil then visitedPois[gen.id] = keepVisited end
    ok(r, "mast reveals map", hidden and revealed,
       string.format("gen=%s hidden=%s revealed=%s",
                     gen and gen.id or "?", tostring(hidden), tostring(revealed)))
  end

  -- 信号中继：修塔后到小镇触发一次总结与奖励
  do
    local keepFixed, keepDone = radioMastFixed, relayDone
    local sm = materials.metal or 0
    local se = materials.electronics or 0
    local sf, sa, sp = fuel or 0, atNode, phase
    radioMastFixed, relayDone, relayCard = true, false, nil
    atNode, phase = "town", "parked"
    local okRelay = updateRelay()
    local rewarded = (materials.metal or 0) == sm + 10 and
                     (materials.electronics or 0) == se + 5
    local card = relayCard ~= nil and (relayCard.t or 0) > 0
    local again = updateRelay()
    radioMastFixed, relayDone, relayCard = keepFixed, keepDone, nil
    materials.metal, materials.electronics, fuel = sm, se, sf
    atNode, phase = sa, sp
    ok(r, "signal relay", okRelay and rewarded and card and again == false,
       string.format("ok=%s reward=%s card=%s again=%s", tostring(okRelay),
                     tostring(rewarded), tostring(card), tostring(again)))
  end

  -- 中继后每日救援补给：当天只发一次
  do
    local keepRelay, keepDay = relayDone, relaySupplyDay
    local km, ke, kf = materials.metal or 0, materials.electronics or 0,
                       fuel or 0
    local kt, kv = toast.text, toast.t
    relayDone, relaySupplyDay = true, 0
    materials.metal, materials.electronics, fuel = 0, 0, 0
    local first = grantRelaySupply()
    local rewarded = (materials.metal or 0) == 3 and
                     (materials.electronics or 0) == 1 and (fuel or 0) >= 9
    local again = grantRelaySupply()
    relayDone, relaySupplyDay = keepRelay, keepDay
    materials.metal, materials.electronics, fuel = km, ke, kf
    toast.text, toast.t = kt, kv
    ok(r, "relay daily supply", first and rewarded and again == false,
       string.format("first=%s reward=%s again=%s", tostring(first),
                     tostring(rewarded), tostring(again)))
  end

  -- 公路电线：相邻电线杆之间两条
  do
    local n = #roadWires
    ok(r, "roadside power wires", n >= 20, string.format("wires=%d", n))
  end

  -- Roadside utility poles (Blender-authored model)
  do
    local n = #roadPoles
    local prefab = ReadText and
      ReadText("assets/prefabs/rv_utility_pole.json") or nil
    ok(r, "roadside utility poles",
       n >= 8 and prefab ~= nil and
       prefab:find("rv_utility_pole.gltf", 1, true) ~= nil,
       string.format("poles=%d prefab=%s", n, tostring(prefab ~= nil)))
  end

  -- 自动挡：车速升挡 + 挡内转速映射
  do
    local g1 = rvGearInfo(0.5)
    local g2 = rvGearInfo(2.5)
    local g3 = rvGearInfo(4.0)
    local g4, rpm4 = rvGearInfo(6.2)
    local g5 = rvGearInfo(8.0)
    ok(r, "rv gearbox",
       g1 == 1 and g2 == 2 and g3 == 3 and g4 == 4 and g5 == 5 and
       rpm4 >= VEH.gearRpmMin and rpm4 <= VEH.gearRpmMax,
       string.format("gears=%d/%d/%d/%d/%d rpm4=%.0f", g1, g2, g3, g4, g5, rpm4))
  end

  -- 自动换挡反馈：挡位变化时计数一次
  do
    local keepGear, keepSpeed = rv.lastGear, rv.speed
    local keepShifts = rv.gearShifts
    rv.lastGear, rv.speed, rv.gearShifts = 1, 2.0, 0
    local shift = updateGearShiftFx()
    local counted = (rv.gearShifts or 0) == 1
    local again = updateGearShiftFx()
    rv.lastGear, rv.speed, rv.gearShifts = keepGear, keepSpeed, keepShifts
    ok(r, "gear shift fx", shift == true and counted and again == false,
       string.format("shift=%s counted=%s again=%s", tostring(shift),
                     tostring(counted), tostring(again)))
  end

  -- 轮胎尖叫：横向 G 高且车速够快才触发
  do
    local calm = tireSquealActive(0.4, 8.0)
    local fast = tireSquealActive(0.8, 8.0)
    local slow = tireSquealActive(0.8, 2.0)
    ok(r, "tire squeal",
       calm == false and fast == true and slow == false,
       string.format("calm=%s fast=%s slow=%s", tostring(calm),
                     tostring(fast), tostring(slow)))
  end

  -- 刹车过热尖叫：热量 >0.5 且车速 >2.5 才触发
  do
    local cool = brakeSquealActive(0.2, 8)
    local slow = brakeSquealActive(0.9, 1.5)
    local hot = brakeSquealActive(0.9, 8)
    ok(r, "brake squeal",
       cool == false and slow == false and hot == true,
       string.format("cool=%s slow=%s hot=%s", tostring(cool),
                     tostring(slow), tostring(hot)))
  end

  -- 侧风：雨雪天高速才有，晴天/低速没有，幅度受限
  do
    local clear = windActive("clear", 8)
    local rainSlow = windActive("rain", 1.5)
    local rainFast = windActive("rain", 8)
    local snowFast = windActive("snow", 8)
    local maxv = 0
    for i = 0, 40 do
      local v = math.abs(windSteerAmount("snow", 8, i * 0.3))
      if v > maxv then maxv = v end
    end
    ok(r, "crosswind",
       clear == false and rainSlow == false and rainFast and snowFast and
       maxv > 0 and maxv <= 0.05,
       string.format("clear=%s slow=%s rain=%s snow=%s max=%.3f",
                     tostring(clear), tostring(rainSlow), tostring(rainFast),
                     tostring(snowFast), maxv))
  end

  -- 左右胎压不均：向低压侧轻微跑偏，且有上限
  do
    local even = tirePullSteer({ 2.4, 2.4, 2.4, 2.4 })
    local leftLow = tirePullSteer({ 1.6, 2.4, 1.6, 2.4 })
    local rightLow = tirePullSteer({ 2.4, 1.6, 2.4, 1.6 })
    local capped = tirePullSteer({ 0.5, 5.0, 0.5, 5.0 })
    ok(r, "tire pull steer",
       even == 0 and leftLow > 0 and rightLow < 0 and
       math.abs(capped) <= 0.06,
       string.format("even=%.3f left=%.3f right=%.3f cap=%.3f",
                     even, leftLow, rightLow, capped))
  end

  -- 定速巡航油门：低于目标给油，接近/超过目标松油
  do
    local below = cruiseThrottle(3.0, 6.0)
    local near = cruiseThrottle(5.9, 6.0)
    local over = cruiseThrottle(7.0, 6.0)
    local off = cruiseThrottle(3.0, 0)
    ok(r, "cruise throttle",
       below == 1 and near == 0 and over == 0 and off == 0,
       string.format("below=%d near=%d over=%d off=%d",
                     below, near, over, off))
  end

  -- 启动引擎：打火音效计数
  do
    local keep = rv.engineStarts
    rv.engineStarts = 0
    playEngineStart()
    local starts = rv.engineStarts or 0
    rv.engineStarts = keep
    ok(r, "engine start cue", starts == 1,
       string.format("starts=%d", starts))
  end

  -- 倒车提示音：挂倒挡按 0.8s 间隔响
  do
    local keepSpeed, keepT = rv.speed, reverseBeepT
    local keepBeeps = reverseBeeps
    rv.speed, reverseBeepT, reverseBeeps = -1.0, 0, 0
    local first = updateReverseBeep(0.1)
    local again = updateReverseBeep(0.1)
    local beeps = reverseBeeps or 0
    rv.speed = 2.0
    local forward = updateReverseBeep(0.1)
    rv.speed, reverseBeepT, reverseBeeps = keepSpeed, keepT, keepBeeps
    ok(r, "reverse beep", first == true and again == false and beeps == 1 and
       forward == false,
       string.format("first=%s again=%s beeps=%d forward=%s",
                     tostring(first), tostring(again), beeps,
                     tostring(forward)))
  end

  -- 夜间步行手电：夜里生成、白天移除
  do
    local sc, sfOn, sfFoot = clock, flashlightOn, onFoot
    clock = 22 * 60
    flashlightOn, onFoot = true, true
    updateFlashlight()
    local on = flashlightEnt ~= nil
    clock = 12 * 60
    updateFlashlight()
    local off = flashlightEnt == nil
    clock, flashlightOn, onFoot = sc, sfOn, sfFoot
    ok(r, "rv flashlight", on and off,
       string.format("night=%s day=%s", tostring(on), tostring(off)))
  end

  -- 营火暖光 + 可见火苗
  ok(r, "campfire glow", campfireLight ~= nil and campfire ~= nil and
     #campfireFlames >= 2,
     campfire and string.format("fire=(%.1f,%.1f) flames=%d",
                                campfire.x, campfire.z, #campfireFlames) or "missing")

  -- 篝火音效：近处可听、远处静音，靠近时会计时播放
  do
    local near = campfireAudible(4)
    local far = campfireAudible(30)
    local iv = campfireCrackleInterval()
    local px, pz, sa = player.x, player.z, onFoot
    local keepT, keepPlays = fireCrackleT, fireCracklePlays
    onFoot = true
    if campfire ~= nil then player.x, player.z = campfire.x, campfire.z end
    fireCrackleT = 0
    local p0 = fireCracklePlays or 0
    updateCampfireAudio(0.1)
    local played = (fireCracklePlays or 0) == p0 + 1
    player.x, player.z, onFoot = px, pz, sa
    fireCrackleT, fireCracklePlays = keepT, keepPlays
    ok(r, "campfire audio", near and not far and iv >= 2.2 and iv <= 3.6 and played,
       string.format("near=%s far=%s interval=%.2f played=%s",
                     tostring(near), tostring(far), iv, tostring(played)))
  end

  -- 营火炊烟：有火堆时按 4/s 发射
  do
    local keepT, keepC = campfireSmokeT, campfireSmokeCount
    campfireSmokeT, campfireSmokeCount = 0, 0
    local rate = campfireSmokeRate()
    updateCampfireSmoke(0.6)
    local emitted = (campfireSmokeCount or 0) >= 2
    local got = campfireSmokeCount or 0
    campfireSmokeT, campfireSmokeCount = keepT, keepC
    ok(r, "campfire smoke", rate > 0 and emitted,
       string.format("rate=%.0f/s emitted=%d", rate, got))
  end

  -- 夜间篝火烟味：火点着 + 夜里 + 35m 内才会招来感染者
  do
    local night = campfireSmokeRisk(true, true, 10)
    local day = campfireSmokeRisk(true, false, 10)
    local far = campfireSmokeRisk(true, true, 60)
    local off = campfireSmokeRisk(false, true, 10)
    ok(r, "campfire smoke risk",
       night == true and day == false and far == false and off == false,
       string.format("night=%s day=%s far=%s off=%s", tostring(night),
                     tostring(day), tostring(far), tostring(off)))
  end

  -- 篝火火星：燃烧时持续升起
  do
    local keepT, keepC = emberT, emberCount
    emberT, emberCount = 0, 0
    local rate = campfireEmberRate()
    updateCampfireEmbers(0.6)
    local emitted = (emberCount or 0) >= 2
    local got = emberCount or 0
    emberT, emberCount = keepT, keepC
    ok(r, "campfire embers", rate > 0 and emitted,
       string.format("rate=%.0f/s emitted=%d", rate, got))
  end

  -- 篝火燃料：烧尽熄灭，添柴重新点燃并消耗木材
  do
    local keepOn, keepFuel = campfireOn, campfireFuelMin
    local keepWood = materials.wood or 0
    local sx, sz, sb = rv.x, rv.z, buildMode
    local sf, sp, sa = onFoot, player.x, player.active
    rv.x, rv.z = campfire and campfire.x or 1.8,
                 campfire and campfire.z or 2.0
    onFoot, buildMode = false, false
    campfireOn, campfireFuelMin = true, 1
    materials.wood = 1
    updateCampfireFuel(2)
    local burnedOut = campfireOn == false and campfireFuelMin == 0
    local okWood = addCampfireWood()
    local relit = campfireOn == true and (campfireFuelMin or 0) >= 479
    local spent = (materials.wood or 0) == 0
    local noWood = addCampfireWood()
    campfireOn, campfireFuelMin = keepOn, keepFuel
    materials.wood = keepWood
    rv.x, rv.z, buildMode = sx, sz, sb
    onFoot, player.x, player.active = sf, sp, sa
    ok(r, "campfire fuel",
       burnedOut and okWood and relit and spent and noWood == false,
       string.format("out=%s relit=%s wood=%d noWood=%s",
                     tostring(burnedOut), tostring(relit),
                     materials.wood or 0, tostring(noWood)))
  end

  -- 雨雪天篝火消耗更快
  do
    local clear = campfireFuelScale("clear")
    local rain = campfireFuelScale("rain")
    local snow = campfireFuelScale("snow")
    local keepOn, keepFuel, sw = campfireOn, campfireFuelMin, weather
    campfireOn, campfireFuelMin, weather = true, 100, "rain"
    updateCampfireFuel(10)
    local burned = math.abs((campfireFuelMin or 0) - 86) < 1e-6
    campfireOn, campfireFuelMin, weather = keepOn, keepFuel, sw
    ok(r, "campfire weather",
       clear == 1.0 and rain > clear and snow > rain and burned,
       string.format("clear=%.1f rain=%.1f snow=%.1f burned=%.0f",
                     clear, rain, snow, 86))
  end

  -- 篝火燃料 HUD 提示：熄灭/将尽时给文案，充足/远离时不给
  do
    local keepOn, keepFuel = campfireOn, campfireFuelMin
    local sx, sz, sb, sf = rv.x, rv.z, buildMode, onFoot
    local sp, sa = player.x, player.active
    rv.x, rv.z = campfire and campfire.x or 1.8,
                 campfire and campfire.z or 2.0
    onFoot, buildMode = false, false
    campfireOn, campfireFuelMin = false, 0
    local outHint = campfireHintText() or ""
    campfireOn, campfireFuelMin = true, 60
    local lowHint = campfireHintText() or ""
    campfireOn, campfireFuelMin = true, 800
    local fullHint = campfireHintText()
    campfireOn, campfireFuelMin = true, 60
    rv.x, rv.z = (campfire and campfire.x or 1.8) + 40,
                  campfire and campfire.z or 2.0
    local farHint = campfireHintText()
    campfireOn, campfireFuelMin = keepOn, keepFuel
    rv.x, rv.z, buildMode, onFoot = sx, sz, sb, sf
    player.x, player.active = sp, sa
    ok(r, "campfire hint",
       outHint:find("熄灭", 1, true) ~= nil and
       lowHint:find("将尽", 1, true) ~= nil and
       fullHint == nil and farHint == nil,
       string.format("out=%s low=%s full=%s far=%s",
                     tostring(outHint ~= ""), tostring(lowHint ~= ""),
                     tostring(fullHint ~= nil), tostring(farHint ~= nil)))
  end

  -- 位置版添柴（联机服务器用）：远处失败、近处成功并点燃
  do
    local keepOn, keepFuel = campfireOn, campfireFuelMin
    local kw = materials.wood or 0
    local cx = campfire and campfire.x or 1.8
    local cz = campfire and campfire.z or 2.0
    materials.wood = 1
    campfireOn, campfireFuelMin = false, 0
    local farOk = campfireAddWoodAt(cx + 50, cz)
    local nearOk = campfireAddWoodAt(cx, cz)
    local relit = campfireOn == true and (campfireFuelMin or 0) >= 479
    local spent = (materials.wood or 0) == 0
    campfireOn, campfireFuelMin = keepOn, keepFuel
    materials.wood = kw
    ok(r, "campfire wood at position",
       farOk == false and nearOk and relit and spent,
       string.format("far=%s near=%s relit=%s wood=%d",
                     tostring(farOk), tostring(nearOk), tostring(relit),
                     materials.wood or 0))
  end

  -- 联机篝火状态同步：客机按服务器标志点燃/熄灭
  do
    local keep = campfireOn
    setCampfireLit(false)
    local off = campfireOn == false
    setCampfireLit(true)
    local on = campfireOn == true
    setCampfireLit(keep)
    ok(r, "campfire sync", off and on,
       string.format("off=%s on=%s", tostring(off), tostring(on)))
  end

  -- 篝火剩余燃料同步：按服务器值设置并夹紧到 0
  do
    local keep = campfireFuelMin
    setCampfireFuel(123)
    local set = math.abs((campfireFuelMin or 0) - 123) < 1e-6
    setCampfireFuel(-5)
    local clamped = (campfireFuelMin or -1) == 0
    campfireFuelMin = keep
    ok(r, "campfire fuel sync", set and clamped,
       string.format("set=%s clamp=%s", tostring(set), tostring(clamped)))
  end

  -- 篝火也能把生食材烤成热饭
  do
    local keepOn = campfireOn
    local kr, kf, km, kd = inv.rawfish, inv.rawfood, inv.meal,
                           inv.dirtywater
    local keepGoal = dailyGoal
    setCampfireLit(true)
    inv.rawfish, inv.rawfood, inv.dirtywater = nil, 1, nil
    inv.meal = 0
    dailyGoal = nil
    local cooked = cookAtCampfire()
    local gotMeal = (inv.meal or 0) == 1
    local usedFood = (inv.rawfood or 0) == 0
    inv.rawfish, inv.rawfood, inv.meal, inv.dirtywater = kr, kf, km, kd
    dailyGoal = keepGoal
    setCampfireLit(keepOn)
    ok(r, "campfire roasts food", cooked and gotMeal and usedFood,
       string.format("cooked=%s meal=%d rawfood=%d", tostring(cooked),
                     inv.meal or 0, inv.rawfood or 0))
  end

  -- 篝火有野浆果时优先炖成浆果炖肉
  do
    local keepOn = campfireOn
    local kr, kf, ks, kb = inv.rawfish, inv.rawfood, inv.stew, inv.berry
    local kd = inv.dirtywater
    local keepGoal = dailyGoal
    setCampfireLit(true)
    inv.rawfish, inv.rawfood, inv.dirtywater = nil, 1, nil
    inv.berry, inv.stew = 2, 0
    dailyGoal = nil
    local cooked = cookAtCampfire()
    local gotStew = (inv.stew or 0) == 1
    local used = (inv.rawfood or 0) == 0 and (inv.berry or 0) == 0
    inv.rawfish, inv.rawfood, inv.stew = kr, kf, ks
    inv.berry, inv.dirtywater = kb, kd
    dailyGoal = keepGoal
    setCampfireLit(keepOn)
    ok(r, "campfire stew", cooked and gotStew and used,
       string.format("cooked=%s stew=%d rawfood=%d berry=%d", tostring(cooked),
                     inv.stew or 0, inv.rawfood or 0, inv.berry or 0))
  end

  -- 篝火煮咖啡：有咖啡豆 + 净水时可煮出热咖啡
  do
    local keepOn, keepFuel = campfireOn, campfireFuelMin
    local kcoffee, kwater, kbrew = inv.coffee, water, inv.brew or 0
    local kf, kr, kd = inv.rawfish, inv.rawfood, inv.dirtywater
    setCampfireLit(true)
    campfireFuelMin = 500
    inv.rawfish, inv.rawfood, inv.dirtywater = nil, nil, nil
    inv.coffee, water, inv.brew = 1, 60, 0
    local cooked = cookAtCampfire()
    local gotBrew = inv.brew or 0
    local leftBeans = inv.coffee or 0
    local leftWater = water or 0
    local got = gotBrew == 1
    local usedBeans = leftBeans == 0
    local usedWater = math.abs(leftWater - 45) < 1e-6
    inv.coffee, water, inv.brew = kcoffee, kwater, kbrew
    inv.rawfish, inv.rawfood, inv.dirtywater = kf, kr, kd
    campfireOn, campfireFuelMin = keepOn, keepFuel
    ok(r, "campfire coffee",
       cooked and got and usedBeans and usedWater,
       string.format("cooked=%s brew=%d beans=%d water=%.0f", tostring(cooked),
                     gotBrew, leftBeans, leftWater))
  end

  -- 篝火菜单：按指定料理制作（fish / coffee），材料不足不消耗
  do
    local keepOn, keepFuel = campfireOn, campfireFuelMin
    local kf, kfish = inv.rawfish, inv.fish or 0
    local kc, kbrew = inv.coffee, inv.brew or 0
    local kw, keepGoal = water, dailyGoal
    local kt, kv = toast.text, toast.t
    setCampfireLit(true)
    campfireFuelMin, dailyGoal = 500, nil
    inv.rawfish, inv.fish = 1, 0
    inv.coffee, inv.brew, water = 1, 0, 60
    local canFish = campfireCan("fish")
    local canCoffee = campfireCan("coffee")
    local cookedFish = campfireCook("fish")
    local cookedCoffee = campfireCook("coffee")
    local gotFishN = inv.fish or 0
    local gotBrewN = inv.brew or 0
    local leftWater = water or 0
    local gotFish = gotFishN == 1
    local gotBrew = gotBrewN == 1
    local spentWater = math.abs(leftWater - 45) < 1e-6
    inv.rawfish, inv.fish = kf, kfish
    inv.coffee, inv.brew = kc, kbrew
    water, dailyGoal = kw, keepGoal
    campfireOn, campfireFuelMin = keepOn, keepFuel
    toast.text, toast.t = kt, kv
    ok(r, "campfire menu",
       canFish and canCoffee and cookedFish and cookedCoffee and gotFish and
       gotBrew and spentWater,
       string.format("fish=%d brew=%d water=%.0f", gotFishN, gotBrewN,
                     leftWater))
  end

  -- 烟熏鱼：生鱼 x2 → 耐存放的烟熏鱼，耗 20 分钟燃料
  do
    local keepOn, keepFuel = campfireOn, campfireFuelMin
    local kraw, ksmoke = inv.rawfish, inv.smokedfish or 0
    local kgoal = dailyGoal
    local kt, kv = toast.text, toast.t
    setCampfireLit(true)
    campfireFuelMin, dailyGoal = 500, nil
    inv.rawfish, inv.smokedfish = 1, 0
    local needTwo = campfireCan("smoke")
    inv.rawfish = 2
    local canSmoke = campfireCan("smoke")
    local cooked = campfireCook("smoke")
    local got = inv.smokedfish or 0
    local leftRaw = inv.rawfish or 0
    local fuelUsed = 500 - (campfireFuelMin or 0)
    inv.rawfish, inv.smokedfish = kraw, ksmoke
    campfireOn, campfireFuelMin, dailyGoal = keepOn, keepFuel, kgoal
    toast.text, toast.t = kt, kv
    ok(r, "campfire smoked fish",
       needTwo == false and canSmoke and cooked and got == 1 and
       leftRaw == 0 and math.abs(fuelUsed - 20) < 1e-6,
       string.format("can=%s got=%d raw=%d fuel=%.0f", tostring(canSmoke),
                     got, leftRaw, fuelUsed))
  end

  -- 篝火鱼汤：烤鱼 x1 + 20L 水 → 鱼汤
  do
    local keepOn, keepFuel = campfireOn, campfireFuelMin
    local kfish, ksoup = inv.fish, inv.fishsoup or 0
    local kw, kg = water, dailyGoal
    local kt, kv = toast.text, toast.t
    setCampfireLit(true)
    campfireFuelMin, dailyGoal = 500, nil
    inv.fish, inv.fishsoup, water = 1, 0, 60
    local canSoup = campfireCan("soup")
    local cooked = campfireCook("soup")
    local got = inv.fishsoup or 0
    local leftFish = inv.fish or 0
    local leftWater = water or 0
    inv.fish, inv.fishsoup = kfish, ksoup
    water, dailyGoal = kw, kg
    campfireOn, campfireFuelMin = keepOn, keepFuel
    toast.text, toast.t = kt, kv
    ok(r, "campfire soup",
       canSoup and cooked and got == 1 and leftFish == 0 and
       math.abs(leftWater - 40) < 1e-6,
       string.format("got=%d fish=%d water=%.0f", got, leftFish, leftWater))
  end

  -- 篝火全部烹饪：把生鱼/生食材批量做熟，每份耗 10 分钟燃料
  do
    local keepOn, keepFuel = campfireOn, campfireFuelMin
    local kraw, kfood = inv.rawfish, inv.rawfood
    local kfish, kmeal = inv.fish, inv.meal
    local kg = dailyGoal
    local kt, kv = toast.text, toast.t
    setCampfireLit(true)
    campfireFuelMin, dailyGoal = 200, nil
    inv.rawfish, inv.rawfood, inv.fish, inv.meal = 3, 2, 0, 0
    local canAll = campfireCan("all")
    local okAll = campfireCook("all")
    local fishN, mealN = inv.fish or 0, inv.meal or 0
    local fuelLeft = campfireFuelMin or 0
    inv.rawfish, inv.rawfood = kraw, kfood
    inv.fish, inv.meal = kfish, kmeal
    campfireOn, campfireFuelMin, dailyGoal = keepOn, keepFuel, kg
    toast.text, toast.t = kt, kv
    ok(r, "campfire cook all",
       canAll and okAll and fishN == 3 and mealN == 2 and fuelLeft == 150,
       string.format("fish=%d meal=%d fuel=%.0f", fishN, mealN, fuelLeft))
  end

  -- 篝火烹饪消耗燃料：每次 10 游戏分钟
  do
    local keepOn, keepFuel = campfireOn, campfireFuelMin
    local kr = inv.rawfish or 0
    local keepGoal = dailyGoal
    local kt, kv = toast.text, toast.t
    setCampfireLit(true)
    campfireFuelMin, dailyGoal = 500, nil
    inv.rawfish = 1
    local cooked = cookAtCampfire()
    local gotFuel = campfireFuelMin or 0
    local spent = math.abs(gotFuel - 490) < 1e-6
    inv.rawfish = kr > 0 and kr or nil
    campfireOn, campfireFuelMin = keepOn, keepFuel
    dailyGoal = keepGoal
    toast.text, toast.t = kt, kv
    ok(r, "campfire cook fuel", cooked and spent,
       string.format("cooked=%s fuel=%.0f", tostring(cooked), gotFuel))
  end

  -- 夜间蟋蟀：晴天夜晚会鸣叫，雨雪/白天安静
  do
    local sc, sw = clock, weather
    clock = 23 * 60
    weather = "clear"
    local clearNight = nightCricketsActive()
    weather = "rain"
    local rainy = nightCricketsActive()
    weather = "snow"
    local snowy = nightCricketsActive()
    weather = "clear"
    clock = 12 * 60
    local daytime = nightCricketsActive()
    local iv = cricketInterval()
    local keepT, keepP = cricketT, cricketPlays
    clock, weather = 23 * 60, "clear"
    cricketT, cricketPlays = 0, 0
    updateNightCrickets(0.1)
    local played = (cricketPlays or 0) == 1
    cricketT, cricketPlays = keepT, keepP
    weather, clock = sw, sc
    ok(r, "night crickets",
       clearNight and not rainy and not snowy and not daytime and
       iv >= 3.5 and iv <= 6.5 and played,
       string.format("clear=%s rain=%s snow=%s day=%s iv=%.1f played=%s",
                     tostring(clearNight), tostring(rainy), tostring(snowy),
                     tostring(daytime), iv, tostring(played)))
  end

  -- 白天鸟鸣：晴天白天会叫，雨雪/夜晚安静
  do
    local sc, sw = clock, weather
    clock = 12 * 60
    weather = "clear"
    local clearDay = dayBirdsActive()
    weather = "rain"
    local rainy = dayBirdsActive()
    weather = "snow"
    local snowy = dayBirdsActive()
    weather = "clear"
    clock = 23 * 60
    local night = dayBirdsActive()
    local iv = birdChirpInterval()
    local keepT, keepP = birdChirpT, birdChirpPlays
    clock, weather = 12 * 60, "clear"
    birdChirpT, birdChirpPlays = 0, 0
    updateDayBirds(0.1)
    local played = (birdChirpPlays or 0) == 1
    birdChirpT, birdChirpPlays = keepT, keepP
    weather, clock = sw, sc
    ok(r, "day birds",
       clearDay and not rainy and not snowy and not night and
       iv >= 4.0 and iv <= 9.0 and played,
       string.format("clear=%s rain=%s snow=%s night=%s iv=%.1f played=%s",
                     tostring(clearDay), tostring(rainy), tostring(snowy),
                     tostring(night), iv, tostring(played)))
  end

  -- 远处狼嚎：夜晚远离营地才会出现，营地/白天/雨雪安静
  do
    local sc, sw = clock, weather
    local sx, sz, sf = rv.x, rv.z, onFoot
    onFoot = false
    clock, weather = 23 * 60, "clear"
    rv.x, rv.z = 150, 150
    local farNight = howlActive()
    rv.x, rv.z = 0, 0
    local atCamp = howlActive()
    rv.x, rv.z = 150, 150
    clock = 12 * 60
    local daytime = howlActive()
    clock = 23 * 60
    weather = "rain"
    local rainy = howlActive()
    clock, weather = 23 * 60, "clear"
    local iv = howlInterval()
    local keepT, keepP = howlT, howlPlays
    howlT, howlPlays = 0, 0
    updateNightHowls(0.1)
    local played = (howlPlays or 0) == 1
    howlT, howlPlays = keepT, keepP
    rv.x, rv.z, onFoot = sx, sz, sf
    weather, clock = sw, sc
    ok(r, "night howls",
       farNight and not atCamp and not daytime and not rainy and
       iv >= 45 and iv <= 90 and played,
       string.format("far=%s camp=%s day=%s rain=%s iv=%.1f played=%s",
                     tostring(farNight), tostring(atCamp), tostring(daytime),
                     tostring(rainy), iv, tostring(played)))
  end

  -- 夜间猫头鹰：晴朗夜晚会叫，白天/雨雪安静
  do
    local sc, sw = clock, weather
    clock, weather = 23 * 60, "clear"
    local clearNight = owlsActive()
    weather = "rain"
    local rainy = owlsActive()
    weather = "snow"
    local snowy = owlsActive()
    weather = "clear"
    clock = 12 * 60
    local daytime = owlsActive()
    local iv = owlInterval()
    local keepT, keepP = owlT, owlPlays
    clock, weather = 23 * 60, "clear"
    owlT, owlPlays = 0, 0
    updateNightOwls(0.1)
    local played = (owlPlays or 0) == 1
    owlT, owlPlays = keepT, keepP
    weather, clock = sw, sc
    ok(r, "night owls",
       clearNight and not rainy and not snowy and not daytime and
       iv >= 35 and iv <= 75 and played,
       string.format("clear=%s rain=%s snow=%s day=%s iv=%.1f played=%s",
                     tostring(clearNight), tostring(rainy), tostring(snowy),
                     tostring(daytime), iv, tostring(played)))
  end

  -- 主路土肩：沥青边缘与草地的过渡
  do
    local n = #roadShoulders
    ok(r, "main road shoulders", n >= 30, string.format("shoulders=%d", n))
  end

  -- 合作客户端手电：由 coop 位置/视角驱动，夜间且未上车时点亮
  do
    local sc = clock
    clock = 22 * 60
    updateCoopFlashlight(4.0, 4.0, 0, 0.5, 0.1, true)
    local on = flashlightEnt ~= nil
    updateCoopFlashlight(4.0, 4.0, 0, 0.5, 0.1, false)
    local offDisabled = flashlightEnt == nil
    clock = 12 * 60
    updateCoopFlashlight(4.0, 4.0, 0, 0.5, 0.1, true)
    local offDay = flashlightEnt == nil
    clock = sc
    ok(r, "rv coop flashlight", on and offDisabled and offDay,
       string.format("on=%s offDisabled=%s offDay=%s",
                     tostring(on), tostring(offDisabled), tostring(offDay)))
  end

  -- 雾天/深夜的地面薄雾发射率
  do
    local sw, sc = weather, clock
    weather = "fog"
    local fogRate = fogWispRate()
    weather = "clear"
    clock = 12 * 60
    local dayRate = fogWispRate()
    clock = 22 * 60
    local nightRate = fogWispRate()
    weather, clock = sw, sc
    ok(r, "rv fog wisps", fogRate == 6 and dayRate == 0 and nightRate == 2,
       string.format("fog=%d day=%d night=%d", fogRate, dayRate, nightRate))
  end

  -- 目标方位差：用于屏幕外箭头
  do
    local keep = job
    job = { destId = "gasstop" }
    local ahead = jobBearingDelta(0, 0, 0)
    local behind = jobBearingDelta(0, 0, math.pi)
    job = keep
    ok(r, "job bearing arrow",
       math.abs(ahead) < 1e-6 and math.abs(math.abs(behind) - math.pi) < 1e-6,
       string.format("ahead=%.3f behind=%.3f", ahead, behind))
  end

  -- 受损冒烟速率：车体越低越浓，引擎磨损再叠一层
  do
    local r100 = rvSmokeRate(100, 100)
    local r30 = rvSmokeRate(30, 100)
    local r10 = rvSmokeRate(10, 100)
    local rEng = rvSmokeRate(100, 20)
    ok(r, "rv damage smoke", r100 == 0 and r30 == 4 and r10 == 8 and rEng == 3,
       string.format("100=%d 30=%d 10=%d engine=%d", r100, r30, r10, rEng))
  end

  -- 尾气：行驶比怠速多，冷车/车况差更浓
  do
    local idle = rvExhaustRate(0, 80, 100)
    local moving = rvExhaustRate(5, 80, 100)
    local cold = rvExhaustRate(5, 25, 100)
    local damaged = rvExhaustRate(5, 80, 30)
    ok(r, "rv exhaust",
       idle > 0 and moving > idle and cold > moving and damaged > moving,
       string.format("idle=%d moving=%d cold=%d damaged=%d",
                     idle, moving, cold, damaged))
  end

  -- 过热白汽：水温超过 105°C 才冒汽，越高越浓
  do
    local cool = engineSteamRate(90)
    local warm = engineSteamRate(110)
    local hot = engineSteamRate(125)
    local capped = engineSteamRate(200)
    ok(r, "engine steam",
       cool == 0 and warm > 0 and hot > warm and capped <= 10,
       string.format("90=%.1f 110=%.1f 125=%.1f cap=%.1f",
                     cool, warm, hot, capped))
  end

  -- 引擎音量随车速：怠速 0.60 → 全速 1.15
  do
    local g0 = engineAudioGain(0)
    local gHalf = engineAudioGain(DRIVE.maxSpeed * 0.5)
    local gMax = engineAudioGain(DRIVE.maxSpeed)
    ok(r, "rv engine audio", g0 < gHalf and gHalf < gMax and g0 >= 0.5,
       string.format("idle=%.2f half=%.2f max=%.2f", g0, gHalf, gMax))
  end

  -- 撞击反馈：短促抖动（0.12–0.50s）+ 受击音效标记
  do
    local keep = rv.shakeT
    rvImpactFeedback(3.0)
    local s = rv.shakeT or 0
    local sfx = rv.impactSfx
    rv.shakeT = keep
    ok(r, "rv impact shake", s > 0.1 and s <= 0.50 and sfx == "hurt",
       string.format("shakeT=%.2f sfx=%s", s, tostring(sfx)))
  end

  -- 开关车门音效：计数递增
  do
    local keep = rv.doorSounds
    rv.doorSounds = 0
    playDoorSfx()
    playDoorSfx()
    local n = rv.doorSounds or 0
    rv.doorSounds = keep
    ok(r, "door sfx", n == 2, string.format("sounds=%d", n))
  end

  -- 雨天闪电：触发后闪光计时开始衰减，并累计雷声次数
  do
    local keep = lightningT
    local c0 = lightningCount or 0
    triggerLightning()
    local on = (lightningT or 0) > 0
    local counted = (lightningCount or 0) == c0 + 1
    local a = math.min(0.35, (lightningT or 0) * 0.9)
    lightningT = keep
    lightningCount = c0
    ok(r, "rv lightning flash", on and a > 0.10 and counted,
       string.format("t=0.50 alpha=%.2f thunder=%d", a, c0 + 1))
  end

  -- 主路两侧废弃车/石块（带静态碰撞）
  do
    local n = #roadDebris
    ok(r, "roadside debris", n >= 4 and (roadDebrisColliders or 0) >= 4,
       string.format("debris=%d colliders=%d", n, roadDebrisColliders or 0))
  end

  -- 远光灯：切换后按高亮 prefab 重建两盏灯
  do
    local sc, sh = clock, highBeam
    clock = 22 * 60
    highBeam = false
    setRvHeadlights(false)
    setRvHeadlights(true)
    local low = #rvHeadlights
    toggleHighBeam()
    local high = #rvHeadlights
    toggleHighBeam()
    local back = #rvHeadlights
    setRvHeadlights(false)
    clock, highBeam = sc, sh
    ok(r, "rv high beam", low == 2 and high == 2 and back == 2 and highBeam == false,
       string.format("low=%d high=%d back=%d", low, high, back))
  end

  -- 萤火虫：深夜非雨天有，白天/雨天没有
  do
    local sw, sc = weather, clock
    weather, clock = "clear", 22 * 60
    local night = fireflyRate()
    clock = 12 * 60
    local day = fireflyRate()
    weather, clock = "rain", 22 * 60
    local rain = fireflyRate()
    weather, clock = sw, sc
    ok(r, "rv fireflies", night == 6 and day == 0 and rain == 0,
       string.format("night=%d day=%d rain=%d", night, day, rain))
  end

  -- 钓鱼鱼种/尺寸/纪录
  do
    local name, size, sp = rollFish()
    local valid = name ~= nil and sp ~= nil and size >= sp.min and size <= sp.max
    local before = bestFish or 0
    if size > before then bestFish = size end
    local recordOk = (bestFish or 0) >= size
    local poolOk = #FISH_SPECIES >= 6
    for _, s in ipairs(FISH_SPECIES) do
      if s.name == nil or s.w == nil or s.min == nil or s.max == nil or
         s.min >= s.max then poolOk = false end
    end
    ok(r, "fishing species", valid and recordOk and poolOk,
       string.format("%s %.0fcm best=%.0f species=%d", tostring(name), size,
                     bestFish or 0, #FISH_SPECIES))
  end

  -- 咬钩窗口：倒计时结束后进入 1.2s 收线窗口
  do
    local keepT, keepB, keepBT = fishingT, fishingBite, fishingBiteT
    fishingT, fishingBite, fishingBiteT = 0.01, false, 0
    local splash0 = fishingSplashCount or 0
    updateFishing(0.05)
    local opened = fishingBite == true and
                   math.abs((fishingBiteT or 0) - 1.2) < 1e-6
    local splashed = (fishingSplashCount or 0) == splash0 + 1
    local biteNow, biteTNow = fishingBite, fishingBiteT
    fishingT, fishingBite, fishingBiteT = keepT, keepB, keepBT
    ok(r, "fishing bite window", opened and splashed,
       string.format("bite=%s t=%.2f splash=%s", tostring(biteNow),
                     biteTNow or 0, tostring(splashed)))
  end

  -- 浮漂：抛竿生成、收线/咬钩清理
  do
    local keepT, keepB, keepBT = fishingT, fishingBite, fishingBiteT
    local keepBait = inv.bait
    local kt, kv = toast.text, toast.t
    inv.bait = nil
    fishingT, fishingBite, fishingBiteT = 0, false, 0
    startFishing()
    local spawned = fishingBobber ~= nil
    despawnFishingBobber()
    local cleared = fishingBobber == nil
    fishingT, fishingBite, fishingBiteT = keepT, keepB, keepBT
    inv.bait = keepBait
    toast.text, toast.t = kt, kv
    ok(r, "fishing bobber", spawned and cleared,
       string.format("spawned=%s cleared=%s", tostring(spawned),
                     tostring(cleared)))
  end

  -- 湖边鱼笼：放置消耗材料，计时后收取生鱼（不放鱼饵）
  do
    local keepTrap = pondTrap
    local sm, sw = materials.metal or 0, materials.wood or 0
    local kr, kb = inv.rawfish or 0, inv.bait or 0
    local kt, kv = toast.text, toast.t
    pondTrap = nil
    inv.bait = nil
    materials.metal, materials.wood = 5, 3
    local placed = togglePondTrapAt(-52, 30)
    local setting = pondTrap ~= nil and pondTrap.ready == false and
                    math.abs((pondTrap.t or 0) - POND_TRAP_MINUTES) < 1e-6
    local visible = pondTrap ~= nil and pondTrap.ent ~= nil
    updatePondTrap(POND_TRAP_MINUTES + 1)
    local ready = pondTrap ~= nil and pondTrap.ready == true
    local collected = togglePondTrapAt(-52, 30)
    local gained = (inv.rawfish or 0) == kr + 2
    local cleared = pondTrap == nil
    materials.metal, materials.wood = 0, 0
    local poor = togglePondTrapAt(-52, 30)
    pondTrap = keepTrap
    materials.metal, materials.wood = sm, sw
    inv.rawfish = kr > 0 and kr or nil
    inv.bait = kb > 0 and kb or nil
    toast.text, toast.t = kt, kv
    ok(r, "pond trap",
       placed and setting and visible and ready and collected and gained and
       cleared and poor == false,
       string.format("place=%s set=%s visible=%s ready=%s got=%s poor=%s",
                     tostring(placed), tostring(setting), tostring(visible),
                     tostring(ready), tostring(gained), tostring(poor)))
  end

  -- 鱼笼放鱼饵：多收 1 条鱼并消耗 1 个鱼饵
  do
    local keepTrap = pondTrap
    local kr, kb = inv.rawfish or 0, inv.bait or 0
    local kt, kv = toast.text, toast.t
    pondTrap = { x = -52, z = 30, t = 0, ready = true }
    inv.bait = 1
    local collected = togglePondTrapAt(-52, 30)
    local gainedN = (inv.rawfish or 0) - kr
    local gained = gainedN == 3
    local spent = (inv.bait or 0) == 0
    pondTrap = keepTrap
    inv.rawfish = kr > 0 and kr or nil
    inv.bait = kb > 0 and kb or nil
    toast.text, toast.t = kt, kv
    ok(r, "trap bait bonus",
       collected and gained and spent,
       string.format("got=%s rawfish=%d bait=%d", tostring(collected),
                     gainedN, inv.bait or 0))
  end

  -- 各鱼种最大尺寸/条数记录
  do
    local keepR, keepC = fishRecords, fishCounts
    local keepB, keepN, keepK = bestFish, bestFishName, fishCaught
    local keepGoal = dailyGoal
    dailyGoal = nil
    fishRecords, fishCounts = {}, {}
    bestFish, bestFishName, fishCaught = 0, nil, 0
    recordFish("鲫鱼", 20, 1)
    local second = recordFish("鲫鱼", 25, 1)
    local third = recordFish("鲤鱼", 30, 1)
    local okRec = second == true and third == true and
                  fishRecords["鲫鱼"] == 25 and fishRecords["鲤鱼"] == 30 and
                  fishCounts["鲫鱼"] == 2 and bestFish == 30 and
                  bestFishName == "鲤鱼" and fishCaught == 3
    local gotJi = fishRecords["鲫鱼"] or 0
    local gotLi = fishRecords["鲤鱼"] or 0
    local gotBest = bestFish or 0
    local gotName = bestFishName
    fishRecords, fishCounts = keepR, keepC
    bestFish, bestFishName, fishCaught = keepB, keepN, keepK
    dailyGoal = keepGoal
    ok(r, "fish species records", okRec,
       string.format("鲫鱼=%.0f 鲤鱼=%.0f best=%s %.0f",
                     gotJi, gotLi, tostring(gotName), gotBest))
  end

  -- 首次钓到某鱼种写入旅行日志
  do
    local keepR, keepC = fishRecords, fishCounts
    local keepB, keepN, keepK = bestFish, bestFishName, fishCaught
    local keepJ, keepGoal = journal, dailyGoal
    fishRecords, fishCounts = {}, {}
    bestFish, bestFishName, fishCaught = 0, nil, 0
    journal, dailyGoal = {}, nil
    recordFish("鲫鱼", 20, 1)
    local first = #journal == 1 and
                  journal[1].text:find("首次钓到", 1, true) ~= nil
    journal = {}
    recordFish("鲫鱼", 22, 1)
    local notFirst = #journal == 1 and
                     journal[1].text:find("新纪录", 1, true) ~= nil
    fishRecords, fishCounts = keepR, keepC
    bestFish, bestFishName, fishCaught = keepB, keepN, keepK
    journal, dailyGoal = keepJ, keepGoal
    ok(r, "first catch journal", first and notFirst,
       string.format("first=%s notFirst=%s", tostring(first),
                     tostring(notFirst)))
  end

  -- 雨天路面抓地折减（与极速惩罚叠加）
  do
    local sw = weather
    weather = "rain"
    local wet = weatherGripScale()
    weather = "clear"
    local dry = weatherGripScale()
    weather = "fog"
    local fog = weatherGripScale()
    weather = sw
    ok(r, "wet road grip", wet < 0.9 and dry == 1.0 and fog == 1.0,
       string.format("rain=%.2f clear=%.2f fog=%.2f", wet, dry, fog))
  end

  -- 每日首单：同一天第一单 +25%，第二单恢复
  do
    local sd, sLast = day, lastJobDay
    local mk = milestones
    milestones = {}
    local dk = dailyGoal
    dailyGoal = nil
    local jd = jobDelivered
    jobDelivered = 0
    day, lastJobDay = 9, 0
    jobRefreshOffers("depot")
    jobAccept(1)
    job.urgent, job.deadlineAbs, job.acceptedAbs = false, nil, nil
    job.pay = { metal = 5, wood = 2 }
    job.condition = 1.0
    local m0 = materials.metal or 0
    jobComplete()
    local firstGain = (materials.metal or 0) - m0
    jobRefreshOffers("depot")
    jobAccept(1)
    job.urgent, job.deadlineAbs, job.acceptedAbs = false, nil, nil
    job.pay = { metal = 5, wood = 2 }
    job.condition = 1.0
    local m1 = materials.metal or 0
    jobComplete()
    local secondGain = (materials.metal or 0) - m1
    day, lastJobDay = sd, sLast
    jobDelivered = jd
    milestones = mk
    dailyGoal = dk
    ok(r, "daily first job bonus", firstGain == 7 and secondGain == 5,
       string.format("first=%d second=%d", firstGain, secondGain))
  end

  -- 今日首单状态（板上提示）
  do
    local sd, sl = day, lastJobDay
    day, lastJobDay = 5, 5
    local used = jobDailyBonusAvailable()
    lastJobDay = 4
    local avail = jobDailyBonusAvailable()
    day, lastJobDay = sd, sl
    ok(r, "daily bonus status", used == false and avail == true,
       string.format("used=%s avail=%s", tostring(used), tostring(avail)))
  end

  -- 钓鱼纪录文本（地图右下）
  do
    local sb, sf = bestFish, fishCaught
    bestFish, fishCaught = 63, 12
    local rich = fishingRecordText()
    bestFish, fishCaught = 0, 0
    local empty = fishingRecordText()
    bestFish, fishCaught = sb, sf
    ok(r, "fishing record text",
       rich:find("63", 1, true) ~= nil and rich:find("12", 1, true) ~= nil and
       empty:find("暂无", 1, true) ~= nil,
       rich .. " | " .. empty)
  end

  -- POI 门头招牌：夜里点亮
  do
    local count = #poiSigns
    local sc = clock
    clock = 22 * 60
    updatePoiSigns()
    local night = poiSignsLit
    clock = 12 * 60
    updatePoiSigns()
    local day = poiSignsLit
    clock = sc
    ok(r, "poi night signs", count >= 6 and night == true and day == false,
       string.format("signs=%d night=%s day=%s", count,
                     tostring(night), tostring(day)))
  end

  -- 挡风玻璃水珠：仅雨天 + 驾驶席视角
  do
    local sw, sp, sv = weather, phase, driveView
    weather, phase, driveView = "rain", "driving", "cab"
    local cab = windshieldDropCount()
    driveView = "chase"
    local chase = windshieldDropCount()
    weather = "clear"
    local clear = windshieldDropCount()
    weather, phase, driveView = sw, sp, sv
    ok(r, "windshield rain drops", cab == 18 and chase == 0 and clear == 0,
       string.format("cab=%d chase=%d clear=%d", cab, chase, clear))
  end

  -- 雪天挡风玻璃积雪：只在驾驶席视角出现
  do
    local sw, sp, sv = weather, phase, driveView
    weather, phase, driveView = "snow", "driving", "cab"
    local cab = windshieldSnowCount()
    driveView = "chase"
    local chase = windshieldSnowCount()
    weather, driveView = "clear", "cab"
    local clear = windshieldSnowCount()
    weather, phase, driveView = sw, sp, sv
    ok(r, "windshield snow drops", cab == 14 and chase == 0 and clear == 0,
       string.format("cab=%d chase=%d clear=%d", cab, chase, clear))
  end

  -- 车体受损时挡风玻璃裂纹条数递增
  do
    local okD = windshieldCrackCount(80)
    local mid = windshieldCrackCount(40)
    local low = windshieldCrackCount(20)
    local bad = windshieldCrackCount(5)
    ok(r, "windshield cracks",
       okD == 0 and mid == 3 and low == 6 and bad == 9,
       string.format("80=%d 40=%d 20=%d 5=%d", okD, mid, low, bad))
  end

  -- 车身越脏，挡风玻璃泥点越多
  do
    local clean = windshieldDirtCount(0.2)
    local dirty = windshieldDirtCount(0.5)
    local bad = windshieldDirtCount(0.7)
    local worst = windshieldDirtCount(0.95)
    ok(r, "windshield dirt",
       clean == 0 and dirty == 3 and bad == 6 and worst == 9,
       string.format("0.2=%d 0.5=%d 0.7=%d 0.95=%d",
                     clean, dirty, bad, worst))
  end

  -- 挡风玻璃起雾：干燥晴天无雾，雨雪/湿衣服更明显，取暖器吹散
  do
    local dry = windshieldFogAlpha(0, "clear", false)
    local rain = windshieldFogAlpha(0, "rain", false)
    local wet = windshieldFogAlpha(85, "rain", false)
    local heat = windshieldFogAlpha(85, "rain", true)
    ok(r, "windshield fog",
       dry == 0 and rain > 0 and wet > rain and heat < wet and
       heat <= wet * 0.25,
       string.format("dry=%.2f rain=%.2f wet=%.2f heat=%.2f",
                     dry, rain, wet, heat))
  end

  -- 雨刷：在 -0.20~0.42 之间来回摆动
  do
    local a0 = wiperAngle(0)
    local a1 = wiperAngle(0.9)
    local a2 = wiperAngle(1.8)
    local bounded = true
    for i = 0, 20 do
      local a = wiperAngle(i * 0.1)
      if a < -0.21 or a > 0.43 then bounded = false end
    end
    ok(r, "rain wipers",
       math.abs(a0 + 0.20) < 1e-6 and a1 > a0 and
       math.abs(a2 - a0) < 1e-6 and bounded,
       string.format("a0=%.2f a1=%.2f wrap=%.2f", a0, a1, a2))
  end

  -- 雨刷在雨/雪天驾驶席显示，晴天/追逐视角不显示
  do
    local sw, sp, sd = weather, phase, driveView
    weather, phase, driveView = "rain", "driving", "cab"
    local rainCab = wipersActive()
    weather, phase, driveView = "snow", "driving", "cab"
    local snowCab = wipersActive()
    weather, phase, driveView = "clear", "driving", "cab"
    local clearCab = wipersActive()
    weather, phase, driveView = "rain", "driving", "chase"
    local chase = wipersActive()
    weather, phase, driveView = sw, sp, sd
    ok(r, "wipers active",
       rainCab and snowCab and not clearCab and not chase,
       string.format("rain=%s snow=%s clear=%s chase=%s",
                     tostring(rainCab), tostring(snowCab),
                     tostring(clearCab), tostring(chase)))
  end

  -- 载重影响刹车：越重越刹不住
  do
    local empty = brakeRate(1.0)
    local loaded = brakeRate(1.4)
    ok(r, "loaded braking", empty > loaded and loaded > 0,
       string.format("empty=%.1f loaded=%.1f m/s²", empty, loaded))
  end

  -- 刹车热衰减：热量越高制动力越低，最低 70%
  do
    local cold = brakeRate(1.0, 0)
    local half = brakeRate(1.0, 0.5)
    local hot = brakeRate(1.0, 1.0)
    ok(r, "brake fade",
       hot < half and half < cold and
       math.abs(brakeFadeScale(1) - 0.70) < 1e-6,
       string.format("cold=%.1f half=%.1f hot=%.1f", cold, half, hot))
  end

  -- 重撞伤引擎/轮胎：轻微剐蹭不伤，重撞有上限
  do
    local e0, t0 = impactPartDamage(3.0)
    local e1, t1 = impactPartDamage(6.0)
    local eCap, tCap = impactPartDamage(100)
    ok(r, "impact part damage",
       e0 == 0 and t0 == 0 and e1 > 0 and t1 > 0 and
       eCap <= 3 and tCap <= 4,
       string.format("light=%.1f/%.1f hit=%.1f/%.1f cap=%.1f/%.1f",
                     e0, t0, e1, t1, eCap, tCap))
  end

  -- 车体装甲：每级 -15% 碰撞损失，满级后不再降低
  do
    local lv0 = armorDamageScale(0)
    local lv1 = armorDamageScale(1)
    local lv3 = armorDamageScale(3)
    local capped = armorDamageScale(99)
    ok(r, "armor damage scale",
       lv0 == 1.0 and lv1 < lv0 and lv3 < lv1 and
       math.abs(lv3 - 0.55) < 1e-6 and math.abs(capped - 0.55) < 1e-6,
       string.format("0=%.2f 1=%.2f 3=%.2f cap=%.2f",
                     lv0, lv1, lv3, capped))
  end

  -- 雨雪天刹车效率下降
  do
    local dry = brakeRate(1.0, 0, "clear")
    local rain = brakeRate(1.0, 0, "rain")
    local snow = brakeRate(1.0, 0, "snow")
    ok(r, "wet brakes",
       dry > snow and dry > rain and rain < snow and
       math.abs(weatherBrakeScale("rain") - 0.92) < 1e-6,
       string.format("dry=%.1f rain=%.1f snow=%.1f", dry, rain, snow))
  end

  -- 刹车热：载重更快累积，雨雪散热更快
  do
    local light = brakeHeatLoadScale(1.0)
    local loaded = brakeHeatLoadScale(1.4)
    local cap = brakeHeatLoadScale(5.0)
    local dry = brakeCoolScale("clear")
    local rain = brakeCoolScale("rain")
    local snow = brakeCoolScale("snow")
    ok(r, "brake heat scales",
       light == 1.0 and loaded > light and cap <= 1.6 and
       dry == 1.0 and rain > dry and snow > rain,
       string.format("load 1=%.2f 1.4=%.2f 5=%.2f | cool clear=%.2f rain=%.2f snow=%.2f",
                     light, loaded, cap, dry, rain, snow))
  end

  -- 载重增加轮胎磨损（超载最高 ×2.2）
  do
    local empty = tireWearScale(1.0)
    local loaded = tireWearScale(1.4)
    local over = tireWearScale(3.0)
    ok(r, "load tire wear",
       empty == 1.0 and loaded > empty and over >= loaded and over <= 2.2,
       string.format("empty=%.2f loaded=%.2f over=%.2f", empty, loaded, over))
  end

  -- 粗糙路面加速轮胎磨损
  do
    local road = surfaceTireWearScale("asphalt")
    local gravel = surfaceTireWearScale("gravel")
    local dirt = surfaceTireWearScale("dirt")
    local mud = surfaceTireWearScale("mud")
    ok(r, "surface tire wear",
       road == 1.0 and gravel > road and dirt > road and mud > gravel,
       string.format("road=%.2f gravel=%.2f dirt=%.2f mud=%.2f",
                     road, gravel, dirt, mud))
  end

  -- 急转弯/打滑额外磨胎
  do
    local calm = slipTireWear(0.4, 8)
    local fast = slipTireWear(1.2, 8)
    local slow = slipTireWear(1.2, 2)
    local cap = slipTireWear(100, 8)
    ok(r, "slip tire wear",
       calm == 0 and fast > 0 and slow == 0 and cap <= 0.6,
       string.format("calm=%.2f fast=%.2f slow=%.2f cap=%.2f",
                     calm, fast, slow, cap))
  end

  -- 太阳能：夜间不出力、白天满发
  do
    local sc, sw = clock, weather
    weather = "clear"
    clock = 22 * 60
    recomputeStats()
    local night = stats.powerGen or 0
    clock = 12 * 60
    recomputeStats()
    local day = stats.powerGen or 0
    clock, weather = sc, sw
    if recomputeStats ~= nil then recomputeStats() end
    ok(r, "solar night output", day > 0 and night == 0,
       string.format("day=%.1fkW night=%.1fkW", day, night))
  end

  -- 手刹：比脚刹更强
  do
    local foot = brakeRate(1.0)
    local hand = handbrakeRate(1.0)
    ok(r, "handbrake rate", hand > foot and foot > 0,
       string.format("foot=%.1f hand=%.1f m/s²", foot, hand))
  end

  -- 农场谷仓外干草捆
  do
    local n = #hayBales
    ok(r, "farm hay bales", n >= 3, string.format("bales=%d", n))
  end

  -- 胎压随里程下降，修轮胎充回 2.4bar
  do
    local keep = {}
    for i = 1, 4 do keep[i] = (rv.tirePressureCold or {})[i] end
    local p1 = pressureAfterMeters(2.4, 1000)
    rv.tirePressureCold = { 1.8, 1.8, 1.8, 1.8 }
    resetTirePressure()
    local reset = math.abs((rv.tirePressureCold or {})[1] - 2.4) < 1e-6
    rv.tirePressureCold = keep
    ok(r, "tire pressure maintenance",
       p1 < 2.4 and p1 > 2.3 and reset,
       string.format("after1000m=%.2f reset=%.2f", p1, reset and 2.4 or 0))
  end

  -- 鱼饵：消耗 1 个，必上钩且多给一条
  -- （临时屏蔽大鱼奖励，避免它额外补鱼饵干扰计数）
  do
    local keepBig = bigFishRewardDone
    bigFishRewardDone = true
    addItem("bait", 1)
    local bait0 = inv.bait or 0
    local raw0 = itemCount("rawfish")
    startFishing()
    local used = fishingBait == true
    finishFishing()
    local caught = itemCount("rawfish") > raw0
    local consumed = (inv.bait or 0) == bait0 - 1
    local gotBait = inv.bait or 0
    local gotRaw = itemCount("rawfish")
    bigFishRewardDone = keepBig
    ok(r, "fishing bait", used and caught and consumed,
       string.format("bait %d->%d raw %d->%d", bait0, gotBait,
                     raw0, gotRaw))
  end

  -- 罗盘航向：+yaw = 左转 = 西
  do
    local n = compassHeading(0)
    local w = compassHeading(math.pi * 0.5)
    local s = compassHeading(math.pi)
    ok(r, "compass heading",
       math.abs(n) < 1e-6 and math.abs(w - 270) < 1e-6 and math.abs(s - 180) < 1e-6,
       string.format("N=%.0f W=%.0f S=%.0f", n, w, s))
  end

  -- 步行房车箭头：正前方 delta≈0，转身 90° 后 delta≈-π/2
  do
    local sx, sz = rv.x, rv.z
    rv.x, rv.z = 0, 0
    local ahead = rvBearingDelta(0, -10, 0)
    local turned = rvBearingDelta(0, -10, math.pi * 0.5)
    rv.x, rv.z = sx, sz
    ok(r, "rv bearing hint",
       math.abs(ahead) < 1e-6 and math.abs(turned + math.pi * 0.5) < 1e-6,
       string.format("ahead=%.2f turned=%.2f", ahead, turned))
  end

  -- 电池充放电模型
  do
    local full = batteryStep(0, 2, 0.5, 5)
    local empty = batteryStep(1, -3, 0.5, 5)
    local clamp = batteryStep(4.9, 2, 1, 5)
    local okGen = powerAvailable(1, 0)
    local noGen = powerAvailable(-1, 0)
    local stored = powerAvailable(-1, 1)
    ok(r, "battery charge model",
       math.abs(full - 1) < 1e-6 and empty == 0 and
       math.abs(clamp - 5) < 1e-6 and okGen and not noGen and stored,
       string.format("full=%.1f empty=%.1f clamp=%.1f gen=%s noGen=%s stored=%s",
                     full, empty, clamp, tostring(okGen),
                     tostring(noGen), tostring(stored)))
  end

  -- 行驶时发电机充电：开车且油量 > 0 才充
  do
    local driving = alternatorPower("driving", 10)
    local parked = alternatorPower("parked", 10)
    local empty = alternatorPower("driving", 0)
    ok(r, "alternator charging",
       driving > 0 and parked == 0 and empty == 0,
       string.format("driving=%.2f parked=%.2f empty=%.2f",
                     driving, parked, empty))
  end

  -- 电瓶低电量提示：低于 20% 且净功率为负才提示
  do
    local low = batteryLowText(0.1, 1.0, -0.5)
    local charging = batteryLowText(0.1, 1.0, 0.5)
    local okBatt = batteryLowText(0.5, 1.0, -0.5)
    ok(r, "battery low hint",
       low ~= nil and charging == nil and okBatt == nil,
       string.format("low=%s charging=%s ok=%s", tostring(low),
                     tostring(charging), tostring(okBatt)))
  end

  -- 鸟群：白天活跃、夜间隐藏
  do
    local sc = clock
    clock = 12 * 60
    local day = birdsActive()
    clock = 22 * 60
    local night = birdsActive()
    clock = sc
    ok(r, "birds day cycle", #birds >= 5 and day == true and night == false,
       string.format("birds=%d day=%s night=%s", #birds,
                     tostring(day), tostring(night)))
  end

  -- 电力不足时舒适加成失效
  do
    local on = comfortWithPower(20, 10, -1, 1)
    local off = comfortWithPower(20, 10, -1, 0)
    local gen = comfortWithPower(20, 10, 1, 0)
    ok(r, "powered comfort", on == 20 and off == 10 and gen == 20,
       string.format("stored=%d empty=%d gen=%d", on, off, gen))
  end

  -- 生食材腐败：冰箱通电时保鲜更久
  do
    local noFridge = rawFoodAging(100, false)
    local fridge = rawFoodAging(100, true)
    addItem("rawfood", 2)
    rawFoodAge = (SURVIVAL.rawSpoilMinutes or 2880) + 1
    updateSurvival(0.01)
    local spoiled = (inv.rawfood or 0) == 0 and (rawFoodAge or 1) == 0
    ok(r, "raw food spoilage", noFridge > fridge and fridge > 0 and spoiled,
       string.format("aging %.0f vs %.1f spoiled=%s", noFridge, fridge,
                     tostring(spoiled)))
  end

  -- 雪天低温减缓腐败
  do
    local sw = weather
    weather = "clear"
    local warm = rawFoodAging(100, false)
    weather = "snow"
    local cold = rawFoodAging(100, false)
    weather = sw
    ok(r, "snow preserves food", cold < warm and cold > 0,
       string.format("clear=%.0f snow=%.0f", warm, cold))
  end

  -- 每日目标：进度与完成奖励
  do
    dailyGoal = dailyKeep
    milestones = milestonesKeep
    local keep = dailyGoal
    dailyGoal = { id = "jobs", name = "测试目标", target = 2, progress = 0,
                  done = false, reward = { metal = 2, fuel = 5 } }
    local m0 = materials.metal or 0
    addDailyProgress("jobs", 1)
    local p1 = dailyGoal.progress
    addDailyProgress("jobs", 1)
    local done = dailyGoal.done
    ok(r, "daily goal", p1 == 1 and done and (materials.metal or 0) >= m0 + 2,
       string.format("progress=%d done=%s metal=%d", p1, tostring(done),
                     materials.metal or 0))
    dailyGoal = keep
  end

  -- 采集每日目标：采浆果/补给会推进 forage 进度
  do
    local keep = dailyGoal
    local b = berryBushes and berryBushes[1] or nil
    local kb = inv.berry or 0
    dailyGoal = { id = "forage", name = "采集测试", target = 4, progress = 0,
                  done = false, reward = { metal = 1, fuel = 5 } }
    if b ~= nil then
      b.ready = true
      gatherBerries(1)
      b.ready = true
      gatherBerries(1)
    end
    local gotProgress = dailyGoal.progress or -1
    local gotDone = dailyGoal.done == true
    local progressed = gotProgress == 4 and gotDone
    inv.berry = kb > 0 and kb or nil
    dailyGoal = keep
    ok(r, "forage daily goal", b ~= nil and progressed,
       string.format("progress=%d done=%s", gotProgress,
                     tostring(gotDone)))
  end

  -- 今日目标 HUD 文案：进度与完成状态
  do
    local keep = dailyGoal
    dailyGoal = { id = "fish", name = "钓 3 条鱼", target = 3, progress = 1,
                  done = false }
    local running = dailyGoalHudText() or ""
    dailyGoal.done = true
    local finished = dailyGoalHudText() or ""
    dailyGoal = keep
    ok(r, "daily goal hud text",
       running:find("钓 3 条鱼", 1, true) ~= nil and
       running:find("1/3", 1, true) ~= nil and
       finished:find("✓", 1, true) ~= nil,
       string.format("running=%s finished=%s", running, finished))
  end

  -- 雨天积水：下雨生成、雨停清除、车开远后重新铺设
  do
    local sw = weather
    local sx, sz = rv.x, rv.z
    weather = "clear"
    updatePuddles(0)
    local dry = #puddles
    weather = "rain"
    updatePuddles(0)
    local wet = #puddles
    local recycled = false
    if wet > 0 then
      puddleCenter = { x = rv.x + 80, z = rv.z + 80 }
      updatePuddles(0)
      recycled = puddleCenter ~= nil and
                 math.abs((puddleCenter.x or 0) - rv.x) < 0.01
    end
    weather = "clear"
    updatePuddles(0)
    local after = #puddles
    weather = sw
    rv.x, rv.z = sx, sz
    ok(r, "rain puddles", dry == 0 and wet >= 4 and recycled and after == 0,
       string.format("dry=%d wet=%d recycled=%s after=%d", dry, wet,
                     tostring(recycled), after))
  end

  -- 雪地斑块：下雪生成、雪停清除、开远重铺
  do
    local sw = weather
    weather = "clear"
    updateSnowPatches(0)
    local dry = #snowPatches
    weather = "snow"
    updateSnowPatches(0)
    local snowy = #snowPatches
    local recycled = false
    if snowy > 0 then
      snowCenter = { x = rv.x + 80, z = rv.z + 80 }
      updateSnowPatches(0)
      recycled = snowCenter ~= nil and
                 math.abs((snowCenter.x or 0) - rv.x) < 0.01
    end
    weather = "clear"
    updateSnowPatches(0)
    local after = #snowPatches
    weather = sw
    ok(r, "snow patches",
       dry == 0 and snowy >= 6 and recycled and after == 0,
       string.format("dry=%d snow=%d recycled=%s after=%d", dry, snowy,
                     tostring(recycled), after))
  end

  -- 积雪斑块局部更滑：位置判定
  do
    local keep = snowPatches
    snowPatches = { { x = 0, z = 0 } }
    local hit = onSnowPatch(1.0, 1.0, 2.0)
    local miss = onSnowPatch(10, 10, 2.0)
    snowPatches = keep
    ok(r, "snow patch slip", hit == true and miss == false,
       string.format("hit=%s miss=%s", tostring(hit), tostring(miss)))
  end

  -- 雨天高速压过积水：水滑让局部抓地再降
  do
    local sw, sp = weather, rv.speed
    local sx, sz, keepP = rv.x, rv.z, puddles
    weather, rv.speed = "rain", 10
    rv.x, rv.z = 0, 0
    puddles = { { x = 0, z = 0 } }
    local wet = puddleGripScale()
    puddles = { { x = 50, z = 50 } }
    local far = puddleGripScale()
    weather = "clear"
    local clear = puddleGripScale()
    weather, rv.speed = sw, sp
    rv.x, rv.z, puddles = sx, sz, keepP
    ok(r, "puddle hydroplane",
       wet < 1.0 and wet > 0.8 and far == 1.0 and clear == 1.0,
       string.format("wet=%.2f far=%.2f clear=%.2f", wet, far, clear))
  end

  -- 农场风车：4 片叶片且会旋转
  do
    local blades = windmill and #(windmill.blades or {}) or 0
    local a0 = windmill and (windmill.angle or 0) or 0
    updateWindmill(1.0)
    local a1 = windmill and (windmill.angle or 0) or 0
    ok(r, "farm windmill",
       blades == 4 and a1 > a0 and windmill ~= nil and
       windmill.tower ~= nil,
       string.format("blades=%d angle %.2f->%.2f tower=%s", blades, a0, a1,
                     tostring(windmill ~= nil and windmill.tower ~= nil)))
  end

  -- 风车水泵：接水 +30L，进入 60 游戏分钟冷却
  do
    if windmill ~= nil then
      local w0, cd0 = water, windmillWaterMin
      water, windmillWaterMin = 0, 0
      local okWater = collectWindmillWater()
      local got = (water or 0) >= 29
      local cooling = (windmillWaterMin or 0) >= 59
      local again = collectWindmillWater()
      water, windmillWaterMin = w0, cd0
      ok(r, "windmill water",
         okWater and got and cooling and again == false,
         string.format("got=%s cooling=%s again=%s", tostring(got),
                       tostring(cooling), tostring(again)))
    end
  end

  -- 营地柴堆：每 60 分钟可取 1 木材，冷却中不能再取
  do
    local keepWood, keepCd = materials.wood or 0, campWoodMin
    local kt, kv = toast.text, toast.t
    materials.wood, campWoodMin = 0, 0
    local got = gatherCampWoodpile()
    local gotWood = materials.wood or 0
    local gotCd = campWoodMin or 0
    local one = gotWood == 1
    local cooling = gotCd >= 59
    local again = gatherCampWoodpile()
    materials.wood, campWoodMin = keepWood, keepCd
    toast.text, toast.t = kt, kv
    ok(r, "camp woodpile",
       got and one and cooling and again == false,
       string.format("wood=%d cd=%.0f again=%s", gotWood, gotCd,
                     tostring(again)))
  end

  -- 风车水泵升级：消耗材料，冷却从 60 分钟缩短到 30 分钟
  do
    local keepUp = windmillUpgraded
    local keepCd = windmillWaterMin
    local keepM, keepW = materials.metal or 0, materials.wood or 0
    local keepSave = autoSave
    autoSave = nil
    windmillUpgraded = false
    materials.metal, materials.wood = 2, 1
    local poor = upgradeWindmill()
    materials.metal, materials.wood = 3, 2
    local okUp = upgradeWindmill()
    local spent = (materials.metal or 0) == 0 and (materials.wood or 0) == 0
    local again = upgradeWindmill()
    local cd = windmillWaterReadyMinutes()
    windmillUpgraded, windmillWaterMin = keepUp, keepCd
    materials.metal, materials.wood = keepM, keepW
    autoSave = keepSave
    ok(r, "windmill upgrade",
       poor == false and okUp and spent and again == false and cd == 30,
       string.format("poor=%s up=%s cd=%.0f", tostring(poor),
                     tostring(okUp), cd))
  end

  -- 农场稻草人：5 件低多边形组合
  do
    local n = #farmScarecrow
    ok(r, "farm scarecrow", n >= 5, string.format("parts=%d", n))
  end

  -- 农田作物：4 行 × 6 列成排
  do
    local n = #farmCrops
    ok(r, "farm crops", n >= 20, string.format("crops=%d", n))
  end

  -- 浆果丛：采摘消耗并进入再生，成熟后可再次采集
  do
    local b = berryBushes and berryBushes[1] or nil
    local kb, kt = inv.berry or 0, toast.text
    local ktt = toast.t
    local okAll = false
    if b ~= nil then
      b.ready, b.t = true, 0
      local got = gatherBerries(1)
      local gained = (inv.berry or 0) == kb + 2
      local wentReady = b.ready == false
      local again = gatherBerries(1)
      b.t = 0.1
      updateBerryBushes(0.2)
      local regrew = b.ready == true
      okAll = got and gained and wentReady and again == false and regrew
    end
    inv.berry = kb > 0 and kb or nil
    toast.text, toast.t = kt, ktt
    ok(r, "berry bushes", #(berryBushes or {}) >= 6 and okAll,
       string.format("bushes=%d ok=%s", #(berryBushes or {}),
                     tostring(okAll)))
  end

  -- 按位置找最近成熟浆果丛（联机服务器用）
  do
    local b = berryBushes and berryBushes[1] or nil
    local okNear = false
    if b ~= nil then
      b.ready = true
      local near = nearestReadyBush(b.x, b.z, 2.8)
      local far = nearestReadyBush(b.x + 40, b.z, 2.8)
      okNear = near ~= nil and far == nil
    end
    ok(r, "nearest berry bush", okNear,
       string.format("found=%s", tostring(okNear)))
  end

  -- 路边补给箱：搜刮得到材料并进入再生，超时后恢复
  do
    local c = roadCaches and roadCaches[1] or nil
    local sm, sc = materials.metal or 0, materials.cloth or 0
    local kt, kv = toast.text, toast.t
    local okAll = false
    if c ~= nil then
      c.ready, c.t = true, 0
      local got = lootRoadCache(1)
      local gained = (materials.metal or 0) > sm and
                     (materials.cloth or 0) > sc
      local wentReady = c.ready == false
      local again = lootRoadCache(1)
      c.t = 0.1
      updateRoadCaches(0.2)
      local regrew = c.ready == true
      okAll = got and gained and wentReady and again == false and regrew
    end
    toast.text, toast.t = kt, kv
    ok(r, "road caches", #(roadCaches or {}) >= 2 and okAll,
       string.format("caches=%d ok=%s", #(roadCaches or {}),
                     tostring(okAll)))
  end

  -- 加油站自动售货机：1 金属换一份食物/水
  do
    local keepVm = vendingMachine
    vendingMachine = vendingMachine or { x = 0, z = 0 }
    local km = materials.metal or 0
    local kc, kw, kco = inv.canned or 0, inv.water or 0, inv.coffee or 0
    materials.metal = 0
    local poor = buyVendingSnack()
    materials.metal = 1
    local okBuy = buyVendingSnack()
    local spent = (materials.metal or 0) == 0
    local gained = (inv.canned or 0) + (inv.water or 0) + (inv.coffee or 0) >
                   kc + kw + kco
    materials.metal = km
    inv.canned = kc > 0 and kc or nil
    inv.water = kw > 0 and kw or nil
    inv.coffee = kco > 0 and kco or nil
    vendingMachine = keepVm
    ok(r, "vending machine",
       poor == false and okBuy and spent and gained,
       string.format("poor=%s buy=%s spent=%s", tostring(poor),
                     tostring(okBuy), tostring(spent)))
  end

  -- 路边抛锚车：生成后可搜刮一次，搜完消失
  do
    if despawnStrandedCar ~= nil then despawnStrandedCar() end
    local sm = materials.metal or 0
    local kt, kv = toast.text, toast.t
    local spawned = spawnStrandedCar()
    local visible = strandedCar ~= nil and strandedCar.body ~= nil
    local got = lootStrandedCar()
    local metalUp = (materials.metal or 0) > sm
    local cleared = strandedCar == nil
    local again = lootStrandedCar()
    if despawnStrandedCar ~= nil then despawnStrandedCar() end
    toast.text, toast.t = kt, kv
    ok(r, "stranded car",
       spawned and visible and got and metalUp and cleared and again == false,
       string.format("spawn=%s got=%s metal=%d cleared=%s", tostring(spawned),
                     tostring(got), (materials.metal or 0) - sm,
                     tostring(cleared)))
  end

  -- 路边搭车客：燃料不足不触发；给 5L 燃料后换回金属并消失
  do
    if despawnHitchhiker ~= nil then despawnHitchhiker() end
    local sf, sm, se = fuel or 0, materials.metal or 0,
                       materials.electronics or 0
    local kc, kr = inv.canned, inv.repairkit
    local kt, kv = toast.text, toast.t
    hitchhiker = { x = 0, z = 0 }
    fuel = 3
    local poor = helpHitchhiker()
    fuel = 20
    materials.metal = 0
    local helped = helpHitchhiker()
    local spent = math.abs((fuel or 0) - 15) < 1e-6
    local gotMetal = (materials.metal or 0) >= 3
    local cleared = hitchhiker == nil
    fuel, materials.metal, materials.electronics = sf, sm, se
    inv.canned, inv.repairkit = kc, kr
    toast.text, toast.t = kt, kv
    if despawnHitchhiker ~= nil then despawnHitchhiker() end
    ok(r, "hitchhiker",
       poor == false and helped and spent and gotMetal and cleared,
       string.format("poor=%s helped=%s spent=%s metal=%d cleared=%s",
                     tostring(poor), tostring(helped), tostring(spent),
                     (materials.metal or 0) - sm, tostring(cleared)))
  end

  -- 搭车客指路：指出最近未探索地点的方向/距离，全探索后返回 nil
  do
    local sx, sz = rv.x, rv.z
    rv.x, rv.z = 0, 0
    local cand = {
      { id = "poi_a", name = "林间木屋 #1", x = 0, z = 100, generated = true },
      { id = "poi_b", name = "路边废宅 #2", x = 100, z = 0, generated = true },
    }
    local north = hitchhikerHintText({ cand[1] })
    local east = hitchhikerHintText({ cand[2] })
    local none = hitchhikerHintText({})
    rv.x, rv.z = sx, sz
    ok(r, "hitchhiker hint",
       north ~= nil and north:find("北", 1, true) ~= nil and
       east ~= nil and east:find("东", 1, true) ~= nil and none == nil,
       string.format("north=%s east=%s", tostring(north), tostring(east)))
  end

  -- 废弃邮局邮箱：首次翻找给金属+补给，第二次为空
  do
    local km = materials.metal or 0
    local kc = inv.canned
    local kt, kv = toast.text, toast.t
    local nd = { id = "poi_mail", mailbox = { x = 0, z = 0, taken = false } }
    local first = lootMailbox(nd)
    local taken = nd.mailbox.taken == true
    local gotMetal = (materials.metal or 0) - km
    local second = lootMailbox(nd)
    materials.metal = km
    inv.canned = kc
    toast.text, toast.t = kt, kv
    ok(r, "mailbox loot",
       first and taken and gotMetal >= 1 and gotMetal <= 3 and second == false,
       string.format("taken=%s metal=%d second=%s", tostring(taken),
                     gotMetal, tostring(second)))
  end

  -- 地图标记：搭车客/抛锚车会出现在路边事件列表，已搜完的抛锚车不显示
  do
    local kh, ks = hitchhiker, strandedCar
    hitchhiker = { x = 1, z = 2 }
    strandedCar = { x = 3, z = 4, looted = false }
    local markers = roadEventMarkers()
    local hasH, hasS = false, false
    for _, m in ipairs(markers) do
      if m.kind == "hitchhiker" then hasH = true end
      if m.kind == "stranded" then hasS = true end
    end
    strandedCar.looted = true
    local afterLoot = roadEventMarkers()
    hitchhiker, strandedCar = kh, ks
    ok(r, "road event markers",
       hasH and hasS and #markers == 2 and #afterLoot == 1,
       string.format("h=%s s=%s count=%d looted=%d", tostring(hasH),
                     tostring(hasS), #markers, #afterLoot))
  end

  -- 小地图道路连线：营地附近能连出道路，范围太小时为空
  do
    local sx, sz = rv.x, rv.z
    rv.x, rv.z = 0, 0
    local near = minimapRoadPairs(140)
    local tiny = minimapRoadPairs(5)
    rv.x, rv.z = sx, sz
    local valid = true
    for _, p in ipairs(near) do
      if p.a == nil or p.b == nil then valid = false end
    end
    ok(r, "minimap roads",
       #near >= 2 and #tiny == 0 and valid,
       string.format("near=%d tiny=%d", #near, #tiny))
  end

  -- 路面/路肩材质：程序化沥青/碎石 albedo + normal + ORM 已接入 prefab
  do
    local seg = ReadText and ReadText("assets/prefabs/rv_road_seg.json") or nil
    local shoulder = ReadText and
      ReadText("assets/prefabs/rv_road_shoulder.json") or nil
    ok(r, "road surface textures",
       seg ~= nil and seg:find("rv_asphalt_albedo", 1, true) ~= nil and
       seg:find("rv_asphalt_normal", 1, true) ~= nil and
       shoulder ~= nil and
       shoulder:find("rv_gravel_albedo", 1, true) ~= nil,
       string.format("seg=%s shoulder=%s", tostring(seg ~= nil),
                     tostring(shoulder ~= nil)))
  end

  -- Lane paint is baked into the asphalt tile; no separate raised cubes.
  do
    local seg = ReadText and
      ReadText("assets/prefabs/rv_road_seg.json") or nil
    local dashes = #(roadDashes or {})
    ok(r, "road lane paint",
       seg ~= nil and seg:find("rv_asphalt_albedo", 1, true) ~= nil and
       dashes == 0,
       string.format("asphalt=%s dashes=%d", tostring(seg ~= nil), dashes))
  end

  -- 小地图额外标记：未搜刮补给箱和鱼笼
  do
    local kc, kt = roadCaches, pondTrap
    roadCaches = { { x = 1, z = 2, ready = true },
                   { x = 3, z = 4, ready = false } }
    pondTrap = { x = 5, z = 6, ready = true }
    local m = minimapMarkers()
    local cacheN, trapN = 0, 0
    for _, e in ipairs(m) do
      if e.kind == "cache" then cacheN = cacheN + 1 end
      if e.kind == "trap" then trapN = trapN + 1 end
    end
    roadCaches, pondTrap = kc, kt
    ok(r, "minimap markers", cacheN == 1 and trapN == 1,
       string.format("cache=%d trap=%d", cacheN, trapN))
  end

  -- 地图服务标签：营地/加油站/诊所/仓库可识别
  do
    local camp = mapServiceTag({ id = "camp", name = "营地" })
    local gas = mapServiceTag({ id = "gasstop", name = "废弃加油站" })
    local clinic = mapServiceTag({ id = "clinic", name = "路边诊所" })
    local depot = mapServiceTag({ id = "depot", name = "物流仓库" })
    local cabin = mapServiceTag({ id = "poi_9", name = "林间木屋 #9" })
    ok(r, "map service tags",
       camp == "营" and gas == "油" and clinic == "医" and depot == "仓" and
       cabin == "屋",
       string.format("camp=%s gas=%s clinic=%s depot=%s cabin=%s",
                     tostring(camp), tostring(gas), tostring(clinic),
                     tostring(depot), tostring(cabin)))
  end

  -- 按位置找最近可取补给箱（联机服务器用）
  do
    local c = roadCaches and roadCaches[1] or nil
    local okNear = false
    if c ~= nil then
      c.ready = true
      local near = nearestReadyCache(c.x, c.z, 2.9)
      local far = nearestReadyCache(c.x + 40, c.z, 2.9)
      okNear = near ~= nil and far == nil
    end
    ok(r, "nearest road cache", okNear,
       string.format("found=%s", tostring(okNear)))
  end

  -- 每日目标池：包含做饭/维修
  do
    local ids = {}
    for _, g in ipairs(DAILY_GOALS) do
      if g.id and g.name and g.target and g.reward then ids[g.id] = true end
    end
    ok(r, "daily goal pool", #DAILY_GOALS >= 6 and ids.jobs and ids.cook and
       ids.repair and ids.fish and ids.explore and ids.distance,
       string.format("goals=%d cook=%s repair=%s", #DAILY_GOALS,
                     tostring(ids.cook), tostring(ids.repair)))
  end

  -- 野兔：白天活跃、夜间隐藏
  do
    local sc = clock
    clock = 12 * 60
    local day = rabbitsActive()
    clock = 22 * 60
    local night = rabbitsActive()
    clock = sc
    ok(r, "rabbits day cycle", #rabbits >= 4 and day == true and night == false,
       string.format("rabbits=%d day=%s night=%s", #rabbits,
                     tostring(day), tostring(night)))
  end

  -- Wildlife models (Blender-authored glTF prefabs)
  do
    local names = { "rv_rabbit", "rv_bird", "rv_duck", "rv_deer" }
    local files = { "rabbit", "bird", "duck", "deer" }
    local missing = {}
    for i, n in ipairs(names) do
      local txt = ReadText and
        ReadText("assets/prefabs/" .. n .. ".json") or nil
      if txt == nil or
         txt:find("rv_" .. files[i] .. ".gltf", 1, true) == nil then
        missing[#missing + 1] = n
      end
    end
    ok(r, "wildlife models", #missing == 0,
       string.format("ok=%d/%d missing=%s", #names - #missing, #names,
                     table.concat(missing, ",")))
  end

  -- 农场鹿群：白天可见、夜间隐藏
  do
    local sc = clock
    clock = 12 * 60
    local day = deerActive()
    clock = 22 * 60
    local night = deerActive()
    clock = sc
    ok(r, "farm deer", #(ambientDeer or {}) >= 2 and day == true and
       night == false,
       string.format("deer=%d day=%s night=%s", #(ambientDeer or {}),
                     tostring(day), tostring(night)))
  end

  -- 水塘野鸭：白天游动、夜间隐藏
  do
    local sc = clock
    clock = 12 * 60
    local day = ducksActive()
    clock = 22 * 60
    local night = ducksActive()
    clock = sc
    ok(r, "pond ducks",
       #(pondDucks or {}) >= 3 and day == true and night == false,
       string.format("ducks=%d day=%s night=%s", #(pondDucks or {}),
                     tostring(day), tostring(night)))
  end

  -- 鸣笛惊走水塘野鸭：设置 fleeT，并随时间消退
  do
    if spawnPondDucks ~= nil then spawnPondDucks() end
    local n = #(pondDucks or {})
    local scared = scarePondDucks(0, 0, 1000)
    local before = (pondDucks[1] and pondDucks[1].fleeT) or 0
    updatePondDucks(1)
    local after = (pondDucks[1] and pondDucks[1].fleeT) or 0
    ok(r, "scare pond ducks",
       n >= 3 and scared == n and before > 0 and after < before,
       string.format("ducks=%d scared=%d flee=%.1f->%.1f", n, scared,
                     before, after))
  end

  -- 鸣笛惊走农场鹿群：设置 fleeT，并随时间消退
  do
    if spawnAmbientDeer ~= nil then spawnAmbientDeer() end
    local n = #(ambientDeer or {})
    local scared = scareAmbientDeer(0, 0, 500)
    local anyFlee = false
    for _, d in ipairs(ambientDeer or {}) do
      if (d.fleeT or 0) > 0 then anyFlee = true end
    end
    local before = (ambientDeer[1] and ambientDeer[1].fleeT) or 0
    updateAmbientDeer(1)
    local after = (ambientDeer[1] and ambientDeer[1].fleeT) or 0
    ok(r, "scare farm deer",
       n >= 2 and scared == n and anyFlee and after < before,
       string.format("deer=%d scared=%d flee=%.1f->%.1f", n, scared,
                     before, after))
  end

  -- 猎鹿：消耗一只农场鹿，获得 2 份野味
  do
    if spawnAmbientDeer ~= nil then spawnAmbientDeer() end
    local n = #(ambientDeer or {})
    local kf = inv.rawfood or 0
    local hunted = huntAmbientDeer(1)
    local got = (inv.rawfood or 0) == kf + 2
    local gone = #(ambientDeer or {}) == n - 1
    if spawnAmbientDeer ~= nil then spawnAmbientDeer() end
    ok(r, "hunt deer", n >= 2 and hunted and got and gone,
       string.format("deer=%d->%d hunted=%s rawfood=%d", n, n - 1,
                     tostring(hunted), (inv.rawfood or 0) - kf))
  end

  -- 蝴蝶：白天活动，夜晚与雨天隐藏
  do
    local sc, sw = clock, weather
    weather = "clear"
    clock = 12 * 60
    local dayOn = butterfliesActive()
    clock = 22 * 60
    local nightOff = butterfliesActive()
    clock = 12 * 60
    weather = "rain"
    local rainOff = butterfliesActive()
    clock, weather = sc, sw
    ok(r, "butterflies day cycle",
       #butterflies >= 6 and dayOn and not nightOff and not rainOff,
       string.format("n=%d day=%s night=%s rain=%s", #butterflies,
                     tostring(dayOn), tostring(nightOff), tostring(rainOff)))
  end

  -- 野生动物锚点：行驶中不跟随，停车远离后迁移
  do
    local keepA = wildlifeAnchor
    local sx, sz, ss = rv.x, rv.z, rv.speed
    wildlifeAnchor = { x = 0, z = 0 }
    rv.x, rv.z, rv.speed = 100, 100, 6
    local movedDriving = wildlifeAnchorStep()
    rv.speed = 0
    local movedParked = wildlifeAnchorStep()
    local anchored = math.abs(wildlifeAnchor.x - 100) < 0.01 and
                     math.abs(wildlifeAnchor.z - 100) < 0.01
    rv.x, rv.z, rv.speed = sx, sz, ss
    wildlifeAnchor = keepA
    ok(r, "wildlife anchor",
       movedDriving == false and movedParked == true and anchored,
       string.format("driving=%s parked=%s at=(%.0f,%.0f)",
                     tostring(movedDriving), tostring(movedParked),
                     wildlifeAnchor.x or 0, wildlifeAnchor.z or 0))
  end

  -- 路面障碍：生成实体+静态碰撞，绕过/超时后清理干净
  do
    local before = #driveProps
    spawnDriveRoadblock()
    local grp = driveProps[#driveProps]
    local spawned = #driveProps == before + 1 and grp ~= nil and
                    grp.kind == "roadblock" and #(grp.parts or {}) >= 2 and
                    #(grp.bodyIds or {}) >= 2
    local parts = grp and #(grp.parts or {}) or 0
    local ids = grp and #(grp.bodyIds or {}) or 0
    if grp then
      despawnDriveRoadblock(grp)
      table.remove(driveProps, #driveProps)
    end
    ok(r, "drive roadblock", spawned and #(grp and grp.parts or {}) == 0 and
       #(grp and grp.bodyIds or {}) == 0,
       string.format("parts=%d colliders=%d props=%d", parts, ids, #driveProps))
  end

  -- 路障有两种外观：倒树（rv_deadtree）/ 废弃车 + 落石
  do
    local tree = roadblockLayout("tree", 24, 1)
    local wreck = roadblockLayout("wreck", 24, -1)
    local treeOk = #tree == 3 and tree[1].pre == "rv_deadtree"
    local wreckOk = #wreck == 3 and wreck[1].pre == "rv_wreck"
    ok(r, "roadblock layout", treeOk and wreckOk,
       string.format("tree=%s wreck=%s", tostring(treeOk),
                     tostring(wreckOk)))
  end

  -- 车轮扬尘/胎烟：土路有、沥青无、手刹变胎烟
  do
    local dirt, dirtKind = wheelDustRate("dirt", 8, false)
    local asphalt = wheelDustRate("asphalt", 8, false)
    local smoke, smokeKind = wheelDustRate("asphalt", 8, true)
    local slow = wheelDustRate("dirt", 1.0, false)
    ok(r, "wheel dust",
       dirt > 0 and dirtKind == "dirt" and asphalt == 0 and
       smoke > dirt and smokeKind == "smoke" and slow == 0,
       string.format("dirt=%d asphalt=%d smoke=%d slow=%d", dirt, asphalt,
                     smoke, slow))
  end

  -- 积水飞溅：雨天、车速够快、压过积水才触发
  do
    local hit = puddleSplashActive("rain", 6.0, 2.0)
    local slow = puddleSplashActive("rain", 1.0, 2.0)
    local dry = puddleSplashActive("clear", 6.0, 2.0)
    local far = puddleSplashActive("rain", 6.0, 10.0)
    ok(r, "puddle splash",
       hit == true and slow == false and dry == false and far == false,
       string.format("hit=%s slow=%s dry=%s far=%s", tostring(hit),
                     tostring(slow), tostring(dry), tostring(far)))
  end

  -- 雪天雪雾：行驶时触发，慢速/晴天不触发
  do
    local fast = snowSprayKind("snow", 6.0)
    local slow = snowSprayKind("snow", 1.0)
    local clear = snowSprayKind("clear", 6.0)
    ok(r, "snow spray",
       fast == "snow" and slow == nil and clear == nil,
       string.format("fast=%s slow=%s clear=%s", tostring(fast),
                     tostring(slow), tostring(clear)))
  end

  -- 燃油罐：靠近房车加注，消耗 1 个、油量增加
  do
    local sf = fuel
    addItem("fuelcan", 1)
    fuel = math.max(0, VEH.tank - 6)
    local before = fuel
    local n0 = inv.fuelcan or 0
    local okUse = useFuelCan()
    local after = fuel
    local consumed = (inv.fuelcan or 0) == n0 - 1
    fuel = sf
    ok(r, "fuel can refuel", okUse and after > before and consumed,
       string.format("fuel %.0f->%.0f cans %d->%d", before, after, n0,
                     inv.fuelcan or 0))
  end

  -- 到站加油：营地免费、加油站按缺口扣金属、金属不足不加油
  do
    local sf, sm = fuel, materials.metal or 0
    local sj = jobDelivered
    jobDelivered = 0
    -- 营地：即使金属为 0 也免费加满
    materials.metal = 0
    fuel = 10
    local campOk = refuelAtNode("camp")
    local campFull = math.abs(fuel - fuelMax()) < 1e-6
    -- 加油站：金属不足（缺口 50L -> 4 金属）不加油
    fuel = fuelMax() - 50
    local okPoor = refuelAtNode("gasstop")
    local poorKept = math.abs(fuel - (fuelMax() - 50)) < 1e-6
    -- 加油站：金属充足时扣 4 金属并加满
    materials.metal = 10
    local okRich, cost = refuelAtNode("gasstop")
    local richFull = math.abs(fuel - fuelMax()) < 1e-6
    local charged = (materials.metal or 0) == 6 and cost == 4
    fuel, materials.metal = sf, sm
    jobDelivered = sj
    ok(r, "gas station refuel cost",
       campOk and campFull and (okPoor == false) and poorKept and
       okRich and richFull and charged,
       string.format("camp=%s free=%s poor=%s kept=%s rich=%s cost=%d",
                     tostring(campOk), tostring(campFull), tostring(okPoor),
                     tostring(poorKept), tostring(richFull), cost or 0))
  end

  -- 老顾客加油折扣：跑单越多每 15L 越便宜，最低 8 折
  do
    local new = fuelLoyaltyDiscount(0)
    local regular = fuelLoyaltyDiscount(5)
    local trusted = fuelLoyaltyDiscount(15)
    local veteran = fuelLoyaltyDiscount(30)
    local legend = fuelLoyaltyDiscount(50)
    local c0 = fuelServiceCost(150, 0)
    local c50 = fuelServiceCost(150, 50)
    ok(r, "fuel loyalty discount",
       new == 1.0 and regular < new and trusted < regular and
       veteran < trusted and legend < veteran and legend >= 0.80 and
       c50 < c0,
       string.format("1/%.2f 5/%.2f 15/%.2f 30/%.2f 50/%.2f cost %d->%d",
                     new, regular, trusted, veteran, legend, c0, c50))
  end

  -- 老顾客维修折扣：机油/快速维修基础价越高，折扣越明显
  do
    local c0 = discountedServiceCost(20, 0)
    local c15 = discountedServiceCost(20, 15)
    local c50 = discountedServiceCost(20, 50)
    local s0 = discountedServiceCost(4, 0)
    local s50 = discountedServiceCost(4, 50)
    ok(r, "service loyalty discount",
       c0 == 20 and c15 < c0 and c50 < c15 and c50 >= 16 and s50 < s0,
       string.format("0=%d 15=%d 50=%d | base4 %d->%d",
                     c0, c15, c50, s0, s50))
  end

  -- 老顾客折扣文案：新客不显示，可靠/传奇分别显示 -10%/-20%
  do
    local none = loyaltyStatusText(0)
    local trusted = loyaltyStatusText(15)
    local legend = loyaltyStatusText(50)
    ok(r, "loyalty status text",
       none == nil and trusted ~= nil and
       trusted:find("-10%", 1, true) ~= nil and legend ~= nil and
       legend:find("-20%", 1, true) ~= nil,
       string.format("0=%s 15=%s 50=%s", tostring(none),
                     tostring(trusted), tostring(legend)))
  end

  -- 长线目标：修好加油站油泵，加油永久 7 折；重复修复无效
  do
    local kfix = fuelPumpFixed
    local km, ke = materials.metal or 0, materials.electronics or 0
    local ka = autoSave
    autoSave = nil
    fuelPumpFixed = false
    materials.metal, materials.electronics = 10, 10
    local nd = { fuelPump = { x = 0, z = 0 } }
    local okFix = repairFuelPumpAt(nd)
    local fixed = fuelPumpFixed == true
    local spentMetal, spentElec = materials.metal or 0,
                                 materials.electronics or 0
    local cNormal = fuelServiceCost(150, 0, false)
    local cPump = fuelServiceCost(150, 0, true)
    local again = repairFuelPumpAt(nd)
    fuelPumpFixed, materials.metal, materials.electronics = kfix, km, ke
    autoSave = ka
    ok(r, "fuel pump repair",
       okFix and fixed and spentMetal == 5 and spentElec == 8 and
       cPump < cNormal and again == false,
       string.format("fixed=%s metal=%d elec=%d cost %d->%d again=%s",
                     tostring(fixed), spentMetal, spentElec, cNormal, cPump,
                     tostring(again)))
  end

  -- 终局目标：四类基础设施全部恢复时一次性发终局补给
  do
    local kR, kD = radioMastFixed, relayDone
    local kW, kP, kDone = waterTowerFixed, fuelPumpFixed, wastelandRestored
    local km, ke = materials.metal or 0, materials.electronics or 0
    local kc, kw = materials.cloth or 0, materials.wood or 0
    local kf, kwater = fuel, water
    local ka = autoSave
    autoSave = nil
    radioMastFixed, relayDone = false, false
    waterTowerFixed, fuelPumpFixed = false, false
    wastelandRestored = false
    local before = infrastructureComplete()
    radioMastFixed, relayDone = true, true
    waterTowerFixed, fuelPumpFixed = true, true
    local complete = infrastructureComplete()
    local first = checkInfrastructureMilestone()
    local restored = wastelandRestored == true
    local second = checkInfrastructureMilestone()
    radioMastFixed, relayDone = kR, kD
    waterTowerFixed, fuelPumpFixed = kW, kP
    wastelandRestored = kDone
    materials.metal, materials.electronics = km, ke
    materials.cloth, materials.wood = kc, kw
    fuel, water = kf, kwater
    autoSave = ka
    ok(r, "wasteland restored",
       before == false and complete == true and first and restored and
       second == false,
       string.format("before=%s complete=%s first=%s again=%s",
                     tostring(before), tostring(complete), tostring(first),
                     tostring(second)))
  end

  -- 基础设施进度文案：未开工不显示，1/4 显示进度，终局显示“荒原已恢复”
  do
    local kR, kD = radioMastFixed, relayDone
    local kW, kP, kDone = waterTowerFixed, fuelPumpFixed, wastelandRestored
    radioMastFixed, relayDone = false, false
    waterTowerFixed, fuelPumpFixed = false, false
    wastelandRestored = false
    local none = infrastructureStatusText()
    radioMastFixed = true
    local partial = infrastructureStatusText()
    wastelandRestored = true
    local done = infrastructureStatusText()
    radioMastFixed, relayDone = kR, kD
    waterTowerFixed, fuelPumpFixed = kW, kP
    wastelandRestored = kDone
    ok(r, "infrastructure status",
       none == nil and partial == "基建 1/4" and done == "荒原已恢复",
       string.format("none=%s partial=%s done=%s", tostring(none),
                     tostring(partial), tostring(done)))
  end

  -- 低油量导航：最近加油站的距离
  do
    local sx, sz = rv.x, rv.z
    rv.x, rv.z = 0, 60
    local near = nearestFuelStop(rv.x, rv.z)
    rv.x, rv.z = 0, 0
    local far = nearestFuelStop(rv.x, rv.z)
    rv.x, rv.z = sx, sz
    ok(r, "fuel stop hint",
       near ~= nil and near.id == "gasstop" and near.dist < 1 and
       far ~= nil and far.dist > near.dist,
       string.format("id=%s near=%.1f far=%.1f", tostring(near and near.id),
                     near and near.dist or -1, far and far.dist or -1))
  end

  -- 低油量提示文案：续航 + 加油站；金属不足时提示营地
  do
    local sf, sm, sx, sz = fuel, materials.metal or 0, rv.x, rv.z
    rv.x, rv.z = 0, 20
    fuel, materials.metal = 8, 0
    local poor = lowFuelHintText() or ""
    materials.metal = 99
    local rich = lowFuelHintText() or ""
    fuel, materials.metal, rv.x, rv.z = sf, sm, sx, sz
    ok(r, "low fuel hint",
       poor:find("续航", 1, true) ~= nil and
       poor:find("金属不足", 1, true) ~= nil and
       rich:find("金属不足", 1, true) == nil,
       string.format("poor=%s rich=%s", poor, rich))
  end

  -- 低油量警告音：油量 0.5%–5% 时每 8 秒响一次，其它区间不响
  do
    local low = lowFuelBeepInterval(0.03)
    local high = lowFuelBeepInterval(0.20)
    local empty = lowFuelBeepInterval(0)
    ok(r, "low fuel beep",
       low == 8 and high == 0 and empty == 0,
       string.format("low=%.0f high=%.0f empty=%.0f", low, high, empty))
  end

  -- 没油提示：油量归零且未提示过才给出
  do
    local first = fuelEmptyWarn(0, false)
    local again = fuelEmptyWarn(0, true)
    local hasFuel = fuelEmptyWarn(10, false)
    ok(r, "fuel empty warning",
       first ~= nil and again == nil and hasFuel == nil,
       string.format("first=%s again=%s has=%s", tostring(first),
                     tostring(again), tostring(hasFuel)))
  end

  -- 低油量罗盘标记：低于 20% 才显示加油站方向
  do
    local low = lowFuelCompassActive(0.1)
    local edge = lowFuelCompassActive(0.2)
    local full = lowFuelCompassActive(0.6)
    ok(r, "low fuel compass",
       low == true and edge == false and full == false,
       string.format("low=%s edge=%s full=%s", tostring(low),
                     tostring(edge), tostring(full)))
  end

  -- 生命垂危：罗盘标出诊所，并给出提示文案
  do
    local low = lowHealthCompassActive(10)
    local edge = lowHealthCompassActive(30)
    local healthy = lowHealthCompassActive(80)
    local hint = lowHealthHintText(20)
    local none = lowHealthHintText(80)
    ok(r, "low health hint",
       low == true and edge == false and healthy == false and
       hint ~= nil and hint:find("诊所", 1, true) ~= nil and none == nil,
       string.format("low=%s edge=%s hint=%s", tostring(low),
                     tostring(edge), tostring(hint)))
  end

  -- 帮助面板覆盖新增按键：保养 O / 装甲 A / 小睡 4 / 篝火菜单
  do
    local text = helpText and helpText() or ""
    local hasO = text:find("O", 1, true) ~= nil
    local hasA = text:find("装甲", 1, true) ~= nil
    local hasNap = text:find("小睡", 1, true) ~= nil
    local hasFire = text:find("篝火菜单", 1, true) ~= nil
    ok(r, "help sections",
       hasO and hasA and hasNap and hasFire,
       string.format("O=%s armor=%s nap=%s campfire=%s", tostring(hasO),
                     tostring(hasA), tostring(hasNap), tostring(hasFire)))
  end

  -- 帮助面板布局：两栏累计行数不超出面板高度
  do
    local ph, lineH, colLines = helpPanelLayout()
    local maxLines = math.max(colLines[1], colLines[2])
    ok(r, "help layout",
       ph <= 700 and maxLines > 0 and ph >= 82 + maxLines * lineH,
       string.format("ph=%d lines=%d/%d lineH=%d", ph, colLines[1],
                     colLines[2], lineH))
  end

  -- 货运板布局：8 条交易 + 3 张合同也能放进 720p
  do
    local ph1, sy1 = jobPanelLayout(3, 9, false)
    local ph2, sy2 = jobPanelLayout(3, 9, true)
    ok(r, "job panel layout",
       ph1 <= 700 and ph2 <= 700 and sy1 == 70 and sy2 == 112 and
       ph2 > ph1,
       string.format("ph=%d/%d start=%d/%d", ph1, ph2, sy1, sy2))
  end

  -- 联机随身物资摘要：包含热饭/净水/烤鱼/修理包/燃油罐/烟熏鱼
  do
    local km, kw, kf = inv.meal, inv.water, inv.fish
    local kr, kc, ks = inv.repairkit, inv.fuelcan, inv.smokedfish
    inv.meal, inv.water, inv.fish = 2, 3, 1
    inv.repairkit, inv.fuelcan, inv.smokedfish = 1, 4, 5
    local text = coopInvSummary and coopInvSummary() or ""
    inv.meal, inv.water, inv.fish = km, kw, kf
    inv.repairkit, inv.fuelcan, inv.smokedfish = kr, kc, ks
    ok(r, "coop inv summary",
       text:find("热2", 1, true) ~= nil and text:find("水3", 1, true) ~= nil and
       text:find("罐4", 1, true) ~= nil and text:find("熏5", 1, true) ~= nil,
       text)
  end

  -- 联机合同串：三张合同可编码/解析
  do
    local keep = jobOffers
    jobOffers = {
      { destId = "gasstop", crates = 2, pay = { metal = 7 }, urgent = true,
        cargoId = "med" },
      { destId = "ruin", crates = 1, pay = { metal = 3 }, fragile = true,
        cargoId = "parts" },
      { destId = "clinic", crates = 3, pay = { metal = 5 }, hazmat = true,
        cargoId = "coldmed" },
    }
    local tok = offerToken()
    local parsed = parseOfferToken(tok)
    local empty = parseOfferToken("-")
    jobOffers = keep
    ok(r, "coop offer token",
       #parsed == 3 and parsed[1].destId == "gasstop" and
       parsed[1].crates == 2 and parsed[1].payMetal == 7 and
       parsed[1].flags % 2 == 1 and parsed[2].destId == "ruin" and
       parsed[3].cargoId == "coldmed" and #empty == 0,
       tostring(tok))
  end

  -- 联机货运板交易按键：前 6 条 4-9，后 3 条 T/Y/U
  do
    local text = ""
    for i = 1, 9 do text = text .. coopTradeKey(i) end
    ok(r, "coop trade keys", text == "456789TYU",
       string.format("keys=%s", text))
  end

  -- 联机队友位置标记：x:z:剩余秒:槽位 可编码/解析，过期/空标记为 "-"
  do
    local tok = pingToken(18, -10, 7.5, 3)
    local p = parsePingToken(tok)
    local none = parsePingToken("-")
    local expired = pingToken(0, 0, 0, 1)
    ok(r, "coop ping token",
       p ~= nil and p.x == 18 and p.z == -10 and
       math.abs((p.t or 0) - 7.5) < 1e-6 and p.slot == 3 and
       none == nil and expired == "-",
       string.format("token=%s", tostring(tok)))
  end

  -- 极低油量供油不稳：2% 以上满动力，越低越弱
  do
    local full = fuelStarvationScale(0.10)
    local edge = fuelStarvationScale(0.02)
    local low = fuelStarvationScale(0.01)
    local none = fuelStarvationScale(0)
    ok(r, "fuel starvation",
       full == 1.0 and edge == 1.0 and low < 1.0 and low > 0.5 and none == 0,
       string.format("10%%=%.2f 2%%=%.2f 1%%=%.2f 0=%.2f",
                     full, edge, low, none))
  end

  -- 营地板以物易物：布料→金属、电子件→燃油罐、金属→燃料
  do
    local sMetal = materials.metal or 0
    materials.cloth = (materials.cloth or 0) + 3
    local traded = doTrade(1)
    local metalUp = (materials.metal or 0) == sMetal + 2
    materials.electronics = (materials.electronics or 0) + 2
    local cans0 = inv.fuelcan or 0
    local traded2 = doTrade(2)
    local canUp = (inv.fuelcan or 0) == cans0 + 1
    ok(r, "camp barter", traded and metalUp and traded2 and canUp,
       string.format("cloth->metal=%s elec->can=%s metal=%d cans=%d",
                     tostring(metalUp), tostring(canUp),
                     materials.metal or 0, inv.fuelcan or 0))
  end

  -- 以物易物：金属 x3 → 修理包 x1
  do
    local km, kr = materials.metal or 0, inv.repairkit or 0
    materials.metal = 3
    inv.repairkit = nil
    local traded = doTrade(5)
    local kit = (inv.repairkit or 0) == 1
    local spent = (materials.metal or 0) == 0
    materials.metal = km
    inv.repairkit = kr > 0 and kr or nil
    ok(r, "repair kit trade", traded and kit and spent,
       string.format("traded=%s kit=%d metal=%d", tostring(traded),
                     inv.repairkit or 0, materials.metal or 0))
  end

  -- 以物易物：金属 x4 → 备胎 x1
  do
    local km, ks = materials.metal or 0, inv.sparetire or 0
    materials.metal = 4
    inv.sparetire = nil
    local traded = doTrade(7)
    local gotTire = inv.sparetire or 0
    local leftMetal = materials.metal or 0
    local tire = gotTire == 1
    local spent = leftMetal == 0
    materials.metal = km
    inv.sparetire = ks > 0 and ks or nil
    ok(r, "spare tire trade", traded and tire and spent,
       string.format("traded=%s tire=%d metal=%d", tostring(traded),
                     gotTire, leftMetal))
  end

  -- 以物易物：生鱼 x3 → 金属 x2
  do
    local km, kr = materials.metal or 0, inv.rawfish or 0
    materials.metal = 0
    inv.rawfish = 3
    local traded = doTrade(8)
    local gotMetal = materials.metal or 0
    local leftFish = inv.rawfish or 0
    local okTrade = traded and gotMetal == 2 and leftFish == 0
    materials.metal = km
    inv.rawfish = kr > 0 and kr or nil
    ok(r, "fish trade", okTrade,
       string.format("traded=%s metal=%d fish=%d", tostring(traded),
                     gotMetal, leftFish))
  end

  -- 以物易物：烟熏鱼 x2 → 金属 x3
  do
    local km, kr = materials.metal or 0, inv.smokedfish or 0
    materials.metal = 0
    inv.smokedfish = 2
    local traded = doTrade(9)
    local gotMetal = materials.metal or 0
    local leftFish = inv.smokedfish or 0
    local okTrade = traded and gotMetal == 3 and leftFish == 0
    materials.metal = km
    inv.smokedfish = kr > 0 and kr or nil
    ok(r, "smoked fish trade", okTrade,
       string.format("traded=%s metal=%d fish=%d", tostring(traded),
                     gotMetal, leftFish))
  end

  -- 以物易物：野浆果 x4 → 鱼饵 x2
  do
    local kb, ka = inv.berry or 0, inv.bait or 0
    inv.berry = 4
    inv.bait = nil
    local traded = doTrade(6)
    local gotBait = inv.bait or 0
    local leftBerry = inv.berry or 0
    local bait = gotBait == 2
    local spent = leftBerry == 0
    inv.berry = kb > 0 and kb or nil
    inv.bait = ka > 0 and ka or nil
    ok(r, "berry bait trade", traded and bait and spent,
       string.format("traded=%s bait=%d berry=%d", tostring(traded),
                     gotBait, leftBerry))
  end

  -- 长期里程碑：探索 3 个地点一次性发奖，且不会重复发
  do
    local keep = milestones
    milestones = {}
    local m0 = materials.metal or 0
    checkMilestones("explore", 3)
    local first = (materials.metal or 0) > m0
    local m1 = materials.metal or 0
    checkMilestones("explore", 3)
    local repeat_ok = (materials.metal or 0) == m1
    milestones = keep
    ok(r, "exploration milestone", first and repeat_ok,
       string.format("metal %d->%d repeat=%s", m0, m1, tostring(repeat_ok)))
  end

  -- 夜空星星：白天无、深夜可见
  do
    local sc = clock
    clock = 12 * 60
    local day = starAlpha()
    clock = 22 * 60
    local night = starAlpha()
    clock = sc
    ok(r, "night sky stars", day == 0 and night > 0.5,
       string.format("day=%.2f night=%.2f", day, night))
  end

  -- 流星：14s 周期内有 0.8s 可见窗口
  do
    local p0, a0 = shootingStar(0)
    local p1, a1 = shootingStar(0.4)
    local p2, a2 = shootingStar(5.0)
    ok(r, "shooting stars",
       p0 ~= nil and a1 > 0 and a1 <= 0.85 and p2 == nil,
       string.format("t0=%s t.4=%.2f clear=%s", tostring(p0 ~= nil),
                     a1, tostring(p2 == nil)))
  end

  -- 白天太阳弧线：正午最高、早晚更低
  do
    local xm, ym = sunArc(6 * 60)
    local xn, yn = sunArc(12 * 60)
    local xe, ye = sunArc(18 * 60)
    ok(r, "day sky sun arc",
       math.abs(xn - 0.5) < 0.01 and yn < ym and yn < ye and
       math.abs(ym - ye) < 0.01,
       string.format("morning=(%.2f,%.2f) noon=(%.2f,%.2f) evening=(%.2f,%.2f)",
                     xm, ym, xn, yn, xe, ye))
  end

  -- 下一里程碑提示：探索/货运都给出下一个阈值
  do
    local sv, sj = visitedCount, jobDelivered
    visitedCount, jobDelivered = 2, 3
    local hint = milestoneHint()
    local okHint = hint:find("3", 1, true) ~= nil and hint:find("5", 1, true) ~= nil
    visitedCount, jobDelivered = sv, sj
    ok(r, "milestone hint", okHint, hint)
  end

  -- 野生动物横穿：生成后沿路横移，清理干净
  do
    local before = #driveDeer
    spawnDriveDeer(1)
    local d = driveDeer[#driveDeer]
    local spawned = #driveDeer == before + 1 and d ~= nil and
                    (d.vx or 0) ~= 0 and (d.life or 0) > 0
    local moved = false
    if d ~= nil then
      local x0 = d.x
      updateDriveWorld(0.5)
      moved = math.abs((d.x or x0) - x0) > 0.05
      for i = #driveDeer, 1, -1 do
        if driveDeer[i] == d then
          despawnDriveDeer(d)
          table.remove(driveDeer, i)
        end
      end
    end
    ok(r, "drive deer crossing", spawned and moved and #driveDeer == before,
       string.format("spawned=%s moved=%s left=%d", tostring(spawned),
                     tostring(moved), #driveDeer))
  end

  -- 撞到野生动物掉落野味
  do
    local raw0 = itemCount("rawfood")
    spawnDriveDeer(1)
    local d = driveDeer[#driveDeer]
    local hit = false
    if d ~= nil then
      d.x, d.z = rv.x, rv.z
      local keepSpeed = rv.speed
      rv.speed = 5.0
      updateDriveWorld(0.016)
      rv.speed = keepSpeed
      hit = itemCount("rawfood") == raw0 + 1
      for i = #driveDeer, 1, -1 do
        if driveDeer[i] == d then
          despawnDriveDeer(d)
          table.remove(driveDeer, i)
        end
      end
    end
    ok(r, "deer roadkill", hit,
       string.format("rawfood %d->%d", raw0, itemCount("rawfood")))
  end

  -- 房车喇叭：吓跑附近感染者，且有冷却
  do
    local keepCd, keepHonks = rv.honkCd, rv.honks
    rv.honkCd, rv.honks = 0, 0
    local ent = SpawnPrefab("rv_zombie", { x = rv.x + 4, y = 0.8, z = rv.z })
    local target = nil
    if ent ~= nil then
      target = { ent = ent, x = rv.x + 4, z = rv.z, speed = 1.5,
                 phase = 0, atkT = 0, life = 20 }
      horde[#horde + 1] = target
    end
    local first = honk()
    local scared = rv.honkScared or 0
    local second = honk()
    local cooldown = second == false
    rv.honkCd, rv.honks = keepCd, keepHonks
    ok(r, "rv horn", first and scared >= 1 and cooldown,
       string.format("scared=%d cooldown=%s", scared, tostring(cooldown)))
  end

  -- 鸣笛惊走附近的鹿
  do
    local keepCd = rv.honkCd
    rv.honkCd = 0
    spawnDriveDeer(1)
    local d = driveDeer[#driveDeer]
    local n0 = #driveDeer
    if d ~= nil then d.x, d.z = rv.x, rv.z end
    honk()
    local gone = #driveDeer == n0 - 1
    local counted = (rv.honkScaredWildlife or 0) >= 1
    rv.honkCd = keepCd
    ok(r, "horn scares deer", d ~= nil and gone and counted,
       string.format("deer %d->%d scared=%d", n0, #driveDeer,
                     rv.honkScaredWildlife or 0))
  end

  -- 鸣笛惊走附近的鸟和野兔
  do
    spawnRabbits()
    local n0 = #rabbits
    local scared = scareSmallGameNear(rv.x, rv.z, 60)
    local after = #rabbits
    local gone = after < n0
    spawnRabbits()
    ok(r, "horn scares small game", n0 >= 4 and scared > 0 and gone,
       string.format("rabbits %d->%d scared=%d", n0, after, scared))
  end

  -- 引擎水温：全油门升温、怠速降温、过热限速
  do
    local keepTemp = engineTemp
    engineTemp = 20
    for _ = 1, 600 do
      engineTemp = stepEngineTemp(engineTemp, 9.0, 1.0, 0.1)
    end
    local hot = engineTemp >= (VEH.tempOverheat or 110)
    local peak = engineTemp
    for _ = 1, 400 do
      engineTemp = stepEngineTemp(engineTemp, 0.0, 1.0, 0.1)
    end
    local cooled = engineTemp < peak - 20
    engineTemp = keepTemp
    ok(r, "engine temperature", hot and cooled and peak <= (VEH.tempMax or 125),
       string.format("peak=%.0f cooled=%.0f", peak, engineTemp))
  end

  -- 天气影响引擎散热：雪天/雨天/雾天升温更慢
  do
    local snow = weatherEngineHeatScale("snow")
    local rain = weatherEngineHeatScale("rain")
    local fog = weatherEngineHeatScale("fog")
    local clear = weatherEngineHeatScale("clear")
    ok(r, "weather engine heat",
       snow < rain and rain < fog and fog < clear and clear == 1.0,
       string.format("snow=%.2f rain=%.2f fog=%.2f clear=%.2f",
                     snow, rain, fog, clear))
  end

  -- 停车冷却：车速 0 时调用 updateEngineTemp 会逐步降温并解除过热
  do
    local keepTemp, keepSpeed, keepHot = engineTemp, rv.speed, rv.engineHot
    engineTemp, rv.speed, rv.engineHot = 120, 0, true
    for _ = 1, 600 do
      updateEngineTemp(0.1, 1.0)
    end
    local cooled = engineTemp < 120 and rv.engineHot == false
    local gotTemp, gotHot = engineTemp, rv.engineHot
    engineTemp, rv.speed, rv.engineHot = keepTemp, keepSpeed, keepHot
    ok(r, "parked engine cool", cooled,
       string.format("temp=%.0f hot=%s", gotTemp, tostring(gotHot)))
  end

  -- 冷车动力：20°C 时受限，60°C 起满动力
  do
    local cold = engineColdPowerScale(20)
    local mid = engineColdPowerScale(40)
    local warm = engineColdPowerScale(60)
    local hot = engineColdPowerScale(120)
    ok(r, "cold engine power",
       math.abs(cold - 0.85) < 1e-6 and mid > cold and
       math.abs(warm - 1.0) < 1e-6 and hot == 1.0,
       string.format("20=%.2f 40=%.2f 60=%.2f 120=%.2f",
                     cold, mid, warm, hot))
  end

  -- 冷车油耗：20°C 时多耗 12%，60°C 起恢复
  do
    local cold = engineColdFuelScale(20)
    local mid = engineColdFuelScale(40)
    local warm = engineColdFuelScale(60)
    ok(r, "cold engine fuel",
       math.abs(cold - 1.12) < 1e-6 and mid < cold and mid > 1.0 and
       math.abs(warm - 1.0) < 1e-6,
       string.format("20=%.2f 40=%.2f 60=%.2f", cold, mid, warm))
  end

  -- 货运信誉：每 5 单 +2%，上限 +30%
  do
    local jd = jobDelivered
    jobDelivered = 0
    local r0 = jobReputation()
    jobDelivered = 10
    local r10 = jobReputation()
    jobDelivered = 30
    local r30 = jobReputation()
    jobDelivered = 400
    local rMax = jobReputation()
    jobDelivered = jd
    ok(r, "job reputation",
       math.abs(r0 - 1.0) < 1e-6 and math.abs(r10 - 1.04) < 1e-6 and
       math.abs(r30 - 1.12) < 1e-6 and math.abs(rMax - 1.30) < 1e-6,
       string.format("0=%.2f 10=%.2f 30=%.2f max=%.2f", r0, r10, r30, rMax))
  end

  -- 信誉称号：随完成单数升级
  do
    local keep = jobDelivered
    jobDelivered = 0
    local a = jobReputationRank()
    jobDelivered = 5
    local b = jobReputationRank()
    jobDelivered = 15
    local c = jobReputationRank()
    jobDelivered = 30
    local d = jobReputationRank()
    jobDelivered = 50
    local e = jobReputationRank()
    jobDelivered = keep
    ok(r, "job reputation rank",
       a == "新手" and b == "熟手" and c == "可靠" and d == "老练" and
       e == "金牌",
       string.format("%s/%s/%s/%s/%s", a, b, c, d, e))
  end

  -- 连续送达：第 3 单起 +2%，上限 +10%；清零后回到 1.0
  do
    local keep = jobStreak
    jobStreak = 0
    local s0 = jobStreakBonus()
    jobStreak = 3
    local s3 = jobStreakBonus()
    jobStreak = 50
    local sMax = jobStreakBonus()
    jobStreak = 0
    local reset = jobStreakBonus()
    jobStreak = keep
    ok(r, "job streak bonus",
       math.abs(s0 - 1.0) < 1e-6 and math.abs(s3 - 1.02) < 1e-6 and
       math.abs(sMax - 1.10) < 1e-6 and math.abs(reset - 1.0) < 1e-6,
       string.format("0=%.2f 3=%.2f max=%.2f reset=%.2f", s0, s3, sMax, reset))
  end

  -- 爆胎风险：磨平/低压才有风险，修理后清除爆胎状态
  do
    local rOk = blowoutRisk(100, 2.4)
    local rTire = blowoutRisk(10, 2.4)
    local rPress = blowoutRisk(100, 1.4)
    local keep = rv.blowout
    rv.blowout = true
    clearBlowout()
    local cleared = rv.blowout == false
    rv.blowout = keep
    ok(r, "tire blowout", rOk == 0 and rTire > 0 and rPress > 0 and cleared,
       string.format("ok=%.3f tire=%.3f press=%.3f cleared=%s",
                     rOk, rTire, rPress, tostring(cleared)))
  end

  -- 爆胎跑偏：左侧向左拉、右侧向右拉，前轮比后轮更明显
  do
    local none = blowoutPullSteer(false, 1)
    local fl = blowoutPullSteer(true, 1)
    local fr = blowoutPullSteer(true, 2)
    local rl = blowoutPullSteer(true, 3)
    ok(r, "blowout pull steer",
       none == 0 and fl > 0 and fr < 0 and
       math.abs(fl) > math.abs(rl) and math.abs(fl) <= 0.06,
       string.format("none=%.3f fl=%.3f fr=%.3f rl=%.3f",
                     none, fl, fr, rl))
  end

  -- 高速爆胎风险：低速基准、高速上升且有上限
  do
    local slow = speedBlowoutScale(0)
    local mid = speedBlowoutScale(7)
    local fast = speedBlowoutScale(9)
    local cap = speedBlowoutScale(100)
    ok(r, "speed blowout risk",
       slow == 1.0 and mid > 1.0 and fast > mid and cap <= 2.2,
       string.format("0=%.2f 7=%.2f 9=%.2f cap=%.2f",
                     slow, mid, fast, cap))
  end

  -- 胎温影响爆胎风险：超过 100°C 后上升，有上限
  do
    local cool = tireTempBlowoutScale(60)
    local warm = tireTempBlowoutScale(110)
    local hot = tireTempBlowoutScale(125)
    local cap = tireTempBlowoutScale(300)
    ok(r, "tire temp blowout risk",
       cool == 1.0 and warm > 1.0 and hot > warm and cap <= 3.0,
       string.format("60=%.2f 110=%.2f 125=%.2f cap=%.2f",
                     cool, warm, hot, cap))
  end

  -- 爆胎音效：触发一次爆胎会播放 bang 并计数
  do
    local keepBlow, keepBangs = rv.blowout, rv.blowoutBangs
    rv.blowout, rv.blowoutBangs = false, 0
    local first = triggerBlowout()
    local again = triggerBlowout()
    local banged = (rv.blowoutBangs or 0) == 1
    local blown = rv.blowout == true
    local gotBangs = rv.blowoutBangs or 0
    rv.blowout, rv.blowoutBangs = keepBlow, keepBangs
    ok(r, "blowout bang",
       first and again == false and banged and blown,
       string.format("first=%s again=%s bangs=%d", tostring(first),
                     tostring(again), gotBangs))
  end

  -- 辅助用电：车内灯/大灯耗电
  do
    local a0 = auxPowerDraw(false, false)
    local ai = auxPowerDraw(true, false)
    local ah = auxPowerDraw(false, true)
    local aboth = auxPowerDraw(true, true)
    local drained = batteryStep(1.0, -(aboth + 0.2), 1.0, 5)
    ok(r, "aux power draw",
       a0 == 0 and ai > 0 and ah > ai and aboth > ah and drained < 1.0,
       string.format("off=%.2f light=%.2f head=%.2f both=%.2f left=%.2f",
                     a0, ai, ah, aboth, drained))
  end

  -- 浑浊水：水塘取水，篝火煮沸补水
  do
    local keepWater = water
    water = 40
    addItem("dirtywater", 1)
    local n0 = inv.dirtywater or 0
    local boiled = cookAtCampfire()
    local gained = water - 40
    local consumed = (inv.dirtywater or 0) == n0 - 1
    water = keepWater
    ok(r, "dirty water purification",
       boiled and gained == 15 and consumed,
       string.format("water 40->%d dirty %d->%d", 40 + gained, n0,
                     inv.dirtywater or 0))
  end

  -- 生水不能直接喝：不消耗，提示去篝火煮沸
  do
    local keep = inv.dirtywater or 0
    inv.dirtywater = 1
    local used = useItem("dirtywater")
    local kept = (inv.dirtywater or 0) == 1
    inv.dirtywater = keep > 0 and keep or nil
    ok(r, "raw water blocked", used == false and kept,
       string.format("used=%s kept=%s", tostring(used), tostring(kept)))
  end

  -- 卫生：干净睡得好、脏了恢复差
  do
    local f100 = hygieneEnergyFactor(100)
    local f0 = hygieneEnergyFactor(0)
    local keep = needs.energy
    local keepH = needs.hygiene
    local sd, sc = day, clock
    local sh, st2, shp = needs.hunger, needs.thirst, needs.health
    needs.energy, needs.hygiene = 20, 100
    local expectedClean = 20 + (100 - 20) * 1.0
    sleepInBed()
    local clean = math.abs(needs.energy - expectedClean) < 1e-6
    needs.energy, needs.hygiene = 20, 0
    sleepInBed()
    local dirty = needs.energy < expectedClean
    needs.energy, needs.hygiene = keep, keepH
    day, clock = sd, sc
    needs.hunger, needs.thirst, needs.health = sh, st2, shp
    morningReport = nil
    ok(r, "hygiene effect", f100 > f0 and clean and dirty,
       string.format("clean=%.0f dirty=%.0f factor %.2f->%.2f",
                     expectedClean, needs.energy, f100, f0))
  end

  -- 存档：水温/胎压/爆胎状态
  do
    local keepTemp = engineTemp
    local keepPressures = rv.tirePressureCold
    local keepBlow = rv.blowout
    engineTemp, rv.blowout = 87, true
    rv.tirePressureCold = { 2.1, 2.0, 1.9, 1.8 }
    saveLayout()
    engineTemp, rv.blowout = 20, false
    rv.tirePressureCold = { 2.4, 2.4, 2.4, 2.4 }
    loadLayout()
    local gotTemp = engineTemp or 0
    local gotBlow = rv.blowout
    local gotP4 = (rv.tirePressureCold or {})[4] or 0
    local okSave = math.abs(gotTemp - 87) < 1e-6 and gotBlow == true and
                   math.abs(gotP4 - 1.8) < 1e-6
    engineTemp, rv.blowout = keepTemp, keepBlow
    rv.tirePressureCold = keepPressures
    ok(r, "save vehicle condition", okSave,
       string.format("temp=%.0f blow=%s p4=%.1f", gotTemp,
                     tostring(gotBlow), gotP4))
  end

  -- 天气预报：下一段天气可预测，并按时切换
  do
    local sw, st2, sn, sf = weather, weatherT, weatherNext, weatherNextId
    local valid = { clear = true, cloudy = true, rain = true, fog = true,
                    snow = true }
    setWeather("clear")
    local f1 = weatherNextId
    weatherNextId = "rain"
    weatherT = weatherNext + 1
    updateWeather(0)
    local switched = weather == "rain"
    local f2 = weatherNextId
    weather, weatherT, weatherNext, weatherNextId = sw, st2, sn, sf
    ok(r, "weather forecast",
       valid[f1] == true and switched and valid[f2] == true,
       string.format("forecast1=%s switched=%s forecast2=%s",
                     tostring(f1), tostring(switched), tostring(f2)))
  end

  -- 里程里程碑：2km 一次性发奖，且下一里程碑提示包含里程
  do
    local keep = milestones
    milestones = {}
    local f0 = fuel
    checkMilestones("distance", 2000)
    local awarded = fuel >= f0
    local hint = milestoneHint()
    local hintOk = hint:find("km", 1, true) ~= nil
    milestones = keep
    ok(r, "distance milestone", awarded and hintOk, hint)
  end

  -- 小镇水塔：支架+水箱+顶盖
  do
    local n = #waterTowers
    local parts = (n > 0) and #(waterTowers[n].parts or {}) or 0
    ok(r, "town water tower",
       n >= 1 and parts >= 1 and waterTowers[n].model ~= nil,
       string.format("towers=%d parts=%d model=%s", n, parts,
                     tostring(waterTowers[n] ~= nil and
                              waterTowers[n].model ~= nil)))
  end

  -- 水塔修复：材料足够时修复并一次性补水，重复修复返回 false
  do
    local kfix = waterTowerFixed
    local km, kw = materials.metal or 0, materials.wood or 0
    local kwater = water
    waterTowerFixed = false
    materials.metal, materials.wood, water = 10, 10, 20
    local okFix = repairWaterTower()
    local fixed = waterTowerFixed == true
    local gotWater = water or 0
    local spentMetal = materials.metal or 0
    local spentWood = materials.wood or 0
    local again = repairWaterTower()
    waterTowerFixed, materials.metal, materials.wood = kfix, km, kw
    water = kwater
    ok(r, "water tower repair",
       okFix and fixed and math.abs(gotWater - 40) < 1e-6 and
       spentMetal == 5 and spentWood == 6 and again == false,
       string.format("fixed=%s water=%.0f metal=%d wood=%d again=%s",
                     tostring(fixed), gotWater, spentMetal, spentWood,
                     tostring(again)))
  end

  -- 货箱按货物类型换颜色
  do
    local med = jobCargoPrefab and jobCargoPrefab("med")
    local parts = jobCargoPrefab and jobCargoPrefab("parts")
    local elec = jobCargoPrefab and jobCargoPrefab("elec")
    local grain = jobCargoPrefab and jobCargoPrefab("grain")
    ok(r, "cargo crate variants",
       med ~= nil and parts ~= nil and elec ~= nil and grain ~= nil and
       med ~= parts and parts ~= elec and elec ~= grain,
       string.format("med=%s parts=%s elec=%s grain=%s", tostring(med),
                     tostring(parts), tostring(elec), tostring(grain)))
  end

  -- 营地休息加成：在营地睡觉回血更多，晨报标注“营地”
  do
    local sd, sc = day, clock
    local sh, st2, se, sg = needs.health, needs.thirst, needs.energy, needs.hygiene
    local sa = atNode
    atNode = "camp"
    needs.health, needs.energy, needs.hygiene = 50, 0, 100
    sleepInBed()
    local healthOk = needs.health == 64
    local gotHealth = needs.health
    local campOk = morningReport ~= nil and morningReport.camp == true
    atNode = sa
    day, clock = sd, sc
    needs.health, needs.thirst, needs.energy, needs.hygiene = sh, st2, se, sg
    morningReport = nil
    ok(r, "camp rest bonus", healthOk and campOk,
       string.format("health=%.0f camp=%s", gotHealth, tostring(campOk)))
  end

  -- 站在点燃的篝火旁持续保暖：4m 内且点燃
  do
    local near = campfireWarmthNear(0, 0, 2, 2, true)
    local far = campfireWarmthNear(0, 0, 10, 10, true)
    local out = campfireWarmthNear(0, 0, 2, 2, false)
    ok(r, "campfire warmth near",
       near == true and far == false and out == false,
       string.format("near=%s far=%s out=%s", tostring(near),
                     tostring(far), tostring(out)))
  end

  -- 营地篝火燃着时睡觉：额外保暖并提高体力恢复
  do
    local sd, sc, sa = day, clock, atNode
    local se, sg, sw = needs.energy, needs.hygiene, warmthMin
    local sk, sf = campfireOn, campfireFuelMin
    atNode, campfireOn, campfireFuelMin = "camp", true, 500
    needs.energy, needs.hygiene, warmthMin = 0, 100, 0
    sleepInBed()
    local gotEnergy = needs.energy or 0
    local gotWarm = warmthMin or 0
    day, clock, atNode = sd, sc, sa
    needs.energy, needs.hygiene, warmthMin = se, sg, sw
    campfireOn, campfireFuelMin = sk, sf
    morningReport = nil
    ok(r, "campfire sleep",
       gotEnergy > 55 and gotWarm >= 120,
       string.format("energy=%.0f warmth=%.0f", gotEnergy, gotWarm))
  end

  -- 小睡：推进 2 小时、恢复部分体力、进入睡饱
  do
    local sc, se, sh, st = clock, needs.energy, needs.hunger, needs.thirst
    local sr, sw = restedMin, wetness
    clock = 12 * 60
    needs.energy, needs.hunger, needs.thirst = 20, 80, 80
    restedMin, wetness = 0, 0
    local okNap = napInBed()
    local advanced = (clock or 0) == 14 * 60
    local gotEnergy = needs.energy or 0
    local rested = (restedMin or 0) >= 60
    clock = sc
    needs.energy, needs.hunger, needs.thirst = se, sh, st
    restedMin, wetness = sr, sw
    ok(r, "nap in bed",
       okNap and advanced and gotEnergy == 45 and rested,
       string.format("clock=%.0f energy=%.0f rested=%s",
                     (clock or 0) % 1440, gotEnergy, tostring(rested)))
  end

  -- 睡到傍晚：下午睡到当天 19:00，晚上睡到次日 19:00
  do
    local sd, sc, se = day, clock, needs.energy
    local sh, st, sg, sr = needs.hunger, needs.thirst, needs.hygiene, restedMin
    local sw = wetness
    local kt, kv = toast.text, toast.t
    day, clock = 9, 12 * 60
    needs.energy, needs.hunger, needs.thirst, needs.hygiene = 20, 80, 80, 60
    restedMin, wetness = 0, 0
    local okEvening = sleepUntilEvening()
    local sameDay = (day == 9 and clock == 19 * 60)
    local energy = needs.energy or 0
    day, clock = 9, 20 * 60
    local okNext = sleepUntilEvening()
    local nextDay = (day == 10 and clock == 19 * 60)
    day, clock, needs.energy = sd, sc, se
    needs.hunger, needs.thirst, needs.hygiene = sh, st, sg
    restedMin, wetness = sr, sw
    toast.text, toast.t = kt, kv
    ok(r, "sleep until evening",
       okEvening and sameDay and energy == 55 and okNext and nextDay,
       string.format("sameDay=%s energy=%.0f nextDay=%s", tostring(sameDay),
                     energy, tostring(nextDay)))
  end

  -- 湿着睡觉恢复打折：30% 以下无影响，越湿越少
  do
    local dry = sleepRecoveryScale(0)
    local damp = sleepRecoveryScale(50)
    local soaked = sleepRecoveryScale(100)
    ok(r, "wetness sleep penalty",
       dry == 1.0 and damp < dry and soaked < damp and soaked >= 0.60,
       string.format("0=%.2f 50=%.2f 100=%.2f", dry, damp, soaked))
  end

  -- 舒适度提高睡眠恢复，最多 +15%
  do
    local a = comfortSleepBonus(0)
    local b = comfortSleepBonus(30)
    local c = comfortSleepBonus(500)
    ok(r, "comfort sleep bonus", a == 0 and math.abs(b - 0.10) < 1e-6 and
       math.abs(c - 0.15) < 1e-6,
       string.format("0=%.2f 30=%.2f max=%.2f", a, b, c))
  end

  -- 猎兔：消耗一只兔子换 1 份野味
  do
    local raw0 = itemCount("rawfood")
    local n0 = #rabbits
    local okHunt = (n0 > 0) and huntRabbit(1)
    local gained = itemCount("rawfood") == raw0 + 1 and #rabbits == n0 - 1
    spawnRabbits()
    ok(r, "rabbit hunting", okHunt and gained and #rabbits >= 4,
       string.format("rawfood %d->%d rabbits %d->%d", raw0,
                     itemCount("rawfood"), n0, #rabbits))
  end

  -- 夜里篝火旁休息：只有夜晚/营地/步行/靠近火堆才生效
  do
    local sc, sa, sf = clock, atNode, onFoot
    local px, pz = player.x, player.z
    clock, atNode, onFoot = 22 * 60, "camp", true
    if campfire ~= nil then player.x, player.z = campfire.x, campfire.z end
    local night = campfireRestAvailable()
    clock = 12 * 60
    local day = campfireRestAvailable()
    clock, atNode, onFoot = sc, sa, sf
    player.x, player.z = px, pz
    ok(r, "campfire rest", night == true and day == false,
       string.format("night=%s day=%s", tostring(night), tostring(day)))
  end

  -- 篝火边休息会持续取暖
  do
    local a = campfireWarmthMinutes(0)
    local b = campfireWarmthMinutes(35)
    ok(r, "campfire warmth", a == 20 and b == 35,
       string.format("0->%.0f 35->%.0f", a, b))
  end

  -- 环境音乐：驾驶=引擎、雨天步行=雨声、营地收音机=radio、其它=风声
  do
    local dn, dv = desiredMusic("driving", "rain", true)
    local rn, rv2 = desiredMusic("parked", "rain", false)
    local wn, wv = desiredMusic("parked", "clear", false)
    local radioN, radioV = desiredMusic("parked", "clear", true)
    local sn = desiredMusic("parked", "snow", false)
    ok(r, "ambient music", dn == "engine" and rn == "rain" and wn == "wind" and
       radioN == "radio" and sn == "snowwind" and
       dv > 0 and rv2 > 0 and wv > 0 and radioV > 0,
       string.format("drive=%s rain=%s clear=%s radio=%s snow=%s",
                     dn, rn, wn, radioN, sn))
  end

  -- 四个电台：1=radio、2=radio2、3=radio3（天气台）、4=radio4（数字台）
  do
    local a = desiredMusic("parked", "clear", true, 1)
    local b = desiredMusic("parked", "clear", true, 2)
    local c = desiredMusic("parked", "clear", true, 3)
    local d = desiredMusic("parked", "clear", true, 4)
    ok(r, "radio stations",
       a == "radio" and b == "radio2" and c == "radio3" and d == "radio4",
       string.format("1=%s 2=%s 3=%s 4=%s",
                     tostring(a), tostring(b), tostring(c), tostring(d)))
  end

  -- 驾驶时车载电台：开启后用 radio/radio2/radio3/radio4 取代引擎声
  do
    local engine = desiredMusic("driving", "clear", false, 1, false)
    local radio1 = desiredMusic("driving", "clear", false, 1, true)
    local radio2 = desiredMusic("driving", "clear", false, 2, true)
    local radio3 = desiredMusic("driving", "clear", false, 3, true)
    local radio4 = desiredMusic("driving", "clear", false, 4, true)
    ok(r, "car radio",
       engine == "engine" and radio1 == "radio" and radio2 == "radio2" and
       radio3 == "radio3" and radio4 == "radio4",
       string.format("engine=%s r1=%s r2=%s r3=%s r4=%s", tostring(engine),
                     tostring(radio1), tostring(radio2), tostring(radio3),
                     tostring(radio4)))
  end

  -- 收音机广播：开启后按游戏分钟触发，关闭不触发
  do
    local keepOn, keepT, keepNext = radioOn, radioBroadcastT, radioBroadcastNext
    local keepToast, keepTT = toast.text, toast.t
    local keepJournal = journal
    journal = {}
    radioOn = false
    radioBroadcastT = 999
    local off = updateRadioBroadcast(10)
    radioOn = true
    radioBroadcastT, radioBroadcastNext = 0, 0.1
    local on = updateRadioBroadcast(1)
    local logged = #journal == 1 and journal[1].kind == "radio"
    local reset = (radioBroadcastT or -1) == 0 and (radioBroadcastNext or 0) > 0
    radioOn, radioBroadcastT, radioBroadcastNext = keepOn, keepT, keepNext
    toast.text, toast.t = keepToast, keepTT
    journal = keepJournal
    ok(r, "radio broadcast", off == false and on == true and logged and reset,
       string.format("off=%s on=%s logged=%s reset=%s", tostring(off),
                     tostring(on), tostring(logged), tostring(reset)))
  end

  -- 天气台（电台 3）：优先播报天气预报，且频率更高
  do
    local keepOn, keepStation = radioOn, radioStation
    local keepT, keepNext = radioBroadcastT, radioBroadcastNext
    local keepToast, keepTT = toast.text, toast.t
    local keepFixed = radioMastFixed
    local keepJournal = journal
    local keepNextId = weatherNextId
    journal = {}
    radioMastFixed = true
    weatherNextId = "rain"
    radioOn, radioStation = true, 3
    radioBroadcastT, radioBroadcastNext = 0, 0.1
    local fired = updateRadioBroadcast(1)
    local weatherLine = tostring(toast.text):find("天气台", 1, true) ~= nil
    local newInterval = radioBroadcastNext or 0
    local faster = newInterval > 0 and newInterval <= 70
    radioOn, radioStation = keepOn, keepStation
    radioBroadcastT, radioBroadcastNext = keepT, keepNext
    toast.text, toast.t = keepToast, keepTT
    radioMastFixed = keepFixed
    weatherNextId = keepNextId
    journal = keepJournal
    ok(r, "weather radio", fired and weatherLine and faster,
       string.format("fired=%s line=%s interval=%.0f", tostring(fired),
                     tostring(weatherLine), newInterval))
  end

  -- 数字台（电台 4）：播报一串数字
  do
    local keepOn, keepStation = radioOn, radioStation
    local keepT, keepNext = radioBroadcastT, radioBroadcastNext
    local keepToast, keepTT = toast.text, toast.t
    local keepFixed, keepJournal = radioMastFixed, journal
    journal = {}
    radioMastFixed = true
    radioOn, radioStation = true, 4
    radioBroadcastT, radioBroadcastNext = 0, 0.1
    local fired = updateRadioBroadcast(1)
    local line = tostring(toast.text)
    local numbers = line:find("数字台", 1, true) ~= nil
    radioOn, radioStation = keepOn, keepStation
    radioBroadcastT, radioBroadcastNext = keepT, keepNext
    toast.text, toast.t = keepToast, keepTT
    radioMastFixed, journal = keepFixed, keepJournal
    ok(r, "numbers station", fired and numbers, line)
  end

  -- 收音机按进度播报：无进度返回 nil，有中继时提到中继
  do
    local kRelay, kJobs = relayDone, jobDelivered
    local kFish, kVis = fishCaught, visitedCount
    local kAll = allUpgradesDone
    relayDone, jobDelivered, fishCaught, visitedCount = false, 0, 0, 0
    allUpgradesDone = false
    local none = radioProgressLine(0.5)
    relayDone = true
    local relay = radioProgressLine(0)
    relayDone, jobDelivered, fishCaught, visitedCount = kRelay, kJobs, kFish, kVis
    allUpgradesDone = kAll
    ok(r, "radio progress line",
       none == nil and relay ~= nil and relay:find("中继", 1, true) ~= nil,
       string.format("none=%s relay=%s", tostring(none), tostring(relay)))
  end

  -- 修塔后收音机切换为“信号恢复”广播池
  do
    local keep = radioMastFixed
    radioMastFixed = false
    local normal = radioBroadcastPool()
    local normalFirst = normal[1]
    radioMastFixed = true
    local fixed = radioBroadcastPool()
    local fixedFirst = fixed[1]
    local switched = #normal > 0 and #fixed > 0 and
                     fixedFirst ~= normalFirst and
                     (RADIO_BROADCASTS_FIXED == nil or
                      fixedFirst == RADIO_BROADCASTS_FIXED[1])
    radioMastFixed = keep
    ok(r, "radio post-repair", switched,
       string.format("normal=%s fixed=%s", tostring(normalFirst),
                     tostring(fixedFirst)))
  end

  -- 完成信号中继后切换为救援队广播池
  do
    local keepFixed, keepRelay = radioMastFixed, relayDone
    radioMastFixed, relayDone = false, false
    local normal = radioBroadcastPool()[1]
    radioMastFixed, relayDone = true, false
    local fixed = radioBroadcastPool()[1]
    radioMastFixed, relayDone = true, true
    local relay = radioBroadcastPool()[1]
    local switched = relay ~= fixed and relay ~= normal and
                     (RADIO_BROADCASTS_RELAY == nil or
                      relay == RADIO_BROADCASTS_RELAY[1])
    radioMastFixed, relayDone = keepFixed, keepRelay
    ok(r, "radio post-relay", switched,
       string.format("normal=%s fixed=%s relay=%s", tostring(normal),
                     tostring(fixed), tostring(relay)))
  end

  -- 收音机天气警告：根据下一段天气给出提示
  do
    local keep = weatherNextId
    weatherNextId = "rain";  local rain = radioWeatherWarning()
    weatherNextId = "snow";  local snow = radioWeatherWarning()
    weatherNextId = "fog";   local fog = radioWeatherWarning()
    weatherNextId = "clear"; local clear = radioWeatherWarning()
    weatherNextId = keep
    ok(r, "radio weather warning",
       rain ~= nil and snow ~= nil and fog ~= nil and clear == nil,
       string.format("rain=%s snow=%s fog=%s clear=%s",
                     tostring(rain ~= nil), tostring(snow ~= nil),
                     tostring(fog ~= nil), tostring(clear ~= nil)))
  end

  -- 信号强度：修塔前有杂音概率，修塔/中继后为 0
  do
    local keepFixed, keepRelay = radioMastFixed, relayDone
    radioMastFixed, relayDone = false, false
    local before = radioSignalLossChance()
    radioMastFixed = true
    local fixed = radioSignalLossChance()
    radioMastFixed, relayDone = false, true
    local relay = radioSignalLossChance()
    radioMastFixed, relayDone = keepFixed, keepRelay
    ok(r, "radio signal loss",
       before > 0 and fixed == 0 and relay == 0,
       string.format("before=%.2f fixed=%.2f relay=%.2f",
                     before, fixed, relay))
  end

  -- 钓鱼水塘木栈桥
  do
    local n = #docks
    local parts = (n > 0) and #(docks[n].parts or {}) or 0
    ok(r, "fishing docks", n >= 2 and parts >= 8,
       string.format("docks=%d parts=%d", n, parts))
  end

  -- 栈桥钓鱼：站桥上标记加成，结束后复位
  do
    local d = docks[1]
    local at = (d ~= nil) and nearDock(d.x, d.z + 4) or false
    local far = (d == nil) or (not nearDock(d.x + 500, d.z + 500))
    local px, pz = player.x, player.z
    if d ~= nil then player.x, player.z = d.x, d.z + 4 end
    startFishing()
    local flagged = fishingDock == true
    finishFishing(true)
    local cleared = fishingDock == false
    player.x, player.z = px, pz
    ok(r, "dock fishing", at and far and flagged and cleared,
       string.format("at=%s far=%s flagged=%s cleared=%s", tostring(at),
                     tostring(far), tostring(flagged), tostring(cleared)))
  end

  -- 仓库无线电塔：支架+信标，夜间闪烁、白天熄灭
  do
    local m = radioMast
    local parts = (m ~= nil) and #(m.parts or {}) or 0
    local sc = clock
    clock = 22 * 60
    if m ~= nil then m.t, m.lit = 0, false end
    updateRadioMast(0.1)
    local night = (m ~= nil) and (m.beacon ~= nil) and (m.lit == true)
    clock = 12 * 60
    updateRadioMast(0.1)
    local dayOff = (m ~= nil) and (m.lit == false)
    clock = sc
    ok(r, "radio mast",
       parts >= 1 and m ~= nil and m.model ~= nil and night and dayOff,
       string.format("parts=%d model=%s night=%s dayOff=%s", parts,
                     tostring(m ~= nil and m.model ~= nil),
                     tostring(night), tostring(dayOff)))
  end

  -- 长线目标：无线电塔修理（材料不足失败、足够则扣材料、常亮并给补给）
  do
    local sm = materials.metal or 0
    local se = materials.electronics or 0
    local keepFixed = radioMastFixed
    local cans0, kits0 = inv.fuelcan or 0, inv.repairkit or 0
    radioMastFixed = false
    materials.metal, materials.electronics = 2, 0
    local poor = repairRadioMast()
    local poorKept = radioMastFixed == false
    materials.metal, materials.electronics = 10, 5
    local okFix = repairRadioMast()
    local paid = (materials.metal or 0) == 4 and (materials.electronics or 0) == 2
    local fixed = radioMastFixed == true
    local supplied = (inv.fuelcan or 0) == cans0 + 1 and
                     (inv.repairkit or 0) == kits0 + 1
    radioMastFixed = keepFixed
    materials.metal, materials.electronics = sm, se
    inv.fuelcan = cans0 > 0 and cans0 or nil
    inv.repairkit = kits0 > 0 and kits0 or nil
    ok(r, "radio mast repair", (poor == false) and poorKept and okFix and
       paid and fixed and supplied,
       string.format("poor=%s paid=%s fixed=%s supply=%s metal=%d elec=%d",
                     tostring(poor), tostring(paid), tostring(fixed),
                     tostring(supplied), materials.metal or 0,
                     materials.electronics or 0))
  end

  -- 天气/时段影响钓鱼：雨天更好、深夜更难
  do
    local sw, sc = weather, clock
    weather, clock = "rain", 12 * 60
    local rc, rs = fishingConditionBonus()
    weather, clock = "clear", 22 * 60
    local nc, ns = fishingConditionBonus()
    weather, clock = "clear", 12 * 60
    local dc, ds = fishingConditionBonus()
    weather, clock = sw, sc
    ok(r, "fishing conditions", rc > dc and rs > ds and nc < dc and ns <= ds,
       string.format("rain=%.2f/%d night=%.2f/%d day=%.2f/%d",
                     rc, rs, nc, ns, dc, ds))
  end

  -- 鱼种偏好：雨天/夜间鲶鱼黑鱼权重更高，晴天鲫鱼鳊鱼略高
  do
    local cat = nil
    local ji = nil
    for _, sp in ipairs(FISH_SPECIES) do
      if sp.name == "鲶鱼" then cat = sp end
      if sp.name == "鲫鱼" then ji = sp end
    end
    local catRain = fishWeight(cat, "rain", false)
    local catClear = fishWeight(cat, "clear", false)
    local catNight = fishWeight(cat, "clear", true)
    local jiClear = fishWeight(ji, "clear", false)
    local jiRain = fishWeight(ji, "rain", false)
    ok(r, "fish weather preferences",
       cat ~= nil and ji ~= nil and catRain > catClear and
       catNight > catClear and jiClear > jiRain,
       string.format("cat rain=%.1f clear=%.1f night=%.1f ji clear=%.1f rain=%.1f",
                     catRain, catClear, catNight, jiClear, jiRain))
  end

  -- 鱼种名册：新增狗鱼/鲟鱼，尺寸区间与权重有效
  do
    local names = {}
    local valid = #FISH_SPECIES >= 8
    for _, sp in ipairs(FISH_SPECIES) do
      names[sp.name] = true
      if not (sp.min < sp.max and (sp.w or 0) > 0) then valid = false end
    end
    ok(r, "fish roster",
       valid and names["狗鱼"] == true and names["鲟鱼"] == true,
       string.format("species=%d dogfish=%s sturgeon=%s",
                     #FISH_SPECIES, tostring(names["狗鱼"]),
                     tostring(names["鲟鱼"])))
  end

  -- 限定鱼种：鳗鱼只在夜间，冰鱼只在雪天
  do
    local eel = nil
    local ice = nil
    for _, sp in ipairs(FISH_SPECIES) do
      if sp.name == "鳗鱼" then eel = sp end
      if sp.name == "冰鱼" then ice = sp end
    end
    local eelDay = eel and fishWeight(eel, "clear", false) or nil
    local eelNight = eel and fishWeight(eel, "clear", true) or nil
    local iceClear = ice and fishWeight(ice, "clear", false) or nil
    local iceSnow = ice and fishWeight(ice, "snow", true) or nil
    ok(r, "conditional fish species",
       eel ~= nil and ice ~= nil and eelDay == 0 and (eelNight or 0) > 0 and
       iceClear == 0 and (iceSnow or 0) > 0,
       string.format("eel day=%s night=%s ice clear=%s snow=%s",
                     tostring(eelDay), tostring(eelNight),
                     tostring(iceClear), tostring(iceSnow)))
  end

  -- 鱼图鉴：10 种、鳗鱼标夜间、冰鱼标雪天
  do
    local rows = fishBookRows()
    local eelHint, iceHint, normalHint = nil, nil, nil
    for _, row in ipairs(rows) do
      if row.name == "鳗鱼" then eelHint = row.hint end
      if row.name == "冰鱼" then iceHint = row.hint end
      if row.name == "鲫鱼" then normalHint = row.hint end
    end
    ok(r, "fish book hints",
       #rows == #FISH_SPECIES and #rows >= 10 and eelHint == "夜间" and
       iceHint == "雪天" and normalHint == nil,
       string.format("rows=%d eel=%s ice=%s normal=%s", #rows,
                     tostring(eelHint), tostring(iceHint),
                     tostring(normalHint)))
  end

  -- 背包面板布局：物品多时自动两栏且不超 720p
  do
    local pw1, ph1, cols1 = inventoryPanelLayout(5, 1280, 720)
    local pw2, ph2, cols2 = inventoryPanelLayout(21, 1280, 720)
    ok(r, "inventory layout",
       cols1 == 1 and ph1 <= 720 and cols2 == 2 and pw2 == 760 and
       ph2 <= 720,
       string.format("small=%dx%d/%d big=%dx%d/%d", pw1, ph1, cols1,
                     pw2, ph2, cols2))
  end

  -- 旅行日志：记录、封顶与内容
  do
    local keep = journal
    local sd, sc = day, clock
    journal = {}
    day, clock = 3, 9 * 60 + 15
    addJournal("测试事件 A", "job")
    addJournal("测试事件 B", "fish")
    local two = #journal == 2 and journal[1].text == "测试事件 A" and
                journal[2].kind == "fish" and journal[2].day == 3
    for i = 1, 40 do addJournal("填充 " .. i, "note") end
    local capped = #journal == 30 and journal[30].text == "填充 40"
    journal = keep
    day, clock = sd, sc
    ok(r, "journey log", two and capped,
       string.format("two=%s capped=%s n=%d", tostring(two), tostring(capped),
                     #journal))
  end

  -- 鱼类图鉴：行数与纪录/条数读取
  do
    local keepR, keepC = fishRecords, fishCounts
    fishRecords = { ["鲫鱼"] = 22 }
    fishCounts = { ["鲫鱼"] = 5 }
    local rows = fishBookRows()
    local ji, li = nil, nil
    for _, r in ipairs(rows) do
      if r.name == "鲫鱼" then ji = r end
      if r.name == "鲤鱼" then li = r end
    end
    local okBook = #rows == #FISH_SPECIES and ji ~= nil and
                   ji.record == 22 and ji.count == 5 and
                   li ~= nil and li.count == 0
    fishRecords, fishCounts = keepR, keepC
    ok(r, "fish encyclopedia", okBook,
       string.format("rows=%d 鲫鱼=%.0f/%.0f 鲤鱼count=%d",
                     #rows, ji and ji.record or 0, ji and ji.count or 0,
                     li and li.count or 0))
  end

  -- 集齐全部鱼种：一次性奖励，且不会重复领取
  do
    local keepC, keepDone = fishCounts, fishCollectionDone
    local sm, se = materials.metal or 0, materials.electronics or 0
    local kb, keepSave = inv.bait or 0, autoSave
    local kt, kv = toast.text, toast.t
    autoSave = nil
    fishCounts, fishCollectionDone = {}, false
    for _, sp in ipairs(FISH_SPECIES) do fishCounts[sp.name] = 1 end
    materials.metal, materials.electronics = 0, 0
    inv.bait = nil
    local first = checkFishCollection()
    local rewarded = (materials.metal or 0) == 8 and
                     (materials.electronics or 0) == 3 and
                     (inv.bait or 0) == 3
    local again = checkFishCollection()
    fishCounts, fishCollectionDone = keepC, keepDone
    materials.metal, materials.electronics = sm, se
    inv.bait = kb > 0 and kb or nil
    autoSave = keepSave
    toast.text, toast.t = kt, kv
    ok(r, "fish collection reward", first and rewarded and again == false,
       string.format("first=%s reward=%s again=%s", tostring(first),
                     tostring(rewarded), tostring(again)))
  end

  -- 60cm 大鱼奖励：一次性，给金属+5 鱼饵+2
  do
    local keepBest, keepBestName, keepCaught = bestFish, bestFishName, fishCaught
    local keepRecords, keepCounts = fishRecords, fishCounts
    local keepDone, keepMetal = bigFishRewardDone, materials.metal or 0
    local keepBait, keepGoal = inv.bait or 0, dailyGoal
    local kt, kv = toast.text, toast.t
    bestFish, bestFishName, fishCaught = 0, nil, 0
    fishRecords, fishCounts = {}, {}
    bigFishRewardDone, dailyGoal = false, nil
    materials.metal = 0
    inv.bait = nil
    recordFish("鲶鱼", 65, 1)
    local rewarded = (materials.metal or 0) == 5 and (inv.bait or 0) == 2
    local done = bigFishRewardDone == true
    recordFish("鲶鱼", 70, 1)
    local once = (materials.metal or 0) == 5 and (inv.bait or 0) == 2
    bestFish, bestFishName, fishCaught = keepBest, keepBestName, keepCaught
    fishRecords, fishCounts = keepRecords, keepCounts
    bigFishRewardDone, dailyGoal = keepDone, keepGoal
    materials.metal = keepMetal
    inv.bait = keepBait > 0 and keepBait or nil
    toast.text, toast.t = kt, kv
    ok(r, "big fish reward", rewarded and done and once,
       string.format("reward=%s done=%s once=%s", tostring(rewarded),
                     tostring(done), tostring(once)))
  end

  -- 全车升级满级：一次性奖励，未满级不触发
  do
    local kEng, kTir = upgrades.engine, upgrades.tires
    local kCar, kRod = upgrades.cargo, upgrades.rod
    local kWat, kFue = upgrades.water, upgrades.fuel
    local kDone, km, ke = allUpgradesDone, materials.metal or 0,
                          materials.electronics or 0
    local kf, kt, kv = fuel or 0, toast.text, toast.t
    local keepSave = autoSave
    autoSave = nil
    upgrades.engine, upgrades.tires = 2, 3
    upgrades.cargo, upgrades.rod = 3, 3
    upgrades.water, upgrades.fuel = 3, 3
    allUpgradesDone = false
    local notYet = allUpgradesMaxed()
    local noReward = checkUpgradeMilestone()
    upgrades.engine = 3
    materials.metal, materials.electronics, fuel = 0, 0, 0
    local granted = checkUpgradeMilestone()
    local rewarded = (materials.metal or 0) == 10 and
                     (materials.electronics or 0) == 5 and (fuel or 0) >= 19
    local again = checkUpgradeMilestone()
    upgrades.engine, upgrades.tires = kEng, kTir
    upgrades.cargo, upgrades.rod = kCar, kRod
    upgrades.water, upgrades.fuel = kWat, kFue
    allUpgradesDone, materials.metal, materials.electronics = kDone, km, ke
    fuel, toast.text, toast.t = kf, kt, kv
    autoSave = keepSave
    ok(r, "all upgrades reward",
       notYet == false and noReward == false and granted and rewarded and
       again == false,
       string.format("notYet=%s reward=%s again=%s", tostring(notYet),
                     tostring(rewarded), tostring(again)))
  end

  -- 跑遍所有固定地点：一次性奖励，且不会重复领取
  do
    local keepVisited = visitedPois
    local keepDone = exploreAllDone
    local sm, se, sc = materials.metal or 0, materials.electronics or 0,
                       materials.cloth or 0
    local sf, keepSave = fuel or 0, autoSave
    local kt, kv = toast.text, toast.t
    autoSave = nil
    visitedPois = { gasstop = true, ruin = true, mall = true, town = true,
                    depot = true, farm = true, lakecamp = true, clinic = true }
    exploreAllDone = false
    materials.metal, materials.electronics, materials.cloth = 0, 0, 0
    fuel = 10
    local first = checkExplorationMilestone()
    local rewarded = (materials.metal or 0) == 12 and
                     (materials.electronics or 0) == 4 and
                     (materials.cloth or 0) == 4 and (fuel or 0) > 10
    local again = checkExplorationMilestone()
    visitedPois, exploreAllDone = keepVisited, keepDone
    materials.metal, materials.electronics, materials.cloth = sm, se, sc
    fuel = sf
    autoSave = keepSave
    toast.text, toast.t = kt, kv
    ok(r, "explore all reward", first and rewarded and again == false,
       string.format("first=%s reward=%s again=%s", tostring(first),
                     tostring(rewarded), tostring(again)))
  end

  -- 路牌笔记：阅读写入日志，集齐全部后一次性奖励，且不能重复读
  do
    local keepFound, keepDone = notesFound, loreRewardDone
    local keepJournal = journal
    local sm, se, sf = materials.metal or 0, materials.electronics or 0,
                       fuel or 0
    local kt, kv = toast.text, toast.t
    local keepSave = autoSave
    autoSave = nil
    notesFound, loreRewardDone = {}, false
    journal = {}
    materials.metal, materials.electronics = 0, 0
    fuel = 0
    local total = #(LORE_NOTES or {})
    local read = 0
    for i = 1, #(loreNotes or {}) do
      if readLoreNote(i) then read = read + 1 end
    end
    local rewarded = (materials.metal or 0) == 8 and
                     (materials.electronics or 0) == 2 and (fuel or 0) >= 14
    local logged = #journal >= total
    local again = #(loreNotes or {}) > 0 and readLoreNote(1)
    notesFound, loreRewardDone = keepFound, keepDone
    journal = keepJournal
    materials.metal, materials.electronics = sm, se
    fuel = sf
    autoSave = keepSave
    toast.text, toast.t = kt, kv
    if refreshLoreNotes ~= nil then refreshLoreNotes() end
    ok(r, "lore notes",
       total >= 6 and read == total and rewarded and logged and again == false,
       string.format("notes=%d read=%d reward=%s logged=%s again=%s",
                     total, read, tostring(rewarded), tostring(logged),
                     tostring(again)))
  end

  -- 地图标记：只有已探索地点的未读笔记才显示
  do
    local n = loreNotes and loreNotes[1] or nil
    local keepFound, keepVisited = notesFound, visitedPois
    local shown, hiddenRead, hiddenUnvisited = false, false, false
    if n ~= nil and n.node ~= nil then
      notesFound, visitedPois = {}, { [n.node] = true }
      shown = loreNoteMapVisible(n) == true
      notesFound[n.id] = true
      hiddenRead = loreNoteMapVisible(n) == false
      notesFound = {}
      visitedPois = {}
      hiddenUnvisited = loreNoteMapVisible(n) == false
    end
    notesFound, visitedPois = keepFound, keepVisited
    ok(r, "lore note map",
       n ~= nil and shown and hiddenRead and hiddenUnvisited,
       string.format("shown=%s read=%s unvisited=%s", tostring(shown),
                     tostring(hiddenRead), tostring(hiddenUnvisited)))
  end

  -- 笔记位串：用于联机同步已读状态
  do
    local keep = notesFound
    notesFound = {}
    local none = notesMask()
    local total = #(LORE_NOTES or {})
    local first = LORE_NOTES and LORE_NOTES[1] or nil
    if first ~= nil then notesFound[first.id] = true end
    local one = notesMask()
    notesFound = keep
    local allZero = #none == total and not none:find("1", 1, true)
    local firstOne = first ~= nil and one:sub(1, 1) == "1"
    ok(r, "notes mask",
       total >= 6 and allZero and firstOne,
       string.format("total=%d none=%s one=%s", total, none, one))
  end

  -- 笔记收集进度文案
  do
    local keep = notesFound
    notesFound = {}
    local none = loreProgressText() or ""
    local first = LORE_NOTES and LORE_NOTES[1] or nil
    if first ~= nil then notesFound[first.id] = true end
    local one = loreProgressText() or ""
    local total = #(LORE_NOTES or {})
    notesFound = keep
    local noneOk = none:find("0/" .. total, 1, true) ~= nil
    local oneOk = one:find("1/" .. total, 1, true) ~= nil
    ok(r, "lore progress text",
       total >= 6 and noneOk and oneOk,
       string.format("none=%s one=%s", none, one))
  end

  -- 旅行日志摘要：天数/货运/里程/钓鱼/笔记
  do
    local keepDay, keepJobs = day, jobDelivered
    local keepFish, keepDist = fishCaught, tripDistance
    local keepVisited, keepStreak = visitedCount, jobStreak
    day, jobDelivered, fishCaught, tripDistance = 5, 3, 7, 1234
    visitedCount, jobStreak = 2, 4
    local summary = journalSummaryText()
    day, jobDelivered, fishCaught, tripDistance = keepDay, keepJobs,
                                                keepFish, keepDist
    visitedCount, jobStreak = keepVisited, keepStreak
    ok(r, "journal summary",
       summary:find("第 5 天", 1, true) ~= nil and
       summary:find("货运 3 单", 1, true) ~= nil and
       summary:find("里程 1.2km", 1, true) ~= nil and
       summary:find("笔记", 1, true) ~= nil and
       summary:find("连送 4", 1, true) ~= nil,
       summary)
  end

  -- 地点特色掉落：数据完整，且物品都在 ITEMS 里
  do
    local function has(id, item)
      for _, b in ipairs(poiLootBonus(id)) do
        if b.id == item then return true end
      end
      return false
    end
    local allItems = true
    for _, id in ipairs({ "clinic", "farm", "gasstop", "ruin", "lakecamp",
                          "depot", "mall", "town" }) do
      for _, b in ipairs(poiLootBonus(id)) do
        if ITEMS[b.id] == nil then allItems = false end
      end
    end
    ok(r, "poi loot variety",
       has("farm", "seed") and has("gasstop", "fuelcan") and
       has("clinic", "repairkit") and allItems,
       string.format("allItems=%s farmSeed=%s gasCan=%s clinicKit=%s",
                     tostring(allItems), tostring(has("farm", "seed")),
                     tostring(has("gasstop", "fuelcan")),
                     tostring(has("clinic", "repairkit"))))
  end

  -- 拖车：野外花 4 金属回营地；金属不足/已在营地时拒绝
  do
    local sm = materials.metal or 0
    local sx, sz, syaw, ssp = rv.x, rv.z, rv.yaw, rv.speed
    local sphase = phase
    phase = "driving"
    rv.x, rv.z, rv.speed = 120, 120, 0
    materials.metal = 10
    local okTow = callTow()
    local moved = math.abs(rv.x) < 0.01 and math.abs(rv.z) < 0.01
    local charged = (materials.metal or 0) == 6
    rv.x, rv.z = 120, 120
    materials.metal = 0
    local poor = callTow()
    local stayed = math.abs(rv.x - 120) < 0.01 and math.abs(rv.z - 120) < 0.01
    materials.metal = sm
    rv.x, rv.z, rv.yaw, rv.speed = sx, sz, syaw, ssp
    phase = sphase
    ok(r, "emergency tow", okTow and moved and charged and
       (poor == false) and stayed,
       string.format("ok=%s moved=%s charged=%s poor=%s stayed=%s",
                     tostring(okTow), tostring(moved), tostring(charged),
                     tostring(poor), tostring(stayed)))
  end

  -- 自动存档：静默保存，不覆盖当前提示
  do
    local kt, kv = toast.text, toast.t
    toast.text, toast.t = "测试提示", 2.0
    local okSave = autoSave()
    local quiet = toast.text == "测试提示" and math.abs((toast.t or 0) - 2.0) < 1e-6
    toast.text, toast.t = kt, kv
    ok(r, "auto save", okSave == true and quiet,
       string.format("ok=%s quiet=%s", tostring(okSave), tostring(quiet)))
  end

  -- 自动存档提示：保存后闪烁计时被点亮
  do
    local keep = autoSaveFlashT
    autoSaveFlashT = 0
    local okSave = autoSave()
    local flashed = (autoSaveFlashT or 0) > 0
    autoSaveFlashT = keep
    ok(r, "auto save flash", okSave == true and flashed,
       string.format("ok=%s flash=%.1f", tostring(okSave), keep or 0))
  end

  -- 雨停后彩虹：白天触发，夜晚不触发
  do
    local sw, st, sn, sf = weather, weatherT, weatherNext, weatherNextId
    local sc, sr = clock, rainbowT
    local kt, kv = toast.text, toast.t
    clock = 10 * 60
    weather, rainbowT = "rain", 0
    setWeather("clear")
    local dayOn = (rainbowT or 0) > 0 and rainbowAlpha() > 0
    weather, rainbowT = "rain", 0
    clock = 23 * 60
    setWeather("clear")
    local nightOff = (rainbowT or 0) == 0
    weather, weatherT, weatherNext, weatherNextId = sw, st, sn, sf
    clock, rainbowT = sc, sr
    toast.text, toast.t = kt, kv
    ok(r, "rainbow after rain", dayOn and nightOff,
       string.format("day=%s night=%s", tostring(dayOn), tostring(nightOff)))
  end

  -- 日出/日落暖色：正午无、傍晚与清晨最强
  do
    local noon = skyWarmth(12 * 60)
    local dusk = skyWarmth(18 * 60)
    local dawn = skyWarmth(6 * 60 + 30)
    local night = skyWarmth(23 * 60)
    ok(r, "sunset tint", noon == 0 and dusk > 0.9 and dawn > 0.9 and night == 0,
       string.format("noon=%.2f dusk=%.2f dawn=%.2f night=%.2f",
                     noon, dusk, dawn, night))
  end

  -- 雪天：天气名、抓地折减与挡风玻璃逻辑
  do
    local sw = weather
    weather = "snow"
    local grip = weatherGripScale()
    local wind = windshieldDropCount()
    weather = sw
    ok(r, "snow weather",
       WEATHER_NAMES.snow ~= nil and math.abs(grip - 0.78) < 1e-6 and
       wind == 0,
       string.format("name=%s grip=%.2f windshield=%d",
                     tostring(WEATHER_NAMES.snow), grip, wind))
  end

  -- 雪天防滑链提示：没链条才提示，已有链条/非雪天不提示
  do
    local bare = snowChainHint("snow", false)
    local chained = snowChainHint("snow", true)
    local rain = snowChainHint("rain", false)
    ok(r, "snow chain hint",
       bare ~= nil and chained == nil and rain == nil,
       string.format("bare=%s chained=%s rain=%s", tostring(bare),
                     tostring(chained), tostring(rain)))
  end

  -- 天气预报文案：包含当前与下一段天气
  do
    local sw, sn, st = weather, weatherNextId, weatherT
    weather, weatherNextId, weatherT = "clear", "rain", 0
    local text = weatherForecastText()
    weather, weatherNextId, weatherT = sw, sn, st
    ok(r, "weather forecast text",
       text:find("晴", 1, true) ~= nil and text:find("雨", 1, true) ~= nil,
       tostring(text))
  end

  -- 环境气温：白天比夜里高，雪天明显更低
  do
    local noon = ambientTempC(12 * 60, "clear")
    local night = ambientTempC(0, "clear")
    local snow = ambientTempC(12 * 60, "snow")
    ok(r, "ambient temperature",
       noon > night and snow < noon and snow <= noon - 10,
       string.format("noon=%d night=%d snow=%d", noon, night, snow))
  end

  -- 潮湿：淋雨比下雪湿得快，有掩体不湿；篝火烘干最快，湿透更耗体力
  do
    local rainGain = wetnessGainRate("rain", true, false)
    local snowGain = wetnessGainRate("snow", true, false)
    local clearGain = wetnessGainRate("clear", true, false)
    local shelterGain = wetnessGainRate("rain", true, true)
    local fireDry = wetnessDryRate("rain", true, false)
    local heatDry = wetnessDryRate("rain", false, true)
    local normalDry = wetnessDryRate("rain", false, false)
    local wetExtra = wetnessEnergyExtra(80)
    local dryExtra = wetnessEnergyExtra(20)
    local warn = wetnessWarningText(80)
    local noWarn = wetnessWarningText(40)
    ok(r, "wetness model",
       rainGain > snowGain and snowGain > 0 and clearGain == 0 and
       shelterGain == 0 and fireDry > heatDry and heatDry > normalDry and
       wetExtra > 0 and dryExtra == 0 and
       warn ~= nil and noWarn == nil,
       string.format("rain=%.1f snow=%.1f fire=%.1f heat=%.1f wetExtra=%.3f",
                     rainGain, snowGain, fireDry, heatDry, wetExtra))
  end

  -- 轮胎环境温度：雪天低于晴天、夹在 5..30，低环境温度让胎温降得更低
  do
    local sw, sc = weather, clock
    weather, clock = "clear", 12 * 60
    local clear = tireAmbientC()
    weather = "snow"
    local snow = tireAmbientC()
    weather, clock = sw, sc
    local cold = { 90, 90, 90, 90 }
    local warm = { 90, 90, 90, 90 }
    for _ = 1, 120 do
      cold = tireRibStep(cold, 0, 0, 0, 0, 1 / 60, 5)
      warm = tireRibStep(warm, 0, 0, 0, 0, 1 / 60, 25)
    end
    local coldT, warmT = cold[1], warm[1]
    ok(r, "tire ambient temp",
       clear >= 5 and clear <= 30 and snow >= 5 and snow <= 30 and
       snow < clear and coldT < warmT,
       string.format("clear=%d snow=%d coldT=%.1f warmT=%.1f",
                     clear, snow, coldT, warmT))
  end

  -- 天气影响步行速度：雪地最慢、雨天略慢、晴天正常
  do
    local snow = walkSpeedScale("snow")
    local rain = walkSpeedScale("rain")
    local clear = walkSpeedScale("clear")
    ok(r, "walk weather speed",
       snow < rain and rain < clear and math.abs(clear - 1.0) < 1e-6,
       string.format("snow=%.2f rain=%.2f clear=%.2f", snow, rain, clear))
  end

  -- 雪地脚步用 step_snow，其它天气用 step
  do
    local snow = footstepSfx("snow")
    local rain = footstepSfx("rain")
    local clear = footstepSfx("clear")
    ok(r, "snow footsteps",
       snow == "step_snow" and rain == "step" and clear == "step",
       string.format("snow=%s rain=%s clear=%s", snow, rain, clear))
  end

  -- 防滑链：雨雪天提高抓地，且不会被 useItem 消耗
  do
    local sw = weather
    local n0 = inv.chains or 0
    local kt, kv = toast.text, toast.t
    inv.chains = nil
    weather = "snow"
    local bare = weatherGripScale()
    weather = "rain"
    local wetBare = weatherGripScale()
    inv.chains = 1
    weather = "snow"
    local chained = weatherGripScale()
    weather = "rain"
    local wetChained = weatherGripScale()
    local kept = (useItem("chains") == false) and (inv.chains or 0) == 1
    inv.chains = n0 > 0 and n0 or nil
    weather = sw
    toast.text, toast.t = kt, kv
    ok(r, "snow chains", chained > bare and wetChained > wetBare and kept,
       string.format("snow %.2f->%.2f rain %.2f->%.2f kept=%s",
                     bare, chained, wetBare, wetChained, tostring(kept)))
  end

  -- 雨雪抓地提示：显示抓地系数与防滑链，晴天不发提示
  do
    local sw = weather
    local n0 = inv.chains or 0
    weather = "rain"
    inv.chains = nil
    local wet = tractionStatusText() or ""
    inv.chains = 1
    local chained = tractionStatusText() or ""
    inv.chains = n0 > 0 and n0 or nil
    weather = "clear"
    local dry = tractionStatusText()
    weather = sw
    ok(r, "traction hint",
       wet:find("抓地", 1, true) ~= nil and
       wet:find("防滑链", 1, true) == nil and
       chained:find("防滑链", 1, true) ~= nil and dry == nil,
       string.format("wet=%s chained=%s dry=%s", wet, chained,
                     tostring(dry)))
  end

  -- 防滑链在干燥铺装路上略拖慢车速（权衡）
  do
    local keep = inv.chains
    inv.chains = 1
    local dry = chainsSpeedScale("asphalt")
    local grass = chainsSpeedScale("grass")
    inv.chains = nil
    local none = chainsSpeedScale("asphalt")
    inv.chains = keep
    ok(r, "chains tradeoff",
       dry < none and grass == none and none == 1.0,
       string.format("dry=%.2f grass=%.2f none=%.2f", dry, grass, none))
  end

  -- 步行鸣笛：必须步行、在房车旁、非建造模式
  do
    local near = canHonkFromFoot(true, true, false)
    local far = canHonkFromFoot(true, false, false)
    local driving = canHonkFromFoot(false, true, false)
    local building = canHonkFromFoot(true, true, true)
    ok(r, "on-foot horn",
       near == true and far == false and driving == false and
       building == false,
       string.format("near=%s far=%s driving=%s build=%s",
                     tostring(near), tostring(far), tostring(driving),
                     tostring(building)))
  end

  -- 防滑链可取下：取下后雨雪抓地不再加成、干路不再减速
  do
    local keepChains, keepOn = inv.chains, chainsOn
    local keepWeather = weather
    inv.chains, chainsOn, weather = 1, true, "rain"
    local onGrip = weatherGripScale()
    local onSpeed = chainsSpeedScale("asphalt")
    chainsOn = false
    local offGrip = weatherGripScale()
    local offSpeed = chainsSpeedScale("asphalt")
    inv.chains, chainsOn, weather = keepChains, keepOn, keepWeather
    ok(r, "chains toggle",
       onGrip > offGrip and onSpeed < 1.0 and offSpeed == 1.0,
       string.format("grip %.2f->%.2f speed %.2f->%.2f",
                     onGrip, offGrip, onSpeed, offSpeed))
  end

  -- 雪天寒冷：没开取暖器时体力消耗更快
  do
    local a = coldEnergyExtra("snow", false)
    local b = coldEnergyExtra("snow", true)
    local c = coldEnergyExtra("clear", false)
    ok(r, "snow cold", a > 0 and b == 0 and c == 0,
       string.format("snow=%.3f heater=%.3f clear=%.3f", a, b, c))
  end

  -- 热食保暖：雪天吃热饭/热汤后一段时间免疫寒冷惩罚
  do
    local a = coldEnergyExtra("snow", false, 30)
    local b = coldEnergyExtra("snow", false, 0)
    local keepWarmth, keepMeal = warmthMin, inv.meal or 0
    warmthMin = 0
    inv.meal = 1
    local okEat = useItem("meal")
    local woke = (warmthMin or 0) >= 30
    inv.meal = keepMeal > 0 and keepMeal or nil
    warmthMin = keepWarmth
    ok(r, "warm meal", a == 0 and b > 0 and okEat and woke,
       string.format("warm=%.3f cold=%.3f woke=%s", a, b, tostring(woke)))
  end

  -- 保暖状态文案：有保暖时显示剩余分钟，没有时不显示
  do
    local keep = warmthMin
    warmthMin = 0
    local none = warmthStatusText()
    warmthMin = 25
    local shown = warmthStatusText() or ""
    warmthMin = keep
    ok(r, "warmth status",
       none == nil and shown:find("暖和", 1, true) ~= nil and
       shown:find("25", 1, true) ~= nil,
       string.format("none=%s shown=%s", tostring(none), shown))
  end

  -- 热咖啡也能提供保暖
  do
    local keepWarmth = warmthMin
    local kb = inv.brew or 0
    warmthMin = 0
    inv.brew = 1
    local okBrew = useItem("brew")
    local warm = (warmthMin or 0) >= 20
    inv.brew = kb > 0 and kb or nil
    warmthMin = keepWarmth
    ok(r, "warm coffee", okBrew and warm,
       string.format("brew=%s warmth=%.0f", tostring(okBrew),
                     warmthMin or 0))
  end

  -- 联机保暖同步：按服务器值设置并夹紧到 0
  do
    local keep = warmthMin
    setWarmth(35)
    local set = math.abs((warmthMin or 0) - 35) < 1e-6
    setWarmth(-5)
    local clamped = (warmthMin or -1) == 0
    warmthMin = keep
    ok(r, "warmth sync", set and clamped,
       string.format("set=%s clamp=%s", tostring(set), tostring(clamped)))
  end

  -- 睡饱状态同步：按服务器值设置并夹紧到 0
  do
    local keep = restedMin
    setRested(120)
    local set = math.abs((restedMin or 0) - 120) < 1e-6
    setRested(-5)
    local clamped = (restedMin or -1) == 0
    restedMin = keep
    ok(r, "rested sync", set and clamped,
       string.format("set=%s clamp=%s", tostring(set), tostring(clamped)))
  end

  -- 夜间驾驶疲劳：短途无、长途有、上限封顶
  do
    local a = nightFatigueExtra(60)
    local b = nightFatigueExtra(120)
    local c = nightFatigueExtra(180)
    local d = nightFatigueExtra(1000)
    ok(r, "night fatigue", a == 0 and b == 0 and c > 0 and d <= 0.02,
       string.format("60=%.3f 120=%.3f 180=%.3f max=%.3f", a, b, c, d))
  end

  -- 咖啡清醒：同样夜驾时长，疲劳额外 -60%
  do
    local coffee = nightFatigueExtra(300, 0, 120)
    local none = nightFatigueExtra(300, 0, 0)
    ok(r, "coffee alertness",
       coffee > 0 and none > 0 and math.abs(coffee - none * 0.4) < 1e-9,
       string.format("none=%.4f coffee=%.4f", none, coffee))
  end

  -- 睡饱状态：疲劳积累减半，睡觉会刷新 restedMin
  do
    local tired = nightFatigueExtra(300, 0)
    local rested = nightFatigueExtra(300, 100)
    local sd, sc = day, clock
    local keepRested = restedMin
    restedMin = 0
    sleepInBed()
    local woke = (restedMin or 0) >= 240
    day, clock = sd, sc
    restedMin = keepRested
    morningReport = nil
    ok(r, "rested bonus",
       rested > 0 and rested < tired and woke,
       string.format("tired=%.4f rested=%.4f woke=%s",
                     tired, rested, tostring(woke)))
  end

  -- 疲劳视觉强度：120 分钟内为 0，之后递增并封顶 1.0
  do
    local a = fatigueAlpha(60)
    local b = fatigueAlpha(150)
    local c = fatigueAlpha(300)
    local d = fatigueAlpha(900)
    ok(r, "fatigue vignette",
       a == 0 and b > 0 and b < c and math.abs(c - 1.0) < 1e-6 and
       math.abs(d - 1.0) < 1e-6,
       string.format("60=%.2f 150=%.2f 300=%.2f max=%.2f", a, b, c, d))
  end

  -- 疲劳方向游移：短途无、长途有且幅度受限
  do
    local a = nightSteerWander(60, 1.0)
    local b = nightSteerWander(300, 1.0)
    local maxv = 0
    for i = 0, 40 do
      local v = math.abs(nightSteerWander(300, i * 0.25))
      if v > maxv then maxv = v end
    end
    ok(r, "fatigue steer wander",
       a == 0 and b ~= 0 and maxv > 0 and maxv <= 0.03,
       string.format("short=%.3f long=%.3f max=%.3f", a, b, maxv))
  end

  -- 睡醒晨报：包含天数/货运单数/今日目标/油量
  do
    local sd, sc = day, clock
    local se, sh, st, sg = needs.energy, needs.hunger, needs.thirst, needs.hygiene
    morningReport = nil
    sleepInBed()
    local rep = morningReport
    local okRep = rep ~= nil and (rep.day or 0) >= 1 and rep.jobs ~= nil and
                  rep.fuel ~= nil and (rep.t or 0) > 0 and
                  (rep.forecast == nil or WEATHER_NAMES[rep.forecast] ~= nil) and
                  rep.oil ~= nil and rep.notes ~= nil
    day, clock = sd, sc
    needs.energy, needs.hunger, needs.thirst, needs.hygiene = se, sh, st, sg
    morningReport = nil
    ok(r, "morning report", okRep,
       rep and string.format("day=%d jobs=%d goal=%s fuel=%.0f oil=%.0f notes=%s",
                             rep.day or 0, rep.jobs or 0,
                             tostring(rep.goal), rep.fuel or 0,
                             rep.oil or 0, tostring(rep.notes)) or "missing")
  end

  -- 存读档：行程/探索/货运单数 + 天气 + 笔记/风车/补给日期
  do
    local keepFireFuel, keepFireOn = campfireFuelMin, campfireOn
    local keepNotes, keepMill = notesFound, windmillUpgraded
    local keepSupplyDay = relaySupplyDay
    tripDistance, tripFuel, jobDelivered = 3210, 88, 7
    visitedPois["ruin"] = true
    bestFish, fishCaught = 63, 12
    lastJobDay = 11
    batteryCharge, rawFoodAge = 1.1, 500
    interiorLightsOn, highBeam = false, true
    if setInteriorLights ~= nil then setInteriorLights(false) end
    weather, weatherT, weatherNext = "rain", 7, 120
    relayDone = true
    notesFound = { note_ruin = true }
    windmillUpgraded = true
    relaySupplyDay = 42
    campfireFuelMin, campfireOn = 123, false
    local saved0 = saveLayout()
    tripDistance, tripFuel, jobDelivered = 0, 0, 0
    visitedPois = {}
    visitedCount = 0
    bestFish, fishCaught = 0, 0
    lastJobDay = 0
    interiorLightsOn, highBeam = true, false
    batteryCharge, rawFoodAge = 0, 0
    weather, weatherT, weatherNext = "clear", 0, 90
    relayDone = false
    campfireFuelMin, campfireOn = 720, true
    notesFound, windmillUpgraded, relaySupplyDay = {}, false, 0
    loadLayout()
    local lightsOk = interiorLightsOn == false and highBeam == true
    local powerOk2 = math.abs((batteryCharge or 0) - 1.1) < 1e-6 and
                     math.abs((rawFoodAge or 0) - 500) < 1
    local relayOk = relayDone == true
    local fireOk = math.abs((campfireFuelMin or 0) - 123) < 1 and
                   campfireOn == false
    local notesOk = notesFound["note_ruin"] == true
    local millOk = windmillUpgraded == true
    local supplyOk = (relaySupplyDay or 0) == 42
    local loadedSupplyDay = tonumber(relaySupplyDay) or 0
    if setInteriorLights ~= nil then setInteriorLights(true) end
    highBeam = false
    campfireFuelMin, campfireOn = keepFireFuel, keepFireOn
    notesFound, windmillUpgraded = keepNotes, keepMill
    relaySupplyDay = keepSupplyDay
    for _, f in ipairs(campfireFlames or {}) do SetVisible(f.ent, campfireOn) end
    if campfireLight ~= nil then SetVisible(campfireLight, campfireOn) end
    ok(r, "save progress stats",
       saved0 ~= false and math.abs((tripDistance or 0) - 3210) < 1 and
       math.abs((tripFuel or 0) - 88) < 1 and jobDelivered == 7 and
       visitedPois["ruin"] == true and weather == "rain" and
       math.abs((weatherNext or 0) - 120) < 1 and
       math.abs((bestFish or 0) - 63) < 1 and fishCaught == 12 and
       lastJobDay == 11 and lightsOk and powerOk2 and relayOk and fireOk and
       notesOk and millOk and supplyOk,
       string.format("trip=%.0f jobs=%d weather=%s fish=%.0f/%d notes=%s mill=%s supply=%d",
                     tripDistance or 0, jobDelivered, tostring(weather),
                     bestFish or 0, fishCaught or 0, tostring(notesOk),
                     tostring(millOk), loadedSupplyDay))
  end

  -- 存读档
  local saved = saveLayout()
  ok(r, "save layout", saved ~= false, "saves/layout.json")

  local text = table.concat(r, "\n")
  selftestActive = false
  WriteText("selftest_result.txt", text)
  print(text)
end
