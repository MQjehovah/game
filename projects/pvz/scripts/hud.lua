-- NeonPvZ HUD (Lua 后端, 由 JS 版改写以兼容默认构建 -- QuickJS 无法用 MSVC 编译).
-- 美化版: 顶部状态条 / 种子卡(主题饰条+快捷键角标+价格+冷却扫屏+选中高亮+不足置灰) /
-- 铲子 / 波次进度条+剩余僵尸 / 悬停格幽灵预览(可放置绿/不可红) / 胜负暂停画面。
-- 读 game.lua 写入的 GameVar, 前后端解耦。

local ROWS, COLS = 5, 9
local X0, Y0, CELL_X, CELL_Y = 140, 110, 100, 100

local PLANTS = {
  { type = "sunflower",  name = "向日葵",   hotkey = "1", cost = 50,  maxCd = 4,  icon = "sunflower.png" },
  { type = "peashooter", name = "豌豆射手", hotkey = "2", cost = 100, maxCd = 4,  icon = "peashooter.png" },
  { type = "snowpea",    name = "寒冰射手", hotkey = "3", cost = 175, maxCd = 4,  icon = "snowpea.png" },
  { type = "wallnut",    name = "坚果墙",   hotkey = "4", cost = 50,  maxCd = 10, icon = "wallnut.png" },
  { type = "repeater",   name = "双发射手", hotkey = "5", cost = 200, maxCd = 6,  icon = "peashooter.png" },
  { type = "cherrybomb", name = "樱桃炸弹", hotkey = "6", cost = 150, maxCd = 30, icon = "cherry.png" },
}
-- 卡片主题色 (顶部饰条)。
local ACCENT = {
  sunflower = { 1.0, 0.95, 0.30 },
  peashooter = { 0.35, 0.90, 0.35 },
  snowpea = { 0.35, 0.80, 1.00 },
  wallnut = { 0.85, 0.60, 0.35 },
  repeater = { 0.95, 0.60, 0.20 },
  cherrybomb = { 1.00, 0.35, 0.30 },
}

local function clamp(v, lo, hi) return math.max(lo, math.min(hi, v)) end

local function rowY(row) return Y0 + row * CELL_Y end
local function colX(col) return X0 + col * CELL_X end
-- 设计坐标(屏幕 y 向下)下某格中心 y: 世界 y 向上 -> 屏幕 y = 720 - 世界y。
local function cellScreenY(row) return 720 - rowY(row) end

local function plantInfo(type)
  for i = 1, #PLANTS do
    if PLANTS[i].type == type then return PLANTS[i] end
  end
  return nil
end

-- 剩余僵尸数 (跨行求和)。
local function zombiesLeft()
  local n = 0
  for r = 0, ROWS - 1 do
    local list = GetVar("row_zombies_" .. r)
    if type(list) == "table" then n = n + #list end
  end
  return n
end

-- 行内指定格是否已有植物。
local function rowHasPlantAt(row, x)
  local list = GetVar("row_plants_" .. row)
  if type(list) ~= "table" then return false end
  for i = 1, #list do
    if math.abs(list[i].x - x) < 45 then return true end
  end
  return false
end

-- 鼠标所在格子 (col,row), 不在草地返回 nil。
local function hoverCell()
  local m = InputMousePos()
  if not m then return nil end
  local wy = 720 - m.y
  local col = math.floor((m.x - X0 + CELL_X * 0.5) / CELL_X)
  local row = math.floor((wy - Y0 + CELL_Y * 0.5) / CELL_Y)
  if col >= 0 and col < COLS and row >= 0 and row < ROWS then return col, row end
  return nil
end

function on_render()
  local sun = GetVar("sun")
  if type(sun) ~= "number" then sun = 0 end
  local selected = GetVar("selected")
  local wave = GetVar("wave")
  if type(wave) ~= "number" then wave = 0 end
  local gameover = GetVar("gameover") == true
  local won = GetVar("won") == true
  local started = GetVar("started") == true
  local paused = GetVar("paused") == true

  -- ============ 顶部状态条 ============
  DrawRect(0, 0, 1280, 58, 0.07, 0.09, 0.13, 0.96)
  DrawRect(0, 0, 1280, 14, 0.12, 0.16, 0.22, 1) -- 上缘高光
  DrawRect(0, 56, 1280, 2, 0.35, 0.65, 1, 1)    -- 下缘饰线

  -- 阳光: 图标 + 数值 (带底板)。
  DrawRect(10, 8, 104, 42, 0.10, 0.13, 0.18, 0.9)
  DrawRectOutline(10, 8, 104, 42, 1, 0.35, 0.65, 1, 0.6)
  DrawSprite("assets/sprites/sun.png", 16, 12, 34, 34)
  DrawText(tostring(sun), 60, 20, 24, 1, 0.95, 0.3, 1)

  -- ============ 种子卡 ============
  local bx = 240
  for i = 1, #PLANTS do
    local p = PLANTS[i]
    local active = selected == p.type
    local afford = sun >= p.cost
    local cd = GetVar("cooldown_" .. p.type)
    if type(cd) ~= "number" then cd = 0 end
    local ac = ACCENT[p.type] or { 0.5, 0.5, 0.6 }

    -- 卡片底
    DrawRect(bx, 6, 130, 44, 0.14, 0.20, 0.28, 0.95)
    -- 顶部饰条 (按植物主题色)
    DrawRect(bx, 6, 130, 4, ac[1], ac[2], ac[3], active and 1 or 0.55)
    if active then
      DrawRectOutline(bx - 1, 5, 132, 46, 2, 1, 1, 0.5, 1)
      DrawRect(bx, 6, 130, 44, 0.30, 0.45, 0.60, 0.35) -- 选中泛光
    end
    -- 图标 (不足够钱置灰)
    if afford then
      DrawSprite("assets/sprites/" .. p.icon, bx + 8, 8, 34, 34)
    else
      DrawSprite("assets/sprites/" .. p.icon, bx + 8, 8, 34, 34, 0.45, 0.45, 0.45, 0.9)
    end
    -- 名称
    DrawText(p.name, bx + 46, 10, 14, 0.92, 0.95, 1, 1)
    -- 快捷键角标
    DrawRect(bx + 1, 7, 14, 14, 0.05, 0.08, 0.12, 0.9)
    DrawText(p.hotkey, bx + 8, 8, 11, 1, 1, 0.6, 1, true, true)
    -- 价格 (不够钱变红)
    if afford then
      DrawText(tostring(p.cost), bx + 44, 34, 13, 1, 0.95, 0.3, 1)
    else
      DrawText(tostring(p.cost), bx + 44, 34, 13, 1, 0.45, 0.25, 0.3, 1)
    end
    -- 冷却扫屏 + 倒计时
    if cd > 0.01 then
      local frac = clamp(cd / p.maxCd, 0, 1)
      DrawRect(bx, 6, 130, 44 * frac, 0.05, 0.06, 0.10, 0.82)
      DrawText(string.format("%.1fs", cd), bx + 65, 30, 12, 1, 0.75, 0.4, 1, true, true)
    end
    bx = bx + 136
  end

  -- 铲子 (简单图形: 柄 + 铲头)。
  local shovelActive = selected == "shovel"
  DrawRect(bx + 2, 6, 60, 44, 0.16, 0.14, 0.12, 0.95)
  if shovelActive then
    DrawRectOutline(bx + 1, 5, 62, 46, 2, 1, 0.8, 0.4, 1)
    DrawRect(bx + 2, 6, 60, 44, 0.35, 0.30, 0.20, 0.4)
  end
  DrawRect(bx + 22, 12, 3, 16, 0.7, 0.55, 0.3, 1)   -- 柄
  DrawRect(bx + 12, 27, 24, 8, 0.72, 0.58, 0.34, 1) -- 铲头
  DrawRect(bx + 14, 33, 3, 8, 0.5, 0.4, 0.22, 1)    -- 铲头斜面
  DrawText("铲子", bx + 36, 12, 12, 0.9, 0.9, 0.85, 1)
  DrawText("S", bx + 52, 34, 11, 0.6, 0.6, 0.6, 1)

  -- ============ 波次进度 + 剩余僵尸 ============
  DrawText(string.format(Loc("wave"), wave), 1002, 6, 20, 0.8, 0.85, 1, 1)
  local prog = GetVar("wave_progress")
  if type(prog) ~= "number" then prog = 0 end
  DrawRect(980, 34, 212, 10, 0.10, 0.13, 0.18, 0.9)
  DrawRect(980, 34, 212 * clamp(prog, 0, 1), 10, 0.35, 0.65, 1, 0.9)
  DrawText("僵尸: " .. tostring(zombiesLeft()), 1002, 44, 13, 0.95, 0.7, 0.7, 1)

  -- ============ 僵尸血条 (仅受伤时显示, 头顶小条) ============
  local ZMAX = { basic = 100, cone = 140, bucket = 200 }
  for r = 0, ROWS - 1 do
    local zlist = GetVar("row_zombies_" .. r)
    if type(zlist) == "table" then
      for i = 1, #zlist do
        local zent = { id = zlist[i].id, gen = zlist[i].gen }
        local hp = GetHealth(zent)
        if hp and hp > 0 then
          local pos = GetPosition(zent)
          if pos then
            local zc = EntityComponent(zent, "zombie")
            local maxHp = (zc and ZMAX[zc.type]) or 100
            local frac = clamp(hp / maxHp, 0, 1)
            if frac < 0.99 then
              -- 世界锚定: 用相机投影(WorldToScreen)转设计坐标, 与相机解耦。
              local s = WorldToScreen(pos)
              if s and s.x then
                local sx, sy = s.x, s.y - 46
                local barCol = frac > 0.5 and { 0.3, 0.9, 0.35 }
                               or (frac > 0.25 and { 0.95, 0.8, 0.3 } or { 1, 0.3, 0.3 })
                DrawRect(sx - 22, sy, 44, 5, 0.08, 0.08, 0.08, 0.9)
                DrawRect(sx - 22, sy, 44 * frac, 5, barCol[1], barCol[2], barCol[3], 1)
              end
            end
          end
        end
      end
    end
  end

  -- ============ 飘字 (SpawnFloatText: 世界锚定, 相机投影, 上浮淡出) ============
  local ft = FloatTexts()
  if type(ft) == "table" then
    for i = 1, #ft do
      local f = ft[i]
      local w = f.world
      if w then
        local s = WorldToScreen({ x = w.x, y = w.y, z = w.z })
        if s and s.x then
          local alpha = clamp(1 - (f.age or 0) / (f.life or 1), 0, 1)
          local sy = s.y - 20 - (f.age or 0) * 26
          DrawText(tostring(f.text), s.x, sy, f.crit and 24 or 18, 1, 0.95, 0.35, alpha,
                   true, true)
        end
      end
    end
  end

  -- ============ 幽灵预览 / 格子高亮 ============
  if started and not gameover and not paused and selected then
    local col, row = hoverCell()
    if col then
      local cx, cy = colX(col), cellScreenY(row)
      if selected == "shovel" then
        if rowHasPlantAt(row, colX(col)) then
          DrawRectOutline(cx - 50, cy - 50, 100, 100, 2, 1, 0.75, 0.4, 1)
        else
          DrawRectOutline(cx - 50, cy - 50, 100, 100, 1, 0.6, 0.6, 0.6, 0.4)
        end
      else
        local p = plantInfo(selected)
        if p then
          local sun2 = GetVar("sun")
          if type(sun2) ~= "number" then sun2 = 0 end
          local cd2 = GetVar("cooldown_" .. p.type)
          if type(cd2) ~= "number" then cd2 = 0 end
          local ok = sun2 >= p.cost and cd2 <= 0.01 and not rowHasPlantAt(row, colX(col))
          if ok then
            DrawRectOutline(cx - 50, cy - 50, 100, 100, 1, 0.4, 1, 0.5, 0.9)
            DrawRect(cx - 50, cy - 50, 100, 100, 0.2, 0.8, 0.3, 0.12)
            DrawSprite("assets/sprites/" .. p.icon, cx - 30, cy - 34, 60, 68,
                       1, 1, 1, 0.65)
          else
            DrawRectOutline(cx - 50, cy - 50, 100, 100, 1, 1, 0.35, 0.35, 0.9)
            DrawRect(cx - 50, cy - 50, 100, 100, 0.8, 0.2, 0.2, 0.10)
          end
        end
      end
    end
  end

  -- 选中提示 (底部)。
  if started and not gameover and not paused and selected then
    DrawText(selected == "shovel" and "点击格子铲除 (右键/ESC 取消)" or "点击格子种植 (右键/ESC 取消)",
             640, 665, 15, 0.9, 0.95, 1, 0.9, true, true)
  end

  -- 波次来袭横幅 (2 秒, 缩放淡出)。
  local banner = GetVar("wave_banner")
  if started and not gameover and not paused and type(banner) == "number" and banner > 0 then
    local t = clamp(2.0 - banner, 0, 1)      -- 0->1 已过时间
    local scale = 1 + 0.06 * (1 - t)
    local alpha = clamp(1 - t * 0.8, 0, 1)
    DrawRect(340, 200, 600, 90, 0.10, 0.16, 0.26, 0.5 * alpha)
    DrawRectOutline(340, 200, 600, 90, 2, 0.35, 0.65, 1, 0.6 * alpha)
    DrawText("第 " .. tostring(wave) .. " 波来袭!", 640, 218, 42 * scale, 1, 0.9, 0.4, alpha,
             true, true)
  end

  -- ============ 胜负 / 暂停 ============
  if gameover and won then
    DrawRect(340, 270, 620, 180, 0.10, 0.40, 0.16, 0.94)
    DrawRect(340, 270, 620, 6, 0.5, 1, 0.6, 1)
    DrawRectOutline(340, 270, 620, 180, 2, 0.5, 1, 0.6, 0.8)
    DrawText(Loc("win"), 640, 302, 46, 0.9, 1, 0.7, 1, true, true)
    DrawText("所有僵尸已被消灭!", 640, 364, 18, 0.85, 0.95, 1, 0.95, true, true)
    DrawText(Loc("restart_hint"), 640, 418, 16, 0.8, 0.9, 1, 0.9, true, true)
  elseif gameover then
    DrawRect(340, 280, 620, 160, 0.35, 0.08, 0.08, 0.94)
    DrawRect(340, 280, 620, 6, 1, 0.4, 0.4, 1)
    DrawRectOutline(340, 280, 620, 160, 2, 1, 0.4, 0.4, 0.8)
    DrawText(Loc("gameover"), 640, 315, 42, 1, 0.55, 0.55, 1, true, true)
    DrawText("僵尸攻破了防线...", 640, 368, 17, 0.9, 0.85, 0.85, 0.95, true, true)
    DrawText(Loc("restart_hint"), 640, 405, 16, 0.9, 0.9, 1, 0.95, true, true)
  end
  if paused and not gameover then
    DrawRect(0, 0, 1280, 720, 0, 0, 0, 0.45)
    DrawRect(490, 300, 300, 120, 0.12, 0.16, 0.22, 0.95)
    DrawRectOutline(490, 300, 300, 120, 2, 0.35, 0.65, 1, 0.8)
    DrawText("暂停中", 640, 330, 36, 1, 1, 1, 1, true, true)
    DrawText("按 P 继续 · R 重开", 640, 382, 16, 0.85, 0.9, 1, 0.9, true, true)
  end
  if not started and not gameover then
    DrawText(Loc("select"), 640, 660, 18, 0.85, 0.9, 1, 0.85, true, true)
  end
end
