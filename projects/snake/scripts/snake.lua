-- NeonSnake: 经典贪吃蛇
-- 数据驱动 2D 游戏:玩法与绘制全在 Lua 脚本(scenes/snake.json 的 script 组件挂载)。
-- 设计坐标 1280x720;方向键 / WASD 控制,Enter 重开,P 暂停。
-- 场景 vars 里 demo=1 时自动演示(无头验证用)。

local COLS, ROWS = 26, 18
local CELL = 36
local OX, OY = 172, 36
local BASE_INTERVAL = 0.13

local DEMO = (type(demo) == "number" and demo == 1) or false

local snake = {}      -- 蛇身 {x,y},下标 1 = 头
local dir = { x = 1, y = 0 }
local queue = {}      -- 转向队列
local food = { x = -1, y = -1 }
local score = 0
local level = 1
local interval = BASE_INTERVAL
local acc = 0
local state = "ready" -- ready | playing | paused | dead | won
local deathCause = ""
local demoTimer = 0
local clock = 0
local hungerTimer = 0

local function in_bounds(x, y) return x >= 1 and x <= COLS and y >= 1 and y <= ROWS end

local function cell_occupied(x, y, ignoreTail)
  local n = #snake
  local limit = n
  if ignoreTail and n > 1 then limit = n - 1 end
  for i = 1, limit do
    if snake[i].x == x and snake[i].y == y then return true end
  end
  return false
end

local function spawn_food()
  local empty = {}
  for y = 1, ROWS do
    for x = 1, COLS do
      if not cell_occupied(x, y) then empty[#empty + 1] = { x = x, y = y } end
    end
  end
  if #empty == 0 then return false end
  local f = empty[math.random(1, #empty)]
  food.x, food.y = f.x, f.y
  return true
end

local function reset()
  snake = {}
  local cy = math.floor(ROWS / 2)
  for i = 0, 3 do snake[#snake + 1] = { x = 4 - i, y = cy } end
  dir = { x = 1, y = 0 }
  queue = {}
  score = 0
  level = 1
  interval = BASE_INTERVAL
  acc = 0
  state = "ready"
  deathCause = ""
  spawn_food()
end

local function push_dir(d)
  -- 禁止 180 度掉头;队列最多 2 步
  local last = dir
  if #queue > 0 then last = queue[#queue] end
  if last.x == -d.x and last.y == -d.y then return end
  if last.x == d.x and last.y == d.y then return end
  if #queue >= 2 then return end
  queue[#queue + 1] = d
end

local function apply_dir()
  if #queue > 0 then
    dir = table.remove(queue, 1)
  end
end

local function step()
  apply_dir()
  local nx, ny = snake[1].x + dir.x, snake[1].y + dir.y
  local tail = snake[#snake]
  local growing = (nx == food.x and ny == food.y)
  if not in_bounds(nx, ny) then
    state, deathCause = "dead", "撞墙了"
    return
  end
  if cell_occupied(nx, ny, growing) then
    state, deathCause = "dead", "咬到自己了"
    return
  end
  table.insert(snake, 1, { x = nx, y = ny })
  if growing then
    score = score + 10
    hungerTimer = 0
    local newLevel = math.floor(score / 50) + 1
    if newLevel ~= level then
      level = newLevel
      interval = math.max(0.06, BASE_INTERVAL - (level - 1) * 0.012)
    end
    if not spawn_food() then
      state = "won"
      return
    end
  else
    snake[#snake] = nil
  end
end

-- 演示 AI:朝食物走,并用"落点可达空格数"评估,避免追尾绕圈和死路
local function reachable_count(nx, ny, simulateTail)
  local seen = {}
  local queue = { { x = nx, y = ny } }
  seen[nx .. "_" .. ny] = true
  local head = 1
  local count = 0
  while head <= #queue do
    local c = queue[head]
    head = head + 1
    count = count + 1
    for _, d in ipairs({ { 1, 0 }, { -1, 0 }, { 0, 1 }, { 0, -1 } }) do
      local qx, qy = c.x + d[1], c.y + d[2]
      local key = qx .. "_" .. qy
      if in_bounds(qx, qy) and not seen[key] and not cell_occupied(qx, qy, true) then
        seen[key] = true
        queue[#queue + 1] = { x = qx, y = qy }
      end
    end
  end
  return count
end

local function demo_dir()
  local hx, hy = snake[1].x, snake[1].y
  -- 四个方向都作为候选,允许绕路脱困
  local candidates = {
    { x = 1, y = 0 }, { x = -1, y = 0 }, { x = 0, y = 1 }, { x = 0, y = -1 },
  }
  local best, bestScore = nil, -1
  for _, c in ipairs(candidates) do
    if not (c.x == -dir.x and c.y == -dir.y) then
      local nx, ny = hx + c.x, hy + c.y
      if in_bounds(nx, ny) and not cell_occupied(nx, ny, true) then
        local reach = reachable_count(nx, ny, true)
        local s = reach * 3 - (math.abs(nx - food.x) + math.abs(ny - food.y)) * 3
        -- 落点可活动空间不足时大幅降权(迟早撞死)
        if reach < #snake * 2 then s = s - 500 end
        if s > bestScore then
          bestScore = s
          best = c
        end
      end
    end
  end
  return best
end

local function show_menu()
  UISetVisible("StartMenu", true)
  UISetVisible("Hud", false)
end

local function start_game()
  UISetVisible("StartMenu", false)
  UISetVisible("Hud", true)
  state = "playing"
end

function on_start(e)
  if os and os.time then math.randomseed(os.time()) else math.randomseed(0) end
  reset()
  UIShow("ui/main.ui.json")
  show_menu()
end

function on_update(e, dt)
  clock = clock + dt
  if state == "ready" then
    if UIClicked("Start") > 0 or InputKey("space") > 0 or InputKey("enter") > 0 then
      start_game()
    elseif DEMO and demoTimer <= 0 then
      start_game()
    end
    if DEMO then demoTimer = demoTimer - dt end
    return
  end
  if state == "paused" then
    if InputKey("p") > 0 then state = "playing" end
    return
  end
  if state == "dead" or state == "won" then
    if InputKey("enter") > 0 or InputKey("space") > 0 then
      reset()
      show_menu()
    end
    if DEMO then
      demoTimer = demoTimer - dt
      if demoTimer <= 0 then
        reset()
        show_menu()
      end
    end
    return
  end

  -- HUD 刷新(游戏中)
  UISetText("ScoreLabel", "得分: " .. tostring(score))
  local prog = (score % 50) / 50
  UISetFill("FoodBar", prog)

  -- 输入
  if InputKey("p") > 0 then state = "paused" return end
  if InputKey("left") > 0 or InputKey("a") > 0 then push_dir({ x = -1, y = 0 }) end
  if InputKey("right") > 0 or InputKey("d") > 0 then push_dir({ x = 1, y = 0 }) end
  if InputKey("up") > 0 or InputKey("w") > 0 then push_dir({ x = 0, y = -1 }) end
  if InputKey("down") > 0 or InputKey("s") > 0 then push_dir({ x = 0, y = 1 }) end
  if DEMO then
    hungerTimer = hungerTimer + dt
    -- 长时间吃不到食物就重开,演示始终保持活跃
    if hungerTimer > 12 then
      reset()
      return
    end
    local d = demo_dir()
    if d then push_dir(d) end
  end

  acc = acc + dt
  while acc >= interval do
    acc = acc - interval
    step()
    if state == "dead" or state == "won" then
      if DEMO then demoTimer = 1.0 end
      break
    end
  end
end

local function cell_rect(x, y)
  return OX + (x - 1) * CELL, OY + (y - 1) * CELL
end

function on_render()
  DrawRect(0, 0, 1280, 720, 0.05, 0.07, 0.10, 1)

  -- 棋盘
  DrawRect(OX - 6, OY - 6, COLS * CELL + 12, ROWS * CELL + 12, 0.14, 0.18, 0.24, 1)
  DrawRect(OX, OY, COLS * CELL, ROWS * CELL, 0.07, 0.10, 0.14, 1)
  for y = 1, ROWS do
    for x = 1, COLS do
      if ((x + y) % 2) == 0 then
        DrawRect(OX + (x - 1) * CELL, OY + (y - 1) * CELL, CELL, CELL, 0.09, 0.13, 0.18, 1)
      end
    end
  end

  -- 食物(脉冲)
  if food.x > 0 then
    local fx, fy = cell_rect(food.x, food.y)
    local pulse = 0.75 + 0.25 * math.sin(4 * clock)
    DrawRect(fx + 6, fy + 6, CELL - 12, CELL - 12, 0.95, 0.25, 0.2, pulse)
    DrawRect(fx + 11, fy + 11, CELL - 22, CELL - 22, 1, 0.6, 0.35, 1)
  end

  -- 蛇
  for i = #snake, 1, -1 do
    local s = snake[i]
    local x, y = cell_rect(s.x, s.y)
    local t = i / #snake
    if i == 1 then
      DrawRect(x + 1, y + 1, CELL - 2, CELL - 2, 0.15, 0.85, 0.35, 1)
      DrawRect(x + 6, y + 6, CELL - 12, CELL - 12, 0.4, 1, 0.55, 1)
    else
      DrawRect(x + 2, y + 2, CELL - 4, CELL - 4, 0.12 + 0.25 * t, 0.55 + 0.2 * t,
               0.18 + 0.12 * t, 1)
    end
  end

  -- HUD
  DrawRect(14, 14, 300, 46, 0.10, 0.13, 0.18, 0.9)
  DrawText("得分: " .. tostring(score), 28, 24, 22, 1, 0.95, 0.25, 1, false, false)
  DrawText("长度: " .. tostring(#snake) .. "   等级: " .. tostring(level),
           190, 28, 18, 0.8, 0.85, 0.9, 1, false, false)

  if state == "ready" then
    DrawRect(440, 290, 400, 130, 0.10, 0.16, 0.22, 0.94)
    DrawText("贪吃蛇", 640, 318, 42, 0.3, 0.95, 0.45, 1, true, true)
    DrawText(DEMO and "自动演示中…" or "按 空格 或 Enter 开始", 640, 372, 18,
             1, 1, 1, 1, true, true)
  elseif state == "paused" then
    DrawRect(500, 300, 280, 90, 0.10, 0.12, 0.16, 0.94)
    DrawText("已暂停 — 按 P 继续", 640, 345, 26, 1, 1, 1, 1, true, true)
  elseif state == "dead" then
    DrawRect(440, 290, 400, 130, 0.32, 0.08, 0.06, 0.94)
    DrawText("游戏结束 — " .. deathCause, 640, 330, 30, 1, 0.35, 0.25, 1, true, true)
    DrawText(DEMO and "演示自动重开…" or "按 Enter 重新开始", 640, 375, 18, 1, 1, 1, 1, true, true)
  elseif state == "won" then
    DrawRect(440, 290, 400, 130, 0.06, 0.28, 0.12, 0.94)
    DrawText("胜利!棋盘已占满", 640, 335, 34, 0.5, 1, 0.6, 1, true, true)
    DrawText("按 Enter 再来一局", 640, 380, 18, 1, 1, 1, 1, true, true)
  end
end
