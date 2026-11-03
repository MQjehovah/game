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

  -- 存读档
  local saved = saveLayout()
  ok(r, "save layout", saved ~= false, "saves/layout.json")

  local text = table.concat(r, "\n")
  WriteText("selftest_result.txt", text)
  print(text)
end
