-- lc_jobs.lua
-- 顺路货运合同：营地货运板接单 -> 开车送到目标地点 -> 结算材料/燃料。
-- 只复用已有节点，不新增地图；货物重量进入 stats.weight，直接压加速/极速/油耗。

local CARGO_SLOTS = {
  { -0.55, 2.62, 0.60 },
  {  0.00, 2.62, 0.60 },
  {  0.55, 2.62, 0.60 },
  {  0.00, 3.16, 0.60 },
}

local function jobDistToNode(nd)
  if nd == nil then return 0 end
  return math.sqrt((nd.x - rv.x) ^ 2 + (nd.z - rv.z) ^ 2)
end

-- 货物重量（没有合同 = 0）：recomputeStats 会把它并进 stats.weight
function jobCargoWeight()
  if job == nil then return 0 end
  return job.weight or 0
end

-- 当前合同到目的地的剩余距离（HUD/到达判定共用）
function jobRemaining()
  if job == nil then return 0, nil end
  local nd = nodeById(job.destId)
  if nd == nil then return 0, nil end
  return jobDistToNode(nd), nd
end

-- 当前合同相对视角的方位差（-π..π）：>0 在左、<0 在右（用于屏幕外箭头）
function jobBearingDelta(px, pz, viewYaw)
  if job == nil then return 0 end
  local nd = nodeById(job.destId)
  if nd == nil then return 0 end
  local b = dirYaw(nd.x - px, nd.z - pz)
  local d = b - (viewYaw or 0)
  while d > math.pi do d = d - 2 * math.pi end
  while d < -math.pi do d = d + 2 * math.pi end
  return d
end

-- 把车顶货物摆到房车坐标系里（跟随俯仰/侧倾/偏航）
function jobPlaceCargo()
  if #jobCargoEnts == 0 then return end
  local gy = rv.groundY or 0
  local pitch, roll, yaw = rv.pitch or 0, rv.roll or 0, rv.yaw
  local cy, sy = math.cos(yaw), math.sin(yaw)
  for i, e in ipairs(jobCargoEnts) do
    local s = CARGO_SLOTS[(i - 1) % #CARGO_SLOTS + 1]
    local lx, ly, lz = s[1], s[2], s[3]
    if i > #CARGO_SLOTS then ly = ly + 0.5 end
    -- 局部坐标 -> 世界（与 rotOf 一致）
    local wx = rv.x + lx * cy + lz * sy
    local wz = rv.z - lx * sy + lz * cy
    SetPosition(e, { x = wx, y = gy + ly, z = wz })
    if SetRotationEuler ~= nil then
      SetRotationEuler(e, pitch, roll, yaw)
    else
      SetRotationY(e, yaw)
    end
  end
end

function jobDespawnCargo()
  for _, e in ipairs(jobCargoEnts) do Despawn(e) end
  jobCargoEnts = {}
end

local CARGO_PREFAB = {
  med = "rv_cargo_med",     -- 白色医疗箱
  parts = "rv_panel_dark",   -- 深灰机械件
  elec = "rv_cargo_elec",    -- 蓝色电子件
  grain = "rv_cargo_crate",  -- 木色粮袋
  livestock = "rv_cargo_crate", -- 活畜：木栏箱
}

function jobCargoPrefab(cargoId)
  return CARGO_PREFAB[cargoId] or "rv_cargo_crate"
end

function jobSpawnCargo()
  jobDespawnCargo()
  if job == nil or coopRole ~= nil then return end
  local n = math.min(JOBS.maxCargo, job.crates or 1)
  local pre = jobCargoPrefab(job.cargoId)
  for i = 1, n do
    local e = SpawnPrefab(pre, { x = rv.x, y = -60, z = rv.z })
    if e ~= nil then
      SetScale(e, 0.52, 0.52, 0.52)
      jobCargoEnts[#jobCargoEnts + 1] = e
    end
  end
  jobPlaceCargo()
end

-- 绝对游戏分钟（跨天不会回绕），限时合同用
function jobNowAbs()
  return (day or 1) * 1440 + (clock or 0)
end

-- 货运预计往返油耗（L）：按距离粗估，方便在货运板上权衡
function jobFuelEstimate(distance)
  return (distance or 0) * 2.0 * 0.016
end

-- 板上合同：优先挑 45m 外的节点，距离越远报酬/载重越高；不足则用最近的补。
function jobRefreshOffers(preferId)
  local pool, near = {}, {}
  for _, nd in ipairs(NODES) do
    if nd.id ~= "camp" then
      local d = math.sqrt(nd.x * nd.x + nd.z * nd.z)
      if d >= JOBS.minDistance then
        pool[#pool + 1] = nd
      else
        near[#near + 1] = nd
      end
    end
  end
  jobOffers = {}
  local used = {}
  -- 测试/导航用：可指定第一张合同的目的地（营地板上仍随机补足其余）
  local prefer = preferId and nodeById(preferId) or nil
  local want = math.min(JOBS.offers, #pool + #near)
  local function makeOffer(nd)
    local cargo = JOBS.cargo[math.random(1, #JOBS.cargo)]
    local dist = math.sqrt(nd.x * nd.x + nd.z * nd.z)
    local crates = math.min(JOBS.maxCargo, 2 + math.floor(dist / 120))
    local pay = {
      metal = 2 + math.floor(dist / 45),
      wood  = 1 + math.floor(dist / 90),
    }
    if (nd.danger or 0) >= 2 then pay.electronics = 1 end
    local fuelPay = JOBS.fuelBase + math.floor(dist / 100) * JOBS.fuelPer100m
    -- 约 30% 是限时急单：报酬 ×1.4，但必须在截止前送到
    local urgent = math.random() < 0.30
    if urgent then
      pay.metal = math.max(1, math.floor(pay.metal * 1.4 + 0.5))
      pay.wood = math.max(1, math.floor(pay.wood * 1.4 + 0.5))
    end
    -- 约 20% 是易碎货（医疗/电子类）：撞击掉货况翻倍，报酬 ×1.25
    local fragile = math.random() < 0.20
    if fragile then
      pay.metal = math.max(1, math.floor(pay.metal * 1.25 + 0.5))
    end
    -- 约 15% 是易腐货（与易碎互斥）：货况随行驶时间下降，报酬 ×1.15
    local perishable = (not fragile) and (math.random() < 0.15)
    if perishable then
      pay.metal = math.max(1, math.floor(pay.metal * 1.15 + 0.5))
    end
    -- 约 10% 是活畜：怕颠簸，撞击/急转弯掉货况更多，报酬 ×1.30
    local livestock = (not fragile) and (not perishable) and
                      (math.random() < 0.10)
    if livestock then
      cargo = { id = "livestock", name = "活畜", kg = 52, mat = "wood" }
      pay.metal = math.max(1, math.floor(pay.metal * 1.30 + 0.5))
    end
    -- 约 25% 是双站路线：两个卸货点，报酬更高
    local stops = nil
    if math.random() < 0.25 then
      local second = nil
      for _ = 1, 8 do
        local cand = NODES[math.random(1, #NODES)]
        if cand ~= nil and cand.id ~= nd.id and cand.id ~= "camp" then
          second = cand
          break
        end
      end
      if second ~= nil then
        stops = { { id = nd.id, name = nd.name },
                  { id = second.id, name = second.name } }
        pay.metal = pay.metal + 2
        pay.wood = pay.wood + 1
        fuelPay = fuelPay + 4
      end
    end
    -- 约 20% 的合同额外给一件实用物品（自检期间不随机，避免污染测试状态）
    local bonusItem = nil
    if not selftestActive and math.random() < 0.20 then
      local poolItems = { "repairkit", "bait", "fuelcan", "chains" }
      bonusItem = poolItems[math.random(1, #poolItems)]
    end
    return {
      destId = nd.id,
      destName = nd.name,
      danger = nd.danger or 0,
      cargo = cargo.name,
      cargoId = cargo.id,
      crates = crates,
      weight = crates * cargo.kg,
      distance = dist,
      pay = pay,
      fuel = fuelPay,
      urgent = urgent,
      fragile = fragile,
      perishable = perishable,
      livestock = livestock,
      bonusItem = bonusItem,
      stops = stops,
      stopIndex = 1,
      deadlineAbs = urgent and (jobNowAbs() + 300 + dist * 0.8) or nil,
      stage = "offer",
    }
  end
  if prefer ~= nil then
    used[prefer.id] = true
    jobOffers[#jobOffers + 1] = makeOffer(prefer)
  end
  for i = 1, want do
    local src = (#pool > 0) and pool or near
    local nd = src[math.random(1, #src)]
    if nd == nil then break end
    if used[nd.id] then
      -- 重抽一次，避免三张合同全是同一个地点
      nd = src[math.random(1, #src)]
    end
    if nd ~= nil and not used[nd.id] then
      used[nd.id] = true
      jobOffers[#jobOffers + 1] = makeOffer(nd)
    end
    -- 抽到的地点从池里移除，保证合同不重复
    for k = #pool, 1, -1 do
      if pool[k].id == nd.id then table.remove(pool, k) end
    end
    for k = #near, 1, -1 do
      if near[k].id == nd.id then table.remove(near, k) end
    end
  end
end

function jobPayText(pay)
  local parts = {}
  for _, k in ipairs({ "metal", "wood", "electronics", "cloth" }) do
    if (pay[k] or 0) > 0 then
      parts[#parts + 1] = MAT_NAMES[k] .. "x" .. pay[k]
    end
  end
  return table.concat(parts, " ")
end

-- 以物易物：材料/背包物品够不够
function tradeAvailable(t)
  if t == nil then return false end
  for mat, n in pairs(t.cost or {}) do
    if (materials[mat] or 0) < n then return false end
  end
  for id, n in pairs(t.costItem or {}) do
    if (inv[id] or 0) < n then return false end
  end
  return true
end

function doTrade(idx)
  local t = TRADES and TRADES[idx]
  if t == nil then return false end
  if not tradeAvailable(t) then
    toast.text = "材料不足：" .. t.name
    toast.t = 2.2
    return false
  end
  for mat, n in pairs(t.cost or {}) do
    materials[mat] = (materials[mat] or 0) - n
  end
  for id, n in pairs(t.costItem or {}) do
    takeItem(id, n)
  end
  local got = {}
  for k, v in pairs(t.give or {}) do
    if k == "fuel" then
      local before = fuel or 0
      fuel = math.min(fuelMax(), before + v)
      got[#got + 1] = string.format("燃料+%dL", math.floor(fuel - before + 0.5))
    elseif k == "item" then
      local id = t.give.item
      local n = t.give.n or 1
      addItem(id, n)
      got[#got + 1] = itemName(id) .. "+" .. n
    else
      materials[k] = (materials[k] or 0) + v
      got[#got + 1] = (MAT_NAMES[k] or k) .. "+" .. v
    end
  end
  toast.text = "交易：" .. t.name .. "  →  " .. table.concat(got, " ")
  toast.t = 3.0
  if PlaySfx then PlaySfx("pickup") end
  return true
end

function jobOpenBoard()
  jobRefreshOffers()
  jobUI = true
end

-- 装货反馈：车顶尘屑 + 浮字（单机；联机以 HUD 状态为准）
function jobLoadFx()
  if coopRole ~= nil then return end
  rv.loadFxT = 0.6
  local wx, wz = rotOf(0.4, 0.4)
  EmitParticles({
    pos = { x = wx, y = (rv.groundY or 0) + 2.45, z = wz },
    count = 12, speedMin = 0.4, speedMax = 1.6,
    vel = { x = 0, y = 0.45, z = 0 },
    lifeMin = 0.4, lifeMax = 0.9,
    sizeStart = 0.18, sizeEnd = 0.7,
    color = { r = 0.64, g = 0.59, b = 0.49, a = 0.5 },
    colorEnd = { r = 0.64, g = 0.59, b = 0.49, a = 0.0 },
    additive = false, gravity = 0.5,
  })
  SpawnFloatText(wx, 2.95, wz, "装货 " .. (job.crates or 1) .. " 箱",
                 false, 1.4, 1.0, 0.88, 0.4)
end

function jobUnloadFx(crates)
  if coopRole ~= nil then return end
  rv.unloadFxT = 0.6
  local wx, wz = rotOf(0.4, 0.4)
  EmitParticles({
    pos = { x = wx, y = (rv.groundY or 0) + 2.45, z = wz },
    count = 14, speedMin = 0.5, speedMax = 1.8,
    vel = { x = 0, y = 0.55, z = 0 },
    lifeMin = 0.4, lifeMax = 0.95,
    sizeStart = 0.20, sizeEnd = 0.8,
    color = { r = 0.66, g = 0.60, b = 0.48, a = 0.55 },
    colorEnd = { r = 0.66, g = 0.60, b = 0.48, a = 0.0 },
    additive = false, gravity = 0.5,
  })
  SpawnFloatText(wx, 2.95, wz, "卸货 " .. (crates or 1) .. " 箱",
                 false, 1.4, 0.95, 0.88, 0.42)
end

function jobAccept(idx)
  local offer = jobOffers[idx]
  if offer == nil then return false end
  if job ~= nil then
    toast.text = "已经载着货了：先送到 " .. nodeName(job.destId)
    toast.t = 2.6
    jobUI = false
    return false
  end
  job = {
    destId = offer.destId, destName = offer.destName, danger = offer.danger,
    cargo = offer.cargo, cargoId = offer.cargoId, crates = offer.crates,
    weight = offer.weight, distance = offer.distance,
    pay = offer.pay, fuel = offer.fuel, stage = "carry", condition = 1.0,
    urgent = offer.urgent, deadlineAbs = offer.deadlineAbs,
    fragile = offer.fragile, perishable = offer.perishable,
    livestock = offer.livestock,
    bonusItem = offer.bonusItem,
    startTripDist = tripDistance or 0,
    startTripFuel = tripFuel or 0,
    stops = offer.stops, stopIndex = offer.stopIndex or 1,
  }
  jobUI = false
  jobOffers = {}
  jobSpawnCargo()
  if jobLoadFx ~= nil then jobLoadFx() end
  recomputeStats()
  toast.text = string.format("接单：%s x%d → %s（%.0fm）", job.cargo, job.crates,
                             job.destName, job.distance)
  toast.t = 4.5
  if PlaySfx then PlaySfx("pickup") end
  return true
end

-- 撞击损伤货物：drop 是前后速度差（m/s）；货况过低时结算打折。
function jobApplyImpact(drop)
  if job == nil then return end
  local before = job.condition or 1.0
  local mult = 0.05
  if job.fragile then mult = 0.10
  elseif job.livestock then mult = 0.12 end
  job.condition = math.max(0, before - math.min(0.5, (drop or 0) * mult))
  if before >= 0.5 and job.condition < 0.5 then
    toast.text = job.fragile and "易碎货物受损过半！报酬会大幅打折"
                              or "货物受损过半！送达报酬会明显打折"
    toast.t = 3.2
  end
end

-- 刷新货运合同：花 1 金属重新抽三张（给玩家更多选择）
function jobReroll()
  if jobOffers == nil or #jobOffers == 0 then return false end
  local cost = 1
  if (materials.metal or 0) < cost then
    toast.text = "刷新合同需要 1 金属"
    toast.t = 2.2
    return false
  end
  materials.metal = (materials.metal or 0) - cost
  jobRefreshOffers()
  toast.text = "已刷新货运合同（-1 金属）"
  toast.t = 2.4
  if PlaySfx then PlaySfx("pickup") end
  return true
end

function jobAbandon()
  if job == nil then
    jobUI = false
    return false
  end
  jobDespawnCargo()
  job = nil
  jobOffers = {}
  jobStreak = 0  -- 主动放弃也算断掉连续送达
  if addJournal ~= nil then addJournal("放弃货运合同，已卸货", "fail") end
  recomputeStats()
  toast.text = "已卸货，合同作废"
  toast.t = 2.6
  jobUI = false
  return true
end

function jobComplete()
  if job == nil then return false end
  local cond = math.max(0, math.min(1, job.condition or 1.0))
  local scale = (0.55 + 0.45 * cond) * jobReputation() * jobStreakBonus()
  -- 夜间送达 +10%：夜跑视线差，但报酬更高
  local nightBonus = (isNight ~= nil and isNight())
  if nightBonus then scale = scale * 1.10 end
  -- 雨天送达 +8%：湿滑路面的危险补贴
  local rainBonus = (weather == "rain")
  if rainBonus then scale = scale * 1.08 end
  -- 雪天送达 +12%：结冰路面风险更高
  local snowBonus = (weather == "snow")
  if snowBonus then scale = scale * 1.12 end
  -- 省油驾驶 +5%：本单平均油耗不高于 1.4L/100m（约 14L/100km）
  local ecoBonus = false
  if job.startTripDist ~= nil then
    local jd = (tripDistance or 0) - job.startTripDist
    local ju = (tripFuel or 0) - (job.startTripFuel or 0)
    if jd >= 100 and ju > 0 and (ju / jd * 100) <= 1.4 then
      ecoBonus = true
      scale = scale * 1.05
    end
  end
  -- 每天第一单：结算 +25%（把跨天/睡觉和货运循环串起来）
  local firstOfDay = jobDailyBonusAvailable()
  if firstOfDay then
    scale = scale * 1.25
    lastJobDay = day or 1
  end
  local pay = job.pay or {}
  local got = {}
  for mat, n in pairs(pay) do
    local v = math.floor(n * scale + 0.5)
    if v > 0 then
      materials[mat] = (materials[mat] or 0) + v
      got[#got + 1] = MAT_NAMES[mat] .. "+" .. v
    end
  end
  local fuelGot = math.floor((job.fuel or 0) * scale + 0.5)
  if fuelGot > 0 then
    local before = fuel
    fuel = math.min(fuelMax(), fuel + fuelGot)
    fuelGot = fuel - before
  end
  local bonusItem = job.bonusItem
  if bonusItem ~= nil and addItem ~= nil then
    addItem(bonusItem, 1)
  else
    bonusItem = nil
  end
  local name, crates = job.cargo, job.crates
  local destName = job.destName or ""
  jobUnloadFx(crates)
  jobDelivered = jobDelivered + 1
  jobStreak = (jobStreak or 0) + 1
  if addDailyProgress ~= nil then
    addDailyProgress("jobs", 1)
    if ecoBonus then addDailyProgress("eco", 1) end
  end
  if checkMilestones ~= nil then checkMilestones("jobs", jobDelivered) end
  jobHistory[#jobHistory + 1] = string.format("%s → %s（%.0f%%）",
                                              name, destName, cond * 100)
  if #jobHistory > 5 then table.remove(jobHistory, 1) end
  jobDespawnCargo()
  job = nil
  jobOffers = {}
  recomputeStats()
  jobResult = {
    ok = true, cargo = name, crates = crates, cond = cond,
    got = table.concat(got, " "), fuel = fuelGot, jobs = jobDelivered,
    bonus = firstOfDay, night = nightBonus, rain = rainBonus,
    snow = snowBonus, eco = ecoBonus, streak = jobStreak,
    item = bonusItem, t = 3.6,
  }
  SpawnFloatText(rv.x, 1.8, rv.z,
                 "送达 " .. name .. " x" .. crates, false, 1.6, 1.0, 0.86, 0.4)
  if addJournal ~= nil then
    addJournal(string.format("完成货运：%s → %s（货况 %.0f%%）", name,
                             destName, cond * 100), "job")
  end
  toast.text = string.format("货运完成（货况 %.0f%%）：%s%s", cond * 100,
                             table.concat(got, " "),
                             fuelGot > 0 and ("  燃料+" .. math.floor(fuelGot + 0.5) .. "L") or "")
  toast.t = 4.5
  if PlaySfx then PlaySfx("pickup") end
  if LC.flow.emit ~= nil then LC.flow.emit("job_done") end
  if autoSave ~= nil then autoSave() end
  return true
end

-- 营地货运板：一块木牌 + 两根柱子 + 顶部小旗
function spawnJobBoard()
  if jobBoard ~= nil then return end
  local bx, bz = 6.6, -3.0
  local rot = -0.6
  local gy = terrainHeight and terrainHeight(bx, bz) or 0
  local panel = SpawnPrefab("rv_panel_light", { x = bx, y = gy + 1.30, z = bz })
  if panel ~= nil then
    SetScale(panel, 1.70, 0.92, 0.10)
    SetRotationY(panel, rot)
  end
  local tx, tz = math.cos(rot), -math.sin(rot)
  for _, s in ipairs({ -0.80, 0.80 }) do
    local post = SpawnPrefab("rv_panel_dark", {
      x = bx + tx * s, y = gy + 0.62, z = bz + tz * s })
    if post ~= nil then
      SetScale(post, 0.10, 1.28, 0.10)
      SetRotationY(post, rot)
    end
  end
  local flag = SpawnPrefab("rv_marker", { x = bx, y = gy + 1.95, z = bz })
  if flag ~= nil then
    SetScale(flag, 0.07, 0.36, 0.07)
    SetRotationY(flag, rot)
  end
  jobBoard = { x = bx, z = bz }
end

-- 货运信誉：完成单数越多，基础报酬越高（每 5 单 +2%，上限 +30%）
function jobReputation()
  local n = jobDelivered or 0
  return 1.0 + math.min(0.30, math.floor(n / 5) * 0.02)
end

-- 信誉称号：让长期跑单有成长感
function jobReputationRank()
  local n = jobDelivered or 0
  if n >= 50 then return "金牌" end
  if n >= 30 then return "老练" end
  if n >= 15 then return "可靠" end
  if n >= 5 then return "熟手" end
  return "新手"
end

-- 连续送达：从第 3 单起每单 +2%，上限 +10%；超时/放弃清零
function jobStreakBonus()
  local s = jobStreak or 0
  if s < 3 then return 1.0 end
  return 1.0 + math.min(0.10, (s - 2) * 0.02)
end

-- 当前送达环境加成标签（HUD 追踪条提示用）
function jobDeliveryBonusTags()
  local tags = {}
  if isNight ~= nil and isNight() then tags[#tags + 1] = "夜间+10%" end
  if weather == "rain" then
    tags[#tags + 1] = "雨天+8%"
  elseif weather == "snow" then
    tags[#tags + 1] = "雪天+12%"
  end
  return table.concat(tags, " · ")
end

-- 今日首单加成是否还能领（每天一次）
function jobDailyBonusAvailable()
  return (day or 1) ~= (lastJobDay or 0)
end

-- 限时急单超时：货物退回、没有报酬
function jobExpire()
  if job == nil then return end
  local name = job.cargo
  jobDespawnCargo()
  job = nil
  jobOffers = {}
  jobStreak = 0  -- 超时断掉连续送达
  recomputeStats()
  jobResult = { ok = false, cargo = name, t = 3.6 }
  if addJournal ~= nil then
    addJournal("合同超时：" .. (name or "货物"), "fail")
  end
  toast.text = "合同超时：" .. (name or "货物") .. " 已退回，没有报酬"
  toast.t = 4.0
  if autoSave ~= nil then autoSave() end
end

-- 到达当前卸货点：多站合同推进到下一站，最后一站才结算
function jobArrive()
  if job == nil then return false end
  local stops = job.stops
  if stops ~= nil and (job.stopIndex or 1) < #stops then
    job.stopIndex = (job.stopIndex or 1) + 1
    local s = stops[job.stopIndex]
    job.destId, job.destName = s.id, s.name
    SpawnFloatText(rv.x, 1.8, rv.z,
                   string.format("第 %d/%d 站已送达", job.stopIndex - 1, #stops),
                   false, 1.4, 1.0, 0.88, 0.4)
    toast.text = string.format("第 %d/%d 站已送达，继续前往 %s",
                               job.stopIndex - 1, #stops, s.name)
    toast.t = 4.0
    if PlaySfx then PlaySfx("pickup") end
    return false
  end
  jobComplete()
  return true
end

-- 急转弯甩货：横向加速度超过阈值时按秒扣货况，易碎货更低阈值/更大损失
function cornerCargoLoss(latG, fragile, dt)
  if latG == nil then return 0 end
  local threshold = fragile and 0.40 or 0.55
  if latG <= threshold then return 0 end
  local mult = fragile and 0.22 or 0.14
  return math.min(0.30, (latG - threshold) * mult * (dt or 0))
end

function updateJobs(dt)
  if job ~= nil then
    -- 限时急单：超过截止时间直接作废
    if job.deadlineAbs ~= nil and jobNowAbs() > job.deadlineAbs then
      jobExpire()
      return
    end
    -- 雨天行驶：易碎货缓慢受潮掉货况（带防雨篷布则免疫）
    if job.fragile and weather == "rain" and phase == "driving" and
       not (hasItem and hasItem("tarp", 1)) then
      job.condition = math.max(0, (job.condition or 1) - 0.006 * (dt or 0))
      if not job.rainWarned and job.condition < 0.85 then
        job.rainWarned = true
        toast.text = "雨水正在打湿易碎货——尽快送达，或先找地方避雨"
        toast.t = 3.4
      end
    end
    -- 易腐货：行驶时货况随时间缓慢下降，逼玩家规划路线
    if job.perishable and phase == "driving" then
      job.condition = math.max(0, (job.condition or 1) - 0.002 * (dt or 0))
      if not job.rotWarned and job.condition < 0.80 then
        job.rotWarned = true
        toast.text = "易腐货开始变质——尽快送达"
        toast.t = 3.4
      end
    end
    -- 急转弯甩货：用横向加速度（updateDrive 已算好 rv.latG）判断
    if phase == "driving" and (dt or 0) > 0 then
      -- 只使用本帧 updateDrive 算出的横向 G，避免旧值造成误判
      local latG = nil
      if rv.latGSet then
        latG = rv.latG
        rv.latGSet = false
      end
      local loss = cornerCargoLoss(latG, job.fragile or job.livestock, dt)
      if loss > 0 then
        job.condition = math.max(0, (job.condition or 1) - loss)
        if not job.tossWarned and job.condition < 0.85 then
          job.tossWarned = true
          toast.text = "急转弯甩动了货物——稳住方向，货况在下降"
          toast.t = 3.4
        end
      end
    end
    jobPlaceCargo()
    -- 停在目的地（setPhase 已把 atNode 设成节点 id）即结算/推进下一站
    if job.stage == "carry" and phase == "parked" and atNode == job.destId then
      if job.stops ~= nil then jobArrive() else jobComplete() end
    end
  end
end
