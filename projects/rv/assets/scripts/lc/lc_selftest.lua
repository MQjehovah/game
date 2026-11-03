-- lc_selftest.lua
-- 无头验证钩子：scriptBaseDir 下放 selftest_on.txt 时执行一条完整系统自检

local function ok(results, name, cond, extra)
  results[#results + 1] = (cond and "PASS  " or "FAIL  ") .. name
                             .. (extra and ("  [" .. extra .. "]") or "")
  return cond
end

function runSelfTest()
  local r = {}
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

  -- 低强度威胁
  local h0 = #horde
  spawnAmbientZombie()
  local spawned = #horde == h0 + 1
  if spawned then killZombie(#horde) end
  ok(r, "ambient threat spawn/kill", spawned and #horde == h0,
     string.format("horde %d->%d", h0, #horde))

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
    local hMid = terrainHeight(12, 10)
    local hTop = terrainHeight(19, 10)
    ok(r, "terrain height profile",
       hFlat < 0.01 and hMid > 1.0 and hTop > hMid,
       string.format("flat=%.2f mid=%.2f top=%.2f", hFlat, hMid, hTop))
    local followed = terrainFollow(0, 19, 10, 1.0, 10)
    ok(r, "terrain ground follow",
       math.abs(followed - hTop) < 0.001,
       string.format("follow=%.2f top=%.2f", followed, hTop))
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

  -- 存读档
  local saved = saveLayout()
  ok(r, "save layout", saved ~= false, "saves/layout.json")

  local text = table.concat(r, "\n")
  WriteText("selftest_result.txt", text)
  print(text)
end
