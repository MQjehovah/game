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
  DrawRect(x, y, w, 174, 0.06, 0.07, 0.08, 0.72)
  DrawRectOutline(x, y, w, 174, 0.46, 0.50, 0.42, 0.85)
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
  local fLow = fuel / VEH.tank < 0.2
  DrawText(string.format("燃油  %.0f / %d L", fuel, VEH.tank), x + 10, yy, 13,
           fLow and 1 or 0.86, fLow and 0.4 or 0.84, fLow and 0.3 or 0.78, 1)
  DrawRect(x + 10, yy + 17, w - 20, 4, 0.12, 0.12, 0.13, 0.9)
  DrawRect(x + 10, yy + 17, (w - 20) * math.max(0, fuel / VEH.tank), 4,
           fLow and 0.9 or 0.55, fLow and 0.25 or 0.78, 0.25, 1)
  yy = yy + 28
  DrawText(string.format("车况  %.0f%%", dur), x + 10, yy, 13,
           dur < 40 and 1 or 0.86, dur < 40 and 0.4 or 0.84, dur < 40 and 0.3 or 0.78, 1)
  DrawRect(x + 10, yy + 17, w - 20, 4, 0.12, 0.12, 0.13, 0.9)
  DrawRect(x + 10, yy + 17, (w - 20) * math.max(0, dur / VEH.durMax), 4,
           dur < 40 and 0.9 or 0.45, dur < 40 and 0.25 or 0.78, 0.35, 1)
  yy = yy + 28
  DrawText(string.format("净水  %d / %d L", math.floor(water), SURVIVAL.waterMax),
           x + 10, yy, 13, water < 15 and 1 or 0.86, water < 15 and 0.4 or 0.84,
           water < 15 and 0.3 or 0.78, 1)
  DrawRect(x + 10, yy + 17, w - 20, 4, 0.12, 0.12, 0.13, 0.9)
  DrawRect(x + 10, yy + 17, (w - 20) * math.max(0, water / SURVIVAL.waterMax), 4,
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
  drawBanner(string.format("第 %d 天  %s  ·  %s  ·  %s%s", day, timeString(), loc,
             wText, isNight() and "  夜晚" or ""), 12, 16)

  drawCrosshair(vw, vh, #horde > 0)
  drawInteractionPrompt(vw, vh)
  drawInventoryStrip(math.floor(vw * 0.5), vh - 156)
  if job ~= nil then drawJobTracker(vw) end

  local hints = {
    "WASD 移动   Shift 疾跑   E 交互",
    "B 改装模式   T 驾驶   M 世界地图   F 手电",
    "1-0 快速使用物品（水/罐头/热饭/咖啡）",
    "F5 保存   F9 读取",
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
    string.format("%d / %d L%s", math.floor(water), SURVIVAL.waterMax,
                  s.waterDraw > 0 and string.format(" (耗%d/h)", s.waterDraw) or ""),
    water / SURVIVAL.waterMax, water < 15)
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
end

function drawDriveHud()
  local vp = GetViewportSize()
  local vw = (vp and vp.w) or 1280
  local vh = (vp and vp.h) or 720
  local kmh = math.floor(math.abs(rv.speed) * 3.6)
  local ratio = stats.weight / weightMax()
  local over = ratio > 1.0

  local pw, ph = 210, 186
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
  local fLow = fuel / VEH.tank < 0.2
  DrawText(string.format("燃料 %.0f / %d L%s", fuel, VEH.tank,
           fuel <= 0 and "  没油了! 龟速" or (fLow and "  油量低!" or "")),
           px + 14, py + 60, 13, fLow and 1 or 0.85,
           fLow and 0.4 or 0.84, fLow and 0.3 or 0.8, 1)
  DrawRect(px + 14, py + 78, pw - 28, 5, 0.13, 0.13, 0.14, 0.9)
  DrawRect(px + 14, py + 78, (pw - 28) * math.max(0, fuel / VEH.tank), 5,
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
    if hottest > 110 then tr, tg, tb = 0.95, 0.45, 0.30
    elseif hottest > 85 then tr, tg, tb = 0.95, 0.78, 0.35 end
    DrawText(string.format("胎温 %.0f°C · 胎压 %.1fbar", hottest, pAvg),
             px + 14, py + 158, 11, tr, tg, tb, 1)
  end
  -- 行程电脑：累计里程 + 百公里油耗（车速/载重曲线的直观反馈）
  local l100 = tripL100 and tripL100() or 0
  if (tripDistance or 0) >= 20 then
    DrawText(string.format("行程 %.1fkm · %.1fL/100km", tripDistance / 1000, l100),
             px + 14, py + 172, 11, 0.74, 0.80, 0.72, 1)
  else
    DrawText(string.format("行程 %.0fm · 油耗 —", tripDistance or 0),
             px + 14, py + 172, 11, 0.68, 0.70, 0.64, 1)
  end

  drawNeedsPanel(vw - 230, 10, 220)
  drawCompass(vw)
  if job ~= nil then drawJobTracker(vw) end

  local hints = {
    "W 油门   S 刹车/倒车   A/D 转向   Shift 手刹",
    "鼠标 张望   空格 停车",
    "C " .. (driveView == "cab" and "追逐视角" or "驾驶席视角") .. "   M 世界地图",
  }
  for i, h in ipairs(hints) do
    drawTextRight(h, vw - 14, vh - 74 + (i - 1) * 19, 12, 0.80, 0.79, 0.74, 0.9)
  end

  local top = string.format("第 %d 天  %s  ·  %s%s", day, timeString(), weatherName(),
                            (weatherHoursLeft and weatherHoursLeft() > 0) and
                            string.format("（%.1fh）", weatherHoursLeft()) or "")
  if nearNode then
    top = top .. "   ▶ 临近 " .. nearNode.name .. "（空格停靠）"
  end
  drawBanner(top, 14, 16)

  if toast.t > 0 then drawBanner(toast.text, 120, 16) end
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
function drawInventoryPanel()
  local vp = GetViewportSize()
  local vw = (vp and vp.w) or 1280
  local vh = (vp and vp.h) or 720
  local rows = {}
  for _, id in ipairs(ITEM_ORDER) do
    if (inv[id] or 0) > 0 then rows[#rows + 1] = id end
  end
  local pw = 540
  local ph = 104 + math.max(1, #rows) * 44 + 34
  local px = math.floor((vw - pw) * 0.5)
  local py = math.floor((vh - ph) * 0.5)
  DrawRect(px, py, pw, ph, 0.04, 0.05, 0.06, 0.92)
  DrawRectOutline(px, py, pw, ph, 0.55, 0.52, 0.42, 0.95)
  drawTextCentered("背包 · 随身物资", vw * 0.5, py + 14, 20, 0.98, 0.90, 0.68, 1)
  DrawText("按数字快速使用；种子/咖啡豆/修理包要去对应家具", px + 20, py + 46, 12,
           0.72, 0.72, 0.66, 1)
  local yy = py + 72
  local key = { water="1", canned="2", meal="3", brew="4", rawfood="5",
                coffee="6", seed="7", repairkit="8", fish="9", fishsoup="0" }
  if #rows == 0 then
    DrawText("背包空空如也", px + 24, yy + 6, 15, 0.65, 0.65, 0.6, 1)
  end
  for _, id in ipairs(rows) do
    local it = ITEMS[id]
    DrawRect(px + 16, yy - 4, pw - 32, 38, 0.08, 0.09, 0.10, 0.7)
    DrawText((key[id] and ("[" .. key[id] .. "] ") or "    ") .. it.name,
             px + 26, yy + 2, 15, 0.95, 0.92, 0.84, 1)
    DrawText(it.desc or "", px + 150, yy + 4, 12, 0.72, 0.72, 0.66, 1)
    drawTextRight("x" .. (inv[id] or 0), px + pw - 26, yy + 2, 15, 0.98, 0.9, 0.68, 1)
    yy = yy + 44
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
  local rows = isStove and #RECIPES or (#REPAIR_PARTS + 2)
  local pw = 540
  local ph = 108 + rows * 58
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
      yy = yy + 58
    end
    drawTextCentered("按 1/2/3 制作    E 关闭", vw * 0.5, py + ph - 30, 13, 0.75, 0.75, 0.68, 1)
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
      yy = yy + 58
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
      yy = yy + 58
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
      yy = yy + 58
    end
    drawTextCentered("按 1/2/3/4 维修 / 5 货架 / 6 鱼竿    E 关闭", vw * 0.5,
                     py + ph - 30, 13, 0.75, 0.75, 0.68, 1)
  end
end

function drawNightOverlay()
  local a = nightAmount()
  if a <= 0.01 then return end
  local vp = GetViewportSize()
  local vw = (vp and vp.w) or 1280
  local vh = (vp and vp.h) or 720
  DrawRect(0, 0, vw, vh, 0.02, 0.04, 0.10, 0.46 * a)
  if isNight() then
    drawTextCentered("夜深了 — 回车里睡觉，或放慢车速", math.floor(vw * 0.5), vh - 34,
                     13, 0.72, 0.78, 0.95, 0.85 * a)
  end
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
  -- 当前货运合同：地图上用金色圆环标出目的地
  if job ~= nil then
    local jn = nodeById(job.destId)
    if jn then
      local jx, jy = mapXY(jn.x, jn.z)
      DrawCircle(jx, jy, 12, 2, 1.0, 0.80, 0.28, 1, false)
      drawTextCentered("货运目标", jx, jy + 13, 12, 1.0, 0.88, 0.50, 1)
    end
  end
  DrawText("● 房车    ● 地点（越红越危险）  ? 未探索    — 公路", px + 16, py + ph - 24,
           12, 0.7, 0.7, 0.65, 1)
  DrawText(string.format("已探索 %d / %d 个地点", visitedCount or 0, #NODES),
           px + pw - 16, py + ph - 24, 12, 0.7, 0.7, 0.65, 1, true)
  if fishingRecordText ~= nil then
    drawTextCentered(fishingRecordText(), px + pw * 0.5, py + ph - 42, 12,
                     0.72, 0.76, 0.68, 1)
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
  local fragTag = job.fragile and " [易碎]" or ""
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
  DrawText(string.format("%.0fm · 货况 %.0f%% · %s", dist, cond * 100, payText),
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
    DrawText(string.format("报酬 %s%s%s", jobResult.got or "",
             (jobResult.fuel or 0) > 0 and
               string.format("   燃料+%dL", jobResult.fuel) or "",
             jobResult.bonus and "   （今日首单 +25%）" or ""),
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
function drawJobPanel()
  local vp = GetViewportSize()
  local vw = (vp and vp.w) or 1280
  local vh = (vp and vp.h) or 720
  local rows = math.max(1, #jobOffers)
  local pw, ph = 620, 158 + rows * 62 + 30
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

  local yy = py + (job ~= nil and 112 or 70)
  for i, o in ipairs(jobOffers) do
    local hot = job == nil
    DrawRect(px + 16, yy, pw - 32, 54, hot and 0.09 or 0.06, 0.09, 0.10, 0.85)
    DrawRectOutline(px + 16, yy, pw - 32, 54,
                    hot and 0.62 or 0.36, hot and 0.56 or 0.34, 0.30, 0.9)
    DrawText(string.format("[%d]", i), px + 26, yy + 15, 16,
             hot and 0.98 or 0.5, hot and 0.86 or 0.5, hot and 0.52 or 0.48, 1)
    DrawText(string.format("%s%s%s%s  ·  %s x%d  ·  %.0fkg",
             (o.urgent and "[急] " or ""), (o.fragile and "[易碎] " or ""),
             (o.stops and "[2站] " or ""), o.destName, o.cargo, o.crates, o.weight),
             px + 64, yy + 7, 14, 0.94, 0.92, 0.84, 1)
    local payText = jobPayText(o.pay or {})
    DrawText(string.format("距离 %.0fm   报酬 %s   燃料+%dL%s%s", o.distance, payText,
             o.fuel or 0, (o.danger or 0) >= 2 and "   危险" or "",
             o.urgent and "   限时" or ""),
             px + 64, yy + 28, 12, 0.78, 0.82, 0.76, 1)
    yy = yy + 62
  end
  DrawText("1-3 接单   0 放弃当前合同   E 关闭", px + 20, py + ph - 26, 12,
           0.70, 0.72, 0.66, 1)
end

