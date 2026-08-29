-- Warcraft-like RTS (3D 版): 3D 世界 + 俯视角透视相机 + 投影 UI。
-- 系统: 选择/指令/采矿经济/建筑放置/生产队列/敌军 AI 波次/胜负。
-- 拾取: 屏幕空间最近匹配 (选择单位/目标) + 射线-地面求交 (移动落点)。

-- ============ 相机常量 (与 scenes/main.json 的相机严格一致) ============
local CAM_POS = { x = 50, y = 70, z = 107.6 }
local CAM_FWD = { x = 0, y = -0.71934, z = -0.69466 }
local CAM_RIGHT = { x = 1, y = 0, z = 0 }
local CAM_UP = { x = 0, y = 0.69466, z = -0.71934 }
local TAN_Y = 0.5206 -- fov 55°/2
local TAN_X = 0.9254 -- * 16/9

-- 屏幕像素 -> 地面 (y=0) 世界点
local function groundPick(sx, sy)
  local nx = (sx / 1280) * 2 - 1
  local ny = 1 - (sy / 720) * 2
  local dx = CAM_FWD.x + CAM_RIGHT.x * nx * TAN_X + CAM_UP.x * ny * TAN_Y
  local dy = CAM_FWD.y + CAM_RIGHT.y * nx * TAN_X + CAM_UP.y * ny * TAN_Y
  local dz = CAM_FWD.z + CAM_RIGHT.z * nx * TAN_X + CAM_UP.z * ny * TAN_Y
  if dy >= -0.001 then return nil end
  local t = -CAM_POS.y / dy
  return { x = CAM_POS.x + dx * t, z = CAM_POS.z + dz * t }
end

-- ============ 数据表 ============
local UNIT_DEFS = {
  peon     = { hp = 90,  dmg = 6,  range = 2.2, cd = 1.0, speed = 5.5, r = 0.7, cost = 75,  bt = 6,  name = "苦工", prefab = "unit_knight" },
  footman  = { hp = 220, dmg = 18, range = 2.4, cd = 1.1, speed = 4.6, r = 0.8, cost = 135, bt = 9,  name = "步兵", prefab = "unit_knight" },
  rifleman = { hp = 160, dmg = 21, range = 12,  cd = 1.6, speed = 4.2, r = 0.7, cost = 205, bt = 12, name = "火枪手", ranged = true, prefab = "unit_mage" },
  grunt    = { hp = 240, dmg = 20, range = 2.4, cd = 1.2, speed = 4.8, r = 0.8, name = "兽人步兵", prefab = "unit_knight" },
}
local BUILD_DEFS = {
  hall     = { hp = 900, w = 5,  d = 5,  cost = 0,   bt = 0,  name = "大本营", prefab = "bld_hall", trains = { "peon" } },
  farm     = { hp = 340, w = 2.6, d = 2.6, cost = 80,  bt = 8,  name = "农场", prefab = "bld_farm" },
  barracks = { hp = 520, w = 3.6, d = 3.2, cost = 160, bt = 14, name = "兵营", prefab = "bld_barracks", trains = { "footman", "rifleman" } },
  mine     = { hp = 1200, w = 3.4, d = 3.4, name = "金矿", prefab = "fx_mine" },
}

-- ============ 状态 ============
local units, buildings = {}, {}
local selected, selectedBuilding = {}, nil
local buildMode = nil
local atkCd = {}
local waveT, waveN = 25, 0
local gameover, won = false, false
local ids = 0

local MINES = {
  { x = 10, z = 62 },
  { x = 92, z = 14 },
}

local function clamp(v, lo, hi) return math.max(lo, math.min(hi, v)) end
local function dist(x1, z1, x2, z2) return math.sqrt((x1 - x2) ^ 2 + (z1 - z2) ^ 2) end
local function newId() ids = ids + 1; return ids end

local function spawnUnit(kind, team, x, z)
  local d = UNIT_DEFS[kind]
  local ent = SpawnPrefab(d.prefab, { x = x, y = 0, z = z })
  -- 敌方单位转向玩家方向
  if team == "enemy" and ent ~= nil then SetRotationY(ent, math.pi) end
  local u = { id = newId(), ent = ent, kind = kind, team = team, x = x, z = z,
              hp = d.hp, maxHp = d.hp, r = d.r, state = "idle", cd = 0,
              tx = x, tz = z, carry = 0 }
  units[#units + 1] = u
  return u
end

local function spawnBuilding(kind, team, x, z, done)
  local d = BUILD_DEFS[kind]
  local ent = SpawnPrefab(d.prefab, { x = x, y = 0, z = z })
  local b = { id = newId(), ent = ent, kind = kind, team = team, x = x, z = z,
              hp = done and d.hp or math.max(60, d.hp * 0.15), maxHp = d.hp,
              w = d.w, d2 = d.d, done = done and true or false, progress = 0, queue = {} }
  buildings[#buildings + 1] = b
  return b
end

local function findBuilding(kind, team)
  for _, b in ipairs(buildings) do
    if b.kind == kind and b.team == team and b.done then return b end
  end
  return nil
end

local function killBuilding(idx)
  local b = buildings[idx]
  if b.kind == "hall" then
    gameover = true
    won = (b.team == "enemy")
  end
  if b.ent ~= nil then Despawn(b.ent) end
  table.remove(buildings, idx)
end

local function nearestEnemy(u, maxR)
  local best, bestD, isB, isUnit = nil, maxR, false, nil
  for _, o in ipairs(units) do
    if o.team ~= u.team then
      local d = dist(u.x, u.z, o.x, o.z)
      if d < bestD then best, bestD, isB, isUnit = o, d, false, o end
    end
  end
  for _, b in ipairs(buildings) do
    if b.team ~= u.team then
      local d = dist(u.x, u.z, b.x, b.z) - b.w * 0.4
      if d < bestD then best, bestD, isB, isUnit = b, d, true, nil end
    end
  end
  if best then return best, bestD, isB, isUnit end
  return nil
end

local function damageUnit(u, dmg) u.hp = u.hp - dmg end

local function damageBuilding(b, dmg)
  b.hp = b.hp - dmg
  for i, bb in ipairs(buildings) do
    if bb == b and bb.hp <= 0 then killBuilding(i) return end
  end
end

local function attackTick(u, target, isB, dt)
  u.cd = u.cd - dt
  if u.cd > 0 then return end
  u.cd = UNIT_DEFS[u.kind].cd
  local d = UNIT_DEFS[u.kind]
  if isB then
    damageBuilding(target, d.dmg)
  else
    damageUnit(target, d.dmg)
    if d.ranged then
      PlaySfx("shoot")
    else
      PlaySfx("splat")
    end
  end
end

local function moveToward(u, tx, tz, speed, dt)
  local dd = dist(u.x, u.z, tx, tz)
  if dd > 0.05 then
    u.x = u.x + (tx - u.x) / dd * speed * dt
    u.z = u.z + (tz - u.z) / dd * speed * dt
    if u.ent ~= nil then
      SetPosition(u.ent, { x = u.x, y = 0, z = u.z })
      SetRotationY(u.ent, math.atan((tx - u.x) / math.max(0.001, dd) * -1, (tz - u.z) / dd * -1))
    end
  end
end

local function updateUnit(u, dt)
  local d = UNIT_DEFS[u.kind]
  u.cd = math.max(0, u.cd - dt)

  if u.kind == "peon" and u.state == "tomine" then
    local mine = MINES[1]
    if u.team == "enemy" then mine = MINES[2] end
    if dist(u.x, u.z, mine.x, mine.z) < 3.2 then
      u.state = "mining"; u.t = 0
    else
      moveToward(u, u.tx, u.tz, d.speed, dt)
    end
    return
  end
  if u.kind == "peon" and u.state == "mining" then
    u.t = (u.t or 0) + dt
    if u.t >= 2.0 then
      local hall = findBuilding("hall", u.team)
      if hall then
        u.state = "return"; u.tx, u.tz = hall.x, hall.z + hall.d2 * 0.5 + 1
        u.carry = 10
      else
        u.state = "idle"
      end
    end
    return
  end
  if u.kind == "peon" and u.state == "return" then
    local hall = findBuilding("hall", u.team)
    if not hall then u.state = "idle" return end
    if dist(u.x, u.z, u.tx, u.tz) < 2.5 then
      local key = "gold_" .. u.team
      SetVar(key, (GetVar(key) or 0) + (u.carry or 10))
      u.carry = 0
      u.state = "tomine"
      local mine = MINES[1]
      if u.team == "enemy" then mine = MINES[2] end
      u.tx, u.tz = mine.x + 1.5, mine.z - 1
      SpawnFloatText(u.x, 1.6, u.z, "+10", false, 0.6)
    else
      moveToward(u, u.tx, u.tz, d.speed, dt)
    end
    return
  end

  -- 战斗索敌
  local target, d2, isB = nearestEnemy(u, d.range + 1)
  if target then
    if d2 <= d.range then
      attackTick(u, target, isB, dt)
    else
      moveToward(u, target.x, target.z, d.speed, dt)
    end
    return
  end

  moveToward(u, u.tx, u.tz, d.speed, dt)
end

-- 敌军攻击单位: 近处索敌, 否则推进玩家大厅
local function updateAttacker(u, dt)
  local d = UNIT_DEFS[u.kind]
  local target, d2, isB = nearestEnemy(u, 9)
  if target then
    if d2 <= d.range then
      attackTick(u, target, isB, dt)
    else
      moveToward(u, target.x, target.z, d.speed, dt)
    end
    return
  end
  local ph = findBuilding("hall", "player")
  local gx, gz = (ph and ph.x) or 18, (ph and ph.z) or 20
  if dist(u.x, u.z, gx, gz) > d.range then
    moveToward(u, gx, gz, d.speed, dt)
  end
end

local function updateBuildings(dt)
  for i = #buildings, 1, -1 do
    local b = buildings[i]
    if not b.done then
      b.progress = b.progress + dt
      b.hp = math.min(b.maxHp, b.hp + (b.maxHp - 60) * dt / BUILD_DEFS[b.kind].bt)
      if b.progress >= BUILD_DEFS[b.kind].bt then b.done = true end
    end
    if b.queue and #b.queue > 0 and b.done then
      local q = b.queue[1]
      q.t = q.t + dt
      if q.t >= UNIT_DEFS[q.kind].bt then
        spawnUnit(q.kind, b.team, b.x, b.z + b.d2 * 0.5 + 1.2).state = "idle"
        table.remove(b.queue, 1)
      end
    end
    if b.hp <= 0 then killBuilding(i) end
  end
end

local function updateWaves(dt)
  waveT = waveT - dt
  if waveT <= 0 then
    waveN = waveN + 1
    waveT = 40
    local hx, hz = 78, 58
    local hall = findBuilding("hall", "enemy")
    if hall then hx, hz = hall.x, hall.z + 4 end
    local count = 2 + waveN
    for i = 1, count do
      local u = spawnUnit("grunt", "enemy", hx + (i - count / 2) * 2, hz)
      u.state = "attack"
    end
    SpawnFloatText(80, 3, 20, "第 " .. waveN .. " 波进攻!", false, 1.2)
    PlaySfx("wave")
  end
end

-- ============ 拾取 (屏幕空间) ============
local function pickUnit(sx, sy, team, maxPx)
  local best, bd, idx = nil, maxPx or 32, nil
  for i, u in ipairs(units) do
    if team == nil or u.team == team then
      local s = WorldToScreen(u.x, 1.1, u.z)
      if s and s.x then
        local d = dist(sx, s.y, s.x, s.y)
        if d < bd then best, bd, idx = u, d, i end
      end
    end
  end
  return best, idx
end

local function pickBuilding(sx, sy, team, maxPx)
  local best, bd = nil, maxPx or 42
  for _, b in ipairs(buildings) do
    if team == nil or b.team == team then
      local s = WorldToScreen(b.x, 1.5, b.z)
      if s and s.x then
        local d = dist(sx, s.y, s.x, s.y)
        if d < bd then best, bd = b, d end
      end
    end
  end
  return best
end

function on_start()
  SetVar("gold_player", 300)
  spawnBuilding("hall", "player", 18, 20, true)
  spawnBuilding("mine", "neutral", 10, 62, true)
  spawnBuilding("mine", "neutral", 92, 14, true)
  for i = 1, 3 do spawnUnit("peon", "player", 20 + i * 2, 30) end
  spawnBuilding("hall", "enemy", 82, 62, true)
  spawnUnit("peon", "enemy", 78, 58)
  spawnUnit("grunt", "enemy", 80, 55)
  spawnUnit("grunt", "enemy", 76, 57)
end


function on_update(e, dt)
  if GetVar("paused") == true then return end
  if gameover then
    if InputMousePressed(0) then ChangeScene("assets/scenes/main.json") end
    return
  end

  updateBuildings(dt)
  for i = #units, 1, -1 do
    local u = units[i]
    if u.hp <= 0 then
      if u.ent ~= nil then Despawn(u.ent) end
      for j = #selected, 1, -1 do if selected[j] == u then table.remove(selected, j) end end
      table.remove(units, i)
    else
      if u.state == "attack" and u.team == "enemy" then
        updateAttacker(u, dt)
      else
        updateUnit(u, dt)
      end
    end
  end
  updateWaves(dt)

  if not findBuilding("hall", "player") then gameover = true; won = false end
  if not findBuilding("hall", "enemy") then gameover = true; won = true end

  -- 法力/金刷新
  SetVar("gold_player", GetVar("gold_player") or 300)

  if ActionPressed("cancel") then buildMode = nil end
  if ActionPressed("build_farm") then buildMode = "farm" end
  if ActionPressed("build_barracks") then buildMode = "barracks" end

  local m = InputMousePos()
  if InputMousePressed(0) and m then
    local handled = false
    if selectedBuilding and selectedBuilding.done and BUILD_DEFS[selectedBuilding.kind].trains then
      for bi, tkind in ipairs(BUILD_DEFS[selectedBuilding.kind].trains) do
        local s = WorldToScreen(selectedBuilding.x, 0.2, selectedBuilding.z)
        if s and s.x then
          local bx, by = s.x + 40, s.y - 20 + (bi - 1) * 34
          if m.x >= bx and m.x <= bx + 170 and m.y >= by and m.y <= by + 30 then
            local cost = UNIT_DEFS[tkind].cost
            local g = GetVar("gold_player") or 0
            if g >= cost and #selectedBuilding.queue < 5 then
              SetVar("gold_player", g - cost)
              selectedBuilding.queue[#selectedBuilding.queue + 1] = { kind = tkind, t = 0 }
              PlaySfx("click")
            end
            handled = true
          end
        end
      end
    end
    if not handled and buildMode == nil and #selected == 1 and selected[1].kind == "peon" then
      for bi, bkind in ipairs({ "farm", "barracks" }) do
        local s = WorldToScreen(selected[1].x, 0.2, selected[1].z)
        if s and s.x then
          local bx, by = s.x + 40, s.y - 20 + (bi - 1) * 34
          if m.x >= bx and m.x <= bx + 170 and m.y >= by and m.y <= by + 30 then
            buildMode = bkind
            handled = true
          end
        end
      end
    end
    if not handled and buildMode and m then
      local d = BUILD_DEFS[buildMode]
      local g = GetVar("gold_player") or 0
      local gp = groundPick(m.x, m.y)
      if gp and g >= d.cost then
        spawnBuilding(buildMode, "player", gp.x, gp.z, false)
        SetVar("gold_player", g - d.cost)
        PlaySfx("plant")
      end
      buildMode = nil
      handled = true
    end
    -- 选择: 单位优先, 其次建筑
    if not handled and m then
      selected = {}
      selectedBuilding = nil
      local u, idx = pickUnit(m.x, m.y, "player", 34)
      if u then selected = { u } end
      if not u then
        local b = pickBuilding(m.x, m.y, "player", 40)
        if b and b.done then selectedBuilding = b end
      end
      if u or selectedBuilding then PlaySfx("click") end
    end
  end

  -- 右键指令
  if InputMousePressed("right") and m then
    if buildMode then buildMode = nil return end
    if #selected > 0 then
      -- 点金矿 -> 采集
      local gp = groundPick(m.x, m.y)
      local mineHit = false
      if gp then
        for _, mn in ipairs(MINES) do
          if dist(gp.x, gp.z, mn.x, mn.z) < 3.4 then mineHit = true end
        end
      end
      -- 点敌对单位 -> 攻击该目标
      local eu, eidx = pickUnit(m.x, m.y, "enemy", 30)
      for _, u in ipairs(selected) do
        if u.kind == "peon" and mineHit then
          u.state = "tomine"
          local mn = MINES[1]
          u.tx, u.tz = mn.x + 1.5, mn.z - 1
        elseif eu then
          u.state = "attack"
          u.tx, u.tz = eu.x, eu.z
          u.targetIdx = eidx
        else
          u.state = "moving"
          if gp then u.tx, u.tz = gp.x, gp.z end
        end
      end
      PlaySfx("click")
    end
  end

  -- 攻击目标跟随 (targetIdx 存活则追击)
  for _, u in ipairs(units) do
    if u.targetIdx then
      local tgt = units[u.targetIdx]
      if tgt == nil or tgt.hp <= 0 or tgt.team == u.team then
        u.targetIdx = nil
      else
        u.state = "attack"
      end
    end
  end


  if not findBuilding("hall", "player") then gameover = true; won = false end
  if not findBuilding("hall", "enemy") then gameover = true; won = true end
end

-- ============ 渲染 ============
local function drawHpBar(x, y, w, frac, col)
  DrawRect(x - w * 0.5, y, w, 4, 0.08, 0.08, 0.08, 0.9)
  if frac > 0 then DrawRect(x - w * 0.5, y, w * frac, 4, col[1], col[2], col[3], 1) end
end

local function drawGroundRing(wx, wz, r, col, segs)
  local prev = nil
  for i = 0, (segs or 26) do
    local a = i / (segs or 26) * math.pi * 2
    local s = WorldToScreen(wx + math.cos(a) * r, 0.06, wz + math.sin(a) * r)
    if s and s.x then
      if prev then
        local steps = math.max(1, math.floor(dist(prev.x, prev.y, s.x, s.y) / 12))
        for k = 0, steps do
          local t = k / steps
          DrawRect(prev.x + (s.x - prev.x) * t - 1.5, prev.y + (s.y - prev.y) * t - 1.5, 3, 3,
                   col[1], col[2], col[3], col[4] or 0.9)
        end
      end
      prev = s
    else
      prev = nil
    end
  end
end

function on_render()
  -- 建筑 (真实 3D 实体, 画布只画标注)
  for _, b in ipairs(buildings) do
    local s = WorldToScreen(b.x, 2.2, b.z)
    if s and s.x then
      local label = BUILD_DEFS[b.kind].name
      if b.kind == "mine" then label = "金矿" end
      DrawText(label, s.x, s.y, 13, 1, 1, 1, 0.9, true, false)
      local frac = clamp(b.hp / b.maxHp, 0, 1)
      if frac < 1 or not b.done then
        drawHpBar(s.x, s.y + 8, 44, frac, (b.team == "player") and { 0.3, 0.9, 0.35 } or { 1, 0.35, 0.3 })
        if not b.done then
          local bf = clamp(b.progress / BUILD_DEFS[b.kind].bt, 0, 1)
          DrawText("建造中 " .. math.floor(bf * 100) .. "%", s.x, s.y + 16, 12, 1, 0.85, 0.3, 1, true, false)
        end
      end
      -- 生产队列
      if b.queue and #b.queue > 0 then
        local q = b.queue[1]
        local fq = clamp(q.t / UNIT_DEFS[q.kind].bt, 0, 1)
        DrawRect(s.x - 22, s.y + 20, 44 * fq, 3, 0.4, 0.8, 1, 1)
      end
    end
    -- 选中建筑: 训练按钮 (投影到建筑旁)
    if b == selectedBuilding and b.done and BUILD_DEFS[b.kind].trains then
      local s2 = WorldToScreen(b.x, 0.2, b.z)
      if s2 and s2.x then
        for bi, tkind in ipairs(BUILD_DEFS[b.kind].trains) do
          local bx, by = s2.x + 46, s2.y - 24 + (bi - 1) * 32
          local afford = (GetVar("gold_player") or 0) >= UNIT_DEFS[tkind].cost
          DrawRect(bx, by, 170, 30, 0.1, 0.15, 0.22, 0.95)
          DrawRectOutline(bx, by, 170, 30, 1.5, afford and 0.4 or 0.6, afford and 0.9 or 0.4, afford and 1 or 0.4, 0.9)
          DrawText(UNIT_DEFS[tkind].name .. " " .. UNIT_DEFS[tkind].cost .. "金", bx + 10, by + 15, 13,
                   afford and 1 or 0.6, afford and 1 or 0.6, afford and 1 or 0.6, 1, false, true)
        end
      end
    end
  end

  -- 单位: 选中环 (贴地) + 血条
  for _, u in ipairs(units) do
    local col = (u.team == "player") and { 0.3, 1, 0.4, 0.95 } or { 1, 0.3, 0.3, 0.95 }
    if u.state == "attack" then col = { 1, 0.6, 0.2, 0.95 } end
    drawGroundRing(u.x, u.z, u.r + 0.25, col, 16)
    local s = WorldToScreen(u.x, 2.3, u.z)
    if s and s.x then
      drawHpBar(s.x, s.y, 30, clamp(u.hp / u.maxHp, 0, 1),
                (u.team == "player") and { 0.3, 0.9, 0.35 } or { 1, 0.35, 0.3 })
    end
  end
  for _, u in ipairs(selected) do
    drawGroundRing(u.x, u.z, u.r + 0.55, { 0.3, 1, 0.4, 1 }, 22)
  end
  if selectedBuilding then
    drawGroundRing(selectedBuilding.x, selectedBuilding.z, selectedBuilding.w * 0.75,
                   { 0.3, 1, 0.4, 1 }, 30)
    -- 训练按钮由建筑循环绘制 (上方)
  end

  -- 建造幽灵
  local m = InputMousePos()
  if buildMode and m then
    local gp = groundPick(m.x, m.y)
    if gp then
      local s = WorldToScreen(gp.x, 0.4, gp.z)
      if s and s.x then
        local d = BUILD_DEFS[buildMode]
        DrawRect(s.x - 30, s.y - 24, 60, 48, 0.3, 0.8, 0.4, 0.35)
        DrawRectOutline(s.x - 30, s.y - 24, 60, 48, 2, 0.4, 1, 0.5, 0.9)
      end
    end
  end

  -- 资源/提示
  DrawRect(0, 0, 1280, 26, 0.05, 0.07, 0.1, 0.95)
  DrawText("黄金: " .. tostring(GetVar("gold_player") or 0), 20, 13, 16, 1, 0.85, 0.2, 1, false, true)
  DrawText("波次: " .. tostring(waveN), 220, 13, 15, 0.9, 0.9, 0.9, 1, false, true)
  DrawText("左键选择 · 右键移动/攻击/采集 (点金矿) · 选中苦工 F 农场 B 兵营",
           420, 13, 12, 0.75, 0.8, 0.85, 1, false, true)

  -- 全屏闪光
  for _, fl in ipairs(flashes or {}) do
    local a = (1 - fl.age / fl.life) * 0.22
    DrawRect(0, 0, 1280, 720, fl.col[1], fl.col[2], fl.col[3], a)
  end

  -- 胜负
  if gameover then
    DrawRect(0, 0, 1280, 720, 0, 0, 0, 0.55)
    if won then
      DrawRect(380, 260, 520, 200, 0.08, 0.32, 0.12, 0.96)
      DrawText("胜利!", 640, 320, 52, 0.95, 1, 0.7, 1, true, true)
      DrawText("敌方大本营已被摧毁", 640, 390, 18, 0.9, 0.95, 1, 1, true, true)
    else
      DrawRect(380, 260, 520, 200, 0.35, 0.06, 0.06, 0.96)
      DrawText("失败", 640, 320, 52, 1, 0.5, 0.5, 1, true, true)
      DrawText("你的大本营已被摧毁", 640, 390, 18, 1, 0.85, 0.85, 1, true, true)
    end
    DrawText("点击任意处重新开始", 640, 430, 15, 0.8, 0.9, 1, 0.9, true, true)
  end
end
