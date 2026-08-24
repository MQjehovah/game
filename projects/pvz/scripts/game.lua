-- NeonPvZ 主游戏逻辑 (Lua 后端)
-- 玩法编排: 阳光经济 / 种植 / 波次(来自 wave_director 插件) / 胜负判定。
-- 动态内容全部走 SpawnPrefab(预制体); 组件数据经 EntityComponent 读取。

local ROWS, COLS = 5, 9
local X0, Y0, CELL_X, CELL_Y = 140, 110, 100, 100

local function rowY(row) return Y0 + row * CELL_Y end
local function colX(col) return X0 + col * CELL_X end

local PLANTS = {
  sunflower =  { cost = 50,  prefab = "sunflower",  action = "plant_sunflower" },
  peashooter = { cost = 100, prefab = "peashooter", action = "plant_peashooter" },
  snowpea =    { cost = 175, prefab = "snowpea",    action = "plant_snowpea" },
  wallnut =    { cost = 50,  prefab = "wallnut",    action = "plant_wallnut" },
}

local ZOMBIES = { basic = "zombie_basic", cone = "zombie_cone", bucket = "zombie_bucket" }

local selected = nil
local lastWave = 0
local started = false

local function rowHasPlant(row, x)
  local list = GetVar("row_plants_" .. row)
  if type(list) ~= "table" then return false end
  for i = 1, #list do
    if math.abs(list[i].x - x) < 45 then return true end
  end
  return false
end

local function spawnZombie(type, row)
  SpawnPrefab(ZOMBIES[type] or "zombie_basic", { x = 1150, y = rowY(row), z = -0.5 })
end

function on_start()
  SetVar("sun", 150)
  SetVar("selected", nil)
  SetVar("gameover", false)
  SetVar("started", false)
  SetVar("wave", 0)
  lastWave = 0
  started = false
  UIShow("ui/main.ui.json")
  PlaySfx("click")
end

function on_update()
  -- UIClicked returns 1/0 (number), so compare explicitly in Lua.
  if UIClicked("Start") == 1 then
    started = true
    SetVar("started", true)
    UIHide()
    PlaySfx("click")
  end

  -- 选择植物: input.json 动作 (数字键 1-4)
  for type, def in pairs(PLANTS) do
    if ActionPressed(def.action) then
      selected = type
      SetVar("selected", type)
      PlaySfx("click")
    end
  end

  if GetVar("gameover") == true then return end

  -- 波次: 轮询 wave_director 插件的作用域 GameVar
  local w = GetVar("plugin:wave_director:wave")
  if started and type(w) == "number" and w > lastWave then
    lastWave = w
    SetVar("wave", w)
    PlaySfx("wave")
    local count = 2 + w
    for i = 1, count do
      spawnZombie("basic", math.random(0, ROWS - 1))
    end
    if w >= 2 and w % 2 == 0 then
      spawnZombie("cone", math.random(0, ROWS - 1))
    end
  end

  -- 点击草地种植
  if InputMousePressed(0) and selected then
    local m = InputMousePos()
    if not m then return end
    local col = math.floor((m.x - X0 + CELL_X * 0.5) / CELL_X)
    local row = math.floor((m.y - Y0 + CELL_Y * 0.5) / CELL_Y)
    if col >= 0 and col < COLS and row >= 0 and row < ROWS then
      local def = PLANTS[selected]
      local sun = GetVar("sun")
      if type(sun) ~= "number" then sun = 0 end
      if sun >= def.cost and not rowHasPlant(row, colX(col)) then
        local e = SpawnPrefab(def.prefab, { x = colX(col), y = rowY(row), z = 0 })
        if e then
          SetVar("sun", sun - def.cost)
          PlaySfx("plant")
        end
      else
        PlaySfx("click")
      end
    end
  end
end
