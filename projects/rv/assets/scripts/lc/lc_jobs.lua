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

function jobSpawnCargo()
  jobDespawnCargo()
  if job == nil or coopRole ~= nil then return end
  local n = math.min(JOBS.maxCargo, job.crates or 1)
  for i = 1, n do
    local e = SpawnPrefab("rv_cargo_crate", { x = rv.x, y = -60, z = rv.z })
    if e ~= nil then
      SetScale(e, 0.52, 0.52, 0.52)
      jobCargoEnts[#jobCargoEnts + 1] = e
    end
  end
  jobPlaceCargo()
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

function jobOpenBoard()
  jobRefreshOffers()
  jobUI = true
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
  }
  jobUI = false
  jobOffers = {}
  jobSpawnCargo()
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
  job.condition = math.max(0, before - math.min(0.5, (drop or 0) * 0.05))
  if before >= 0.5 and job.condition < 0.5 then
    toast.text = "货物受损过半！送达报酬会明显打折"
    toast.t = 3.2
  end
end

function jobAbandon()
  if job == nil then
    jobUI = false
    return false
  end
  jobDespawnCargo()
  job = nil
  jobOffers = {}
  recomputeStats()
  toast.text = "已卸货，合同作废"
  toast.t = 2.6
  jobUI = false
  return true
end

function jobComplete()
  if job == nil then return false end
  local cond = math.max(0, math.min(1, job.condition or 1.0))
  local scale = 0.55 + 0.45 * cond
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
    fuel = math.min(VEH.tank, fuel + fuelGot)
    fuelGot = fuel - before
  end
  local name, crates = job.cargo, job.crates
  jobDelivered = jobDelivered + 1
  jobDespawnCargo()
  job = nil
  jobOffers = {}
  recomputeStats()
  SpawnFloatText(rv.x, 1.8, rv.z,
                 "送达 " .. name .. " x" .. crates, false, 1.6, 1.0, 0.86, 0.4)
  toast.text = string.format("货运完成（货况 %.0f%%）：%s%s", cond * 100,
                             table.concat(got, " "),
                             fuelGot > 0 and ("  燃料+" .. math.floor(fuelGot + 0.5) .. "L") or "")
  toast.t = 4.5
  if PlaySfx then PlaySfx("pickup") end
  if LC.flow.emit ~= nil then LC.flow.emit("job_done") end
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

function updateJobs(dt)
  if job ~= nil then
    jobPlaceCargo()
    -- 停在目的地（setPhase 已把 atNode 设成节点 id）即结算
    if job.stage == "carry" and phase == "parked" and atNode == job.destId then
      jobComplete()
    end
  end
end
