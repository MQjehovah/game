-- NeonPvZ HUD overlay (Lua 后端).
-- 顶栏(阳光/卡片/铲子/波次)由声明式文档 ui/hud.ui.json 负责(BoxFlex 布局),
-- game.lua 通过 UISetText/UISetFill/UISetColor 更新动态值。
-- 本脚本只画世界锚定与全屏动态层: 僵尸血条 / 飘字 / 幽灵预览 / 波次横幅 /
-- 胜负与暂停覆盖。坐标全部是视口像素; 世界换算只走 WorldToScreen /
-- ScreenToWorld, 不再做任何 720-y 之类的手动设计空间翻转。

local ROWS, COLS = 5, 9
local X0, Y0, CELL_X, CELL_Y = 140, 110, 100, 100

local PLANTS = {
  { type = "sunflower",  name = "向日葵",   cost = 50,  icon = "sunflower.png" },
  { type = "peashooter", name = "豌豆射手", cost = 100, icon = "peashooter.png" },
  { type = "snowpea",    name = "寒冰射手", cost = 175, icon = "snowpea.png" },
  { type = "wallnut",    name = "坚果墙",   cost = 50,  icon = "wallnut.png" },
  { type = "repeater",   name = "双发射手", cost = 200, icon = "peashooter.png" },
  { type = "cherrybomb", name = "樱桃炸弹", cost = 150, icon = "cherry.png" },
}

local function clamp(v, lo, hi) return math.max(lo, math.min(hi, v)) end
local function rowY(row) return Y0 + row * CELL_Y end
local function colX(col) return X0 + col * CELL_X end

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

-- 行内指定格是否已有植物 (世界坐标)。
local function rowHasPlantAt(row, x)
  local list = GetVar("row_plants_" .. row)
  if type(list) ~= "table" then return false end
  for i = 1, #list do
    if math.abs(list[i].x - x) < 45 then return true end
  end
  return false
end

-- 鼠标(视口像素)所在格子 (col,row); 不在草地返回 nil。
local function hoverCell()
  local m = InputMousePos()
  if not m then return nil end
  local w = ScreenToWorld(m)
  if not w then return nil end
  local col = math.floor((w.x - X0 + CELL_X * 0.5) / CELL_X)
  local row = math.floor((w.y - Y0 + CELL_Y * 0.5) / CELL_Y)
  if col >= 0 and col < COLS and row >= 0 and row < ROWS then return col, row end
  return nil
end

-- 格子的两个屏幕角点(像素): a=左上(世界 -50), b=右下(世界 +50)。
local function cellCorners(col, row)
  local cx, cy = colX(col), rowY(row)
  local a = WorldToScreen({ x = cx - 50, y = cy - 50, z = 0 })
  local b = WorldToScreen({ x = cx + 50, y = cy + 50, z = 0 })
  if not (a and a.x and b and b.x) then return nil end
  return a, b
end

function on_render()
  local selected = GetVar("selected")
  local wave = GetVar("wave")
  if type(wave) ~= "number" then wave = 0 end
  local gameover = GetVar("gameover") == true
  local won = GetVar("won") == true
  local started = GetVar("started") == true
  local paused = GetVar("paused") == true
  local vp = GetViewportSize()
  local vw, vh = (vp and vp.w) or 1280, (vp and vp.h) or 720
  local vcx = vw * 0.5

  -- ============ 僵尸血条 (世界锚定, 相机投影) ============
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
              local s = WorldToScreen(pos)
              if s and s.x then
                local bw = 44 * (vh / 720.0)
                local sy = s.y - 46 * (vh / 720.0)
                local barCol = frac > 0.5 and { 0.3, 0.9, 0.35 }
                               or (frac > 0.25 and { 0.95, 0.8, 0.3 } or { 1, 0.3, 0.3 })
                DrawRect(s.x - bw * 0.5, sy, bw, 5, 0.08, 0.08, 0.08, 0.9)
                DrawRect(s.x - bw * 0.5, sy, bw * frac, 5, barCol[1], barCol[2], barCol[3], 1)
              end
            end
          end
        end
      end
    end
  end

  -- ============ 飘字 (世界锚定, 相机投影, 上浮淡出) ============
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

  -- ============ 幽灵预览 / 格子高亮 (世界锚定) ============
  if started and not gameover and not paused and selected then
    local col, row = hoverCell()
    if col then
      local a, b = cellCorners(col, row)
      if a then
        local x0, y0 = math.min(a.x, b.x), math.min(a.y, b.y)
        local w0, h0 = math.abs(b.x - a.x), math.abs(b.y - a.y)
        if selected == "shovel" then
          if rowHasPlantAt(row, colX(col)) then
            DrawRectOutline(x0, y0, w0, h0, 2, 1, 0.75, 0.4, 1)
          else
            DrawRectOutline(x0, y0, w0, h0, 1, 0.6, 0.6, 0.6, 0.4)
          end
        else
          local p = plantInfo(selected)
          if p then
            local sun2 = GetVar("sun")
            if type(sun2) ~= "number" then sun2 = 0 end
            local cd2 = GetVar("cooldown_" .. p.type)
            if type(cd2) ~= "number" then cd2 = 0 end
            local ok = sun2 >= (p.cost or 0) and cd2 <= 0.01 and
                       not rowHasPlantAt(row, colX(col))
            if ok then
              DrawRectOutline(x0, y0, w0, h0, 1, 0.4, 1, 0.5, 0.9)
              DrawRect(x0, y0, w0, h0, 0.2, 0.8, 0.3, 0.12)
              DrawSprite("assets/sprites/" .. p.icon,
                         x0 + w0 * 0.5 - w0 * 0.3, y0 + h0 * 0.5 - h0 * 0.36,
                         w0 * 0.6, h0 * 0.72, 1, 1, 1, 0.65)
            else
              DrawRectOutline(x0, y0, w0, h0, 1, 1, 0.35, 0.35, 0.9)
              DrawRect(x0, y0, w0, h0, 0.8, 0.2, 0.2, 0.10)
            end
          end
        end
      end
    end
  end

  -- 选中提示 (底部居中)。
  if started and not gameover and not paused and selected then
    DrawText(selected == "shovel" and "点击格子铲除 (右键/ESC 取消)" or "点击格子种植 (右键/ESC 取消)",
             vcx, vh - 55, 15, 0.9, 0.95, 1, 0.9, true, true)
  end

  -- 波次来袭横幅 (2 秒, 缩放淡出)。
  local banner = GetVar("wave_banner")
  if started and not gameover and not paused and type(banner) == "number" and banner > 0 then
    local t = clamp(2.0 - banner, 0, 1)
    local scale = 1 + 0.06 * (1 - t)
    local alpha = clamp(1 - t * 0.8, 0, 1)
    local bw, bh = 600 * (vw / 1280.0), 90 * (vh / 720.0)
    DrawRect(vcx - bw * 0.5, vh * 0.28, bw, bh, 0.10, 0.16, 0.26, 0.5 * alpha)
    DrawRectOutline(vcx - bw * 0.5, vh * 0.28, bw, bh, 2, 0.35, 0.65, 1, 0.6 * alpha)
    DrawText("第 " .. tostring(wave) .. " 波来袭!", vcx, vh * 0.28 + 18 * scale, 42 * scale,
             1, 0.9, 0.4, alpha, true, true)
  end

  -- ============ 胜负 / 暂停 (视口居中自适应) ============
  if gameover and won then
    local pw, ph = 620 * (vw / 1280.0), 180 * (vh / 720.0)
    DrawRect(vcx - pw * 0.5, vh * 0.38, pw, ph, 0.10, 0.40, 0.16, 0.94)
    DrawRect(vcx - pw * 0.5, vh * 0.38, pw, 6, 0.5, 1, 0.6, 1)
    DrawRectOutline(vcx - pw * 0.5, vh * 0.38, pw, ph, 2, 0.5, 1, 0.6, 0.8)
    DrawText(Loc("win"), vcx, vh * 0.42, 46, 0.9, 1, 0.7, 1, true, true)
    DrawText("所有僵尸已被消灭!", vcx, vh * 0.51, 18, 0.85, 0.95, 1, 0.95, true, true)
    DrawText(Loc("restart_hint"), vcx, vh * 0.58, 16, 0.8, 0.9, 1, 0.9, true, true)
  elseif gameover then
    local pw, ph = 620 * (vw / 1280.0), 160 * (vh / 720.0)
    DrawRect(vcx - pw * 0.5, vh * 0.39, pw, ph, 0.35, 0.08, 0.08, 0.94)
    DrawRect(vcx - pw * 0.5, vh * 0.39, pw, 6, 1, 0.4, 0.4, 1)
    DrawRectOutline(vcx - pw * 0.5, vh * 0.39, pw, ph, 2, 1, 0.4, 0.4, 0.8)
    DrawText(Loc("gameover"), vcx, vh * 0.43, 42, 1, 0.55, 0.55, 1, true, true)
    DrawText("僵尸攻破了防线...", vcx, vh * 0.51, 17, 0.9, 0.85, 0.85, 0.95, true, true)
    DrawText(Loc("restart_hint"), vcx, vh * 0.56, 16, 0.9, 0.9, 1, 0.95, true, true)
  end
  if paused and not gameover then
    DrawRect(0, 0, vw, vh, 0, 0, 0, 0.45)
    local pw, ph = 300 * (vw / 1280.0), 120 * (vh / 720.0)
    DrawRect(vcx - pw * 0.5, vh * 0.42, pw, ph, 0.12, 0.16, 0.22, 0.95)
    DrawRectOutline(vcx - pw * 0.5, vh * 0.42, pw, ph, 2, 0.35, 0.65, 1, 0.8)
    DrawText("暂停中", vcx, vh * 0.46, 36, 1, 1, 1, 1, true, true)
    DrawText("按 P 继续 · R 重开", vcx, vh * 0.53, 16, 0.85, 0.9, 1, 0.9, true, true)
  end
  if not started and not gameover then
    DrawText(Loc("select"), vcx, vh - 60, 18, 0.85, 0.9, 1, 0.85, true, true)
  end
end
