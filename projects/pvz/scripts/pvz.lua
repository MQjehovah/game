-- NeonPvZ: complete Plants-vs-Zombies clone, ONE content pipeline.
-- Scene sprite entities ARE the game content: the editor renders them and the
-- runtime renders the SAME entities during playtest. The script drives logic
-- through entity handles (FindNamedEntity / SpawnSprite / SetPosition /
-- SetVisible / Despawn); on_render only draws UI overlays (HUD/cards/fx).
local ROWS, COLS = 5, 9
local CELL = 100
local BX, BY = 190, 160
local SPAWN_X, LOSE_X = 1150, 185
local SPR = "assets/sprites/"

local PLANTS = {
  sunflower  = { cost = 50,  hp = 80,  cd = 5,   timer = 8,   sun = 25, dmg = 0,
                 slow = false, sprite = "sunflower.png", w = 60, h = 68 },
  peashooter = { cost = 100, hp = 80,  cd = 5,   timer = 1.5, sun = 0,  dmg = 20,
                 slow = false, sprite = "peashooter.png", w = 60, h = 68 },
  snowpea    = { cost = 175, hp = 80,  cd = 7.5, timer = 1.5, sun = 0,  dmg = 20,
                 slow = true, sprite = "snowpea.png", w = 60, h = 68 },
  wallnut    = { cost = 50,  hp = 400, cd = 10,  timer = 0,   sun = 0,  dmg = 0,
                 slow = false, sprite = "wallnut.png", w = 60, h = 68 },
  cherry     = { cost = 150, hp = 80,  cd = 30,  timer = 0,   sun = 0,  dmg = 0,
                 slow = false, sprite = "cherry.png", w = 60, h = 68,
                 fuse = 1.5, blast = 200, blastRadius = 1.5 },
}
local CARD_ORDER = { "sunflower", "peashooter", "snowpea", "wallnut", "cherry" }
local PLANT_NAMES = {
  sunflower = "\u{5411}\u{65E5}\u{8475}",
  peashooter = "\u{8C4C}\u{8C46}\u{5C04}\u{624B}",
  snowpea = "\u{5BD2}\u{51B0}\u{5C04}\u{624B}",
  wallnut = "\u{575A}\u{679C}\u{5899}",
  cherry = "\u{6A31}\u{6843}\u{70B8}\u{5F39}",
}

local ZOMBIES = {
  basic  = { hp = 100, speed = 30, eat = 12, sprite = "zombie.png" },
  cone   = { hp = 250, speed = 30, eat = 12, sprite = "cone.png" },
  bucket = { hp = 650, speed = 30, eat = 12, sprite = "bucket.png" },
}

-- Procedural waves on top of the scene's scripted spawns.
local WAVES = {
  { t = 14, n = 1, tough = 0.0 },
  { t = 26, n = 2, tough = 0.2 },
  { t = 40, n = 2, tough = 0.4 },
  { t = 54, n = 3, tough = 0.5 },
  { t = 70, n = 3, tough = 0.75 },
  { t = 88, n = 4, tough = 0.85 },
}

local board = {}
local zombies = {}
local peas = {}
local suns = {}
local spawns = {}
local mowers = {}
local fx = {}
local sun = 150
local selected = "sunflower"
local cooldowns = {}
local elapsed = 0
local skyTimer = 6
local state = "ready"
local spawnCursor = 1
local waveIndex = 1
local banner = 0
local bigWave = false
local message = ""
local messageTimer = 0
local readyTimer = 5.0
local prevP = false
local winTime = 0
local nextId = 1

for _, name in ipairs(CARD_ORDER) do cooldowns[name] = 0 end

local function cell_center(r, c)
  return BX + (c - 0.5) * CELL, BY + (r - 0.5) * CELL
end

local function row_y(r)
  return BY + (r - 0.5) * CELL
end

local function vec3(x, y, z)
  return { x = x, y = y, z = z or 0 }
end

local function load_level()
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
            local ent = FindNamedEntity(e.name or "")
            board[row][col] = { plant = pname, hp = PLANTS[pname].hp, t = 0, ent = ent }
          end
        end
        local z = comps.zombie
        if z ~= nil then
          local row = (z.row or 0) + 1
          if row >= 1 and row <= ROWS then
            local zt = z.type or "basic"
            if ZOMBIES[zt] == nil then zt = "basic" end
            local ent = FindNamedEntity(e.name or "")
            if ent ~= nil then SetVisible(ent, false) end -- appears at its delay
            table.insert(spawns, { row = row, delay = z.delay or 10, type = zt, ent = ent })
          end
        end
      end
    end
  end
  table.sort(spawns, function(a, b) return a.delay < b.delay end)
end

local function pick_zombie_type(tough)
  local r = math.random()
  if r < tough * 0.6 then return "bucket" end
  if r < tough * 0.6 + 0.3 then return "cone" end
  return "basic"
end

local function has_zombie_in_row(row, rightOf)
  for _, z in ipairs(zombies) do
    if z.row == row and z.x > rightOf then return true end
  end
  return false
end

local function spawn_zombie(row, ztype, ent)
  local def = ZOMBIES[ztype or "basic"]
  local y = row_y(row)
  if ent == nil then
    ent = SpawnSprite(SPR .. def.sprite, vec3(SPAWN_X, y, -0.5), 56, 72, false, false)
  else
    SetVisible(ent, true)
    SetPosition(ent, vec3(SPAWN_X, y, -0.5))
  end
  table.insert(zombies, { row = row, x = SPAWN_X, hp = def.hp, maxHp = def.hp,
                          speed = def.speed, baseSpeed = def.speed,
                          slowTimer = 0, type = ztype or "basic", ent = ent })
end

local function push_fx(kind, x, y, str, life)
  table.insert(fx, { kind = kind, x = x, y = y, life = life or 0.6, str = str })
end

local function say(msg)
  message = msg
  messageTimer = 2.2
end

function on_start(e)
  for r = 1, ROWS do
    board[r] = {}
    local y = row_y(r)
    mowers[r] = { x = 165, used = false, active = false,
                  ent = SpawnSprite(SPR .. "mower.png", vec3(165, y, -1), 40, 52,
                                    false, false) }
  end
  load_level()
  state = "ready"
  readyTimer = 5.0
end

function on_update(e, dt)
  if state == "ready" then
    readyTimer = readyTimer - dt
    local start = InputKey("enter") > 0 or InputMousePressed("left") or readyTimer <= 0
    if start then
      state = "playing"
      elapsed = 0
    end
    return
  end
  if state == "paused" then
    if InputKey("p") > 0 and not prevP then state = "playing" end
    prevP = InputKey("p") > 0
    return
  end
  if state == "won" or state == "lost" then
    if InputKey("enter") > 0 then ChangeScene("scenes/pvz.json") end
    return
  end

  elapsed = elapsed + dt
  for k, v in pairs(cooldowns) do cooldowns[k] = math.max(0, v - dt) end

  if InputKey("p") > 0 and not prevP then
    state = "paused"
    prevP = true
    return
  end
  prevP = InputKey("p") > 0

  local nextWave = WAVES[waveIndex]
  if nextWave and banner <= 0 and elapsed >= nextWave.t - 2.0 then
    banner = 2.8
    bigWave = nextWave.tough >= 0.7
    PlaySfx("wave")
  end
  if banner > 0 then banner = banner - dt end
  if messageTimer > 0 then messageTimer = messageTimer - dt end

  -- Sky sun.
  skyTimer = skyTimer - dt
  if skyTimer <= 0 then
    skyTimer = 10
    local col = (math.floor(elapsed) % COLS) + 1
    local x = BX + (col - 0.5) * CELL
    table.insert(suns, { x = x, y = 40, vy = 40, life = 12, value = 25,
                         ent = SpawnSprite(SPR .. "sun.png", vec3(x, 40, -1), 48, 48,
                                           false, false) })
  end

  -- Suns.
  local i = 1
  while i <= #suns do
    local s = suns[i]
    s.y = s.y + s.vy * dt
    if s.vy > 0 and s.y > BY + 40 then s.vy = 0 end
    s.life = s.life - dt
    SetPosition(s.ent, vec3(s.x, s.y, -1))
    if s.life <= 0 then
      Despawn(s.ent)
      table.remove(suns, i)
    else
      i = i + 1
    end
  end

  -- Plants.
  for r = 1, ROWS do
    for c = 1, COLS do
      local cell = board[r][c]
      if cell then
        local def = PLANTS[cell.plant]
        cell.t = cell.t + dt
        if cell.plant == "sunflower" and cell.t >= def.timer then
          cell.t = 0
          local cx, cy = cell_center(r, c)
          table.insert(suns, { x = cx, y = cy, vy = -12, life = 10, value = 25,
                               ent = SpawnSprite(SPR .. "sun.png", vec3(cx, cy, -1),
                                                 48, 48, false, false) })
        elseif cell.plant == "cherry" and cell.t >= def.fuse then
          local cx, cy = cell_center(r, c)
          push_fx("boom", cx, cy, nil, 0.55)
          PlaySfx("boom")
          local i2 = 1
          while i2 <= #zombies do
            local z = zombies[i2]
            local dx = z.x - cx
            local dy = row_y(z.row) - cy
            if math.abs(dx) <= def.blastRadius * CELL
                and math.abs(dy) <= def.blastRadius * CELL then
              z.hp = z.hp - def.blast
            end
            i2 = i2 + 1
          end
          if cell.ent ~= nil then Despawn(cell.ent) end
          board[r][c] = nil
        elseif (cell.plant == "peashooter" or cell.plant == "snowpea")
            and cell.t >= def.timer then
          local px = BX + (c - 1) * CELL
          if has_zombie_in_row(r, px) then
            cell.t = 0
            local y = row_y(r)
            local tex = cell.plant == "snowpea" and "snow_pea.png" or "pea.png"
            table.insert(peas, { row = r, x = px + CELL * 0.5, dmg = def.dmg,
                                 slow = def.slow,
                                 ent = SpawnSprite(SPR .. tex, vec3(px + CELL * 0.5, y, -1),
                                                   18, 18, false, false) })
            PlaySfx("shoot")
          end
        end
      end
    end
  end

  -- Peas.
  i = 1
  while i <= #peas do
    local p = peas[i]
    p.x = p.x + 300 * dt
    SetPosition(p.ent, vec3(p.x, row_y(p.row), -1))
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
    if hit or p.x > SPAWN_X + 60 then
      Despawn(p.ent)
      table.remove(peas, i)
    else
      i = i + 1
    end
  end
  i = 1
  while i <= #zombies do
    if zombies[i].hp <= 0 then
      local z = zombies[i]
      if z.ent ~= nil then Despawn(z.ent) end
      table.remove(zombies, i)
    else
      i = i + 1
    end
  end

  -- Zombies: move / eat / slow recovery.
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
          if cell.hp <= 0 then
            if cell.ent ~= nil then Despawn(cell.ent) end
            board[z.row][c] = nil
            PlaySfx("eat")
          end
          eating = true
          break
        end
      end
    end
    if not eating then
      z.x = z.x - z.speed * dt
      SetPosition(z.ent, vec3(z.x, row_y(z.row), -0.5))
      local m = mowers[z.row]
      if z.x < LOSE_X then
        if m ~= nil and not m.used then
          m.used = true
          m.active = true
          PlaySfx("mower")
        else
          state = "lost"
          winTime = elapsed
          PlaySfx("lose")
        end
      end
    end
  end

  -- Mowers.
  for r = 1, ROWS do
    local m = mowers[r]
    if m ~= nil and m.active then
      m.x = m.x + 260 * dt
      SetPosition(m.ent, vec3(m.x, row_y(r), -1))
      local i2 = 1
      while i2 <= #zombies do
        local z = zombies[i2]
        if z.row == r and z.x < m.x + 40 then
          if z.ent ~= nil then Despawn(z.ent) end
          table.remove(zombies, i2)
        else
          i2 = i2 + 1
        end
      end
      if m.x > SPAWN_X + 40 then
        m.active = false
        SetVisible(m.ent, false)
      end
    end
  end

  -- Scene spawns.
  while spawnCursor <= #spawns and spawns[spawnCursor].delay <= elapsed do
    local s = spawns[spawnCursor]
    spawn_zombie(s.row, s.type, s.ent)
    spawnCursor = spawnCursor + 1
  end

  -- Procedural waves.
  if waveIndex <= #WAVES and elapsed >= WAVES[waveIndex].t then
    local w = WAVES[waveIndex]
    local row = (math.floor(elapsed * 7) % ROWS) + 1
    for k = 1, w.n do
      spawn_zombie(((row + k - 1) % ROWS) + 1, pick_zombie_type(w.tough), nil)
    end
    waveIndex = waveIndex + 1
  end

  -- Win.
  if spawnCursor > #spawns and waveIndex > #WAVES and #zombies == 0
      and #peas == 0 then
    state = "won"
    winTime = elapsed
    PlaySfx("win")
  end

  -- Input.
  if InputMousePressed("left") then
    local pos = InputMousePos()
    local cardHit = false
    for ci, name in ipairs(CARD_ORDER) do
      local cx = 14 + (ci - 1) * 132
      if pos.x >= cx and pos.x <= cx + 124 and pos.y >= 660 and pos.y <= 712 then
        selected = name
        cardHit = true
        PlaySfx("click")
        break
      end
    end
    if not cardHit then
      local got = false
      for si = 1, #suns do
        local s = suns[si]
        local dx, dy = s.x - pos.x, s.y - pos.y
        if dx * dx + dy * dy <= 22 * 22 * 2.25 then
          sun = sun + s.value
          push_fx("text", s.x, s.y - 8, "+" .. tostring(s.value), 0.8)
          Despawn(s.ent)
          table.remove(suns, si)
          got = true
          PlaySfx("sun")
          break
        end
      end
      if not got then
        local col = math.floor((pos.x - BX) / CELL) + 1
        local row = math.floor((pos.y - BY) / CELL) + 1
        if row >= 1 and row <= ROWS and col >= 1 and col <= COLS then
          local def = PLANTS[selected]
          if def then
            if board[row][col] ~= nil then
              say("\u{683C}\u{5B50}\u{5DF2}\u{88AB}\u{5360}\u{7528}")
            elseif cooldowns[selected] > 0 then
              say("\u{8BE5}\u{690D}\u{7269}\u{51B7}\u{5374}\u{4E2D}")
            elseif sun < def.cost then
              say("\u{9633}\u{5149}\u{4E0D}\u{8DB3}!")
            else
              sun = sun - def.cost
              cooldowns[selected] = def.cd
              local cx, cy = cell_center(row, col)
              local ent = SpawnSprite(SPR .. def.sprite, vec3(cx, cy, 0),
                                      def.w, def.h, false, false)
              board[row][col] = { plant = selected, hp = def.hp, t = 0, ent = ent }
              push_fx("text", cx, cy, "-" .. tostring(def.cost), 0.6)
              PlaySfx("plant")
            end
          end
        end
      end
    end
  end

  -- FX.
  i = 1
  while i <= #fx do
    fx[i].life = fx[i].life - dt
    if fx[i].kind == "text" then fx[i].y = fx[i].y - 36 * dt end
    if fx[i].life <= 0 then table.remove(fx, i) else i = i + 1 end
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

local function draw_cards()
  DrawRect(14, 14, 190, 42, 0.12, 0.14, 0.18, 0.85)
  DrawText("\u{9633}\u{5149}: " .. tostring(sun), 26, 26, 22, 1, 0.9, 0.1, 1, false, false)
  for i, name in ipairs(CARD_ORDER) do
    local def = PLANTS[name]
    local x = 14 + (i - 1) * 132
    local usable = sun >= def.cost and cooldowns[name] <= 0
    if selected == name then
      DrawRect(x, 660, 124, 52, 0.25, 0.45, 0.30, 1)
    else
      DrawRect(x, 660, 124, 52, 0.14, 0.20, 0.26, 1)
    end
    DrawRectOutline(x, 660, 124, 52, selected == name and 3 or 1,
                    selected == name and 1 or 0.5,
                    selected == name and 0.9 or 0.5,
                    selected == name and 0.1 or 0.5, 1)
    sprite(def.sprite, x + 6, 666, 38, 40)
    DrawText(PLANT_NAMES[name], x + 50, 668, 15, 1, 1, 1, 1, false, false)
    if usable then
      DrawText(tostring(def.cost), x + 116, 692, 16, 1, 0.9, 0.1, 1, true, false)
    else
      DrawText(tostring(def.cost), x + 116, 692, 16, 1, 0.35, 0.12, 0.1, 1, true, false)
    end
    local cd = cooldowns[name]
    if cd > 0 then
      DrawRect(x, 660, 124, 52 * math.min(1, cd / def.cd), 0, 0, 0, 0.55)
    elseif sun < def.cost then
      DrawRect(x, 660, 124, 52, 0, 0, 0, 0.30)
    end
  end
end

local function draw_hover()
  local pos = InputMousePos()
  local col = math.floor((pos.x - BX) / CELL) + 1
  local row = math.floor((pos.y - BY) / CELL) + 1
  if row < 1 or row > ROWS or col < 1 or col > COLS then return end
  local def = PLANTS[selected]
  if not def then return end
  local x, y = BX + (col - 1) * CELL, BY + (row - 1) * CELL
  local ok = board[row][col] == nil and cooldowns[selected] <= 0 and sun >= def.cost
  DrawRectOutline(x, y, CELL, CELL, 3, ok and 0.3 or 0.9, ok and 0.9 or 0.2,
                  ok and 0.3 or 0.2, 0.9)
  if ok then
    DrawRect(x + 8, y + 8, CELL - 16, CELL - 16, 0.3, 0.9, 0.3, 0.18)
  end
end

local function draw_fx()
  for _, f in ipairs(fx) do
    if f.kind == "boom" then
      local t = 1 - math.max(0, f.life) / 0.55
      local r = 24 + t * 84
      DrawRectOutline(f.x - r, f.y - r, r * 2, r * 2, 4, 1, 0.35, 0.2, 1 - t * 0.4)
      DrawRect(f.x - r * 0.5, f.y - r * 0.5, r, r, 1, 0.5, 0.2, 0.45 * (1 - t))
    elseif f.kind == "text" then
      DrawText(f.str, f.x, f.y, 20, 1, 1, 0.4, math.min(1, f.life * 2), true, true)
    end
  end
end

function on_render()
  if state == "ready" then
    DrawRect(0, 0, 1280, 720, 0, 0, 0, 0.55)
    DrawRect(400, 150, 480, 380, 0.10, 0.14, 0.20, 0.92)
    DrawText("\u{690D}\u{7269}\u{5927}\u{6218}\u{50F5}\u{5C38}", 640, 205, 52,
             1, 0.9, 0.2, 1, true, true)
    DrawText("NeonPvZ", 640, 262, 22, 0.7, 0.85, 1, 1, true, true)
    DrawRect(520, 300, 240, 62, 0.18, 0.55, 0.25, 1)
    DrawRectOutline(520, 300, 240, 62, 3, 0.9, 1, 0.5, 1)
    DrawText("\u{5F00}\u{59CB}\u{6E38}\u{620F}", 640, 331, 30, 1, 1, 1, 1, true, true)
    DrawText("\u{9632}\u{5FA1}\u{50F5}\u{5C38}\u{5165}\u{4FB5}\u{4F60}\u{7684}\u{623F}\u{5B50}",
             640, 392, 18, 1, 1, 1, 1, true, true)
    DrawText("\u{9009}\u{62E9}\u{5361}\u{724C}\u{79CD}\u{690D}\u{690D}\u{7269}\u{FF0C}\u{6536}\u{96C6}\u{9633}\u{5149}\u{FF0C}\u{7528}\u{8C4C}\u{8C46}\u{51FB}\u{8D25}\u{50F5}\u{5C38}\u{3002}",
             640, 420, 16, 0.8, 0.85, 0.9, 1, true, true)
    DrawText("\u{70B9}\u{51FB}\u{4EFB}\u{610F}\u{4F4D}\u{7F6E}\u{6216}\u{6309} Enter \u{5F00}\u{59CB}",
             640, 470, 16, 1, 0.9, 0.3, 1, true, true)
    DrawText("\u{81EA}\u{52A8}\u{5F00}\u{59CB}: " .. tostring(math.max(0, math.ceil(readyTimer))),
             640, 498, 16, 0.6, 0.6, 0.6, 1, true, true)
    return
  end

  -- House strip (decor left of the lawn; the lawn itself is a scene sprite).
  DrawRect(0, BY - 12, BX + 8, 500 + 24, 0.25, 0.18, 0.12, 1)
  DrawText("\u{5BB6}", BX * 0.5, 360, 34, 1, 1, 1, 1, true, true)

  -- Plant HP bars (entities render the sprites).
  for r = 1, ROWS do
    for c = 1, COLS do
      local cell = board[r][c]
      if cell then
        local def = PLANTS[cell.plant]
        local cx, cy = cell_center(r, c)
        if cell.plant == "cherry" then
          DrawText(tostring(math.max(0, math.ceil(def.fuse - cell.t))),
                   cx, cy - 40, 16, 1, 0.4, 0.3, 1, true, true)
        end
        if cell.hp < def.hp then
          hp_bar(cx - 28, BY + (r - 1) * CELL + CELL - 12, 56, cell.hp, def.hp)
        end
      end
    end
  end
  -- Zombie HP bars + slow marker.
  for _, z in ipairs(zombies) do
    local y = row_y(z.row)
    if z.hp < z.maxHp then
      hp_bar(z.x - 26, y - 48, 52, z.hp, z.maxHp)
    end
    if z.slowTimer > 0 then
      DrawText("\u{5BD2}\u{51B0}", z.x, y - 54, 16, 0.6, 0.85, 1, 1, true, true)
    end
  end

  draw_hover()
  draw_fx()
  draw_cards()

  if banner > 0 and state == "playing" then
    local flash = (math.floor(banner * 6) % 2) == 0
    if flash then
      local msg = bigWave and "\u{4E00}\u{5927}\u{6CE2}\u{50F5}\u{5C38}\u{6B63}\u{5728}\u{63A5}\u{8FD1}\u{FF01}"
                            or "\u{50F5}\u{5C38}\u{6B63}\u{5728}\u{63A5}\u{8FD1}\u{FF01}"
      DrawRect(360, 92, 560, 46, 0.05, 0.05, 0.08, 0.85)
      DrawText(msg, 640, 115, 26, 1, 0.95, 0.15, 1, true, true)
    end
  end

  DrawText("\u{70B9}\u{51FB}\u{5361}\u{724C}\u{9009}\u{62E9}\u{FF0C}\u{70B9}\u{51FB}\u{683C}\u{5B50}\u{79CD}\u{690D}\u{FF1B}\u{70B9}\u{51FB}\u{9633}\u{5149}\u{6536}\u{96C6}\u{FF1B}\u{6A31}\u{6843}\u{70B8}\u{5F39}\u{5EF6}\u{65F6}\u{7206}\u{70B8}\u{FF1B}P \u{6682}\u{505C}",
           14, 718, 14, 0.6, 0.6, 0.6, 1, false, false)
  if messageTimer > 0 and message ~= "" then
    DrawRect(440, 620, 400, 34, 0, 0, 0, 0.75)
    DrawText(message, 640, 637, 20, 1, 0.35, 0.3, 1, true, true)
  end

  if state == "paused" then
    DrawRect(0, 0, 1280, 720, 0, 0, 0, 0.55)
    DrawRect(460, 280, 360, 120, 0.10, 0.14, 0.20, 0.95)
    DrawText("\u{6682}\u{505C}\u{4E2D}", 640, 320, 40, 1, 1, 1, 1, true, true)
    DrawText("\u{6309} P \u{7EE7}\u{7EED}  |  \u{6309} Enter \u{91CD}\u{65B0}\u{5F00}\u{59CB}",
             640, 362, 18, 0.8, 0.85, 0.9, 1, true, true)
  end

  if state == "won" then
    DrawRect(440, 260, 400, 150, 0.05, 0.25, 0.10, 0.94)
    DrawText("\u{80DC}\u{5229}\u{FF01}", 640, 300, 46, 1, 0.9, 0.1, 1, true, true)
    local mins = math.floor(winTime / 60)
    local secs = math.floor(winTime % 60)
    DrawText("\u{7528}\u{65F6} " .. string.format("%d:%02d", mins, secs)
             .. "    \u{5269}\u{4F59}\u{9633}\u{5149} " .. tostring(sun),
             640, 352, 20, 1, 1, 1, 1, true, true)
    DrawText("\u{6309} Enter \u{91CD}\u{65B0}\u{5F00}\u{59CB}", 640, 386, 16,
             0.8, 0.9, 0.8, 1, true, true)
  elseif state == "lost" then
    DrawRect(440, 260, 400, 150, 0.35, 0.08, 0.06, 0.94)
    DrawText("\u{5931}\u{8D25}\u{FF01}", 640, 300, 46, 1, 0.3, 0.3, 1, true, true)
    DrawText("\u{50F5}\u{5C38}\u{5403}\u{6389}\u{4E86}\u{4F60}\u{7684}\u{8111}\u{5B50}\u{FF01}",
             640, 350, 22, 1, 0.6, 0.6, 0.7, 1, true, true)
    DrawText("\u{6309} Enter \u{91CD}\u{65B0}\u{5F00}\u{59CB}", 640, 384, 16,
             0.9, 0.8, 0.8, 1, true, true)
  end
end
