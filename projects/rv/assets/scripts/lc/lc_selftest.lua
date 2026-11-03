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
  local weightWithCargo = stats.weight
  ok(r, "job accept loads cargo",
     offerOk and payOk and accepted and job ~= nil and
     weightWithCargo >= (job.weight or 0) and jobCargoWeight() > cargo0 and
     #jobCargoEnts > 0,
     string.format("offers=%d dest=%s cargo=%s x%d %.0fkg total=%.0fkg",
                   #jobOffers + 1, usedOffer and usedOffer.destId or "?",
                   usedOffer and usedOffer.cargo or "?",
                   usedOffer and usedOffer.crates or 0,
                   job and job.weight or 0, weightWithCargo))

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
     stats.weight < weightWithCargo and #jobCargoEnts == 0 and fuel >= fuelBefore,
     string.format("dest=%s delivered=%d->%d weight=%.0f->%.0f fuel=%.0f->%.0f",
                   tostring(dest), doneBefore, jobDelivered, weightWithCargo,
                   stats.weight, fuelBefore, fuel))
  ok(r, "job result card",
     jobResult ~= nil and jobResult.ok == true and (jobResult.crates or 0) > 0,
     jobResult and string.format("cargo=%s cond=%.2f jobs=%d",
                                 tostring(jobResult.cargo), jobResult.cond or 0,
                                 jobResult.jobs or 0) or "missing")

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
    rv.speed = savedSpeed
    ok(r, "rv brake lights", onLit == true and offLit == false and #rvTaillights == 2,
       string.format("lamps=%d on=%s off=%s", #rvTaillights,
                     tostring(onLit), tostring(offLit)))
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

  -- 营火暖光：隐藏网格但保留点光
  ok(r, "campfire glow", campfireLight ~= nil and campfire ~= nil,
     campfire and string.format("fire=(%.1f,%.1f)", campfire.x, campfire.z) or "missing")

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

  -- 撞击反馈：短促抖动（0.12–0.50s）
  do
    local keep = rv.shakeT
    rvImpactFeedback(3.0)
    local s = rv.shakeT or 0
    rv.shakeT = keep
    ok(r, "rv impact shake", s > 0.1 and s <= 0.50,
       string.format("shakeT=%.2f", s))
  end

  -- 雨天闪电：触发后闪光计时开始衰减
  do
    local keep = lightningT
    triggerLightning()
    local on = (lightningT or 0) > 0
    local a = math.min(0.35, (lightningT or 0) * 0.9)
    lightningT = keep
    ok(r, "rv lightning flash", on and a > 0.10,
       string.format("t=0.50 alpha=%.2f", a))
  end

  -- 主路两侧废弃车/石块
  do
    local n = #roadDebris
    ok(r, "roadside debris", n >= 4, string.format("debris=%d", n))
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

  -- 存读档：行程/探索/货运单数
  do
    tripDistance, tripFuel, jobDelivered = 3210, 88, 7
    visitedPois["ruin"] = true
    local saved0 = saveLayout()
    tripDistance, tripFuel, jobDelivered = 0, 0, 0
    visitedPois = {}
    visitedCount = 0
    loadLayout()
    ok(r, "save progress stats",
       saved0 ~= false and math.abs((tripDistance or 0) - 3210) < 1 and
       math.abs((tripFuel or 0) - 88) < 1 and jobDelivered == 7 and
       visitedPois["ruin"] == true,
       string.format("trip=%.0f fuel=%.0f jobs=%d ruin=%s",
                     tripDistance or 0, tripFuel or 0, jobDelivered,
                     tostring(visitedPois["ruin"])))
  end

  -- 存读档
  local saved = saveLayout()
  ok(r, "save layout", saved ~= false, "saves/layout.json")

  local text = table.concat(r, "\n")
  WriteText("selftest_result.txt", text)
  print(text)
end
