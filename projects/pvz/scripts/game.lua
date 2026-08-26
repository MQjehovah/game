-- NeonPvZ master game logic (Lua back-end)
-- Sun economy / planting (with shovel + seed-packet cooldowns) / waves /
-- pause / win-loss. The wave_director plugin drives the wave counter.
local ROWS, COLS = 5, 9
local X0, Y0, CELL_X, CELL_Y = 140, 110, 100, 100

local function rowY(row) return Y0 + row * CELL_Y end
local function colX(col) return X0 + col * CELL_X end

local PLANTS = {
  sunflower  = { cost = 50,  prefab = "sunflower",  action = "plant_sunflower",  cd = 4 },
  peashooter = { cost = 100, prefab = "peashooter", action = "plant_peashooter", cd = 4 },
  snowpea    = { cost = 175, prefab = "snowpea",    action = "plant_snowpea",    cd = 4 },
  repeater   = { cost = 200, prefab = "repeater",   action = "plant_repeater",   cd = 6 },
  cherrybomb = { cost = 150, prefab = "cherrybomb", action = "plant_cherrybomb", cd = 30 },
  wallnut    = { cost = 50,  prefab = "wallnut",    action = "plant_wallnut",    cd = 10 },
}
local ORDER = { "sunflower", "peashooter", "snowpea", "repeater", "cherrybomb", "wallnut" }
local ZOMBIES = { basic = "zombie_basic", cone = "zombie_cone", bucket = "zombie_bucket" }

local selected = nil
local lastWave = 0
local started = false
local cooldowns = {}   -- type -> seconds remaining before it can be planted again
local waveTimer = 0    -- seconds since the last wave launched (self-contained)
local WAVE_INTERVAL = 10 -- seconds between waves
local MAX_WAVES = 8      -- survive this many waves to win
local mowers_ = {}     -- row -> { ent, x, active } (割草机)

local function rowHasPlant(row, x)
  local list = GetVar("row_plants_" .. row)
  if type(list) ~= "table" then return false end
  for i = 1, #list do
    if math.abs(list[i].x - x) < 45 then return true end
  end
  return false
end

local function findRowPlant(row, x)
  local list = GetVar("row_plants_" .. row)
  if type(list) ~= "table" then return nil end
  for i = 1, #list do
    if math.abs(list[i].x - x) < 45 then return list[i] end
  end
  return nil
end

local function removeRowPlant(row, id)
  local list = GetVar("row_plants_" .. row)
  if type(list) ~= "table" then return end
  for i = #list, 1, -1 do
    if list[i].id == id then table.remove(list, i) break end
  end
  SetVar("row_plants_" .. row, list)
end

local function spawnZombie(type, row)
  SpawnPrefab(ZOMBIES[type] or "zombie_basic", { x = 1150, y = rowY(row), z = -0.5 })
end

-- 场上是否已无僵尸 (所有行清空): 胜利需"最后一波发射"且清光全场。
local function allZombiesGone()
  for r = 0, ROWS - 1 do
    local list = GetVar("row_zombies_" .. r)
    if type(list) == "table" and #list > 0 then return false end
  end
  return true
end

-- 重开: 重新加载本场景, 所有状态/实体/插件(wave_director)从头再来。
local function restartGame()
  ChangeScene("scenes/pvz.json")
end

function on_start()
  SetVar("sun", 150)
  SetVar("selected", nil)
  SetVar("gameover", false)
  SetVar("won", false)
  SetVar("started", false)
  SetVar("wave", 0)
  SetVar("paused", false)
  lastWave = 0
  waveTimer = 0
  started = false
  cooldowns = {}
  selected = nil
  mowers_ = {}
  -- 每行一台割草机, 停在防线左侧 (x=100); 僵尸越过 x=130 触发横扫。
  local mowerList = {}
  for r = 0, ROWS - 1 do
    local m = SpawnPrefab("mower", { x = 100, y = rowY(r), z = -1 })
    mowers_[r] = { ent = m, x = 100, active = false }
    mowerList[r + 1] = { active = false }
  end
  SetVar("mowers", mowerList)
  UIShow("ui/main.ui.json")
  PlaySfx("click")

  -- demo 模式 (场景 Game 实体 vars.demo=1): 自动开始, 供冒烟/演示/无人值守验证。
  if demo == 1 then
    started = true
    SetVar("started", true)
    UIHide()
    print("demo mode: auto-started")
  end
end

function on_update(e, dt)
  -- 重开 (R 键): 任何时候都可从头再来 (含结算画面)。
  if ActionPressed("restart") then restartGame(); return end

  if UIClicked("Start") then
    started = true
    SetVar("started", true)
    UIHide()
    PlaySfx("click")
  end

  -- Pick a plant (1-6) or the shovel (S).
  for i, type in ipairs(ORDER) do
    if ActionPressed(PLANTS[type].action) then
      selected = type
      SetVar("selected", type)
      PlaySfx("click")
    end
  end
  if ActionPressed("shovel") then
    selected = "shovel"
    SetVar("selected", "shovel")
    PlaySfx("click")
  end

  -- Pause toggle (P).
  if ActionPressed("pause") then
    local p = GetVar("paused")
    SetVar("paused", not (p == true))
    PlaySfx("click")
  end

  if GetVar("gameover") == true then
    -- 结算画面: 点击任意处重开。
    if InputMousePressed(0) then restartGame() end
    return
  end
  if GetVar("paused") == true then return end

  -- 鼠标点选顶部种子包 (与 hud.js 几何一致: bx=240, 步长 136, 6 植物 + 铲子)。
  if InputMousePressed(0) then
    local m = InputMousePos()
    if m and m.y < 58 then
      local idx = math.floor((m.x - 240) / 136)
      if idx >= 0 and idx < 6 then
        selected = ORDER[idx + 1]
        SetVar("selected", selected)
        PlaySfx("click")
      elseif m.x >= 240 + 6 * 136 + 2 then
        selected = "shovel"
        SetVar("selected", "shovel")
        PlaySfx("click")
      end
    end
  end

  -- Seed-packet cooldowns (real time via dt).
  for type, cd in pairs(cooldowns) do
    if cd > 0 then cooldowns[type] = cd - dt end
    SetVar("cooldown_" .. type, cooldowns[type] or 0)
  end

  -- 波次(自包含, 不依赖插件, 打包可玩): 每 WAVE_INTERVAL 秒推一波,
  -- 波数递增并混合更硬的僵尸; 坚持到 MAX_WAVES 且清光全场即胜。
  if started then
    waveTimer = waveTimer + dt
    if waveTimer >= WAVE_INTERVAL and lastWave < MAX_WAVES then
      waveTimer = 0
      lastWave = lastWave + 1
      SetVar("wave", lastWave)
      PlaySfx("wave")
      print("wave " .. lastWave .. " spawned " .. (2 + lastWave) .. " zombies")
      local count = 2 + lastWave
      for i = 1, count do
        spawnZombie("basic", math.random(0, ROWS - 1))
      end
      if lastWave >= 2 and lastWave % 2 == 0 then spawnZombie("cone", math.random(0, ROWS - 1)) end
      if lastWave >= 5 and lastWave % 3 == 0 then spawnZombie("bucket", math.random(0, ROWS - 1)) end
    end

    -- Win: 最后一波已发射 且 场上所有僵尸清空才算胜利。
    if lastWave >= MAX_WAVES and allZombiesGone() then
      SetVar("won", true)
      SetVar("gameover", true)
      PlaySfx("win")
    end
  end

  -- 割草机: 行内僵尸越过 x=130 触发, 快速右扫并击杀沿途僵尸 (标准 PvZ 兜底)。
  local mowerList = GetVar("mowers")
  for r = 0, ROWS - 1 do
    local m = mowers_[r]
    if m then
      if not m.active then
        local zlist = GetVar("row_zombies_" .. r)
        if type(zlist) == "table" then
          for i = 1, #zlist do
            local zp = GetPosition({ id = zlist[i].id, gen = zlist[i].gen })
            if zp and zp.x <= 130 then
              m.active = true
              if type(mowerList) == "table" then mowerList[r + 1].active = true end
              PlaySfx("mower")
              break
            end
          end
        end
      end
      if m.active then
        m.x = m.x + 420 * dt
        SetPosition(m.ent, { x = m.x, y = rowY(r), z = -1 })
        local zlist = GetVar("row_zombies_" .. r)
        if type(zlist) == "table" then
          for i = #zlist, 1, -1 do
            local zent = { id = zlist[i].id, gen = zlist[i].gen }
            local zp = GetPosition(zent)
            if zp and math.abs(zp.x - m.x) < 70 then SetHealth(zent, 0) end
          end
        end
        if m.x > 1180 then
          Despawn(m.ent)
          mowers_[r] = nil
          if type(mowerList) == "table" then mowerList[r + 1] = nil end
        end
      end
    end
  end
  SetVar("mowers", mowerList)

  -- Click the lawn to plant / shovel. InputMousePos() returns DESIGN coords
  -- (y down); the world is y-up, so convert y before mapping to a row.
  if InputMousePressed(0) and selected then
    local m = InputMousePos()
    if not m then return end
    local wy = 720 - m.y
    local col = math.floor((m.x - X0 + CELL_X * 0.5) / CELL_X)
    local row = math.floor((wy - Y0 + CELL_Y * 0.5) / CELL_Y)
    if col >= 0 and col < COLS and row >= 0 and row < ROWS then
      if selected == "shovel" then
        local existing = findRowPlant(row, colX(col))
        if existing then
          removeRowPlant(row, existing.id)
          Despawn({ id = existing.id, gen = existing.gen })
          PlaySfx("plant")
        end
      else
        local def = PLANTS[selected]
        local sun = GetVar("sun")
        if type(sun) ~= "number" then sun = 0 end
        local cd = cooldowns[selected] or 0
        if sun >= def.cost and cd <= 0 and not rowHasPlant(row, colX(col)) then
          local e = SpawnPrefab(def.prefab, { x = colX(col), y = rowY(row), z = 0 })
          if e then
            SetVar("sun", sun - def.cost)
            cooldowns[selected] = def.cd
            PlaySfx("plant")
          end
        else
          PlaySfx("click")
        end
      end
    end
  end
end
