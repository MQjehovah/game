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

function on_start()
  SetVar("sun", 150)
  SetVar("selected", nil)
  SetVar("gameover", false)
  SetVar("won", false)
  SetVar("started", false)
  SetVar("wave", 0)
  SetVar("paused", false)
  lastWave = 0
  started = false
  cooldowns = {}
  selected = nil
  UIShow("ui/main.ui.json")
  PlaySfx("click")
end

function on_update()
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

  if GetVar("gameover") == true then return end
  if GetVar("paused") == true then return end

  -- Seed-packet cooldowns (about 60Hz).
  for type, cd in pairs(cooldowns) do
    if cd > 0 then cooldowns[type] = cd - 0.016 end
    SetVar("cooldown_" .. type, cooldowns[type] or 0)
  end

  -- Win: the wave_director stops after the final wave and flags "won".
  local won = GetVar("plugin:wave_director:won")
  if started and won == true then
    SetVar("gameover", true)
    PlaySfx("win")
  end

  -- Waves scale in count and mix harder zombies later.
  local w = GetVar("plugin:wave_director:wave")
  if started and type(w) == "number" and w > lastWave then
    lastWave = w
    SetVar("wave", w)
    PlaySfx("wave")
    local count = 2 + w
    for i = 1, count do
      spawnZombie("basic", math.random(0, ROWS - 1))
    end
    if w >= 2 and w % 2 == 0 then spawnZombie("cone", math.random(0, ROWS - 1)) end
    if w >= 5 and w % 3 == 0 then spawnZombie("bucket", math.random(0, ROWS - 1)) end
  end

  -- Click the lawn to plant / shovel.
  if InputMousePressed(0) and selected then
    local m = InputMousePos()
    if not m then return end
    local col = math.floor((m.x - X0 + CELL_X * 0.5) / CELL_X)
    local row = math.floor((m.y - Y0 + CELL_Y * 0.5) / CELL_Y)
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
