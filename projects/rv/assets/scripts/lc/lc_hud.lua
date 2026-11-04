-- lc_hud.lua
-- HUD：第一人称生存面板 / 交互提示 / 驾驶仪表 / 改装热栏 / 世界地图

-- 引擎的 DrawText 居中/右对齐按字形宽度测量，中文（UTF-8 多字节）会偏左/被裁。
-- 这里自己做 UTF-8 宽度估算，统一走左对齐绘制。
local function utf8Width(s, size)
  local w, i = 0, 1
  while i <= #s do
    local b = string.byte(s, i)
    local n = (b < 0x80) and 1 or (b < 0xE0) and 2 or (b < 0xF0) and 3 or 4
    local cp = b
    if n == 2 then
      cp = ((b & 0x1F) << 6) | (string.byte(s, i + 1) & 0x3F)
    elseif n == 3 then
      cp = ((b & 0x0F) << 12) | ((string.byte(s, i + 1) & 0x3F) << 6)
           | (string.byte(s, i + 2) & 0x3F)
    elseif n == 4 then
      cp = ((b & 0x07) << 18) | ((string.byte(s, i + 1) & 0x3F) << 12)
           | ((string.byte(s, i + 2) & 0x3F) << 6) | (string.byte(s, i + 3) & 0x3F)
    end
    w = w + ((cp >= 0x2E80) and size * 0.62 or size * 0.52)
    i = i + n
  end
  return w
end

function drawTextCentered(text, cx, y, size, r, g, b, a)
  DrawText(text, cx - utf8Width(text, size) * 0.5, y, size, r, g, b, a, false)
end

function drawTextRight(text, rx, y, size, r, g, b, a)
  DrawText(text, rx - utf8Width(text, size), y, size, r, g, b, a, false)
end

function hotbarHit(mx, my)
  local vp = GetViewportSize()
  local vw = (vp and vp.w) or 1280
  local vh = (vp and vp.h) or 720
  local slotW, slotH, gap = 96, 46, 6
  local cols = 7
  local rowsN = math.ceil(#CATALOG / cols)
  local barW = cols * slotW + (cols - 1) * gap
  local bx = math.floor(vw * 0.5 - barW * 0.5)
  local by = vh - rowsN * (slotH + gap) - 12
  if mx < bx or my < by then return nil end
  local col = math.floor((mx - bx) / (slotW + gap))
  local row = math.floor((my - by) / (slotH + gap))
  if col < 0 or col >= cols or row < 0 or row >= rowsN then return nil end
  local lx = (mx - bx) - col * (slotW + gap)
  local ly = (my - by) - row * (slotH + gap)
  if lx > slotW or ly > slotH then return nil end
  local idx = row * cols + col + 1
  if idx > #CATALOG then return nil end
  return idx
end

function drawStatBar(x, y, w, label, valueText, frac, warn)
  DrawText(label, x, y, 14, 0.92, 0.90, 0.84, 1)
  DrawText(valueText, x + w, y, 14, warn and 1 or 0.92, warn and 0.42 or 0.90,
           warn and 0.36 or 0.84, 1, true)
  local barW, barH = w, 5
  DrawRect(x, y + 18, barW, barH, 0.12, 0.12, 0.13, 0.9)
  local f = math.max(0, math.min(1, frac or 0))
  local br, bg, bb = 0.45, 0.78, 0.44
  if warn then br, bg, bb = 0.88, 0.28, 0.24 end
  DrawRect(x, y + 18, barW * f, barH, br, bg, bb, 1)
end

function drawBanner(text, y, size)
  local vp = GetViewportSize()
  local vw = (vp and vp.w) or 1280
  local bw = math.min(760, vw - 40)
  DrawRect(math.floor(vw * 0.5 - bw * 0.5), y - 6, bw, size + 14,
           0.03, 0.04, 0.05, 0.74)
  drawTextCentered(text, math.floor(vw * 0.5), y, size, 0.98, 0.94, 0.82, 1)
end

function drawNeedsPanel(x, y, w)
  local n = needs or {}
  DrawRect(x, y, w, 196, 0.06, 0.07, 0.08, 0.72)
  DrawRectOutline(x, y, w, 196, 0.46, 0.50, 0.42, 0.85)
  DrawText("状态", x + 10, y + 6, 14, 0.90, 0.84, 0.62, 1)
  local yy = y + 26
  local inner = w - 20
  drawStatBar(x + 10, yy, inner, "生命", string.format("%.0f", n.health or 0),
              (n.health or 0) / 100, (n.health or 0) < 30)
  yy = yy + 30
  drawStatBar(x + 10, yy, inner, "饥饿", string.format("%.0f", n.hunger or 0),
              (n.hunger or 0) / 100, (n.hunger or 0) < 25)
  yy = yy + 30
  drawStatBar(x + 10, yy, inner, "口渴", string.format("%.0f", n.thirst or 0),
              (n.thirst or 0) / 100, (n.thirst or 0) < 25)
  yy = yy + 30
  drawStatBar(x + 10, yy, inner, "体力", string.format("%.0f", n.energy or 0),
              (n.energy or 0) / 100, (n.energy or 0) < 20)
  yy = yy + 30
  drawStatBar(x + 10, yy, inner, "卫生", string.format("%.0f", n.hygiene or 0),
              (n.hygiene or 0) / 100, (n.hygiene or 0) < 20)
  if warmthStatusText ~= nil then
    local w = warmthStatusText()
    if w ~= nil then
      DrawText(w, x + 10, y + 180, 11, 0.98, 0.74, 0.42, 1)
    end
  end
  if restedStatusText ~= nil then
    local r = restedStatusText()
    if r ~= nil then
      DrawText(r, x + 10, y + 196, 11, 0.62, 0.86, 0.78, 1)
    end
  end
end

function drawCrosshair(vw, vh, threat)
  local cx, cy = math.floor(vw * 0.5), math.floor(vh * 0.5)
  local r, g, b = 0.92, 0.90, 0.84
  if threat then r, g, b = 1.0, 0.35, 0.25 end
  DrawRect(cx - 8, cy - 0.5, 5, 1, r, g, b, 0.85)
  DrawRect(cx + 3, cy - 0.5, 5, 1, r, g, b, 0.85)
  DrawRect(cx - 0.5, cy - 8, 1, 5, r, g, b, 0.85)
  DrawRect(cx - 0.5, cy + 3, 1, 5, r, g, b, 0.85)
  if hitMarkT > 0 then
    local a = hitMarkT / 0.12
    DrawRect(cx - 12, cy - 0.75, 6, 1.5, 1, 0.3, 0.25, a)
    DrawRect(cx + 6, cy - 0.75, 6, 1.5, 1, 0.3, 0.25, a)
    DrawRect(cx - 0.75, cy - 12, 1.5, 6, 1, 0.3, 0.25, a)
    DrawRect(cx - 0.75, cy + 6, 1.5, 6, 1, 0.3, 0.25, a)
  end
end

function drawRvStatus(x, y, w)
  DrawRect(x, y, w, 190, 0.06, 0.07, 0.08, 0.72)
  DrawRectOutline(x, y, w, 190, 0.46, 0.50, 0.42, 0.85)
  DrawText("房车", x + 10, y + 6, 14, 0.90, 0.84, 0.62, 1)
  local yy = y + 26
  local fLow = fuel / fuelMax() < 0.2
  DrawText(string.format("燃油  %.0f / %d L", fuel, fuelMax()), x + 10, yy, 13,
           fLow and 1 or 0.86, fLow and 0.4 or 0.84, fLow and 0.3 or 0.78, 1)
  DrawRect(x + 10, yy + 17, w - 20, 4, 0.12, 0.12, 0.13, 0.9)
  DrawRect(x + 10, yy + 17, (w - 20) * math.max(0, fuel / fuelMax()), 4,
           fLow and 0.9 or 0.55, fLow and 0.25 or 0.78, 0.25, 1)
  yy = yy + 28
  DrawText(string.format("车况  %.0f%%", dur), x + 10, yy, 13,
           dur < 40 and 1 or 0.86, dur < 40 and 0.4 or 0.84, dur < 40 and 0.3 or 0.78, 1)
  DrawRect(x + 10, yy + 17, w - 20, 4, 0.12, 0.12, 0.13, 0.9)
  DrawRect(x + 10, yy + 17, (w - 20) * math.max(0, dur / VEH.durMax), 4,
           dur < 40 and 0.9 or 0.45, dur < 40 and 0.25 or 0.78, 0.35, 1)
  yy = yy + 28
  DrawText(string.format("净水  %d / %d L", math.floor(water), waterMax()),
           x + 10, yy, 13, water < 15 and 1 or 0.86, water < 15 and 0.4 or 0.84,
           water < 15 and 0.3 or 0.78, 1)
  DrawRect(x + 10, yy + 17, w - 20, 4, 0.12, 0.12, 0.13, 0.9)
  DrawRect(x + 10, yy + 17, (w - 20) * math.max(0, water / waterMax()), 4,
           0.32, 0.62, 0.90, 1)
  yy = yy + 28
  local net = (stats.powerGen or 0) - (stats.powerDraw or 0)
  DrawText(string.format("电力  %+.2f kW", net), x + 10, yy, 13,
           net < 0 and 1 or 0.86, net < 0 and 0.4 or 0.84, net < 0 and 0.3 or 0.78, 1)
  yy = yy + 24
  DrawText(string.format("引擎 %.0f%%  轮胎 %.0f%%  电瓶 %.0f%%",
           (parts and parts.engine) or 100, (parts and parts.tires) or 100,
           (parts and parts.battery) or 100),
           x + 10, yy, 11, 0.74, 0.74, 0.70, 1)
  if rv.tireTemps ~= nil then
    local t = rv.tireTemps
    local hottest = math.max(t[1] or 22, t[2] or 22, t[3] or 22, t[4] or 22)
    local tr, tg, tb = 0.74, 0.74, 0.70
    if hottest > 110 then tr, tg, tb = 0.95, 0.45, 0.30
    elseif hottest > 85 then tr, tg, tb = 0.95, 0.78, 0.35 end
    DrawText(string.format("胎温 %.0f/%.0f/%.0f/%.0f°C",
             t[1] or 22, t[2] or 22, t[3] or 22, t[4] or 22),
             x + 10, yy + 17, 11, tr, tg, tb, 1)
  elseif rv.tireTemp ~= nil then
    local t = rv.tireTemp
    local tr, tg, tb = 0.74, 0.74, 0.70
    if t > 110 then tr, tg, tb = 0.95, 0.45, 0.30
    elseif t > 85 then tr, tg, tb = 0.95, 0.78, 0.35 end
    DrawText(string.format("胎温 %.0f°C", t), x + 10, yy + 17, 11, tr, tg, tb, 1)
  end
end

function drawInventoryStrip(x, y)
  local parts = {}
  for _, id in ipairs(ITEM_ORDER) do
    if (inv[id] or 0) > 0 then
      parts[#parts + 1] = itemName(id) .. "x" .. inv[id]
    end
  end
  if #parts == 0 then parts[1] = "背包空空" end
  drawTextCentered(table.concat(parts, "   "), x, y, 13, 0.86, 0.84, 0.76, 0.95)
end

function drawInteractionPrompt(vw, vh)
  if nearAction == nil then return end
  local y = vh - 196
  local enabled = nearAction.enabled ~= false
  local label = "[E] " .. nearAction.verb
  DrawRect(vw * 0.5 - 230, y - 8, 460, 46, 0.03, 0.04, 0.05, 0.84)
  DrawRectOutline(vw * 0.5 - 230, y - 8, 460, 46,
                  enabled and 0.85 or 0.45, enabled and 0.70 or 0.45, 0.38, 0.95)
  drawTextCentered(label, math.floor(vw * 0.5), y, 17,
                   enabled and 0.98 or 0.62, enabled and 0.92 or 0.62,
                   enabled and 0.70 or 0.60, 1)
  if nearAction.detail and nearAction.detail ~= "" then
    drawTextCentered(nearAction.detail, math.floor(vw * 0.5), y + 22, 13,
                     enabled and 0.84 or 0.62, enabled and 0.82 or 0.62,
                     enabled and 0.72 or 0.58, 1)
  end
  -- 世界空间小标记
  if (nearAction.kind == "station" or nearAction.kind == "fish"
      or nearAction.kind == "campfire") and nearAction.x then
    local sp = WorldToScreen(nearAction.x, 1.0, nearAction.z)
    if sp then
      local col = enabled and 0.92 or 0.45
      DrawCircle(sp.x, sp.y, 5, 1.5, col, col * 0.85, 0.4, 1, true)
      DrawText("E", sp.x, sp.y - 22, 14, col, col * 0.9, 0.5, 1, true)
    end
  end
end

function drawThreatBadge(vw)
  if #horde == 0 then return end
  drawTextRight(string.format("⚠ 附近感染者 %d", #horde),
                vw - 14, 210, 14, 1, 0.55, 0.4, 1)
  drawTextRight("左键 射击", vw - 14, 230, 13, 0.9, 0.7, 0.55, 1)
end

-- 首次进入：屏幕底部显示关键按键提示（12 秒后淡出）
function drawNewGameHint(vw, vh)
  local t = newGameHintT or 0
  if t <= 0 then return end
  local a = math.min(1, t)
  drawTextCentered("新旅程：P 操作说明 · J 旅行日志 · K 鱼类图鉴 · M 地图",
                   math.floor(vw * 0.5), 74, 13,
                   0.86, 0.90, 0.74, 0.9 * a)
end

-- 自动存档：右上角短暂闪一下“已自动保存”，不打断当前提示
function drawAutoSaveHint()
  local t = autoSaveFlashT or 0
  if t <= 0 then return end
  local vp = GetViewportSize()
  local vw = (vp and vp.w) or 1280
  local a = math.min(1, t)
  drawTextRight("已自动保存", vw - 14, 212, 11, 0.62, 0.80, 0.66, 0.85 * a)
end

-- 步行时房车方位角差（用于屏幕边缘箭头）
function rvBearingDelta(px, pz, viewYaw)
  local b = dirYaw(rv.x - (px or 0), rv.z - (pz or 0))
  local d = b - (viewYaw or 0)
  while d > math.pi do d = d - 2 * math.pi end
  while d < -math.pi do d = d + 2 * math.pi end
  return d
end

function drawFpHud()
  local vp = GetViewportSize()
  local vw = (vp and vp.w) or 1280
  local vh = (vp and vp.h) or 720


  drawNeedsPanel(10, vh - 186, 220)
  drawRvStatus(vw - 230, 10, 220)
  drawThreatBadge(vw)

  local loc = atNode and ("停靠 · " .. nodeName(atNode)) or (nearRv() and "房车旁" or "荒野")
  local wText = weatherName()
  if weatherHoursLeft ~= nil and weatherHoursLeft() > 0 then
    wText = wText .. string.format("（%.1fh）", weatherHoursLeft())
  end
  if weatherNextId ~= nil and WEATHER_NAMES ~= nil and
     WEATHER_NAMES[weatherNextId] ~= nil then
    wText = wText .. " → " .. WEATHER_NAMES[weatherNextId]
  end
  drawBanner(string.format("第 %d 天  %s  ·  %s  ·  %s%s", day, timeString(), loc,
             wText, isNight() and "  夜晚" or ""), 12, 16)

  drawCrosshair(vw, vh, #horde > 0)
  drawInteractionPrompt(vw, vh)
  drawInventoryStrip(math.floor(vw * 0.5), vh - 156)
  if job ~= nil then drawJobTracker(vw) end

  local hints = {
    "WASD 移动   Shift 疾跑   E 交互",
    "B 改装   T 驾驶   M 地图   P 说明",
    "J 旅行日志   K 鱼类图鉴",
    "1-0 快速使用物品   F5/F9 存/读",
  }
  DrawRect(vw - 292, 252, 282, 92, 0.03, 0.04, 0.05, 0.45)
  for i, h in ipairs(hints) do
    drawTextRight(h, vw - 14, 260 + (i - 1) * 19, 12, 0.86, 0.84, 0.78, 0.95)
  end

  if fishingT > 0 then
    local frac = 1 - fishingT / math.max(0.01, fishingTotal)
    local bx = math.floor(vw * 0.5 - 150)
    DrawRect(bx, vh - 250, 300, 42, 0.04, 0.05, 0.06, 0.8)
    DrawRectOutline(bx, vh - 250, 300, 42, 0.55, 0.72, 0.82, 0.9)
    drawTextCentered(string.format("钓鱼中… %.1f 秒", fishingT), math.floor(vw * 0.5),
                     vh - 244, 15, 0.85, 0.92, 0.95, 1)
    DrawRect(bx + 16, vh - 222, 268, 5, 0.12, 0.14, 0.16, 0.9)
    DrawRect(bx + 16, vh - 222, 268 * math.max(0, math.min(1, frac)), 5,
             0.35, 0.72, 0.85, 1)
  end

  if atNode then drawScavengeOverlay() end

  if toast.t > 0 then drawBanner(toast.text, 120, 16) end
  if hurtT > 0 then
    local a = math.min(0.35, hurtT * 0.5)
    DrawRect(0, 0, vw, 12, 0.8, 0.05, 0.02, a)
    DrawRect(0, vh - 12, vw, 12, 0.8, 0.05, 0.02, a)
    DrawRect(0, 0, 12, vh, 0.8, 0.05, 0.02, a)
    DrawRect(vw - 12, 0, 12, vh, 0.8, 0.05, 0.02, a)
  end

  -- 走远后：屏幕边缘箭头指向房车，避免在荒野迷路
  if onFoot and player ~= nil and player.active then
    local dx, dz = rv.x - player.x, rv.z - player.z
    local dist = math.sqrt(dx * dx + dz * dz)
    if dist > 12 then
      local delta = rvBearingDelta(player.x, player.z, player.yaw)
      local ax2, ay2 = -math.sin(delta), -math.cos(delta)
      local cx, cy = vw * 0.5, vh * 0.5
      local ex, ey = cx + ax2 * 190, cy + ay2 * 145
      DrawLine(ex - ax2 * 12 - ay2 * 9, ey - ay2 * 12 + ax2 * 9, ex, ey,
               3, 0.55, 0.82, 0.96, 0.92)
      DrawLine(ex - ax2 * 12 + ay2 * 9, ey - ay2 * 12 - ax2 * 9, ex, ey,
               3, 0.55, 0.82, 0.96, 0.92)
      drawTextCentered(string.format("房车 %.0fm", dist), ex, ey + 14, 12,
                       0.62, 0.86, 0.98, 0.95)
    end
  end

  -- 篝火燃料：低燃料/熄灭时在屏幕底部提示添柴
  if campfireHintText ~= nil then
    local fireHint = campfireHintText()
    if fireHint ~= nil then
      drawTextCentered(fireHint, math.floor(vw * 0.5), vh - 108, 12,
                       1.0, 0.72, 0.42, 0.95)
    end
  end
  if drawNewGameHint ~= nil then drawNewGameHint(vw, vh) end
end

-- 快速使用物品：1 水瓶 / 2 罐头 / 3 热饭 / 4 热咖啡 / 5 生食材
function handleQuickItem()
  local map = { ["1"] = "water", ["2"] = "canned", ["3"] = "meal",
                ["4"] = "brew", ["5"] = "rawfood", ["6"] = "coffee",
                ["7"] = "seed", ["8"] = "repairkit", ["9"] = "fish",
                ["0"] = "fishsoup" }
  for key, id in pairs(map) do
    if ActionPressed(key) then
      if id == "repairkit" then
        toast.text = "修理包要在工作台使用"
        toast.t = 2.0
      elseif id == "seed" then
        toast.text = "种子要在种植箱使用"
        toast.t = 2.0
      elseif id == "coffee" then
        toast.text = "咖啡豆要在咖啡机煮"
        toast.t = 2.0
      elseif hasItem(id, 1) then
        useItem(id)
      else
        toast.text = "没有 " .. itemName(id)
        toast.t = 1.6
      end
      return true
    end
  end
  return false
end

-- 第一人称自由摆放 HUD
function drawPlaceHud()
  local vp = GetViewportSize()
  local vw = (vp and vp.w) or 1280
  local vh = (vp and vp.h) or 720
  local cat = CATALOG[selected]

  drawBanner("改装模式 · 自由摆放（第一人称）  ·  " .. weatherName(), 12, 16)

  local px, py, pw, ph = 10, 44, 300, 154
  DrawRect(px, py, pw, ph, 0.05, 0.06, 0.07, 0.78)
  DrawRectOutline(px, py, pw, ph, 0.55, 0.52, 0.42, 0.9)
  DrawText("正在摆放：" .. cat.name, px + 12, py + 8, 16, 0.96, 0.90, 0.72, 1)
  DrawText(cat.desc or "", px + 12, py + 30, 12, 0.78, 0.76, 0.70, 1)
  DrawText(string.format("尺寸 %d×%d 格   重量 %d kg", cat.w, cat.h, cat.weight or 0),
           px + 12, py + 52, 12, 0.78, 0.76, 0.70, 1)
  DrawText(string.format("旋转 %d°   层：%s", math.floor(placeRot * 180 / math.pi),
           curLayer == "roof" and "车顶" or "地板"),
           px + 12, py + 74, 13, 0.86, 0.84, 0.76, 1)
  DrawText("材料 " .. costText(cat.cost or {}), px + 12, py + 96, 12, 0.82, 0.80, 0.72, 1)
  DrawText(placeValid and "✓ 可以放置" or "✗ 位置冲突 / 越界 / 材料不足",
           px + 12, py + 120, 13,
           placeValid and 0.55 or 1, placeValid and 0.92 or 0.35,
           placeValid and 0.55 or 0.28, 1)

  drawCrosshair(vw, vh, false)
  drawTextCentered("[左键] 放置    [右键/X] 拆除    [R] 旋转 15°    [滚轮/1-0] 换家具    [Tab] 地板/车顶    [B/E] 退出",
                   math.floor(vw * 0.5), vh - 196, 13, 0.92, 0.88, 0.74, 1)
  if hover and hover.inst then
    drawTextCentered("准星： " .. hover.inst.cat.name .. "（右键拆除）",
                     math.floor(vw * 0.5), vh - 176, 13, 1, 0.65, 0.45, 1)
  end

  -- 底部家具选择栏（与热栏同布局）
  local slotW, slotH, gap = 96, 46, 6
  local cols = 7
  local rowsN = math.ceil(#CATALOG / cols)
  local barW = cols * slotW + (cols - 1) * gap
  local bx = math.floor(vw * 0.5 - barW * 0.5)
  local by = vh - rowsN * (slotH + gap) - 12
  for i, c in ipairs(CATALOG) do
    local row = math.floor((i - 1) / cols)
    local col = (i - 1) % cols
    local x = bx + col * (slotW + gap)
    local y = by + row * (slotH + gap)
    local sel = (i == selected)
    local isRoof = (c.layer == "roof")
    DrawRect(x, y, slotW, slotH, sel and 0.20 or 0.10, sel and 0.18 or 0.10,
             sel and 0.12 or 0.11, 0.85)
    DrawRectOutline(x, y, slotW, slotH, sel and 0.98 or 0.45, sel and 0.85 or 0.42,
                    sel and 0.55 or 0.38, 1)
    local mr, mg, mb = hexColor(c.color)
    DrawRect(x + 6, y + 6, 10, slotH - 12, mr, mg, mb, 1)
    DrawText(c.name, x + 22, y + 5, 14, 0.95, 0.92, 0.86, 1)
    local key = HOTKEYS[i]
    local tag = key and ("[" .. key .. "]") or "点击"
    if isRoof then tag = tag .. " 顶" end
    DrawText(tag, x + 22, y + 23, 12, 0.65, 0.65, 0.62, 1)
    if hover and hover.uiSlot == i then
      drawTextCentered(c.desc, x + slotW * 0.5, y - 18, 13, 0.95, 0.9, 0.75, 1)
    end
  end

  if toast.t > 0 then drawBanner(toast.text, 120, 16) end
end

function drawBuildHud()
  local vp = GetViewportSize()
  local vw = (vp and vp.w) or 1280
  local vh = (vp and vp.h) or 720
  local sprinting = onFoot and (player.anim == "Run")
  local pw, ph = 250, 270
  DrawRect(10, 10, pw, ph, 0.08, 0.09, 0.10, 0.72)
  DrawRectOutline(10, 10, pw, ph, 0.55, 0.52, 0.42, 0.9)
  DrawText("改装模式 · Nomad Caravan", 20, 16, 17, 0.95, 0.88, 0.70, 1)
  local pad = 12
  local rowY = 46
  local innerW = pw - pad * 2
  local s = stats
  local netPower = s.powerGen - s.powerDraw
  drawStatBar(10 + pad, rowY, innerW, "电力",
    string.format("%+.2f kW · 电 %.1f/%.1f kWh", netPower,
                  batteryCharge or 0, batteryCapacity or 0),
    s.powerDraw > 0 and math.min(1, s.powerDraw / math.max(s.powerGen, 0.001)) or 0,
    netPower < 0)
  rowY = rowY + 34
  drawStatBar(10 + pad, rowY, innerW, "水",
    string.format("%d / %d L%s", math.floor(water), waterMax(),
                  s.waterDraw > 0 and string.format(" (耗%d/h)", s.waterDraw) or ""),
    water / waterMax(), water < 15)
  rowY = rowY + 34
  drawStatBar(10 + pad, rowY, innerW, "载重",
    string.format("%.0f / %d kg", s.weight, weightMax()),
    s.weight / weightMax(), s.weight > weightMax())
  rowY = rowY + 34
  drawStatBar(10 + pad, rowY, innerW, "储物",
    string.format("%d L", s.storage), math.min(1, s.storage / 400), false)
  rowY = rowY + 34
  drawStatBar(10 + pad, rowY, innerW, "舒适",
    string.format("%d%%", math.min(100, s.comfort)),
    math.min(1, s.comfort / 100), false)
  rowY = rowY + 34
  drawStatBar(10 + pad, rowY, innerW, "车体",
    string.format("%.0f%%", dur), dur / VEH.durMax, dur < 40)

  if #unreachableNames > 0 then
    DrawText("不可达: " .. table.concat(unreachableNames, "、"),
             10 + pad, 10 + ph - 10, 13, 1, 0.45, 0.3, 1)
  end

  local slotW, slotH, gap = 96, 46, 6
  local cols = 7
  local rowsN = math.ceil(#CATALOG / cols)
  local barW = cols * slotW + (cols - 1) * gap
  local bx = math.floor(vw * 0.5 - barW * 0.5)
  local by = vh - rowsN * (slotH + gap) - 12
  for i, cat in ipairs(CATALOG) do
    local row = math.floor((i - 1) / cols)
    local col = (i - 1) % cols
    local x = bx + col * (slotW + gap)
    local y = by + row * (slotH + gap)
    local sel = (i == selected)
    local isRoof = (cat.layer == "roof")
    DrawRect(x, y, slotW, slotH, sel and 0.20 or 0.10, sel and 0.18 or 0.10,
             sel and 0.12 or 0.11, 0.85)
    DrawRectOutline(x, y, slotW, slotH, sel and 0.98 or 0.45, sel and 0.85 or 0.42,
                    sel and 0.55 or 0.38, 1)
    local mr, mg, mb = hexColor(cat.color)
    DrawRect(x + 6, y + 6, 10, slotH - 12, mr, mg, mb, 1)
    DrawText(cat.name, x + 22, y + 5, 14, 0.95, 0.92, 0.86, 1)
    local key = HOTKEYS[i]
    local tag = key and ("[" .. key .. "]") or "点击"
    if isRoof then tag = tag .. " 顶" end
    DrawText(tag, x + 22, y + 23, 12, 0.65, 0.65, 0.62, 1)
    if hover and hover.uiSlot == i then
      drawTextCentered(cat.desc, x + slotW * 0.5, y - 18, 13, 0.95, 0.9, 0.75, 1)
    end
  end

  local hints = {
    "左键 放置   右键/X 拆除   R 旋转",
    "Tab 地板/车顶   G 网格   工作台修车",
    "B 退出改装   T 上路   M 地图   F5/F9 存/读",
  }
  for i, h in ipairs(hints) do
    drawTextRight(h, vw - 14, 14 + (i - 1) * 20, 14, 0.85, 0.84, 0.80, 0.9)
  end

  local mw, mh2 = 150, 118
  local mx0 = vw - mw - 10
  local my0 = 78
  DrawRect(mx0, my0, mw, mh2, 0.08, 0.09, 0.10, 0.72)
  DrawRectOutline(mx0, my0, mw, mh2, 0.45, 0.52, 0.42, 0.9)
  DrawText("材料库存", mx0 + 10, my0 + 6, 14, 0.85, 0.80, 0.62, 1)
  local mats = {
    { MAT_NAMES.metal, materials.metal },
    { MAT_NAMES.electronics, materials.electronics },
    { MAT_NAMES.cloth, materials.cloth },
    { MAT_NAMES.wood, materials.wood },
  }
  for i, mv in ipairs(mats) do
    local yy = my0 + 30 + (i - 1) * 21
    DrawText(mv[1], mx0 + 10, yy, 14, 0.88, 0.86, 0.80, 1)
    DrawText(tostring(mv[2]), mx0 + mw - 14, yy, 14, 0.95, 0.92, 0.82, 1, true)
  end

  drawTextCentered(curLayer == "roof" and "【车顶层】" or "【地板层】",
                   math.floor(vw * 0.5), 14, 16, 0.55, 0.85, 0.98, 1)

  if hover and hover.inst then
    local c = hover.inst.cat
    drawTextCentered(c.name .. "  ·  " .. (c.desc or "") .. "  ·  " .. costText(c.cost or {}),
                     math.floor(vw * 0.5), vh - rowsN * (slotH + gap) - 40,
                     14, 0.95, 0.90, 0.78, 1)
  end

  if toast.t > 0 then drawBanner(toast.text, 120, 16) end
end

-- 驾驶罗盘：顶部一条 ±60° 的刻度带，标 N/E/S/W 与货运目标方位
function drawCompass(vw)
  local heading = compassHeading(rv.yaw + (driveLookYaw or 0))
  local cw, ch = 340, 24
  local cx = vw * 0.5
  local cy = 150
  DrawRect(cx - cw * 0.5, cy, cw, ch, 0.05, 0.06, 0.07, 0.55)
  DrawRectOutline(cx - cw * 0.5, cy, cw, ch, 0.45, 0.44, 0.36, 0.7)
  local half = cw * 0.5
  local first = math.floor((heading - 60) / 10) * 10
  for deg = first, heading + 60, 10 do
    local diff = deg - heading
    local x = cx + diff / 60 * half
    if x >= cx - half and x <= cx + half then
      local major = (deg % 90 == 0)
      DrawLine(x, cy + 2, x, cy + (major and 13 or 8), 1,
               0.85, 0.85, 0.78, major and 0.9 or 0.5)
    end
  end
  local pts = { { 0, "N" }, { 45, "NE" }, { 90, "E" }, { 135, "SE" },
                { 180, "S" }, { 225, "SW" }, { 270, "W" }, { 315, "NW" } }
  for _, p in ipairs(pts) do
    local diff = p[1] - heading
    while diff > 180 do diff = diff - 360 end
    while diff < -180 do diff = diff + 360 end
    if math.abs(diff) <= 60 then
      local x = cx + diff / 60 * half
      local col = (p[2] == "N" and 1.0) or 0.86
      drawTextCentered(p[2], x, cy + 2, 11, col,
                       p[2] == "N" and 0.72 or 0.86,
                       p[2] == "N" and 0.35 or 0.78, 1)
    end
  end
  -- 货运目标方位：金色三角
  if job ~= nil then
    local delta = jobBearingDelta(rv.x, rv.z, rv.yaw + (driveLookYaw or 0))
    local deg = -math.deg(delta)
    if math.abs(deg) <= 60 then
      local x = cx + deg / 60 * half
      DrawLine(x - 5, cy + ch - 2, x, cy + ch - 8, 2, 1.0, 0.82, 0.32, 1)
      DrawLine(x + 5, cy + ch - 2, x, cy + ch - 8, 2, 1.0, 0.82, 0.32, 1)
    end
  end
  -- 低油量：青色小三角标出最近的加油站
  if lowFuelCompassActive ~= nil and
     lowFuelCompassActive((fuel or 0) / fuelMax()) and
     nearestFuelStop ~= nil then
    local st = nearestFuelStop(rv.x, rv.z)
    local nd = (st ~= nil and nodeById ~= nil) and nodeById(st.id) or nil
    if nd ~= nil then
      local b = dirYaw(nd.x - rv.x, nd.z - rv.z)
      local d = b - (rv.yaw + (driveLookYaw or 0))
      while d > math.pi do d = d - 2 * math.pi end
      while d < -math.pi do d = d + 2 * math.pi end
      local deg = -math.deg(d)
      if math.abs(deg) <= 60 then
        local x = cx + deg / 60 * half
        DrawLine(x - 4, cy + 2, x, cy + 7, 2, 0.45, 0.90, 0.95, 1)
        DrawLine(x + 4, cy + 2, x, cy + 7, 2, 0.45, 0.90, 0.95, 1)
      end
    end
  end
  -- 生命垂危：红色小三角标出最近的诊所
  if lowHealthCompassActive ~= nil and
     lowHealthCompassActive(needs and needs.health) and nodeById ~= nil then
    local nd = nodeById("clinic")
    if nd ~= nil then
      local b = dirYaw(nd.x - rv.x, nd.z - rv.z)
      local d = b - (rv.yaw + (driveLookYaw or 0))
      while d > math.pi do d = d - 2 * math.pi end
      while d < -math.pi do d = d + 2 * math.pi end
      local deg = -math.deg(d)
      if math.abs(deg) <= 60 then
        local x = cx + deg / 60 * half
        DrawLine(x - 4, cy + 2, x, cy + 7, 2, 0.98, 0.42, 0.40, 1)
        DrawLine(x + 4, cy + 2, x, cy + 7, 2, 0.98, 0.42, 0.40, 1)
      end
    end
  end
end

-- 今日目标短文案（HUD 用）
function dailyGoalHudText()
  if dailyGoal == nil then return nil end
  local name = dailyGoal.name or "今日目标"
  if dailyGoal.done then
    return string.format("今日目标 ✓ %s", name)
  end
  return string.format("今日目标 %s %d/%d", name,
                       math.floor(dailyGoal.progress or 0),
                       dailyGoal.target or 0)
end

-- 小地图额外标记：未搜刮补给箱 / 鱼笼（绘制/自检共用）
function minimapMarkers()
  local out = {}
  for _, c in ipairs(roadCaches or {}) do
    if c.ready then
      out[#out + 1] = { kind = "cache", x = c.x, z = c.z }
    end
  end
  if pondTrap ~= nil then
    out[#out + 1] = { kind = "trap", x = pondTrap.x, z = pondTrap.z,
                      ready = pondTrap.ready == true }
  end
  if coopPingMarker ~= nil then
    local p = coopPingMarker()
    if p ~= nil then
      out[#out + 1] = { kind = "ping", x = p.x, z = p.z }
    end
  end
  return out
end

-- 小地图道路连线：返回两端都在范围内的节点对（绘制/自检共用）
function minimapRoadPairs(range)
  local out = {}
  local r2 = (range or 140) * (range or 140)
  local seen = {}
  for _, nd in ipairs(NODES) do
    local dx, dz = nd.x - rv.x, nd.z - rv.z
    if dx * dx + dz * dz <= r2 then
      for _, lid in ipairs(nd.links or {}) do
        local o = nodeById(lid)
        if o ~= nil then
          local ox, oz = o.x - rv.x, o.z - rv.z
          if ox * ox + oz * oz <= r2 then
            local key = (nd.id < lid) and (nd.id .. "|" .. lid)
                                    or (lid .. "|" .. nd.id)
            if not seen[key] then
              seen[key] = true
              out[#out + 1] = { a = nd, b = o }
            end
          end
        end
      end
    end
  end
  return out
end

-- 右下角小地图：附近地点、未知 POI、货运目标与 RV 朝向（北在上）
function drawMinimap(vw, vh)
  local size = 150
  local mx = vw - size - 16
  local my = vh - size - 96
  if dailyGoalHudText ~= nil then
    local g = dailyGoalHudText()
    if g ~= nil then
      drawTextRight(g, vw - 16, my - 18, 11, 0.78, 0.80, 0.64, 0.9)
    end
  end
  DrawRect(mx, my, size, size, 0.04, 0.05, 0.06, 0.60)
  DrawRectOutline(mx, my, size, size, 0.45, 0.44, 0.36, 0.8)
  local cx, cy = mx + size * 0.5, my + size * 0.5
  local range = 140
  local scale = (size * 0.5 - 10) / range
  for _, nd in ipairs(NODES) do
    local dx, dz = nd.x - rv.x, nd.z - rv.z
    if dx * dx + dz * dz <= range * range then
      local px2, py2 = cx + dx * scale, cy - dz * scale
      local danger = nd.danger or 0
      if mapNodeKnown == nil or mapNodeKnown(nd) then
        DrawCircle(px2, py2, 3, 1.5, 0.45 + danger * 0.17,
                   0.85 - danger * 0.18, 0.32, 1, true)
      else
        DrawCircle(px2, py2, 2.5, 1, 0.55, 0.57, 0.55, 0.9, false)
      end
    end
  end
  -- 附近道路连线：让局部地图看起来像路网
  for _, pair in ipairs(minimapRoadPairs and minimapRoadPairs(range) or {}) do
    local ax = cx + (pair.a.x - rv.x) * scale
    local ay = cy - (pair.a.z - rv.z) * scale
    local bx2 = cx + (pair.b.x - rv.x) * scale
    local by2 = cy - (pair.b.z - rv.z) * scale
    DrawLine(ax, ay, bx2, by2, 1, 0.58, 0.57, 0.50, 0.40)
  end
  if job ~= nil then
    local nd = nodeById(job.destId)
    if nd ~= nil then
      local dx, dz = nd.x - rv.x, nd.z - rv.z
      local d = math.sqrt(dx * dx + dz * dz)
      local px2, py2
      if d <= range then
        px2, py2 = cx + dx * scale, cy - dz * scale
      else
        px2 = cx + dx / d * (size * 0.5 - 10)
        py2 = cy - dz / d * (size * 0.5 - 10)
      end
      -- 从小地图中心（房车）到目标的路线线
      DrawLine(cx, cy, px2, py2, 2, 0.92, 0.78, 0.36, 0.55)
      DrawCircle(px2, py2, 6, 1.5, 1.0, 0.80, 0.30, 1, false)
    end
  end
  -- 鱼笼位置：成熟时青色高亮，未成熟灰色
  if pondTrap ~= nil then
    local dx, dz = pondTrap.x - rv.x, pondTrap.z - rv.z
    local d = math.sqrt(dx * dx + dz * dz)
    local px2, py2
    if d <= range then
      px2, py2 = cx + dx * scale, cy - dz * scale
    elseif d > 0.01 then
      px2 = cx + dx / d * (size * 0.5 - 10)
      py2 = cy - dz / d * (size * 0.5 - 10)
    end
    if px2 ~= nil then
      local ready = pondTrap.ready == true
      DrawCircle(px2, py2, ready and 4.5 or 3, 1.5,
                 ready and 0.45 or 0.62, ready and 0.95 or 0.62,
                 ready and 0.82 or 0.62, 1, ready)
    end
  end
  -- 路边补给箱：未搜刮时标出位置
  for _, c in ipairs(roadCaches or {}) do
    if c.ready then
      local dx, dz = c.x - rv.x, c.z - rv.z
      local d = math.sqrt(dx * dx + dz * dz)
      local px2, py2
      if d <= range then
        px2, py2 = cx + dx * scale, cy - dz * scale
      elseif d > 0.01 then
        px2 = cx + dx / d * (size * 0.5 - 10)
        py2 = cy - dz / d * (size * 0.5 - 10)
      end
      if px2 ~= nil then
        DrawCircle(px2, py2, 3, 1.5, 0.92, 0.72, 0.36, 0.95, false)
      end
    end
  end
  -- 未读路牌笔记：已探索地点才标出
  for _, n in ipairs(loreNotes or {}) do
    if loreNoteMapVisible ~= nil and loreNoteMapVisible(n) then
      local dx, dz = n.x - rv.x, n.z - rv.z
      local d = math.sqrt(dx * dx + dz * dz)
      local px2, py2
      if d <= range then
        px2, py2 = cx + dx * scale, cy - dz * scale
      elseif d > 0.01 then
        px2 = cx + dx / d * (size * 0.5 - 10)
        py2 = cy - dz / d * (size * 0.5 - 10)
      end
      if px2 ~= nil then
        DrawCircle(px2, py2, 2.5, 1.5, 0.95, 0.95, 0.88, 0.95, true)
      end
    end
  end
  -- 路边事件：搭车客（青）/ 抛锚车（橙）
  for _, e in ipairs((roadEventMarkers and roadEventMarkers()) or {}) do
    local dx, dz = e.x - rv.x, e.z - rv.z
    local d = math.sqrt(dx * dx + dz * dz)
    local px2, py2
    if d <= range then
      px2, py2 = cx + dx * scale, cy - dz * scale
    elseif d > 0.01 then
      px2 = cx + dx / d * (size * 0.5 - 10)
      py2 = cy - dz / d * (size * 0.5 - 10)
    end
    if px2 ~= nil then
      if e.kind == "hitchhiker" then
        DrawCircle(px2, py2, 3.5, 1.5, 0.45, 0.92, 0.95, 1, false)
      else
        DrawCircle(px2, py2, 3.0, 1.5, 0.95, 0.62, 0.30, 1, false)
      end
    end
  end
  -- 未搜刮补给箱（琥珀）/ 鱼笼（青绿，可收时实心）
  for _, m in ipairs((minimapMarkers and minimapMarkers()) or {}) do
    local dx, dz = m.x - rv.x, m.z - rv.z
    local d = math.sqrt(dx * dx + dz * dz)
    local px2, py2
    if d <= range then
      px2, py2 = cx + dx * scale, cy - dz * scale
    elseif d > 0.01 then
      px2 = cx + dx / d * (size * 0.5 - 10)
      py2 = cy - dz / d * (size * 0.5 - 10)
    end
    if px2 ~= nil then
      if m.kind == "cache" then
        DrawCircle(px2, py2, 3.0, 1.5, 0.95, 0.75, 0.35, 1, false)
      elseif m.kind == "ping" then
        DrawCircle(px2, py2, 4.0, 1.5, 1.0, 0.82, 0.30, 1, false)
        DrawCircle(px2, py2, 1.5, 1.0, 1.0, 0.88, 0.45, 1, true)
      else
        DrawCircle(px2, py2, 3.2, 1.5, 0.45, 0.90, 0.85,
                   m.ready and 1 or 0.7, m.ready)
      end
    end
  end
  DrawLine(cx, cy, cx + math.sin(rv.yaw) * 10,
           cy - math.cos(rv.yaw) * 10, 2, 1.0, 0.85, 0.35, 1)
  DrawCircle(cx, cy, 3, 2, 1.0, 0.85, 0.35, 1, true)
  drawTextCentered("N", cx, my + 3, 10, 0.72, 0.74, 0.68, 0.9)
end

-- 睡醒晨报：天数、累计货运、今日目标进度、当前油量
function drawMorningReport()
  if morningReport == nil then return end
  local vp = GetViewportSize()
  local vw = (vp and vp.w) or 1280
  local vh = (vp and vp.h) or 720
  local pw, ph = 440, 170
  local px = math.floor((vw - pw) * 0.5)
  local py = math.floor(vh * 0.24)
  local a = math.min(1, (morningReport.t or 0) * 1.6)
  DrawRect(px, py, pw, ph, 0.05, 0.055, 0.065, 0.94 * a)
  DrawRectOutline(px, py, pw, ph, 0.62, 0.56, 0.38, a)
  drawTextCentered(string.format("第 %d 天 · %s清晨", morningReport.day or 1,
                   morningReport.camp and "营地" or ""),
                   vw * 0.5, py + 12, 22, 0.98, 0.90, 0.66, a)
  local fc = morningReport.forecast
  local fcName = (fc ~= nil and WEATHER_NAMES ~= nil and WEATHER_NAMES[fc]) or nil
  DrawText(string.format("天气 %s%s   油量 %.0f / %d L",
           (weatherName and weatherName()) or "晴",
           fcName and ("  →  " .. fcName) or "",
           morningReport.fuel or 0, fuelMax()),
           px + 22, py + 48, 13, 0.82, 0.84, 0.78, a)
  DrawText(string.format("累计完成货运 %d 单", morningReport.jobs or 0),
           px + 22, py + 70, 13, 0.82, 0.84, 0.78, a)
  if morningReport.goal ~= nil then
    local ok = morningReport.goalDone
    DrawText(string.format("今日目标：%s  %d/%d%s", morningReport.goal,
             math.min(morningReport.goalProgress or 0, morningReport.goalTarget or 0),
             morningReport.goalTarget or 0, ok and "  ✓" or ""),
             px + 22, py + 94, 13,
             ok and 0.58 or 0.95, ok and 0.85 or 0.82, ok and 0.58 or 0.50, a)
  end
  local extra = {}
  if morningReport.notes ~= nil then extra[#extra + 1] = morningReport.notes end
  if morningReport.oil ~= nil then
    extra[#extra + 1] = string.format("机油 %.0f%%", morningReport.oil)
  end
  if #extra > 0 then
    DrawText(table.concat(extra, "   "), px + 22, py + 116, 12,
             0.72, 0.78, 0.70, a)
  end
  DrawText("W 上车出发 / E 下车活动 / M 看地图", px + 22, py + 140, 12,
           0.68, 0.70, 0.64, a)
end

-- 信号中继恢复：旅程总结卡
function drawRelayCard()
  if relayCard == nil then return end
  local vp = GetViewportSize()
  local vw = (vp and vp.w) or 1280
  local vh = (vp and vp.h) or 720
  local pw, ph = 480, 236
  local px = math.floor((vw - pw) * 0.5)
  local py = math.floor(vh * 0.22)
  local a = math.min(1, (relayCard.t or 0) * 1.4)
  DrawRect(px, py, pw, ph, 0.04, 0.05, 0.06, 0.95 * a)
  DrawRectOutline(px, py, pw, ph, 0.72, 0.62, 0.32, a)
  drawTextCentered("信号中继已恢复", vw * 0.5, py + 14, 22, 0.98, 0.90, 0.66, a)
  DrawText(string.format("第 %d 天 · 累计货运 %d 单 · 里程 %.1fkm",
           relayCard.day or 1, relayCard.jobs or 0, relayCard.km or 0),
           px + 26, py + 52, 13, 0.86, 0.86, 0.78, a)
  DrawText(string.format("钓鱼 %d 条 · 探索 %d / %d",
           relayCard.fish or 0, relayCard.explored or 0, relayCard.total or 0),
           px + 26, py + 74, 13, 0.86, 0.86, 0.78, a)
  DrawText(string.format("奖励：金属 +10 · 电子件 +5 · 燃料 +%.0fL",
           relayCard.fuelGain or 0), px + 26, py + 104, 13, 0.80, 0.90, 0.70, a)
  DrawText("你联系上了外界——这片荒野不再只有你一个人。",
           px + 26, py + 142, 13, 0.92, 0.82, 0.58, a)
  DrawText("可以继续跑单、探索，或回营地休整。", px + 26, py + 168, 12,
           0.72, 0.74, 0.68, a)
end

function drawDriveHud()
  local vp = GetViewportSize()
  local vw = (vp and vp.w) or 1280
  local vh = (vp and vp.h) or 720
  local kmh = math.floor(math.abs(rv.speed) * 3.6)
  local ratio = stats.weight / weightMax()
  local over = ratio > 1.0

  local pw, ph = 210, 216
  local px, py = 10, vh - ph - 10
  DrawRect(px, py, pw, ph, 0.08, 0.09, 0.10, 0.72)
  DrawRectOutline(px, py, pw, ph, 0.55, 0.52, 0.42, 0.9)
  local gear, rpm = 1, VEH.gearRpmMin or 850
  if rvGearInfo ~= nil then gear, rpm = rvGearInfo(rv.speed or 0) end
  local gearText
  if (rv.speed or 0) < -0.05 then
    gearText = "R"
  elseif math.abs(rv.speed or 0) < 0.05 then
    gearText = "N"
  else
    gearText = tostring(gear)
  end
  DrawText(string.format("%d", kmh), px + 14, py + 10, 34,
           over and 1.0 or 0.95, over and 0.5 or 0.92, over and 0.35 or 0.78, 1)
  DrawText("km/h", px + 84, py + 26, 15, 0.7, 0.68, 0.62, 1)
  DrawText("挡位 " .. gearText, px + 140, py + 26, 15, 0.6, 0.85, 0.95, 1)
  DrawText(string.format("%.0f rpm", rpm), px + 140, py + 44, 11, 0.55, 0.75, 0.82, 1)
  if carRadio then
    DrawText(string.format("车载电台 %d（R）", radioStation or 1),
             px + 140, py + 6, 10, 0.52, 0.86, 0.96, 1)
  end
  local fLow = fuel / fuelMax() < 0.2
  DrawText(string.format("燃料 %.0f / %d L%s", fuel, fuelMax(),
           fuel <= 0 and "  没油了! 龟速" or (fLow and "  油量低!" or "")),
           px + 14, py + 60, 13, fLow and 1 or 0.85,
           fLow and 0.4 or 0.84, fLow and 0.3 or 0.8, 1)
  DrawRect(px + 14, py + 78, pw - 28, 5, 0.13, 0.13, 0.14, 0.9)
  DrawRect(px + 14, py + 78, (pw - 28) * math.max(0, fuel / fuelMax()), 5,
           0.5, 0.75, 0.2, 1)
  DrawText(string.format("车体 %.0f%%%s", dur, dur < 40 and "  需修理" or ""),
           px + 14, py + 92, 13, dur < 40 and 1 or 0.85,
           dur < 40 and 0.4 or 0.84, dur < 40 and 0.3 or 0.8, 1)
  DrawRect(px + 14, py + 110, pw - 28, 5, 0.13, 0.13, 0.14, 0.9)
  DrawRect(px + 14, py + 110, (pw - 28) * math.max(0, dur / VEH.durMax), 5,
           dur < 40 and 0.88 or 0.45, dur < 40 and 0.28 or 0.78,
           dur < 40 and 0.24 or 0.44, 1)
  DrawText(string.format("载重 %.0f / %d kg", stats.weight, weightMax()),
           px + 14, py + 124, 12, 0.8, 0.8, 0.78, 1)
  DrawText(string.format("引擎 %.0f%%  轮胎 %.0f%%  电瓶 %.0f%%",
           (parts and parts.engine) or 100, (parts and parts.tires) or 100,
           (parts and parts.battery) or 100),
           px + 14, py + 142, 11, 0.74, 0.74, 0.70, 1)
  if rv.tireTemps ~= nil then
    local t = rv.tireTemps
    local hottest = math.max(t[1] or 22, t[2] or 22, t[3] or 22, t[4] or 22)
    local pAvg = 2.4
    if rv.tirePressureCold ~= nil then
      local sum = 0
      for i = 1, 4 do sum = sum + (rv.tirePressureCold[i] or 2.4) end
      pAvg = sum / 4
    end
    local tr, tg, tb = 0.74, 0.74, 0.70
    if rv.blowout then tr, tg, tb = 1.0, 0.35, 0.28
    elseif hottest > 110 then tr, tg, tb = 0.95, 0.45, 0.30
    elseif hottest > 85 then tr, tg, tb = 0.95, 0.78, 0.35 end
    DrawText(string.format("胎温 %.0f°C · 胎压 %.1fbar%s", hottest, pAvg,
             rv.blowout and "  爆胎!" or ""),
             px + 14, py + 158, 11, tr, tg, tb, 1)
  end
  -- 水温：过热变红提示散热；冷车标蓝提示动力/油耗受限
  local et = engineTemp or 20
  local hot = et >= (VEH.tempOverheat or 110)
  local cold = et < 45
  local tr2, tg2, tb2 = 0.74, 0.82, 0.86
  if cold then tr2, tg2, tb2 = 0.55, 0.78, 0.95 end
  if et > (VEH.tempOverheat or 110) - 18 then tr2, tg2, tb2 = 0.95, 0.78, 0.35 end
  if hot then tr2, tg2, tb2 = 0.98, 0.40, 0.30 end
  DrawText(string.format("水温 %.0f°C%s%s", et, cold and "  冷车" or "",
           hot and "  过热!" or ""),
           px + 14, py + 172, 11, tr2, tg2, tb2, 1)
  -- 行程电脑：累计里程 + 百公里油耗 + 剩余续航（车速/载重曲线的直观反馈）
  local l100 = tripL100 and tripL100() or 0
  local rangeKm = (rangeEstimateM and rangeEstimateM() or 0) / 1000
  if (tripDistance or 0) >= 20 then
    DrawText(string.format("行程 %.1fkm · %.1fL/100km · 续航 %.1fkm",
             tripDistance / 1000, l100, rangeKm),
             px + 14, py + 186, 11, 0.74, 0.80, 0.72, 1)
  else
    DrawText(string.format("行程 %.0fm · 续航 %.1fkm", tripDistance or 0, rangeKm),
             px + 14, py + 186, 11, 0.68, 0.70, 0.64, 1)
  end
  -- 机油/冷却液：过低分别变黄/红
  local oil = oilLife or 100
  local orr, og, ob = 0.74, 0.80, 0.72
  if oil < 20 then
    orr, og, ob = 1.0, 0.45, 0.30
  elseif oil < 40 then
    orr, og, ob = 0.95, 0.78, 0.35
  end
  local cool = coolant or 100
  local cr, cg, cb = 0.74, 0.80, 0.72
  if cool < 20 then
    cr, cg, cb = 1.0, 0.45, 0.30
  elseif cool < 40 then
    cr, cg, cb = 0.95, 0.78, 0.35
  end
  DrawText(string.format("机油 %.0f%%", oil),
           px + 14, py + 200, 11, orr, og, ob, 1)
  DrawText(string.format("冷却液 %.0f%%", cool),
           px + 76, py + 200, 11, cr, cg, cb, 1)
  local dirt = rv.dirt or 0
  local dr, dg, db = 0.74, 0.80, 0.72
  if dirt >= 0.8 then dr, dg, db = 1.0, 0.45, 0.30
  elseif dirt >= 0.6 then dr, dg, db = 0.95, 0.78, 0.35 end
  DrawText(string.format("清洁 %.0f%%", (1 - dirt) * 100),
           px + 150, py + 200, 11, dr, dg, db, 1)

  drawNeedsPanel(vw - 230, 10, 220)
  drawCompass(vw)
  drawMinimap(vw, vh)
  if job ~= nil then drawJobTracker(vw) end

  local hints = {
    "W 油门   S 刹车/倒车   A/D 转向   Shift 手刹",
    "鼠标 张望   空格 停车   R 车载电台   V 定速",
    "C " .. (driveView == "cab" and "追逐视角" or "驾驶席视角") .. "   M 世界地图   P 说明",
  }
  for i, h in ipairs(hints) do
    drawTextRight(h, vw - 14, vh - 74 + (i - 1) * 19, 12, 0.80, 0.79, 0.74, 0.9)
  end

  local top = string.format("第 %d 天  %s  ·  %s%s", day, timeString(), weatherName(),
                            (weatherHoursLeft and weatherHoursLeft() > 0) and
                            string.format("（%.1fh）", weatherHoursLeft()) or "")
  if surfaceInfo ~= nil and surfaceLabel ~= nil then
    local sname = surfaceInfo(rv.x, rv.z)
    top = top .. "   · 路面 " .. surfaceLabel(sname)
  end
  if tractionStatusText ~= nil then
    local traction = tractionStatusText()
    if traction ~= nil then top = top .. "   · " .. traction end
  end
  if windActive ~= nil and windActive(weather, rv.speed) then
    top = top .. "   · 侧风"
  end
  if (brakeHeat or 0) > 0.6 then
    top = top .. string.format("   · 刹车过热 %.0f%%", (brakeHeat or 0) * 100)
  end
  if highBeam and isNight ~= nil and isNight() then
    top = top .. "   · 远光（招感染者）"
  end
  if (coffeeBoostMin or 0) > 0 then
    top = top .. string.format("   · 咖啡清醒 %.0f 分钟", coffeeBoostMin)
  end
  if (wetness or 0) >= 30 then
    top = top .. string.format("   · 湿透 %.0f%%", wetness)
  end
  if infrastructureStatusText ~= nil then
    local infra = infrastructureStatusText()
    if infra ~= nil then top = top .. "   · " .. infra end
  end
  if maintenanceWarningActive ~= nil and maintenanceWarningActive() then
    top = top .. "   · 保养!"
  end
  if cruiseOn then
    top = top .. string.format("   · 定速 %.0f km/h",
                               (cruiseSpeed or 0) * 3.6)
  end
  if weatherNextId ~= nil and WEATHER_NAMES ~= nil and
     WEATHER_NAMES[weatherNextId] ~= nil then
    top = top .. "  →  " .. WEATHER_NAMES[weatherNextId]
  end
  if nearNode then
    top = top .. "   ▶ 临近 " .. nearNode.name .. "（空格停靠）"
  end
  if radioMastFixed and not relayDone then
    top = top .. "   ▶ 信号中继：废弃小镇"
  end
  drawBanner(top, 14, 16)

  if toast.t > 0 then drawBanner(toast.text, 120, 16) end

  -- 低油量：续航 + 最近加油站距离；金属不足时提示营地免费补给
  if (fuel or 0) / fuelMax() < 0.2 and lowFuelHintText ~= nil then
    local hint = lowFuelHintText()
    if hint ~= nil then
      drawTextCentered(hint, vw * 0.5, 52, 13, 1.0, 0.72, 0.38, 0.95)
    end
  end
  if drawNewGameHint ~= nil then drawNewGameHint(vw, vh) end
end

-- 停靠搜刮叠加：物资箱标记 / 搜获袋 / 感染者
function drawScavengeOverlay()
  local vp = GetViewportSize()
  local vw = (vp and vp.w) or 1280
  local vh = (vp and vp.h) or 720

  local cats = 0
  for _ in pairs(stopBag) do cats = cats + 1 end
  local bw = 190
  local bh = 34 + 21 * math.max(1, cats)
  local bx, by = 10, vh - 186 - bh - 8
  DrawRect(bx, by, bw, bh, 0.08, 0.09, 0.10, 0.72)
  DrawRectOutline(bx, by, bw, bh, 0.55, 0.52, 0.42, 0.9)
  DrawText("本次搜获", bx + 12, by + 7, 14, 0.85, 0.8, 0.62, 1)
  if cats == 0 then
    DrawText("（按 E 搜物资箱）", bx + 12, by + 30, 12, 0.6, 0.6, 0.55, 1)
  else
    local yy = by + 30
    for mat, n in pairs(stopBag) do
      DrawText(MAT_NAMES[mat], bx + 12, yy, 13, 0.88, 0.86, 0.8, 1)
      DrawText("x" .. n, bx + bw - 14, yy, 13, 0.95, 0.92, 0.82, 1, true)
      yy = yy + 21
    end
  end

  for _, c in ipairs(crates) do
    local sp = WorldToScreen(c.x, 0.75, c.z)
    if sp then
      DrawText("□ 搜", math.floor(sp.x), math.floor(sp.y), 14,
               1, 0.75, 0.35, 1, true)
    end
  end
end

-- 背包面板
-- 背包面板布局：物品多时自动两栏，保证 720p 不溢出
function inventoryPanelLayout(count, vw, vh)
  local n = math.max(0, count or 0)
  local cols = (n > 9) and 2 or 1
  local perCol = math.max(1, math.ceil(n / cols))
  local pw = (cols == 2) and 760 or 540
  local ph = 104 + perCol * 44 + 34
  return pw, ph, cols, perCol
end

function drawInventoryPanel()
  local vp = GetViewportSize()
  local vw = (vp and vp.w) or 1280
  local vh = (vp and vp.h) or 720
  local rows = {}
  for _, id in ipairs(ITEM_ORDER) do
    if (inv[id] or 0) > 0 then rows[#rows + 1] = id end
  end
  local pw, ph, cols, perCol = inventoryPanelLayout(#rows, vw, vh)
  local px = math.floor((vw - pw) * 0.5)
  local py = math.floor((vh - ph) * 0.5)
  DrawRect(px, py, pw, ph, 0.04, 0.05, 0.06, 0.92)
  DrawRectOutline(px, py, pw, ph, 0.55, 0.52, 0.42, 0.95)
  drawTextCentered("背包 · 随身物资", vw * 0.5, py + 14, 20, 0.98, 0.90, 0.68, 1)
  DrawText("按数字快速使用；种子/咖啡豆/修理包要去对应家具", px + 20, py + 46, 12,
           0.72, 0.72, 0.66, 1)
  local colW = (pw - 32) / cols
  local key = { water="1", canned="2", meal="3", brew="4", rawfood="5",
                coffee="6", seed="7", repairkit="8", fish="9", fishsoup="0" }
  if #rows == 0 then
    DrawText("背包空空如也", px + 24, py + 78, 15, 0.65, 0.65, 0.6, 1)
  end
  for i, id in ipairs(rows) do
    local it = ITEMS[id]
    local col = math.floor((i - 1) / perCol)
    local rowIdx = (i - 1) % perCol
    local cx = px + 16 + col * colW
    local cy = py + 72 + rowIdx * 44
    local rowW = colW - 12
    DrawRect(cx, cy - 4, rowW, 40, 0.08, 0.09, 0.10, 0.7)
    DrawText((key[id] and ("[" .. key[id] .. "] ") or "    ") .. it.name,
             cx + 10, cy, 14, 0.95, 0.92, 0.84, 1)
    drawTextRight("x" .. (inv[id] or 0), cx + rowW - 10, cy, 14, 0.98, 0.9, 0.68, 1)
    DrawText(it.desc or "", cx + 10, cy + 18, 10, 0.72, 0.72, 0.66, 1)
  end
  DrawText(string.format("材料  金属%d  电子%d  布料%d  木材%d",
           materials.metal or 0, materials.electronics or 0,
           materials.cloth or 0, materials.wood or 0),
           px + 20, py + ph - 32, 12, 0.72, 0.72, 0.66, 1)
  drawTextRight("I / E 关闭", px + pw - 20, py + ph - 32, 12, 0.72, 0.72, 0.66, 1)
end

-- 灶台配方菜单
function drawStationMenu()
  if stationUI == nil then return end
  local vp = GetViewportSize()
  local vw = (vp and vp.w) or 1280
  local vh = (vp and vp.h) or 720
  local isStove = (stationUI.kind == "stove")
  local rows = isStove and #RECIPES or (#REPAIR_PARTS + 7)
  local pw = 540
  local ph = 118 + rows * 54
  local px = math.floor((vw - pw) * 0.5)
  local py = math.floor((vh - ph) * 0.5)
  DrawRect(px, py, pw, ph, 0.04, 0.05, 0.06, 0.92)
  DrawRectOutline(px, py, pw, ph, 0.55, 0.52, 0.42, 0.95)
  drawTextCentered(isStove and "灶台 · 做饭" or "工作台 · 维修",
                   vw * 0.5, py + 14, 20, 0.98, 0.90, 0.68, 1)
  if isStove then
    DrawText(string.format("水 %.1fL   电 %s", water, powerOk() and "正常" or "不足"),
             px + 20, py + 48, 13, 0.78, 0.78, 0.72, 1)
  else
    DrawText("选择要维修的部件（修理包优先）", px + 20, py + 48, 13, 0.78, 0.78, 0.72, 1)
  end
  local yy = py + 78
  if isStove then
    for i, r in ipairs(RECIPES) do
      local ok, why = recipeAvailable(r)
      local cost = {}
      for id, n in pairs(r.cost or {}) do cost[#cost + 1] = itemName(id) .. "x" .. n end
      cost[#cost + 1] = string.format("水%.1fL", r.water or 0)
      DrawRect(px + 16, yy - 4, pw - 32, 52, ok and 0.10 or 0.12,
               ok and 0.12 or 0.08, ok and 0.09 or 0.08, 0.7)
      DrawText("[" .. i .. "]", px + 26, yy, 16, ok and 0.95 or 0.5, ok and 0.85 or 0.5, 0.55, 1)
      DrawText(r.name, px + 66, yy, 16, ok and 0.96 or 0.6, ok and 0.92 or 0.6, 0.82, 1)
      DrawText(r.desc, px + 66, yy + 21, 12, 0.78, 0.76, 0.70, 1)
      drawTextRight(table.concat(cost, "  ") .. (ok and "" or ("  ✗ " .. why)),
                    px + pw - 24, yy + 8, 12, ok and 0.85 or 1,
                    ok and 0.82 or 0.35, ok and 0.72 or 0.28)
      yy = yy + 54
    end
    drawTextCentered("按 1-5 选择配方    E 关闭", vw * 0.5, py + ph - 30, 13, 0.75, 0.75, 0.68, 1)
  else
    for i, part in ipairs(REPAIR_PARTS) do
      local value = partValue(part.id)
      local full = value >= VEH.partMax
      local lv = upgrades and upgrades[part.id]
      local canUp = full and lv ~= nil and lv < 3
      local kit = hasItem("repairkit", 1)
      local cost = partRepairCost(part.id)
      local ok = (not full) and (kit or canAfford(cost))
      local costStr = kit and "修理包x1" or costText(cost)
      if canUp then
        local ucost = partUpgradeCost(part.id)
        costStr = costText(ucost)
        ok = canAfford(ucost)
      end
      DrawRect(px + 16, yy - 4, pw - 32, 52, ok and 0.10 or 0.12,
               ok and 0.12 or 0.08, ok and 0.09 or 0.08, 0.7)
      DrawText("[" .. i .. "]", px + 26, yy, 16, ok and 0.95 or 0.5, ok and 0.85 or 0.5, 0.55, 1)
      DrawText(part.name, px + 66, yy, 16, ok and 0.96 or 0.6, ok and 0.92 or 0.6, 0.82, 1)
      local status = string.format("%.0f%%", value)
      if full and lv ~= nil then status = "Lv" .. lv end
      DrawText(status, px + 66, yy + 21, 12,
               value < 40 and 1 or 0.8, value < 40 and 0.35 or 0.8, 0.35, 1)
      local rightText = "正常"
      if canUp then
        rightText = "升级 Lv" .. lv .. "->" .. (lv + 1) .. "  " .. costStr
      elseif full and lv ~= nil then
        rightText = "满级 Lv3"
      elseif not full then
        rightText = costStr
      end
      drawTextRight(rightText, px + pw - 24, yy + 8, 12,
                    (ok or full) and 0.85 or 1, (ok or full) and 0.82 or 0.35,
                    (ok or full) and 0.72 or 0.28)
      yy = yy + 54
    end
    -- 货架升级（第 5 项）：提高载重上限
    do
      local lv = (upgrades and upgrades.cargo) or 0
      local maxed = lv >= 3
      local cost = { metal = 3 + lv * 2, wood = 2 + lv }
      local ok = (not maxed) and canAfford(cost)
      DrawRect(px + 16, yy - 4, pw - 32, 52, ok and 0.10 or 0.12,
               ok and 0.12 or 0.08, ok and 0.09 or 0.08, 0.7)
      DrawText("[5]", px + 26, yy, 16, ok and 0.95 or 0.5, ok and 0.85 or 0.5, 0.55, 1)
      DrawText("货架", px + 66, yy, 16, ok and 0.96 or 0.6, ok and 0.92 or 0.6, 0.82, 1)
      DrawText(string.format("Lv%d · 载重上限 %dkg", lv, weightMax()),
               px + 66, yy + 21, 12, 0.80, 0.80, 0.74, 1)
      local rightText = maxed and "满级 Lv3" or
        ("升级 Lv" .. lv .. "->" .. (lv + 1) .. "  " .. costText(cost))
      drawTextRight(rightText, px + pw - 24, yy + 8, 12,
                    (ok or maxed) and 0.85 or 1, (ok or maxed) and 0.82 or 0.35,
                    (ok or maxed) and 0.72 or 0.28)
      yy = yy + 54
    end
    -- 鱼竿升级（第 6 项）
    do
      local lv = (upgrades and upgrades.rod) or 0
      local maxed = lv >= 3
      local cost = { wood = 2 + lv * 2, cloth = 1 + lv }
      local ok = (not maxed) and canAfford(cost)
      DrawRect(px + 16, yy - 4, pw - 32, 52, ok and 0.10 or 0.12,
               ok and 0.12 or 0.08, ok and 0.09 or 0.08, 0.7)
      DrawText("[6]", px + 26, yy, 16, ok and 0.95 or 0.5, ok and 0.85 or 0.5, 0.55, 1)
      DrawText("鱼竿", px + 66, yy, 16, ok and 0.96 or 0.6, ok and 0.92 or 0.6, 0.82, 1)
      DrawText(string.format("Lv%d · 上钩率 +%d%% · 尺寸 +%dcm",
               lv, lv * 7, lv * 4), px + 66, yy + 21, 12, 0.80, 0.80, 0.74, 1)
      local rightText = maxed and "满级 Lv3" or
        ("升级 Lv" .. lv .. "->" .. (lv + 1) .. "  " .. costText(cost))
      drawTextRight(rightText, px + pw - 24, yy + 8, 12,
                    (ok or maxed) and 0.85 or 1, (ok or maxed) and 0.82 or 0.35,
                    (ok or maxed) and 0.72 or 0.28)
      yy = yy + 54
    end
    -- 制作修理包（第 7 项）
    do
      local cost = { metal = 2, cloth = 1 }
      local ok = canAfford(cost)
      DrawRect(px + 16, yy - 4, pw - 32, 52, ok and 0.10 or 0.12,
               ok and 0.12 or 0.08, ok and 0.09 or 0.08, 0.7)
      DrawText("[7]", px + 26, yy, 16, ok and 0.95 or 0.5, ok and 0.85 or 0.5, 0.55, 1)
      DrawText("修理包", px + 66, yy, 16, ok and 0.96 or 0.6, ok and 0.92 or 0.6, 0.82, 1)
      DrawText("制作 1 个修理包（金属 2 + 布料 1）", px + 66, yy + 21, 12,
               0.80, 0.80, 0.74, 1)
      drawTextRight(costText(cost), px + pw - 24, yy + 8, 12,
                    ok and 0.85 or 1, ok and 0.82 or 0.35, ok and 0.72 or 0.28)
      yy = yy + 54
    end
    -- 制作防雨篷布（第 8 项）
    do
      local cost = { metal = 2, cloth = 3 }
      local ok = canAfford(cost)
      local owned = (inv.tarp or 0) > 0
      DrawRect(px + 16, yy - 4, pw - 32, 52, ok and 0.10 or 0.12,
               ok and 0.12 or 0.08, ok and 0.09 or 0.08, 0.7)
      DrawText("[8]", px + 26, yy, 16, ok and 0.95 or 0.5, ok and 0.85 or 0.5, 0.55, 1)
      DrawText("防雨篷布", px + 66, yy, 16, ok and 0.96 or 0.6, ok and 0.92 or 0.6, 0.82, 1)
      DrawText(string.format("保护易碎货不受雨水%s", owned and "（已携带）" or ""),
               px + 66, yy + 21, 12, 0.80, 0.80, 0.74, 1)
      drawTextRight(costText(cost), px + pw - 24, yy + 8, 12,
                    ok and 0.85 or 1, ok and 0.82 or 0.35, ok and 0.72 or 0.28)
      yy = yy + 54
    end
    -- 水箱升级（第 9 项）
    do
      local lv = (upgrades and upgrades.water) or 0
      local maxed = lv >= 3
      local cost = { metal = 2 + lv, cloth = 2 + lv }
      local ok = (not maxed) and canAfford(cost)
      DrawRect(px + 16, yy - 4, pw - 32, 52, ok and 0.10 or 0.12,
               ok and 0.12 or 0.08, ok and 0.09 or 0.08, 0.7)
      DrawText("[9]", px + 26, yy, 16, ok and 0.95 or 0.5, ok and 0.85 or 0.5, 0.55, 1)
      DrawText("水箱", px + 66, yy, 16, ok and 0.96 or 0.6, ok and 0.92 or 0.6, 0.82, 1)
      DrawText(string.format("Lv%d · 容量 %dL", lv, waterMax()),
               px + 66, yy + 21, 12, 0.80, 0.80, 0.74, 1)
      local rightText = maxed and "满级 Lv3" or
        ("升级 Lv" .. lv .. "->" .. (lv + 1) .. "  " .. costText(cost))
      drawTextRight(rightText, px + pw - 24, yy + 8, 12,
                    (ok or maxed) and 0.85 or 1, (ok or maxed) and 0.82 or 0.35,
                    (ok or maxed) and 0.72 or 0.28)
      yy = yy + 54
    end
    -- 油箱升级（第 10 项，按 0）
    do
      local lv = (upgrades and upgrades.fuel) or 0
      local maxed = lv >= 3
      local cost = { metal = 3 + lv, electronics = 1 + lv }
      local ok = (not maxed) and canAfford(cost)
      DrawRect(px + 16, yy - 4, pw - 32, 52, ok and 0.10 or 0.12,
               ok and 0.12 or 0.08, ok and 0.09 or 0.08, 0.7)
      DrawText("[0]", px + 26, yy, 16, ok and 0.95 or 0.5, ok and 0.85 or 0.5, 0.55, 1)
      DrawText("油箱", px + 66, yy, 16, ok and 0.96 or 0.6, ok and 0.92 or 0.6, 0.82, 1)
      DrawText(string.format("Lv%d · 容量 %dL", lv, fuelMax()),
               px + 66, yy + 21, 12, 0.80, 0.80, 0.74, 1)
      local rightText = maxed and "满级 Lv3" or
        ("升级 Lv" .. lv .. "->" .. (lv + 1) .. "  " .. costText(cost))
      drawTextRight(rightText, px + pw - 24, yy + 8, 12,
                    (ok or maxed) and 0.85 or 1, (ok or maxed) and 0.82 or 0.35,
                    (ok or maxed) and 0.72 or 0.28)
      yy = yy + 54
    end
    -- 车体装甲升级（按 A）
    do
      local lv = (upgrades and upgrades.armor) or 0
      local maxed = lv >= 3
      local cost = { metal = 4 + lv * 2, cloth = 2 + lv }
      local ok = (not maxed) and canAfford(cost)
      DrawRect(px + 16, yy - 4, pw - 32, 52, ok and 0.10 or 0.12,
               ok and 0.12 or 0.08, ok and 0.09 or 0.08, 0.7)
      DrawText("[A]", px + 26, yy, 16, ok and 0.95 or 0.5, ok and 0.85 or 0.5, 0.55, 1)
      DrawText("车体装甲", px + 66, yy, 16, ok and 0.96 or 0.6, ok and 0.92 or 0.6, 0.82, 1)
      DrawText(string.format("Lv%d · 碰撞损失 -%d%%", lv, lv * 15),
               px + 66, yy + 21, 12, 0.80, 0.80, 0.74, 1)
      local rightText = maxed and "满级 Lv3" or
        ("升级 Lv" .. lv .. "->" .. (lv + 1) .. "  " .. costText(cost))
      drawTextRight(rightText, px + pw - 24, yy + 8, 12,
                    (ok or maxed) and 0.85 or 1, (ok or maxed) and 0.82 or 0.35,
                    (ok or maxed) and 0.72 or 0.28)
      yy = yy + 54
    end
    drawTextCentered("1-4 维修 / 5 货架 / 6 鱼竿 / 7 修理包 / 8 篷布 / 9 水箱 / 0 油箱 / A 装甲    E 关闭",
                     vw * 0.5, py + ph - 30, 13, 0.75, 0.75, 0.68, 1)
  end
end

-- 夜空星星透明度：只有深夜才明显（纯函数，自检用）
function starAlpha()
  local a = (nightAmount and nightAmount()) or 0
  if a <= 0.55 then return 0 end
  return math.min(1, (a - 0.55) / 0.45) * 0.9
end

-- 夜空：确定性星点 + 月亮（画在夜幕之后，避免被蓝色遮罩压掉）
-- 流星：每 14s 出现一次，持续 0.8s（返回进度和透明度；不在窗口返回 nil）
function shootingStar(t)
  local period = 14.0
  local localT = (t or 0) % period
  if localT > 0.8 then return nil end
  local p = localT / 0.8
  local a = math.sin(math.pi * p) * 0.85
  return p, a
end

function drawNightSky(vw, vh)
  local sa = starAlpha()
  if sa <= 0.01 then return end
  local t = ((weatherT or 0) * 0.6)
  for i = 1, 90 do
    local h1 = math.abs(math.sin(i * 12.9898) * 43758.5453) % 1
    local h2 = math.abs(math.sin(i * 78.233) * 12543.123) % 1
    local x = h1 * vw
    local y = h2 * vh * 0.46
    local tw = 0.55 + 0.45 * math.abs(math.sin(t + i * 1.7))
    local r = (i % 11 == 0) and 2.0 or 1.0
    DrawCircle(x, y, r, 1, 0.90, 0.93, 1.0, sa * tw * 0.8, true)
  end
  -- 月亮：柔光 + 圆盘
  local mx, my = vw * 0.76, vh * 0.16
  DrawCircle(mx, my, 34, 1, 0.55, 0.62, 0.80, sa * 0.10, true)
  DrawCircle(mx, my, 20, 1, 0.72, 0.78, 0.92, sa * 0.16, true)
  DrawCircle(mx, my, 12, 2, 0.94, 0.95, 0.88, sa * 0.85, true)
  DrawCircle(mx + 4, my - 3, 10, 1, 0.02, 0.04, 0.10, sa * 0.35, true)
  -- 流星：偶尔划过一道短线
  local sp, sAlpha = shootingStar(weatherT or 0)
  if sp ~= nil then
    local x0 = vw * (0.16 + 0.48 * sp)
    local y0 = vh * (0.10 + 0.16 * sp)
    DrawLine(x0, y0, x0 - 54, y0 - 16, 2, 1.0, 1.0, 1.0, sAlpha * sa)
    DrawLine(x0, y0, x0 - 20, y0 - 6, 1, 1.0, 1.0, 1.0, sAlpha * sa * 0.7)
  end
end

-- 太阳弧线：返回屏幕比例坐标（06:00 东边、12:00 最高、18:00 西边）
function sunArc(clockMin)
  local t = math.max(0, math.min(1, ((clockMin or 720) - 6 * 60) / (12 * 60)))
  return 0.14 + 0.72 * t, 0.40 - math.sin(math.pi * t) * 0.26
end

-- 白天天空：太阳光晕 + 缓慢飘动的云（画在天气层之下、场景之上）
-- 日出/日落暖色强度（0–1）：清晨 5–8 点、傍晚 16–20 点最强
function skyWarmth(clockMin)
  local hour = (clockMin or 720) / 60
  local warm = 0
  if hour >= 5 and hour < 8 then
    warm = 1 - math.abs(hour - 6.5) / 1.5
  elseif hour >= 16 and hour < 20 then
    warm = 1 - math.abs(hour - 18) / 2
  end
  return math.max(0, math.min(1, warm))
end

function drawDaySky()
  local vp = GetViewportSize()
  local vw = (vp and vp.w) or 1280
  local vh = (vp and vp.h) or 720
  local a = (nightAmount and nightAmount()) or 0
  local day = 1 - a
  if day <= 0.02 then return end
  local vis = day
  if weather == "rain" or weather == "fog" then
    vis = vis * 0.12
  elseif weather == "snow" then
    vis = vis * 0.25
  elseif weather == "cloudy" then
    vis = vis * 0.45
  end
  if vis <= 0.02 then return end
  local fx, fy = sunArc(clock or 720)
  local sx, sy = vw * fx, vh * fy
  -- 日出/日落：太阳和云染成暖橙色，并加一层地平线辉光
  local warm = skyWarmth(clock or 720)
  local sr = 1.0
  local sg = 0.96 - 0.34 * warm
  local sb = 0.80 - 0.45 * warm
  DrawCircle(sx, sy, 54, 1, sr, sg, sb, 0.10 * vis, true)
  DrawCircle(sx, sy, 30, 1, sr, sg, sb, 0.16 * vis, true)
  DrawCircle(sx, sy, 15, 2, 1.0, 0.96 - 0.20 * warm, 0.80 - 0.35 * warm,
             0.92 * vis, true)
  if warm > 0.05 then
    DrawCircle(sx, vh * 0.46, vh * 0.40, 1, 1.0, 0.58, 0.34,
               0.07 * warm * vis, true)
  end
  local cr = 0.94 + 0.06 * warm
  local cg = 0.96 - 0.10 * warm
  local cb = 0.98 - 0.20 * warm
  local drift = ((weatherT or 0) * 6.0) % (vw + 300)
  for i = 1, 4 do
    local cx = (i * 331 + drift) % (vw + 260) - 130
    local cy = vh * (0.10 + 0.05 * i) + math.sin(i * 1.7) * 12
    local al = (0.10 + 0.02 * (i % 2)) * vis
    DrawCircle(cx, cy, 42, 1, cr, cg, cb, al, true)
    DrawCircle(cx + 38, cy + 6, 30, 1, cr, cg, cb, al * 0.9, true)
    DrawCircle(cx - 34, cy + 8, 26, 1, cr, cg, cb, al * 0.8, true)
  end
end

-- 雨停后的彩虹：白天可见，随时间淡出
function rainbowAlpha()
  local t = rainbowT or 0
  if t <= 0 then return 0 end
  local day = 1 - ((nightAmount and nightAmount()) or 0)
  return math.min(1, t / 2) * day * 0.5
end

function drawRainbow()
  local a = rainbowAlpha()
  if a <= 0.01 then return end
  local vp = GetViewportSize()
  local vw = (vp and vp.w) or 1280
  local vh = (vp and vp.h) or 720
  local cx, cy = vw * 0.30, vh * 1.12
  local bands = {
    { 1.00, 0.35, 0.30 }, { 1.00, 0.68, 0.30 }, { 0.98, 0.90, 0.35 },
    { 0.38, 0.86, 0.48 }, { 0.38, 0.68, 0.96 }, { 0.56, 0.46, 0.92 },
  }
  for bi, c in ipairs(bands) do
    local r = vh * 0.72 - bi * 6
    local prevX, prevY = nil, nil
    for i = 0, 40 do
      local ang = math.pi + (i / 40) * math.pi
      local x = cx + math.cos(ang) * r
      local y = cy + math.sin(ang) * r
      if prevX ~= nil then
        DrawLine(prevX, prevY, x, y, 4, c[1], c[2], c[3], a)
      end
      prevX, prevY = x, y
    end
  end
end

-- 夜间疲劳：屏幕四周逐渐变暗并随呼吸节奏轻微脉动
function drawFatigueVignette()
  if fatigueAlpha == nil then return end
  local a = fatigueAlpha(nightDriveMin or 0)
  if a <= 0.01 then return end
  local vp = GetViewportSize()
  local vw = (vp and vp.w) or 1280
  local vh = (vp and vp.h) or 720
  local pulse = 0.82 + 0.18 * math.sin((weatherT or 0) * 1.6)
  local alpha = 0.38 * a * pulse
  local band = 70
  DrawRect(0, 0, vw, band, 0.02, 0.03, 0.06, alpha)
  DrawRect(0, vh - band, vw, band, 0.02, 0.03, 0.06, alpha)
  DrawRect(0, 0, band, vh, 0.02, 0.03, 0.06, alpha)
  DrawRect(vw - band, 0, band, vh, 0.02, 0.03, 0.06, alpha)
  if a > 0.55 then
    drawTextCentered("眼皮有点沉……停车睡一觉或喝杯热咖啡",
                     math.floor(vw * 0.5), vh - 46, 12,
                     0.86, 0.88, 0.96, 0.75 * a)
  end
end

function drawNightOverlay()
  local a = nightAmount()
  if a <= 0.01 then return end
  local vp = GetViewportSize()
  local vw = (vp and vp.w) or 1280
  local vh = (vp and vp.h) or 720
  DrawRect(0, 0, vw, vh, 0.02, 0.04, 0.10, 0.46 * a)
  drawNightSky(vw, vh)
  if isNight() then
    drawTextCentered("夜深了 — 回车里睡觉，或放慢车速", math.floor(vw * 0.5), vh - 34,
                     13, 0.72, 0.78, 0.95, 0.85 * a)
  end
end

-- 旅行日志面板（J）：最近 12 条事件，带天数/时间
function drawJournal()
  if not journalOpen then return end
  local vp = GetViewportSize()
  local vw = (vp and vp.w) or 1280
  local vh = (vp and vp.h) or 720
  local pw, ph = 640, 440
  local px = math.floor((vw - pw) * 0.5)
  local py = math.floor((vh - ph) * 0.5)
  DrawRect(px, py, pw, ph, 0.04, 0.05, 0.06, 0.94)
  DrawRectOutline(px, py, pw, ph, 0.55, 0.52, 0.42, 0.95)
  drawTextCentered("旅行日志", vw * 0.5, py + 12, 20, 0.98, 0.90, 0.68, 1)
  DrawText("J / E 关闭", px + pw - 108, py + 16, 12, 0.68, 0.70, 0.64, 1)
  local summary = (journalSummaryText and journalSummaryText()) or
    string.format("货运 %d 单 · 里程 %.1fkm · 钓鱼 %d 条 · 探索 %d/%d",
                  jobDelivered or 0, (tripDistance or 0) / 1000, fishCaught or 0,
                  visitedCount or 0, #NODES)
  drawTextCentered(summary, vw * 0.5, py + 42, 11, 0.78, 0.80, 0.72, 1)
  local n = #journal
  local show = math.min(12, n)
  local yy = py + 70
  if n == 0 then
    DrawText("还没有记录——去跑一单、钓条鱼或探索新地点",
             px + 24, yy, 13, 0.72, 0.74, 0.68, 1)
  else
    for i = n - show + 1, n do
      local e = journal[i]
      local hh = math.floor((e.clock or 0) / 60)
      local mm = math.floor((e.clock or 0) % 60)
      local col = { 0.80, 0.82, 0.76 }
      if e.kind == "job" then col = { 0.86, 0.80, 0.58 }
      elseif e.kind == "fail" then col = { 0.92, 0.56, 0.44 }
      elseif e.kind == "goal" then col = { 0.62, 0.86, 0.62 }
      elseif e.kind == "fish" then col = { 0.58, 0.80, 0.92 }
      elseif e.kind == "explore" then col = { 0.86, 0.72, 0.52 }
      end
      DrawText(string.format("第%d天 %02d:%02d", e.day or 1, hh, mm),
               px + 22, yy, 12, 0.60, 0.62, 0.56, 1)
      DrawText(e.text or "", px + 132, yy, 13, col[1], col[2], col[3], 1)
      yy = yy + 28
    end
  end
  DrawText(string.format("共 %d 条记录（最多保留 30 条）", n),
           px + 22, py + ph - 26, 12, 0.64, 0.66, 0.60, 1)
end

-- 鱼类图鉴（K）：各鱼种纪录、条数与尺寸区间
function drawFishBook()
  if not fishBookOpen then return end
  local vp = GetViewportSize()
  local vw = (vp and vp.w) or 1280
  local vh = (vp and vp.h) or 720
  local rows = (fishBookRows and fishBookRows()) or {}
  local pw, ph = 600, 130 + #rows * 38
  local px = math.floor((vw - pw) * 0.5)
  local py = math.floor((vh - ph) * 0.5)
  DrawRect(px, py, pw, ph, 0.04, 0.05, 0.06, 0.94)
  DrawRectOutline(px, py, pw, ph, 0.55, 0.52, 0.42, 0.95)
  drawTextCentered("鱼类图鉴", vw * 0.5, py + 12, 20, 0.98, 0.90, 0.68, 1)
  local found = 0
  for _, r in ipairs(rows) do
    if (r.count or 0) > 0 then found = found + 1 end
  end
  DrawText(string.format("已记录 %d / %d 种   累计 %d 条", found, #rows,
                         fishCaught or 0),
           px + 22, py + 46, 13, 0.78, 0.82, 0.74, 1)
  DrawText("K / E 关闭", px + pw - 96, py + 18, 12, 0.68, 0.70, 0.64, 1)
  local yy = py + 82
  for _, r in ipairs(rows) do
    local caught = (r.count or 0) > 0
    local col = caught and 0.90 or 0.52
    DrawRect(px + 18, yy - 4, pw - 36, 34, caught and 0.08 or 0.10, 0.09, 0.08, 0.7)
    local label = r.name .. (r.hint and (" · " .. r.hint) or "")
    DrawText(label, px + 30, yy, 14, col, col, caught and 0.82 or col, 1)
    DrawText(string.format("纪录 %s", caught and
             string.format("%.0fcm", r.record) or "—"),
             px + 180, yy + 2, 13, col, col, caught and 0.72 or col, 1)
    DrawText(string.format("%d 条", r.count or 0),
             px + 330, yy + 2, 13, col, col, caught and 0.72 or col, 1)
    DrawText(string.format("尺寸 %.0f–%.0fcm", r.min or 0, r.max or 0),
             px + 410, yy + 2, 12, 0.66, 0.68, 0.62, 1)
    yy = yy + 38
  end
  DrawText(string.format("总纪录：%s %.0fcm", bestFishName or "—", bestFish or 0),
           px + 22, py + ph - 28, 12, 0.82, 0.76, 0.54, 1)
end

-- 操作说明（P）：驾驶/步行/工作台按键与生存提示
local HELP_SECTIONS = {
  { title = "驾驶", lines = {
      "W / S      油门 / 刹车",
      "A / D      左转 / 右转",
      "空格       停车",
      "Shift      手刹",
      "C          切换驾驶席/追逐视角",
      "H          远光灯",
      "X          喇叭（吓退附近感染者）",
      "Y          呼叫拖车（4 金属，回营地）",
      "R          车载电台（引擎声/电台）",
      "V          定速巡航（再按取消）",
      "G          喷玻璃水（消耗 1L 水）",
    } },
  { title = "步行 / 营地", lines = {
      "WASD       移动，鼠标张望",
      "E          交互（上车/搜箱/篝火菜单/货运板/收音机）",
      "T          回到房车旁上路",
      "B          合作改装/建造",
      "R          现场维修（消耗修理包）",
      "G          篝火添柴（消耗木材）",
      "F / L      手电 / 车内灯",
      "C          防滑链取下/装上",
      "X          喇叭（需在房车旁）",
      "Q          水塘取浑浊水",
      "1 / 2 / 3  吃 / 喝 / 睡觉",
      "4          在床上小睡 2 小时",
      "0          放弃当前货运合同",
      "联机客机：T 添柴 / V 采浆果",
    } },
  { title = "界面", lines = {
      "M          世界地图",
      "J          旅行日志",
      "K          鱼类图鉴",
      "I          背包",
      "O          车辆保养",
      "P / E      开关本说明",
    } },
  { title = "工作台", lines = {
      "1-4        维修 车体/引擎/轮胎/电瓶",
      "5 / 6      升级货架 / 鱼竿",
      "7 / 8      制作修理包 / 防雨篷布",
      "9 / 0      升级水箱 / 油箱",
      "A          升级车体装甲",
    } },
}

-- 帮助面板文本汇总（自检用）
function helpText()
  local parts = {}
  for _, sec in ipairs(HELP_SECTIONS) do
    for _, line in ipairs(sec.lines) do parts[#parts + 1] = line end
  end
  return table.concat(parts, "\n")
end

-- 帮助面板布局：按列累计行数，避免章节互相覆盖
function helpPanelLayout()
  local colLines = { 0, 0 }
  for i, sec in ipairs(HELP_SECTIONS) do
    local col = (i - 1) % 2 + 1
    colLines[col] = colLines[col] + 1 + #sec.lines +
                    (colLines[col] > 0 and 1 or 0)
  end
  local maxLines = math.max(colLines[1], colLines[2])
  local lineH = 19
  local ph = 82 + maxLines * lineH + 36
  return ph, lineH, colLines
end

function drawHelp()
  if not helpOpen then return end
  local vp = GetViewportSize()
  local vw = (vp and vp.w) or 1280
  local vh = (vp and vp.h) or 720
  local pw = 860
  local ph, lineH = helpPanelLayout()
  local px = math.floor((vw - pw) * 0.5)
  local py = math.floor((vh - ph) * 0.5)
  DrawRect(px, py, pw, ph, 0.04, 0.05, 0.06, 0.95)
  DrawRectOutline(px, py, pw, ph, 0.55, 0.52, 0.42, 0.95)
  drawTextCentered("操作说明", vw * 0.5, py + 12, 20, 0.98, 0.90, 0.68, 1)
  DrawText("P / E 关闭", px + pw - 100, py + 16, 12, 0.68, 0.70, 0.64, 1)
  local colW = (pw - 48) * 0.5
  local colOff = { 0, 0 }
  for i, sec in ipairs(HELP_SECTIONS) do
    local col = (i - 1) % 2
    local x = px + 24 + col * (colW + 24)
    local y = py + 52 + colOff[col + 1] * lineH
    DrawText(sec.title, x, y, 15, 0.96, 0.84, 0.56, 1)
    for j, line in ipairs(sec.lines) do
      DrawText(line, x + 6, y + 26 + (j - 1) * lineH, 12, 0.80, 0.82, 0.76, 1)
    end
    colOff[col + 1] = colOff[col + 1] + 1 + #sec.lines + 1
  end
  drawTextCentered("提示：营地油/水免费；加油站加油消耗金属；夜间/雨天送达有额外报酬",
                   vw * 0.5, py + ph - 30, 12, 0.82, 0.76, 0.54, 1)
end

-- 地图地点服务标签（帮助快速找到营地/加油站/诊所等）
function mapServiceTag(nd)
  if nd == nil then return nil end
  local id = nd.id or ""
  local nm = nd.name or ""
  if id == "camp" then return "营" end
  if id == "gasstop" or nm:find("加油站", 1, true) then return "油" end
  if id == "clinic" or nm:find("诊所", 1, true) or nm:find("医院", 1, true) then
    return "医"
  end
  if id == "depot" or nm:find("仓库", 1, true) then return "仓" end
  if id == "mall" or nm:find("商场", 1, true) then return "商" end
  if id == "farm" or nm:find("农场", 1, true) then return "农" end
  if id == "lakecamp" or nm:find("湖边", 1, true) then return "湖" end
  if id == "town" or nm:find("小镇", 1, true) then return "镇" end
  if nm:find("木屋", 1, true) then return "屋" end
  if nm:find("邮局", 1, true) then return "邮" end
  return nil
end

function drawMapHud()
  local vp = GetViewportSize()
  local vw = (vp and vp.w) or 1280
  local vh = (vp and vp.h) or 720
  DrawRect(0, 0, vw, vh, 0.02, 0.03, 0.04, 0.55)
  local pw, ph = 560, 420
  local px = math.floor((vw - pw) * 0.5)
  local py = math.floor((vh - ph) * 0.5)
  DrawRect(px, py, pw, ph, 0.07, 0.08, 0.09, 0.93)
  DrawRectOutline(px, py, pw, ph, 0.55, 0.52, 0.42, 0.95)
  DrawText("公路地图", px + 16, py + 10, 16, 0.95, 0.88, 0.7, 1)
  if weatherForecastText ~= nil then
    DrawText(weatherForecastText(), px + 100, py + 14, 12, 0.78, 0.82, 0.74, 1)
  end
  drawTextRight("M 关闭", px + pw - 16, py + 13, 13, 0.6, 0.6, 0.55, 1)

  -- 动态边界：包含所有节点与房车
  local wx0, wx1, wz0, wz1 = rv.x, rv.x, rv.z, rv.z
  for _, nd in ipairs(NODES) do
    wx0 = math.min(wx0, nd.x); wx1 = math.max(wx1, nd.x)
    wz0 = math.min(wz0, nd.z); wz1 = math.max(wz1, nd.z)
  end
  local padw = math.max(40, (wx1 - wx0) * 0.12)
  wx0, wx1 = wx0 - padw, wx1 + padw
  wz0, wz1 = wz0 - padw, wz1 + padw
  local function mapXY(x, z)
    return px + 26 + (x - wx0) / math.max(1, wx1 - wx0) * (pw - 52),
           py + 44 + (z - wz0) / math.max(1, wz1 - wz0) * (ph - 84)
  end
  for _, nd in ipairs(NODES) do
    for _, lid in ipairs(nd.links or {}) do
      local o = nodeById(lid)
      if o then
        local ax, ay = mapXY(nd.x, nd.z)
        local bx2, by2 = mapXY(o.x, o.z)
        DrawLine(ax, ay, bx2, by2, 2, 0.42, 0.46, 0.4, 0.85)
      end
    end
  end
  for _, nd in ipairs(NODES) do
    local mx2, my2 = mapXY(nd.x, nd.z)
    local cur = (nd.id == (atNode or curNode))
    local danger = nd.danger or 0
    local known = (mapNodeKnown == nil) or mapNodeKnown(nd)
    if known then
      DrawCircle(mx2, my2, cur and 7 or 5, cur and 2 or 1.5,
                 0.45 + danger * 0.17, 0.85 - danger * 0.18, 0.32, 1, true)
      local tag = mapServiceTag(nd)
      if tag ~= nil then
        drawTextCentered(tag, mx2 + 13, my2 - 5, 10, 0.95, 0.86, 0.52, 0.95)
      end
      drawTextCentered(nd.name .. (danger > 0 and string.rep("!", danger) or ""),
                       mx2, my2 - 20, 12, cur and 1 or 0.82,
                       cur and 0.9 or 0.78, cur and 0.6 or 0.68, 1)
    else
      -- 未探索的程序化地点：只给一个灰色“?”，去过之后才显示名字
      DrawCircle(mx2, my2, 4, 1.5, 0.55, 0.57, 0.55, 0.9, false)
      drawTextCentered("?", mx2, my2 - 18, 13, 0.62, 0.64, 0.62, 1)
    end
  end
  local rx, ry = mapXY(rv.x, rv.z)
  DrawLine(rx, ry, rx + math.sin(rv.yaw) * 20, ry + math.cos(rv.yaw) * 20,
           2, 0.98, 0.85, 0.4, 1)
  DrawCircle(rx, ry, 4, 2, 0.98, 0.85, 0.4, 1, true)
  -- 当前货运合同：地图上标出当前站（高亮）与双站的另一站
  if job ~= nil then
    local stops = job.stops or { { id = job.destId, name = job.destName } }
    local cur = job.stopIndex or 1
    -- 路线线：从房车画到当前目标站
    local activeNd = nodeById((stops[cur] or stops[1]).id)
    if activeNd ~= nil then
      local ax2, ay2 = mapXY(activeNd.x, activeNd.z)
      DrawLine(rx, ry, ax2, ay2, 2, 0.95, 0.78, 0.32, 0.55)
    end
    for i, s in ipairs(stops) do
      local nd2 = nodeById(s.id)
      if nd2 then
        local jx, jy = mapXY(nd2.x, nd2.z)
        local active = (i == cur)
        DrawCircle(jx, jy, active and 12 or 9, 2,
                   1.0, active and 0.80 or 0.58, active and 0.28 or 0.30,
                   active and 1.0 or 0.65, false)
        drawTextCentered(#stops > 1 and tostring(i) or "货运目标",
                         jx, jy + 13, 12,
                         active and 1.0 or 0.70, active and 0.88 or 0.68,
                         0.50, 1)
      end
    end
  end
  -- 鱼笼位置：成熟时青色高亮
  if pondTrap ~= nil then
    local tx, ty = mapXY(pondTrap.x, pondTrap.z)
    local ready = pondTrap.ready == true
    DrawCircle(tx, ty, ready and 7 or 5, 1.5,
               ready and 0.42 or 0.60, ready and 0.95 or 0.60,
               ready and 0.80 or 0.60, 1, ready)
    drawTextCentered(ready and "鱼笼（可收）" or "鱼笼",
                     tx, ty + 13, 11,
                     ready and 0.55 or 0.66, ready and 0.92 or 0.66,
                     ready and 0.78 or 0.66, 1)
  end
  -- 路边补给箱：未搜刮时标出位置
  for _, c in ipairs(roadCaches or {}) do
    if c.ready then
      local cx2, cy2 = mapXY(c.x, c.z)
      DrawCircle(cx2, cy2, 5, 1.5, 0.92, 0.72, 0.36, 0.95, false)
      drawTextCentered("补给箱", cx2, cy2 + 12, 10, 0.86, 0.72, 0.46, 1)
    end
  end
  -- 未读路牌笔记：已探索地点才标出
  for _, n in ipairs(loreNotes or {}) do
    if loreNoteMapVisible ~= nil and loreNoteMapVisible(n) then
      local nx2, ny2 = mapXY(n.x, n.z)
      DrawCircle(nx2, ny2, 4, 1.5, 0.95, 0.95, 0.88, 0.95, true)
      drawTextCentered("笔记", nx2, ny2 + 12, 10, 0.90, 0.90, 0.82, 1)
    end
  end
  DrawText("● 房车  ● 地点（越红越危险）  ? 未探索  — 公路  ◎ 货运目标  营/油/医/仓=服务",
           px + 16, py + ph - 24, 11, 0.7, 0.7, 0.65, 1)
  local exploreText = string.format("已探索 %d / %d 个地点", visitedCount or 0, #NODES)
  if loreProgressText ~= nil then
    local lp = loreProgressText()
    if lp ~= nil then exploreText = exploreText .. " · " .. lp end
  end
  DrawText(exploreText, px + pw - 16, py + ph - 24, 12, 0.7, 0.7, 0.65, 1, true)
  if fishingRecordText ~= nil then
    drawTextCentered(fishingRecordText(), px + pw * 0.5, py + ph - 42, 12,
                     0.72, 0.76, 0.68, 1)
  end
  if milestoneHint ~= nil then
    local hint = milestoneHint()
    if hint ~= nil and hint ~= "" then
      drawTextCentered("下一里程碑  " .. hint, px + pw * 0.5, py + ph - 60, 12,
                       0.78, 0.74, 0.58, 1)
    end
  end
  if jobHistory ~= nil and #jobHistory > 0 then
    DrawText("最近货运", px + 20, py + ph - 96, 12, 0.82, 0.78, 0.62, 1)
    local shown = math.min(3, #jobHistory)
    for i = 1, shown do
      local line = jobHistory[#jobHistory - shown + i]
      DrawText(line, px + 20, py + ph - 78 + (i - 1) * 16, 11,
               0.72, 0.76, 0.70, 1)
    end
  end
  if dailyGoal ~= nil then
    local p = math.min(dailyGoal.progress or 0, dailyGoal.target or 1)
    DrawText(string.format("今日目标：%s  %d/%d%s", dailyGoal.name or "",
             p, dailyGoal.target or 0,
             dailyGoal.done and "  ✓" or ""),
             px + 20, py + ph - 114, 12,
             dailyGoal.done and 0.55 or 0.92,
             dailyGoal.done and 0.85 or 0.82,
             dailyGoal.done and 0.55 or 0.55, 1)
  end
end

-- ===========================================================================
-- 顺路货运合同 HUD
-- ===========================================================================

-- 驾车/步行时顶部紧凑追踪条：货物、目的地、剩余距离、报酬
function drawJobTracker(vw)
  if job == nil then return end
  local dist, nd = jobRemaining()
  local done = job.distance and job.distance > 1
                and math.max(0, 1 - dist / job.distance) or 0
  local bw = 430
  local bx = math.floor((vw - bw) * 0.5)
  DrawRect(bx, 44, bw, 50, 0.05, 0.06, 0.07, 0.78)
  DrawRectOutline(bx, 44, bw, 50, 0.72, 0.62, 0.38, 0.95)
  local urgentTag = ""
  local tracR, tracG, tracB = 0.96, 0.90, 0.72
  if job.urgent and job.deadlineAbs ~= nil then
    local remain = math.max(0, job.deadlineAbs - jobNowAbs())
    urgentTag = string.format("  [急 %d分]", math.floor(remain + 0.5))
    if remain <= 60 then tracR, tracG, tracB = 1.0, 0.52, 0.40 end
  end
  local fragTag = ""
  if job.fragile then
    fragTag = " [易碎]"
  elseif job.livestock then
    fragTag = " [活畜]"
  elseif job.hazmat then
    fragTag = " [危险品]"
  end
  local stopTag = ""
  if job.stops ~= nil then
    stopTag = string.format(" [%d/%d]", job.stopIndex or 1, #job.stops)
  end
  DrawText(string.format("货运%s%s%s  %s x%d  →  %s", urgentTag, fragTag, stopTag,
           job.cargo, job.crates, job.destName or (nd and nd.name) or "?"),
           bx + 12, 50, 13, tracR, tracG, tracB, 1)
  local payText = jobPayText(job.pay or {})
  if (job.fuel or 0) > 0 then
    payText = payText .. (payText ~= "" and "  " or "") .. "燃料+" .. job.fuel .. "L"
  end
  local cond = math.max(0, math.min(1, job.condition or 1.0))
  local cr, cg, cb = 0.80, 0.84, 0.78
  if cond < 0.5 then
    cr, cg, cb = 0.95, 0.42, 0.34
  elseif cond < 0.8 then
    cr, cg, cb = 0.95, 0.76, 0.36
  end
  local bonusTags = (jobDeliveryBonusTags and jobDeliveryBonusTags()) or ""
  if bonusTags ~= "" then bonusTags = " · " .. bonusTags end
  local eta = (jobEtaMinutes and jobEtaMinutes(dist, rv.speed)) or 0
  DrawText(string.format("%.0fm · 约 %d 分钟 · 货况 %.0f%% · %s%s",
           dist, eta, cond * 100, payText, bonusTags),
           bx + 12, 68, 12, cr, cg, cb, 1)
  DrawRect(bx + 12, 84, bw - 24, 3, 0.14, 0.15, 0.14, 0.9)
  DrawRect(bx + 12, 84, (bw - 24) * done, 3, 0.78, 0.66, 0.32, 1)
  drawJobWaypoint(vw)
end

-- 交付/失败结算卡：屏幕中上方的短时面板
function drawJobResult()
  if jobResult == nil then return end
  local vp = GetViewportSize()
  local vw = (vp and vp.w) or 1280
  local vh = (vp and vp.h) or 720
  local ok = jobResult.ok == true
  local pw, ph = 430, 148
  local px = math.floor((vw - pw) * 0.5)
  local py = math.floor(vh * 0.30)
  local a = math.min(1, (jobResult.t or 0) * 2)
  DrawRect(px, py, pw, ph, 0.05, 0.06, 0.07, 0.92 * a)
  DrawRectOutline(px, py, pw, ph,
                  ok and 0.72 or 0.85, ok and 0.62 or 0.32, 0.30, a)
  drawTextCentered(ok and "货运完成" or "合同超时", vw * 0.5, py + 12, 22,
                   ok and 0.98 or 1.0, ok and 0.90 or 0.48, ok and 0.62 or 0.40, a)
  DrawText(string.format("%s x%d", jobResult.cargo or "货物", jobResult.crates or 0),
           px + 20, py + 48, 15, 0.90, 0.90, 0.84, a)
  if ok then
    local cond = math.max(0, math.min(1, jobResult.cond or 1))
    DrawText(string.format("货况 %.0f%%", cond * 100), px + 20, py + 72, 13,
             cond < 0.5 and 0.95 or 0.80, cond < 0.5 and 0.42 or 0.82,
             cond < 0.5 and 0.34 or 0.74, a)
    DrawRect(px + 20, py + 92, pw - 40, 5, 0.14, 0.15, 0.14, 0.9 * a)
    DrawRect(px + 20, py + 92, (pw - 40) * cond, 5,
             cond < 0.5 and 0.9 or 0.72, cond < 0.5 and 0.3 or 0.62, 0.28, a)
    DrawText(string.format("报酬 %s%s%s%s%s%s%s%s%s%s%s%s", jobResult.got or "",
             (jobResult.fuel or 0) > 0 and
               string.format("   燃料+%dL", jobResult.fuel) or "",
             jobResult.item ~= nil and
               ("   " .. itemName(jobResult.item) .. "+1") or "",
             jobResult.bonus and "   （今日首单 +25%）" or "",
             jobResult.night and "   （夜间 +10%）" or "",
             jobResult.rain and "   （雨天 +8%）" or "",
             jobResult.snow and "   （雪天 +12%）" or "",
             jobResult.fog and "   （雾天 +6%）" or "",
             jobResult.eco and "   （省油 +5%）" or "",
             jobResult.pristine and "   （完好 +5%）" or "",
             jobResult.early and "   （急单提前 +10%）" or "",
             (jobResult.streak or 0) >= 3 and
               string.format("   （连送 %d 单）", jobResult.streak) or ""),
             px + 20, py + 106, 13, 0.88, 0.86, 0.72, a)
    drawTextCentered(string.format("累计完成 %d 单", jobResult.jobs or 0),
                     vw * 0.5, py + 128, 12, 0.72, 0.76, 0.70, a)
  else
    DrawText("货物已退回，报酬取消", px + 20, py + 74, 14, 0.86, 0.70, 0.60, a)
    DrawText("下次注意合同上的限时提示", px + 20, py + 100, 12, 0.70, 0.72, 0.66, a)
  end
end

function drawJobWaypoint(vw)
  if job == nil then return end
  local nd = nodeById(job.destId)
  if nd == nil then return end
  local vp = GetViewportSize()
  local vh = (vp and vp.h) or 720
  local sp = WorldToScreen(nd.x, 3.4, nd.z)
  local dist = jobRemaining()
  local onScreen = sp ~= nil and sp.x >= 34 and sp.x <= vw - 34 and
                   sp.y >= 104 and sp.y <= vh - 90
  if onScreen then
    DrawLine(sp.x, sp.y - 10, sp.x, sp.y - 24, 2, 1.0, 0.82, 0.32, 0.9)
    DrawCircle(sp.x, sp.y, 9, 2, 1.0, 0.82, 0.32, 1, false)
    DrawCircle(sp.x, sp.y, 4, 2, 1.0, 0.82, 0.32, 1, true)
    drawTextCentered(string.format("货运目标 · %s · %.0fm", nd.name or "", dist),
                     sp.x, sp.y - 42, 13, 1.0, 0.90, 0.55, 1)
    return
  end
  -- 目标在视野外：屏幕中上方给一个指向箭头 + 距离
  local px, pz, viewYaw
  if phase == "driving" then
    px, pz = rv.x, rv.z
    viewYaw = rv.yaw + (driveLookYaw or 0)
  else
    px, pz, viewYaw = player.x, player.z, player.yaw
  end
  local delta = jobBearingDelta(px, pz, viewYaw)
  local dx, dy = -math.sin(delta), -math.cos(delta)
  local cx, cy = vw * 0.5, vh * 0.5
  local ax, ay = cx + dx * 150, cy + dy * 110
  DrawLine(ax - dx * 12 - dy * 9, ay - dy * 12 + dx * 9, ax, ay,
           3, 1.0, 0.82, 0.32, 0.95)
  DrawLine(ax - dx * 12 + dy * 9, ay - dy * 12 - dx * 9, ax, ay,
           3, 1.0, 0.82, 0.32, 0.95)
  drawTextCentered(string.format("%s  %.0fm", nd.name or "货运目标", dist),
                   ax, ay + 14, 12, 1.0, 0.88, 0.52, 0.95)
end

-- 营地货运板菜单：列 3 张合同，1-3 接单，0 放弃当前，E 关闭
-- 货运板布局：按合同数/交易数动态算高度，避免交易列表溢出
function jobPanelLayout(offerCount, tradeCount, hasJob)
  local rows = math.max(1, offerCount or 0)
  local startY = hasJob and 112 or 70
  local trades = math.max(0, tradeCount or 0)
  local ph = startY + rows * 62 + 26 + trades * 30 + 52
  return ph, startY
end

function drawJobPanel()
  local vp = GetViewportSize()
  local vw = (vp and vp.w) or 1280
  local vh = (vp and vp.h) or 720
  local rows = math.max(1, #jobOffers)
  local tradeRows = (TRADES ~= nil) and #TRADES or 0
  local pw = 620
  local ph, startY = jobPanelLayout(rows, tradeRows, job ~= nil)
  local px = math.floor((vw - pw) * 0.5)
  local py = math.floor((vh - ph) * 0.5)
  DrawRect(px, py, pw, ph, 0.04, 0.05, 0.06, 0.93)
  DrawRectOutline(px, py, pw, ph, 0.55, 0.52, 0.42, 0.95)
  drawTextCentered("货运板 · 顺路合同", vw * 0.5, py + 12, 20, 0.98, 0.90, 0.68, 1)
  DrawText("把货送到指定地点即可结算；载重会拉低加速与极速，也增加油耗",
           px + 20, py + 44, 12, 0.74, 0.74, 0.68, 1)
  local bonusAvail = (jobDailyBonusAvailable == nil) or jobDailyBonusAvailable()
  DrawText(bonusAvail and "今日首单 +25% 待领取" or "今日首单加成已用",
           px + 20, py + 60, 12,
           bonusAvail and 0.95 or 0.64, bonusAvail and 0.82 or 0.66,
           bonusAvail and 0.45 or 0.62, 1)
  if jobReputation ~= nil then
    local rep = jobReputation()
    local rank = (jobReputationRank and jobReputationRank()) or ""
    local repText = string.format("信誉 x%.2f · %s（%d 单）", rep, rank,
                                  jobDelivered or 0)
    if (jobStreak or 0) >= 3 then
      repText = string.format("信誉 x%.2f · %s · 连送 %d 单 x%.2f", rep,
                              rank, jobStreak,
                              jobStreakBonus and jobStreakBonus() or 1)
    end
    drawTextRight(repText, px + pw - 20, py + 60, 12, 0.80, 0.78, 0.62, 1)
  end

  if job ~= nil then
    local dist = jobRemaining()
    DrawRect(px + 16, py + 66, pw - 32, 34, 0.10, 0.09, 0.05, 0.9)
    DrawText(string.format("当前：%s x%d → %s（%.0fm）  按 0 放弃",
             job.cargo, job.crates, nodeName(job.destId), dist),
             px + 26, py + 74, 13, 1, 0.86, 0.52, 1)
  elseif #jobOffers == 0 then
    DrawText("这一带暂时没有可接的合同，跑远一点再回来看",
             px + 24, py + 78, 14, 0.68, 0.68, 0.62, 1)
  end

  local yy = py + startY
  for i, o in ipairs(jobOffers) do
    local hot = job == nil
    DrawRect(px + 16, yy, pw - 32, 54, hot and 0.09 or 0.06, 0.09, 0.10, 0.85)
    DrawRectOutline(px + 16, yy, pw - 32, 54,
                    hot and 0.62 or 0.36, hot and 0.56 or 0.34, 0.30, 0.9)
    DrawText(string.format("[%d]", i), px + 26, yy + 15, 16,
             hot and 0.98 or 0.5, hot and 0.86 or 0.5, hot and 0.52 or 0.48, 1)
    DrawText(string.format("%s%s%s%s%s%s%s  ·  %s x%d  ·  %.0fkg",
             (o.urgent and "[急] " or ""), (o.fragile and "[易碎] " or ""),
             (o.perishable and "[易腐] " or ""),
             (o.livestock and "[活畜] " or ""),
             (o.hazmat and "[危险品] " or ""),
             (o.stops and "[2站] " or ""), o.destName, o.cargo, o.crates, o.weight),
             px + 64, yy + 7, 14, 0.94, 0.92, 0.84, 1)
    local payText = jobPayText(o.pay or {})
    local fuelEst = (jobFuelEstimate ~= nil) and jobFuelEstimate(o.distance) or 0
    DrawText(string.format("距离 %.0fm   报酬 %s   燃料+%dL   往返约耗 %.1fL%s%s%s",
             o.distance, payText, o.fuel or 0, fuelEst,
             o.bonusItem ~= nil and ("   物品+" .. itemName(o.bonusItem)) or "",
             (o.danger or 0) >= 2 and "   危险" or "",
             o.urgent and "   限时" or ""),
             px + 64, yy + 28, 12, 0.78, 0.82, 0.76, 1)
    yy = yy + 62
  end
  -- 以物易物：把多余物资换成燃油/金属
  if TRADES ~= nil then
    DrawText("以物易物", px + 20, yy + 4, 14, 0.90, 0.84, 0.66, 1)
    yy = yy + 26
    for i, t in ipairs(TRADES) do
      local ok2 = (tradeAvailable == nil) or tradeAvailable(t)
      local keyLabel = (i <= 6) and tostring(i + 3)
                       or (i == 7 and "T")
                       or (i == 8 and "Y")
                       or "U"
      DrawRect(px + 16, yy - 4, pw - 32, 26,
               ok2 and 0.08 or 0.11, 0.09, 0.06, 0.7)
      DrawText("[" .. keyLabel .. "]", px + 26, yy, 14,
               ok2 and 0.95 or 0.55, ok2 and 0.85 or 0.55, 0.55, 1)
      DrawText(t.name, px + 66, yy, 13,
               ok2 and 0.90 or 0.60, ok2 and 0.88 or 0.62, 0.78, 1)
      yy = yy + 30
    end
  end
  DrawText("1-3 接单   0 放弃   4-9/T/Y/U 以物易物   R 刷新(1金属)   E 关闭",
           px + 20, py + ph - 26, 11, 0.70, 0.72, 0.66, 1)
end

-- 车辆保养面板（O）：各子系统状态 + 四轮胎压/胎温 + 服务费用提示
function drawMaintenancePanel()
  if not maintOpen then return end
  local vp = GetViewportSize()
  local vw = (vp and vp.w) or 1280
  local vh = (vp and vp.h) or 720
  local rows = maintenanceRows and maintenanceRows() or {}
  local pw, ph = 460, 108 + #rows * 24 + 96
  local px = math.floor((vw - pw) * 0.5)
  local py = math.floor((vh - ph) * 0.5)
  DrawRect(px, py, pw, ph, 0.04, 0.05, 0.06, 0.94)
  DrawRectOutline(px, py, pw, ph, 0.55, 0.52, 0.42, 0.95)
  drawTextCentered("车辆保养", vw * 0.5, py + 12, 20, 0.98, 0.90, 0.68, 1)
  DrawText("O 关闭", px + pw - 76, py + 16, 12, 0.68, 0.70, 0.64, 1)
  local yy = py + 46
  for i = 1, #rows, 2 do
    local a = rows[i]
    local b = rows[i + 1]
    local function cell(r, x)
      if r == nil then return end
      DrawText(r.label, x, yy, 13, 0.74, 0.76, 0.70, 1)
      DrawText(r.value, x + 62, yy, 13,
               r.ok and 0.78 or 1.0, r.ok and 0.92 or 0.45,
               r.ok and 0.74 or 0.32, 1)
    end
    cell(a, px + 22)
    cell(b, px + 236)
    yy = yy + 24
  end
  -- 四轮
  local wheels = wheelConditionRows and wheelConditionRows() or {}
  local names = { "左前", "右前", "左后", "右后" }
  yy = yy + 6
  DrawText("轮胎  压 / 温 / 磨损", px + 22, yy, 12, 0.90, 0.84, 0.66, 1)
  yy = yy + 20
  for i = 1, 4 do
    local w = wheels[i] or {}
    local bad = (w.pressure or 2.4) < 1.95
    DrawText(string.format("%s  %.1fbar  %.0f°C  %.0f%%",
             names[i] or ("轮" .. i), w.pressure or 2.4, w.temp or 22,
             w.wear or 100),
             px + 30 + ((i - 1) % 2) * 210, yy + math.floor((i - 1) / 2) * 20,
             12, bad and 1.0 or 0.78, bad and 0.45 or 0.86,
             bad and 0.32 or 0.74, 1)
  end
  yy = yy + 52
  local tips = {}
  if (oilLife or 100) < 40 then tips[#tips + 1] = "机油" end
  if (coolant or 100) < 40 then tips[#tips + 1] = "冷却液" end
  if (parts and parts.tires or 100) < 40 then tips[#tips + 1] = "轮胎" end
  local tip = (#tips > 0)
    and ("建议到加油站/工作台处理：" .. table.concat(tips, "、"))
    or "各系统状态良好，继续上路"
  DrawText(tip, px + 22, yy, 12, 0.80, 0.86, 0.72, 1)
  local cost = maintenanceCostText and maintenanceCostText() or nil
  if cost ~= nil then
    DrawText(cost, px + 22, yy + 20, 11, 0.86, 0.78, 0.52, 1)
  end
end

-- 床铺菜单：1 睡到早上 / 2 小睡 2 小时 / 0 或 E 关闭
function drawBedMenu()
  if not bedMenuOpen then return end
  local vp = GetViewportSize()
  local vw = (vp and vp.w) or 1280
  local vh = (vp and vp.h) or 720
  local pw, ph = 430, 220
  local px = math.floor((vw - pw) * 0.5)
  local py = math.floor((vh - ph) * 0.5)
  DrawRect(px, py, pw, ph, 0.04, 0.05, 0.06, 0.94)
  DrawRectOutline(px, py, pw, ph, 0.52, 0.52, 0.62, 0.95)
  drawTextCentered("床铺", vw * 0.5, py + 12, 20, 0.96, 0.92, 0.78, 1)
  DrawText("0 / E 关闭", px + pw - 96, py + 16, 12, 0.68, 0.70, 0.64, 1)
  DrawText(string.format("当前 第%d天 %02d:%02d   体力 %.0f",
           day or 1, math.floor((clock or 0) / 60) % 24,
           math.floor((clock or 0) % 60), needs.energy or 0),
           px + 20, py + 44, 12, 0.80, 0.82, 0.74, 1)
  DrawText("[1] 睡到早上 07:00（恢复最多，可能跨天）", px + 24, py + 78, 14,
           0.94, 0.88, 0.66, 1)
  DrawText("[2] 小睡 2 小时（少量恢复，不弹晨报）", px + 24, py + 112, 14,
           0.78, 0.86, 0.92, 1)
  DrawText("[3] 睡到傍晚 19:00（夜晚跑单）", px + 24, py + 146, 14,
           0.92, 0.80, 0.92, 1)
end

-- 篝火烹饪菜单：1-N 选择料理，0 / E 关闭
function drawCampfireMenu()
  if not campfireMenuOpen then return end
  local vp = GetViewportSize()
  local vw = (vp and vp.w) or 1280
  local vh = (vp and vp.h) or 720
  local list = campfireRecipeList and campfireRecipeList() or {}
  local pw, ph = 470, 100 + #list * 30
  local px = math.floor((vw - pw) * 0.5)
  local py = math.floor((vh - ph) * 0.5)
  DrawRect(px, py, pw, ph, 0.04, 0.05, 0.06, 0.94)
  DrawRectOutline(px, py, pw, ph, 0.62, 0.48, 0.30, 0.95)
  drawTextCentered("篝火烹饪", vw * 0.5, py + 12, 20, 0.98, 0.88, 0.64, 1)
  DrawText(string.format("1-%d 选择   0 / E 关闭", #list),
           px + pw - 186, py + 16, 12, 0.68, 0.70, 0.64, 1)
  DrawText(string.format("燃料 %.0f 分钟", campfireFuelMin or 0),
           px + 22, py + 44, 12, 0.80, 0.82, 0.72, 1)
  local yy = py + 70
  for i, rec in ipairs(list) do
    local ok2 = (campfireCan == nil) or campfireCan(rec.kind)
    DrawRect(px + 16, yy - 4, pw - 32, 26,
             ok2 and 0.08 or 0.11, 0.07, 0.05, 0.75)
    DrawText("[" .. i .. "]", px + 26, yy, 14,
             ok2 and 0.95 or 0.55, ok2 and 0.85 or 0.55, 0.55, 1)
    DrawText(rec.label, px + 66, yy, 13,
             ok2 and 0.92 or 0.58, ok2 and 0.88 or 0.60, 0.74, 1)
    yy = yy + 30
  end
end

