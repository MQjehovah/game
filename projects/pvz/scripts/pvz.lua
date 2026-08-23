-- NeonPvZ: 植物大战僵尸（数据驱动原型）。
-- 全部玩法与绘制都在脚本里完成：关卡 JSON 由编辑器 2D 模式摆放
-- （assets/levels/pvz_level.json），运行时用引擎的脚本绑定
-- （Json.Parse / ReadText / Input* / Draw*）实现。没有任何 C++ 玩法代码。
--
-- 棋盘布局（设计坐标 1280x720，与编辑器 2D 模式一致）：
--   9 列 x 5 行，每格 100x100，原点 (190,160)；僵尸从右侧进入向左走。

local ROWS, COLS = 5, 9
local CELL = 100
local BX, BY = 190, 160
local SPAWN_X, LOSE_X = 1150, 185

local PLANTS = {
  sunflower  = { cost = 50,  hp = 80,  cd = 5,  timer = 8,   sun = 25, dmg = 0,
                 label = "葵", color = {0.95, 0.78, 0.10, 1} },
  peashooter = { cost = 100, hp = 80,  cd = 5,  timer = 1.5, sun = 0,  dmg = 20,
                 label = "豆", color = {0.28, 0.75, 0.25, 1} },
  wallnut    = { cost = 50,  hp = 400, cd = 10, timer = 0,   sun = 0,  dmg = 0,
                 label = "坚", color = {0.62, 0.42, 0.20, 1} },
}
local CARD_ORDER = { "sunflower", "peashooter", "wallnut" }

local board = {}   -- board[row][col] = { plant=name, hp=..., t=... } or nil
local zombies = {} -- { row, x, hp, maxHp }
local peas = {}    -- { row, x }
local suns = {}    -- { x, y, vy, life, value }
local spawns = {}  -- { row, delay }
local sun = 150
local selected = "sunflower"
local cooldowns = { sunflower = 0, peashooter = 0, wallnut = 0 }
local elapsed = 0
local skyTimer = 6
local state = "playing"
local spawnCursor = 1

local function cell_center(r, c)
  return BX + (c - 0.5) * CELL, BY + (r - 0.5) * CELL
end

local function load_level()
  local text = ReadText("assets/levels/pvz_level.json")
  if text == nil or text == "" then return end
  local dom = Json.Parse(text)
  if dom == nil then return end
  if dom.plants ~= nil then
    for _, p in ipairs(dom.plants) do
      local row, col = (p.row or 0) + 1, (p.col or 0) + 1
      if PLANTS[p.plant] and row >= 1 and row <= ROWS and col >= 1 and col <= COLS
          and board[row] and board[row][col] == nil then
        board[row][col] = { plant = p.plant, hp = PLANTS[p.plant].hp, t = 0 }
      end
    end
  end
  if dom.zombies ~= nil then
    for _, z in ipairs(dom.zombies) do
      local row = (z.row or 0) + 1
      if row >= 1 and row <= ROWS then
        table.insert(spawns, { row = row, delay = z.delay or 10 })
      end
    end
    table.sort(spawns, function(a, b) return a.delay < b.delay end)
  end
end

local function handle_click(x, y)
  -- 卡牌（底部一排，与 on_render 的 HUD 一致）
  for i, name in ipairs(CARD_ORDER) do
    local cx = 14 + (i - 1) * 128
    if x >= cx and x <= cx + 120 and y >= 660 and y <= 712 then
      selected = name
      return
    end
  end
  -- 收集阳光
  for i = 1, #suns do
    local s = suns[i]
    local dx, dy = s.x - x, s.y - y
    if dx * dx + dy * dy <= 22 * 22 * 2.25 then
      sun = sun + s.value
      table.remove(suns, i)
      return
    end
  end
  -- 种植
  local col = math.floor((x - BX) / CELL) + 1
  local row = math.floor((y - BY) / CELL) + 1
  if row >= 1 and row <= ROWS and col >= 1 and col <= COLS then
    local def = PLANTS[selected]
    if def and board[row][col] == nil and cooldowns[selected] <= 0
        and sun >= def.cost then
      sun = sun - def.cost
      cooldowns[selected] = def.cd
      board[row][col] = { plant = selected, hp = def.hp, t = 0 }
    end
  end
end

function on_start(e)
  for r = 1, ROWS do board[r] = {} end
  load_level()
end

function on_update(e, dt)
  elapsed = elapsed + dt
  for k, v in pairs(cooldowns) do cooldowns[k] = math.max(0, v - dt) end

  -- 天空阳光（确定性计时，不用随机数保证可复现）
  skyTimer = skyTimer - dt
  if skyTimer <= 0 then
    skyTimer = 10
    local col = (math.floor(elapsed) % COLS) + 1
    table.insert(suns, { x = BX + (col - 0.5) * CELL, y = 40, vy = 40,
                         life = 12, value = 25 })
  end

  -- 阳光移动/过期
  local i = 1
  while i <= #suns do
    local s = suns[i]
    s.y = s.y + s.vy * dt
    if s.vy > 0 and s.y > BY + 40 then s.vy = 0 end
    s.life = s.life - dt
    if s.life <= 0 then table.remove(suns, i) else i = i + 1 end
  end

  -- 植物行为
  for r = 1, ROWS do
    for c = 1, COLS do
      local cell = board[r][c]
      if cell then
        local def = PLANTS[cell.plant]
        cell.t = cell.t + dt
        if cell.plant == "sunflower" and cell.t >= def.timer then
          cell.t = 0
          local cx, cy = cell_center(r, c)
          table.insert(suns, { x = cx, y = cy, vy = -12, life = 10, value = 25 })
        elseif cell.plant == "peashooter" and cell.t >= def.timer then
          local px = BX + (c - 1) * CELL
          local target = false
          for _, z in ipairs(zombies) do
            if z.row == r and z.x > px then target = true break end
          end
          if target then
            cell.t = 0
            table.insert(peas, { row = r, x = px + CELL * 0.5 })
          end
        end
      end
    end
  end

  -- 豌豆飞行与命中
  i = 1
  while i <= #peas do
    local p = peas[i]
    p.x = p.x + 300 * dt
    local hit = false
    for _, z in ipairs(zombies) do
      if z.row == p.row and math.abs(z.x - p.x) < 28 then
        z.hp = z.hp - PLANTS.peashooter.dmg
        hit = true
        break
      end
    end
    if hit or p.x > SPAWN_X + 60 then table.remove(peas, i) else i = i + 1 end
  end
  i = 1
  while i <= #zombies do
    if zombies[i].hp <= 0 then table.remove(zombies, i) else i = i + 1 end
  end

  -- 僵尸移动/啃食
  for _, z in ipairs(zombies) do
    local eating = false
    for c = 1, COLS do
      local cell = board[z.row][c]
      if cell then
        local cellX = BX + (c - 1) * CELL
        if z.x <= cellX + CELL and z.x >= cellX - 20 then
          cell.hp = cell.hp - 12 * dt
          if cell.hp <= 0 then board[z.row][c] = nil end
          eating = true
          break
        end
      end
    end
    if not eating then
      z.x = z.x - 30 * dt
      if z.x < LOSE_X then state = "lost" end
    end
  end

  -- 刷怪队列
  while spawnCursor <= #spawns and spawns[spawnCursor].delay <= elapsed do
    local s = spawns[spawnCursor]
    table.insert(zombies, { row = s.row, x = SPAWN_X, hp = 100, maxHp = 100 })
    spawnCursor = spawnCursor + 1
  end

  -- 胜利条件：所有刷怪出完且场上无僵尸/豌豆
  if state == "playing" and spawnCursor > #spawns
      and #zombies == 0 and #peas == 0 then
    state = "won"
  end

  -- 输入
  if state == "playing" and InputMousePressed("left") then
    local pos = InputMousePos()
    handle_click(pos.x, pos.y)
  end
end

local function hp_bar(x, y, w, hp, maxHp)
  DrawRect(x, y, w, 5, 0.1, 0.1, 0.1, 0.85)
  if maxHp > 0 and hp > 0 then
    local t = math.max(0, math.min(1, hp / maxHp))
    DrawRect(x, y, w * t, 5, t > 0.4 and 0.2 or 0.85, t > 0.4 and 0.8 or 0.15,
             t > 0.4 and 0.2 or 0.15, 1)
  end
end

function on_render()
  -- 背景与房屋
  DrawRect(0, 0, 1280, 720, 0.06, 0.09, 0.12, 1)
  DrawRect(0, BY - 12, BX + 8, 500 + 24, 0.25, 0.18, 0.12, 1)
  DrawText("家", BX * 0.5, 360, 34, 1, 1, 1, 1, true, true)
  DrawRect(BX - 6, BY - 12, 900 + 12, 500 + 24, 0.10, 0.25, 0.10, 1)

  -- 棋盘格
  for r = 1, ROWS do
    for c = 1, COLS do
      local x, y = BX + (c - 1) * CELL, BY + (r - 1) * CELL
      local dark = ((r + c) % 2) == 0
      if dark then
        DrawRect(x, y, CELL, CELL, 0.18, 0.42, 0.16, 1)
      else
        DrawRect(x, y, CELL, CELL, 0.24, 0.50, 0.20, 1)
      end
    end
  end
  for i = 0, COLS do
    DrawRect(BX + i * CELL, BY, 1.5, 500, 0.08, 0.22, 0.08, 1)
  end
  for i = 0, ROWS do
    DrawRect(BX, BY + i * CELL, 900, 1.5, 0.08, 0.22, 0.08, 1)
  end

  -- 植物
  for r = 1, ROWS do
    for c = 1, COLS do
      local cell = board[r][c]
      if cell then
        local def = PLANTS[cell.plant]
        local cx, cy = cell_center(r, c)
        DrawRect(cx - 34, cy - 34, 68, 68, def.color[1], def.color[2], def.color[3],
                 def.color[4])
        DrawRectOutline(cx - 34, cy - 34, 68, 68, 2, 0, 0, 0, 0.5)
        DrawText(def.label, cx, cy, 30, 1, 1, 1, 1, true, true)
        hp_bar(cx - 32, BY + (r - 1) * CELL + CELL - 12, 64, cell.hp, def.hp)
      end
    end
  end

  -- 豌豆
  for _, p in ipairs(peas) do
    local y = BY + (p.row - 0.5) * CELL
    DrawRect(p.x - 7, y - 7, 14, 14, 0.2, 0.85, 0.25, 1)
  end

  -- 僵尸
  for _, z in ipairs(zombies) do
    local y = BY + (z.row - 0.5) * CELL
    DrawRect(z.x - 28, y - 34, 56, 68, 0.45, 0.55, 0.40, 1)
    DrawRectOutline(z.x - 28, y - 34, 56, 68, 2, 0, 0, 0, 0.5)
    DrawText("僵", z.x, y - 4, 26, 1, 1, 1, 1, true, true)
    hp_bar(z.x - 26, y - 44, 52, z.hp, z.maxHp)
  end

  -- 阳光
  for _, s in ipairs(suns) do
    DrawRect(s.x - 22, s.y - 22, 44, 44, 1, 0.9, 0.1, 1)
    DrawText("阳", s.x, s.y, 20, 0.55, 0.35, 0.05, 1, true, true)
  end

  -- HUD：阳光计数 + 卡牌
  DrawRect(14, 14, 170, 42, 0.12, 0.14, 0.18, 0.85)
  DrawText("阳光: " .. tostring(sun), 26, 26, 22, 1, 0.9, 0.1, 1, false, false)
  for i, name in ipairs(CARD_ORDER) do
    local def = PLANTS[name]
    local x = 14 + (i - 1) * 128
    if selected == name then
      DrawRect(x, 660, 120, 52, 0.25, 0.45, 0.30, 1)
    else
      DrawRect(x, 660, 120, 52, 0.14, 0.20, 0.26, 1)
    end
    DrawRectOutline(x, 660, 120, 52, selected == name and 3 or 1,
                    selected == name and 1 or 0.5,
                    selected == name and 0.9 or 0.5,
                    selected == name and 0.1 or 0.5, 1)
    DrawText(PlantNameOf(name), x + 8, 668, 18, 1, 1, 1, 1, false, false)
    DrawText(tostring(def.cost), x + 112, 668, 18, 1, 0.9, 0.1, 1, true, false)
    local cd = cooldowns[name]
    if cd > 0 then
      local h = 52 * math.min(1, cd / def.cd)
      DrawRect(x, 660, 120, h, 0, 0, 0, 0.55)
    end
  end
  DrawText("点击卡牌选择，点击格子种植；点击阳光收集", 14, 718, 14, 0.6, 0.6, 0.6, 1,
           false, false)

  -- 胜负
  if state == "won" then
    DrawRect(440, 280, 400, 110, 0.05, 0.25, 0.10, 0.92)
    DrawText("胜利！", 640, 335, 46, 1, 0.9, 0.1, 1, true, true)
  elseif state == "lost" then
    DrawRect(440, 280, 400, 110, 0.35, 0.08, 0.06, 0.92)
    DrawText("失败！", 640, 335, 46, 1, 0.2, 0.2, 1, true, true)
  end
end

-- 卡牌显示名（中文字形需要收集进播放器字体）
local PLANT_NAMES = {
  sunflower = "向日葵", peashooter = "豌豆射手", wallnut = "坚果",
}
function PlantNameOf(name)
  return PLANT_NAMES[name] or name
end
