-- NeonPvZ: 植物大战僵尸（数据驱动原型）。
-- 玩法与绘制全部在脚本里：关卡 JSON 由编辑器 2D 模式摆放
-- （scenes/pvz.json 的 "level" 字段，与 3D 场景统一放在 scenes/），
-- 精灵图在 assets/sprites/。
--
-- 棋盘布局（设计坐标 1280x720，与编辑器 2D 模式一致）：
--   9 列 x 5 行，每格 100x100，原点 (190,160)；僵尸从右侧进入向左走。

local ROWS, COLS = 5, 9
local CELL = 100
local BX, BY = 190, 160
local SPAWN_X, LOSE_X = 1150, 185

local SPR = "assets/sprites/"

local PLANTS = {
  sunflower  = { cost = 50,  hp = 80,  cd = 5,   timer = 8,   sun = 25, dmg = 0,
                 slow = false, sprite = "sunflower.png", label = "葵" },
  peashooter = { cost = 100, hp = 80,  cd = 5,   timer = 1.5, sun = 0,  dmg = 20,
                 slow = false, sprite = "peashooter.png", label = "豆" },
  snowpea    = { cost = 175, hp = 80,  cd = 7.5, timer = 1.5, sun = 0,  dmg = 20,
                 slow = true, sprite = "snowpea.png", label = "冰" },
  wallnut    = { cost = 50,  hp = 400, cd = 10,  timer = 0,   sun = 0,  dmg = 0,
                 slow = false, sprite = "wallnut.png", label = "坚" },
  cherry     = { cost = 150, hp = 80,  cd = 30,  timer = 0,   sun = 0,  dmg = 0,
                 slow = false, sprite = "cherry.png", label = "樱",
                 fuse = 1.5, blast = 200, blastRadius = 1.5 },
}
local CARD_ORDER = { "sunflower", "peashooter", "snowpea", "wallnut", "cherry" }
local PLANT_NAMES = {
  sunflower = "向日葵", peashooter = "豌豆", snowpea = "寒冰",
  wallnut = "坚果", cherry = "樱桃炸弹",
}

local ZOMBIES = {
  basic  = { hp = 100, speed = 30, eat = 12, sprite = "zombie.png" },
  cone   = { hp = 250, speed = 30, eat = 12, sprite = "cone.png" },
  bucket = { hp = 650, speed = 30, eat = 12, sprite = "bucket.png" },
}

local board = {}   -- board[row][col] = { plant=name, hp=..., t=... } or nil
local zombies = {} -- { row, x, hp, maxHp, speed, baseSpeed, slowTimer }
local peas = {}    -- { row, x, dmg, slow }
local suns = {}    -- { x, y, vy, life, value }
local spawns = {}  -- { row, delay, type }
local mowers = {}  -- mowers[row] = { x, used, active }
local sun = 150
local selected = "sunflower"
local cooldowns = {}
local elapsed = 0
local skyTimer = 6
local state = "playing"
local spawnCursor = 1

for _, name in ipairs(CARD_ORDER) do cooldowns[name] = 0 end

local function cell_center(r, c)
  return BX + (c - 0.5) * CELL, BY + (r - 0.5) * CELL
end

local function load_level()
  -- The level is a SCENE (scenes/pvz.json): plants and zombies are scene
  -- entities carrying "plant"/"zombie" components. The runtime's ECS keeps
  -- the script entry entity; the board data is read from the same file the
  -- editor writes, so 2D and 3D projects share one scene pipeline.
  local text = ReadText("scenes/pvz.json")
  if text == nil or text == "" then return end
  local dom = Json.Parse(text)
  if dom == nil then return end
  if dom.entities ~= nil then
    for _, e in ipairs(dom.entities) do
      local comps = e.components
      if comps ~= nil then
        local p = comps.plant
        if p ~= nil then
          local row, col = (p.row or 0) + 1, (p.col or 0) + 1
          local pname = p.type or "sunflower"
          if PLANTS[pname] and row >= 1 and row <= ROWS and col >= 1 and col <= COLS
              and board[row] and board[row][col] == nil then
            board[row][col] = { plant = pname, hp = PLANTS[pname].hp, t = 0 }
          end
        end
        local z = comps.zombie
        if z ~= nil then
          local row = (z.row or 0) + 1
          if row >= 1 and row <= ROWS then
            local zt = z.type or "basic"
            if ZOMBIES[zt] == nil then zt = "basic" end
            table.insert(spawns, { row = row, delay = z.delay or 10, type = zt })
          end
        end
      end
    end
  end
  table.sort(spawns, function(a, b) return a.delay < b.delay end)
end

local function handle_click(x, y)
  -- 卡牌
  for i, name in ipairs(CARD_ORDER) do
    local cx = 14 + (i - 1) * 132
    if x >= cx and x <= cx + 124 and y >= 660 and y <= 712 then
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

local function has_zombie_in_row(row, rightOf)
  for _, z in ipairs(zombies) do
    if z.row == row and z.x > rightOf then return true end
  end
  return false
end

local function spawn_zombie(s)
  local def = ZOMBIES[s.type or "basic"]
  table.insert(zombies, { row = s.row, x = SPAWN_X, hp = def.hp, maxHp = def.hp,
                          speed = def.speed, baseSpeed = def.speed,
                          slowTimer = 0, type = s.type or "basic" })
end

function on_start(e)
  for r = 1, ROWS do
    board[r] = {}
    mowers[r] = { x = 165, used = false, active = false }
  end
  load_level()
end

function on_update(e, dt)
  elapsed = elapsed + dt
  for k, v in pairs(cooldowns) do cooldowns[k] = math.max(0, v - dt) end

  -- 天空阳光
  skyTimer = skyTimer - dt
  if skyTimer <= 0 then
    skyTimer = 10
    local col = (math.floor(elapsed) % COLS) + 1
    table.insert(suns, { x = BX + (col - 0.5) * CELL, y = 40, vy = 40,
                         life = 12, value = 25 })
  end

  -- 阳光
  local i = 1
  while i <= #suns do
    local s = suns[i]
    s.y = s.y + s.vy * dt
    if s.vy > 0 and s.y > BY + 40 then s.vy = 0 end
    s.life = s.life - dt
    if s.life <= 0 then table.remove(suns, i) else i = i + 1 end
  end

  -- 植物
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
        elseif cell.plant == "cherry" and cell.t >= def.fuse then
          -- 樱桃炸弹：3x3 范围爆炸
          local cx, cy = cell_center(r, c)
          local i2 = 1
          while i2 <= #zombies do
            local z = zombies[i2]
            local dx, dy = z.x - cx, (BY + (z.row - 0.5) * CELL) - cy
            if math.abs(dx) <= def.blastRadius * CELL
                and math.abs(dy) <= def.blastRadius * CELL then
              z.hp = z.hp - def.blast
            end
            i2 = i2 + 1
          end
          board[r][c] = nil
        elseif (cell.plant == "peashooter" or cell.plant == "snowpea")
            and cell.t >= def.timer then
          local px = BX + (c - 1) * CELL
          if has_zombie_in_row(r, px) then
            cell.t = 0
            table.insert(peas, { row = r, x = px + CELL * 0.5,
                                 dmg = def.dmg, slow = def.slow })
          end
        end
      end
    end
  end

  -- 豌豆
  i = 1
  while i <= #peas do
    local p = peas[i]
    p.x = p.x + 300 * dt
    local hit = false
    for _, z in ipairs(zombies) do
      if z.row == p.row and math.abs(z.x - p.x) < 28 then
        z.hp = z.hp - p.dmg
        if p.slow then
          z.slowTimer = 3.0
          z.speed = z.baseSpeed * 0.5
        end
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

  -- 僵尸移动/啃食/减速恢复
  for _, z in ipairs(zombies) do
    if z.slowTimer > 0 then
      z.slowTimer = z.slowTimer - dt
      if z.slowTimer <= 0 then z.speed = z.baseSpeed end
    end
    local eating = false
    for c = 1, COLS do
      local cell = board[z.row][c]
      if cell then
        local cellX = BX + (c - 1) * CELL
        if z.x <= cellX + CELL and z.x >= cellX - 20 then
          cell.hp = cell.hp - ZOMBIES[z.type].eat * dt
          if cell.hp <= 0 then board[z.row][c] = nil end
          eating = true
          break
        end
      end
    end
    if not eating then
      z.x = z.x - z.speed * dt
      -- 推草机：僵尸逼近房屋时触发，清空整行
      local m = mowers[z.row]
      if z.x < LOSE_X then
        if m ~= nil and not m.used then
          m.used = true
          m.active = true
        else
          state = "lost"
        end
      end
    end
  end

  -- 推草机推进
  for r = 1, ROWS do
    local m = mowers[r]
    if m ~= nil and m.active then
      m.x = m.x + 260 * dt
      local i2 = 1
      while i2 <= #zombies do
        local z = zombies[i2]
        if z.row == r and z.x < m.x + 40 then
          table.remove(zombies, i2)
        else
          i2 = i2 + 1
        end
      end
      if m.x > SPAWN_X + 40 then m.active = false end
    end
  end

  -- 刷怪
  while spawnCursor <= #spawns and spawns[spawnCursor].delay <= elapsed do
    spawn_zombie(spawns[spawnCursor])
    spawnCursor = spawnCursor + 1
  end

  -- 胜利：全部出怪且场上无僵尸/豌豆
  if state == "playing" and spawnCursor > #spawns
      and #zombies == 0 and #peas == 0 then
    state = "won"
  end

  -- 胜利/失败后按 Enter 重开本关（ChangeScene 数据驱动循环）
  if (state == "won" or state == "lost") and InputKey("enter") > 0 then
    ChangeScene("scenes/pvz.json")
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

local function sprite(name, x, y, w, h)
  DrawSprite(SPR .. name, x, y, w, h)
end

local function draw_zombie(z)
  local y = BY + (z.row - 0.5) * CELL
  local def = ZOMBIES[z.type or "basic"]
  sprite(def.sprite, z.x - 28, y - 36, 56, 72)
  if z.hp < z.maxHp then
    hp_bar(z.x - 26, y - 48, 52, z.hp, z.maxHp)
  end
  if z.slowTimer > 0 then
    DrawText("冰", z.x, y - 52, 16, 0.6, 0.85, 1, 1, true, true)
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

  -- 推草机（未触发时停在左侧）
  for r = 1, ROWS do
    local m = mowers[r]
    if m ~= nil and not m.used then
      local y = BY + (r - 0.5) * CELL
      sprite("mower.png", m.x - 20, y - 26, 40, 52)
    elseif m ~= nil and m.active then
      local y = BY + (r - 0.5) * CELL
      sprite("mower.png", m.x - 20, y - 26, 40, 52)
    end
  end

  -- 植物
  for r = 1, ROWS do
    for c = 1, COLS do
      local cell = board[r][c]
      if cell then
        local def = PLANTS[cell.plant]
        local cx, cy = cell_center(r, c)
        sprite(def.sprite, cx - 30, cy - 34, 60, 68)
        if cell.plant == "cherry" then
          -- 倒计时提示
          DrawText(tostring(math.max(0, math.ceil(def.fuse - cell.t))),
                   cx, cy - 40, 16, 1, 0.4, 0.3, 1, true, true)
        end
        if cell.hp < def.hp then
          hp_bar(cx - 28, BY + (r - 1) * CELL + CELL - 12, 56, cell.hp, def.hp)
        end
      end
    end
  end

  -- 豌豆
  for _, p in ipairs(peas) do
    local y = BY + (p.row - 0.5) * CELL
    if p.slow then
      sprite("snow_pea.png", p.x - 9, y - 9, 18, 18)
    else
      sprite("pea.png", p.x - 9, y - 9, 18, 18)
    end
  end

  -- 僵尸
  for _, z in ipairs(zombies) do draw_zombie(z) end

  -- 阳光
  for _, s in ipairs(suns) do
    sprite("sun.png", s.x - 24, s.y - 24, 48, 48)
  end

  -- HUD
  DrawRect(14, 14, 190, 42, 0.12, 0.14, 0.18, 0.85)
  DrawText("阳光: " .. tostring(sun), 26, 26, 22, 1, 0.9, 0.1, 1, false, false)
  for i, name in ipairs(CARD_ORDER) do
    local def = PLANTS[name]
    local x = 14 + (i - 1) * 132
    if selected == name then
      DrawRect(x, 660, 124, 52, 0.25, 0.45, 0.30, 1)
    else
      DrawRect(x, 660, 124, 52, 0.14, 0.20, 0.26, 1)
    end
    DrawRectOutline(x, 660, 124, 52, selected == name and 3 or 1,
                    selected == name and 1 or 0.5,
                    selected == name and 0.9 or 0.5,
                    selected == name and 0.1 or 0.5, 1)
    DrawText(PLANT_NAMES[name], x + 6, 666, 16, 1, 1, 1, 1, false, false)
    DrawText(tostring(def.cost), x + 116, 668, 18, 1, 0.9, 0.1, 1, true, false)
    local cd = cooldowns[name]
    if cd > 0 then
      DrawRect(x, 660, 124, 52 * math.min(1, cd / def.cd), 0, 0, 0, 0.55)
    end
  end
  DrawText("点击卡牌选择，点击格子种植；点击阳光收集；樱桃炸弹延时爆炸", 14, 718, 14,
           0.6, 0.6, 0.6, 1, false, false)

  -- 胜负
  if state == "won" then
    DrawRect(440, 280, 400, 110, 0.05, 0.25, 0.10, 0.92)
    DrawText("胜利！", 640, 335, 46, 1, 0.9, 0.1, 1, true, true)
    DrawText("按 Enter 重新开始", 640, 372, 18, 1, 1, 1, 1, true, true)
  elseif state == "lost" then
    DrawRect(440, 280, 400, 110, 0.35, 0.08, 0.06, 0.92)
    DrawText("失败！", 640, 335, 46, 1, 0.2, 0.2, 1, true, true)
    DrawText("按 Enter 重新开始", 640, 372, 18, 1, 1, 1, 1, true, true)
  end
end
