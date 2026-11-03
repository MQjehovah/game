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
    if ReadText ~= nil and Json ~= nil then
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
    ok(r, "night delivery bonus", nightGain == 11 and dayGain == 10,
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
      job.stops, job.stopIndex = nil, 1
      job.pay = { metal = 10 }
      job.condition = 1.0
      local m0 = materials.metal or 0
      jobComplete()
      return (materials.metal or 0) - m0
    end
    local clearGain = deliverWith("clear")
    local rainGain = deliverWith("rain")
    weather, clock, day, lastJobDay = sw, sc, sd, slast
    jobDelivered, jobStreak = jd, js
    ok(r, "rain delivery bonus", rainGain == 11 and clearGain == 10,
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
    ok(r, "snow delivery bonus", snowGain == 22 and clearGain == 20,
       string.format("clear=%d snow=%d", clearGain, snowGain))
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

  -- 防雨篷布：带在身上时易碎货不再被雨水打湿
  do
    local sw, sp = weather, phase
    local k0 = inv.tarp or 0
    inv.tarp = (inv.tarp or 0) + 1
    jobRefreshOffers("gasstop")
    jobAccept(1)
    job.stops, job.stopIndex = nil, 1
    job.fragile = true
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
    local savedClock, savedPhase = clock, phase
    phase = "driving"
    clock = 22 * 60
    updateVehicleLights()
    local onCount = #rvHeadlights
    clock = 12 * 60
    updateVehicleLights()
    local offCount = #rvHeadlights
    clock, phase = savedClock, savedPhase
    ok(r, "rv night headlights", onCount == 2 and offCount == 0,
       string.format("night=%d day=%d parts=%d", onCount, offCount, #rvParts))
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

  -- 机油劣化增加油耗（最高 +8%）
  do
    local full = engineOilFuelScale(100)
    local mid = engineOilFuelScale(20)
    local low = engineOilFuelScale(0)
    ok(r, "oil fuel penalty",
       full == 1.0 and mid > full and low > mid and low <= 1.08,
       string.format("100=%.3f 20=%.3f 0=%.3f", full, mid, low))
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

  -- 公路电线：相邻电线杆之间两条
  do
    local n = #roadWires
    ok(r, "roadside power wires", n >= 20, string.format("wires=%d", n))
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

  -- 湖边鱼笼：放置消耗材料，计时后收取生鱼
  do
    local keepTrap = pondTrap
    local sm, sw = materials.metal or 0, materials.wood or 0
    local kr = inv.rawfish or 0
    local kt, kv = toast.text, toast.t
    pondTrap = nil
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
    toast.text, toast.t = kt, kv
    ok(r, "pond trap",
       placed and setting and visible and ready and collected and gained and
       cleared and poor == false,
       string.format("place=%s set=%s visible=%s ready=%s got=%s poor=%s",
                     tostring(placed), tostring(setting), tostring(visible),
                     tostring(ready), tostring(gained), tostring(poor)))
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
    job.pay = { metal = 5, wood = 2 }
    job.condition = 1.0
    local m0 = materials.metal or 0
    jobComplete()
    local firstGain = (materials.metal or 0) - m0
    jobRefreshOffers("depot")
    jobAccept(1)
    job.pay = { metal = 5, wood = 2 }
    job.condition = 1.0
    local m1 = materials.metal or 0
    jobComplete()
    local secondGain = (materials.metal or 0) - m1
    day, lastJobDay = sd, sLast
    jobDelivered = jd
    milestones = mk
    dailyGoal = dk
    ok(r, "daily first job bonus", firstGain == 6 and secondGain == 5,
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

  -- 载重增加轮胎磨损（超载最高 ×2.2）
  do
    local empty = tireWearScale(1.0)
    local loaded = tireWearScale(1.4)
    local over = tireWearScale(3.0)
    ok(r, "load tire wear",
       empty == 1.0 and loaded > empty and over >= loaded and over <= 2.2,
       string.format("empty=%.2f loaded=%.2f over=%.2f", empty, loaded, over))
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
  do
    addItem("bait", 1)
    local bait0 = inv.bait or 0
    local raw0 = itemCount("rawfish")
    startFishing()
    local used = fishingBait == true
    finishFishing()
    local caught = itemCount("rawfish") > raw0
    local consumed = (inv.bait or 0) == bait0 - 1
    ok(r, "fishing bait", used and caught and consumed,
       string.format("bait %d->%d raw %d->%d", bait0, inv.bait or 0,
                     raw0, itemCount("rawfish")))
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

  -- 农场风车：4 片叶片且会旋转
  do
    local blades = windmill and #(windmill.blades or {}) or 0
    local a0 = windmill and (windmill.angle or 0) or 0
    updateWindmill(1.0)
    local a1 = windmill and (windmill.angle or 0) or 0
    ok(r, "farm windmill", blades == 4 and a1 > a0,
       string.format("blades=%d angle %.2f->%.2f", blades, a0, a1))
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
    ok(r, "gas station refuel cost",
       campOk and campFull and (okPoor == false) and poorKept and
       okRich and richFull and charged,
       string.format("camp=%s free=%s poor=%s kept=%s rich=%s cost=%d",
                     tostring(campOk), tostring(campFull), tostring(okPoor),
                     tostring(poorKept), tostring(richFull), cost or 0))
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
    ok(r, "town water tower", n >= 1 and parts >= 6,
       string.format("towers=%d parts=%d", n, parts))
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

  -- 环境音乐：驾驶=引擎、雨天步行=雨声、营地收音机=radio、其它=风声
  do
    local dn, dv = desiredMusic("driving", "rain", true)
    local rn, rv2 = desiredMusic("parked", "rain", false)
    local wn, wv = desiredMusic("parked", "clear", false)
    local radioN, radioV = desiredMusic("parked", "clear", true)
    ok(r, "ambient music", dn == "engine" and rn == "rain" and wn == "wind" and
       radioN == "radio" and dv > 0 and rv2 > 0 and wv > 0 and radioV > 0,
       string.format("drive=%s rain=%s clear=%s radio=%s", dn, rn, wn, radioN))
  end

  -- 两个电台：1=radio、2=radio2
  do
    local a = desiredMusic("parked", "clear", true, 1)
    local b = desiredMusic("parked", "clear", true, 2)
    ok(r, "radio stations", a == "radio" and b == "radio2",
       string.format("station1=%s station2=%s", tostring(a), tostring(b)))
  end

  -- 驾驶时车载电台：开启后用 radio/radio2 取代引擎声
  do
    local engine = desiredMusic("driving", "clear", false, 1, false)
    local radio1 = desiredMusic("driving", "clear", false, 1, true)
    local radio2 = desiredMusic("driving", "clear", false, 2, true)
    ok(r, "car radio",
       engine == "engine" and radio1 == "radio" and radio2 == "radio2",
       string.format("engine=%s r1=%s r2=%s", tostring(engine),
                     tostring(radio1), tostring(radio2)))
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
    ok(r, "radio mast", parts >= 6 and night and dayOff,
       string.format("parts=%d night=%s dayOff=%s", parts,
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

  -- 夜间驾驶疲劳：短途无、长途有、上限封顶
  do
    local a = nightFatigueExtra(60)
    local b = nightFatigueExtra(120)
    local c = nightFatigueExtra(180)
    local d = nightFatigueExtra(1000)
    ok(r, "night fatigue", a == 0 and b == 0 and c > 0 and d <= 0.02,
       string.format("60=%.3f 120=%.3f 180=%.3f max=%.3f", a, b, c, d))
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
                  (rep.forecast == nil or WEATHER_NAMES[rep.forecast] ~= nil)
    day, clock = sd, sc
    needs.energy, needs.hunger, needs.thirst, needs.hygiene = se, sh, st, sg
    morningReport = nil
    ok(r, "morning report", okRep,
       rep and string.format("day=%d jobs=%d goal=%s fuel=%.0f",
                             rep.day or 0, rep.jobs or 0,
                             tostring(rep.goal), rep.fuel or 0) or "missing")
  end

  -- 存读档：行程/探索/货运单数 + 天气
  do
    local keepFireFuel, keepFireOn = campfireFuelMin, campfireOn
    tripDistance, tripFuel, jobDelivered = 3210, 88, 7
    visitedPois["ruin"] = true
    bestFish, fishCaught = 63, 12
    lastJobDay = 11
    batteryCharge, rawFoodAge = 1.1, 500
    interiorLightsOn, highBeam = false, true
    if setInteriorLights ~= nil then setInteriorLights(false) end
    weather, weatherT, weatherNext = "rain", 7, 120
    relayDone = true
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
    loadLayout()
    local lightsOk = interiorLightsOn == false and highBeam == true
    local powerOk2 = math.abs((batteryCharge or 0) - 1.1) < 1e-6 and
                     math.abs((rawFoodAge or 0) - 500) < 1
    local relayOk = relayDone == true
    local fireOk = math.abs((campfireFuelMin or 0) - 123) < 1 and
                   campfireOn == false
    if setInteriorLights ~= nil then setInteriorLights(true) end
    highBeam = false
    campfireFuelMin, campfireOn = keepFireFuel, keepFireOn
    for _, f in ipairs(campfireFlames or {}) do SetVisible(f.ent, campfireOn) end
    if campfireLight ~= nil then SetVisible(campfireLight, campfireOn) end
    ok(r, "save progress stats",
       saved0 ~= false and math.abs((tripDistance or 0) - 3210) < 1 and
       math.abs((tripFuel or 0) - 88) < 1 and jobDelivered == 7 and
       visitedPois["ruin"] == true and weather == "rain" and
       math.abs((weatherNext or 0) - 120) < 1 and
       math.abs((bestFish or 0) - 63) < 1 and fishCaught == 12 and
       lastJobDay == 11 and lightsOk and powerOk2 and relayOk and fireOk,
       string.format("trip=%.0f jobs=%d weather=%s fish=%.0f/%d day=%d lights=%s batt=%.2f age=%.0f relay=%s fire=%s",
                     tripDistance or 0, jobDelivered, tostring(weather),
                     bestFish or 0, fishCaught or 0, lastJobDay or 0,
                     tostring(lightsOk), batteryCharge or 0, rawFoodAge or 0,
                     tostring(relayOk), tostring(fireOk)))
  end

  -- 存读档
  local saved = saveLayout()
  ok(r, "save layout", saved ~= false, "saves/layout.json")

  local text = table.concat(r, "\n")
  selftestActive = false
  WriteText("selftest_result.txt", text)
  print(text)
end
