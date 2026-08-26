-- NeonPvZ HUD (Lua 后端, 由 JS 版改写以兼容默认构建 -- QuickJS 无法用 MSVC 编译).
-- 顶部: 阳光 / 植物卡片(冷却+选中+价格) / 铲子 / 波次 / 暂停 / 胜负画面。
-- 读 game.lua 写入的 GameVar, 前后端解耦。

local PLANTS = {
  { type = "sunflower",  label = "1 向日葵 50",  maxCd = 4 },
  { type = "peashooter", label = "2 豌豆 100",   maxCd = 4 },
  { type = "snowpea",    label = "3 寒冰 175",   maxCd = 4 },
  { type = "wallnut",    label = "4 坚果 50",    maxCd = 10 },
  { type = "repeater",   label = "5 双发 200",   maxCd = 6 },
  { type = "cherrybomb", label = "6 樱桃 150",   maxCd = 30 }
}

local function clamp(v, lo, hi) return math.max(lo, math.min(hi, v)) end

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

  DrawRect(0, 0, 1280, 58, 0.10, 0.12, 0.16, 0.92)
  DrawRect(0, 56, 1280, 2, 0.35, 0.65, 1, 1)

  DrawSprite("assets/sprites/sun.png", 14, 8, 40, 40, false, false)
  DrawText(tostring(sun), 66, 16, 26, 1, 0.95, 0.25, 1)

  local bx = 240
  for i = 1, #PLANTS do
    local p = PLANTS[i]
    local active = selected == p.type
    local r, g, b = 0.22, 0.40, 0.62
    if not active then r, g, b = 0.16, 0.22, 0.30 end
    DrawRect(bx, 6, 130, 44, r, g, b, 0.95)
    DrawText(p.label, bx + 6, 13, 15, 0.9, 0.92, 1, 1)
    local cd = GetVar("cooldown_" .. p.type)
    if type(cd) ~= "number" then cd = 0 end
    if cd > 0.01 then
      local frac = clamp(cd / p.maxCd, 0, 1)
      DrawRect(bx, 6, 130, 44 * frac, 0.05, 0.06, 0.10, 0.75)
      DrawText(tostring(math.ceil(cd)) .. "s", bx + 6, 6, 13, 1, 0.7, 0.4, 1)
    end
    bx = bx + 136
  end
  local shovelActive = selected == "shovel"
  DrawRect(bx + 2, 6, 60, 44, shovelActive and 0.3 or 0.18, 0.3, 0.3, 0.95)
  DrawText("铲子", bx + 8, 16, 13, 1, 0.9, 0.9, 1)

  DrawText(string.format(Loc("wave"), wave), 1010, 16, 20, 0.8, 0.85, 1, 1)

  if gameover and won then
    DrawRect(340, 280, 620, 170, 0.10, 0.35, 0.15, 0.92)
    DrawText(Loc("win"), 640, 320, 44, 0.8, 1, 0.7, 1, true, true)
    DrawText(Loc("restart_hint"), 640, 395, 16, 0.85, 0.95, 1, 0.95, true, false)
  elseif gameover then
    DrawRect(340, 280, 620, 150, 0.35, 0.1, 0.1, 0.9)
    DrawText(Loc("gameover"), 640, 315, 40, 1, 0.5, 0.5, 1, true, true)
    DrawText(Loc("restart_hint"), 640, 385, 16, 0.9, 0.9, 1, 0.95, true, false)
  end
  if paused and not gameover then
    DrawRect(0, 0, 1280, 720, 0, 0, 0, 0.4)
    DrawText("暂停中", 640, 360, 44, 1, 1, 1, 1, true, true)
  end
  if not started and not gameover then
    DrawText(Loc("select"), 640, 660, 18, 0.85, 0.9, 1, 0.85, true, true)
  end
end
