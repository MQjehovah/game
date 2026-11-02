-- lc_hud.lua
-- HUD：第一人称生存面板 / 交互提示 / 驾驶仪表 / 改装热栏 / 世界地图

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
  DrawText(text, math.floor(vw * 0.5), y, size, 0.98, 0.94, 0.82, 1, true)
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
end

function drawInventoryStrip(x, y)
  local parts = {}
  for _, id in ipairs(ITEM_ORDER) do
    if (inv[id] or 0) > 0 then
      parts[#parts + 1] = itemName(id) .. "x" .. inv[id]
    end
  end
  if #parts == 0 then parts[1] = "背包空空" end
  DrawText(table.concat(parts, "   "), x, y, 13, 0.86, 0.84, 0.76, 0.95, true)
end

function drawInteractionPrompt(vw, vh)
  if nearAction == nil then return end
  local y = vh - 196
  local enabled = nearAction.enabled ~= false
  local label = "[E] " .. nearAction.verb
  DrawRect(vw * 0.5 - 230, y - 8, 460, 46, 0.03, 0.04, 0.05, 0.84)
  DrawRectOutline(vw * 0.5 - 230, y - 8, 460, 46,
                  enabled and 0.85 or 0.45, enabled and 0.70 or 0.45, 0.38, 0.95)
  DrawText(label, math.floor(vw * 0.5), y, 17,
           enabled and 0.98 or 0.62, enabled and 0.92 or 0.62,
           enabled and 0.70 or 0.60, 1, true)
  if nearAction.detail and nearAction.detail ~= "" then
    DrawText(nearAction.detail, math.floor(vw * 0.5), y + 22, 13,
             enabled and 0.84 or 0.62, enabled and 0.82 or 0.62,
             enabled and 0.72 or 0.58, 1, true)
  end
  -- 世界空间小标记
  if (nearAction.kind == "station" or nearAction.kind == "fish") and nearAction.x then
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
  DrawText(string.format("⚠ 附近感染者 %d", #horde),
           vw - 14, 210, 14, 1, 0.55, 0.4, 1, true, true)
  DrawText("左键 射击", vw - 14, 230, 13, 0.9, 0.7, 0.55, 1, true, true)
end

function drawFpHud()
  local vp = GetViewportSize()
  local vw = (vp and vp.w) or 1280
  local vh = (vp and vp.h) or 720

  drawNeedsPanel(10, vh - 186, 220)
  drawRvStatus(vw - 230, 10, 220)
  drawThreatBadge(vw)

  local loc = atNode and ("停靠 · " .. nodeName(atNode)) or (nearRv() and "房车旁" or "荒野")
  drawBanner(string.format("第 %d 天  %s  ·  %s  ·  %s%s", day, timeString(), loc,
             weatherName(), isNight() and "  夜晚" or ""), 12, 16)

  drawCrosshair(vw, vh, #horde > 0)
  drawInteractionPrompt(vw, vh)
  drawInventoryStrip(math.floor(vw * 0.5), vh - 156)

  local hints = {
    "WASD 移动   Shift 疾跑   E 交互",
    "B 改装模式   T 驾驶   M 世界地图",
    "1-0 快速使用物品（水/罐头/热饭/咖啡）",
    "F5 保存   F9 读取",
  }
  DrawRect(vw - 292, 252, 282, 92, 0.03, 0.04, 0.05, 0.45)
  for i, h in ipairs(hints) do
    DrawText(h, vw - 14, 260 + (i - 1) * 19, 12, 0.86, 0.84, 0.78, 0.95, true, true)
  end

  if fishingT > 0 then
    local frac = 1 - fishingT / math.max(0.01, fishingTotal)
    local bx = math.floor(vw * 0.5 - 150)
    DrawRect(bx, vh - 250, 300, 42, 0.04, 0.05, 0.06, 0.8)
    DrawRectOutline(bx, vh - 250, 300, 42, 0.55, 0.72, 0.82, 0.9)
    DrawText(string.format("钓鱼中… %.1f 秒", fishingT), math.floor(vw * 0.5),
             vh - 244, 15, 0.85, 0.92, 0.95, 1, true)
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
                ["7"] = "seed", ["8"] = "repairkit", ["9"] = "fish" }
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
  DrawText("[左键] 放置    [右键/X] 拆除    [R] 旋转 15°    [滚轮/1-0] 换家具    [Tab] 地板/车顶    [B/E] 退出",
           math.floor(vw * 0.5), vh - 196, 13, 0.92, 0.88, 0.74, 1, true)
  if hover and hover.inst then
    DrawText("准星： " .. hover.inst.cat.name .. "（右键拆除）",
             math.floor(vw * 0.5), vh - 176, 13, 1, 0.65, 0.45, 1, true)
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
      DrawText(c.desc, x + slotW * 0.5, y - 18, 13, 0.95, 0.9, 0.75, 1, true)
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
    string.format("%+.2f kW%s", netPower, s.battery > 0 and string.format(" (+%.0fkWh)", s.battery) or ""),
    s.powerDraw > 0 and math.min(1, s.powerDraw / math.max(s.powerGen, 0.001)) or 0,
    netPower < 0)
  rowY = rowY + 34
  drawStatBar(10 + pad, rowY, innerW, "水",
    string.format("%d / %d L%s", math.floor(water), SURVIVAL.waterMax,
                  s.waterDraw > 0 and string.format(" (耗%d/h)", s.waterDraw) or ""),
    water / SURVIVAL.waterMax, water < 15)
  rowY = rowY + 34
  drawStatBar(10 + pad, rowY, innerW, "载重",
    string.format("%.0f / %d kg", s.weight, WEIGHT_MAX),
    s.weight / WEIGHT_MAX, s.weight > WEIGHT_MAX)
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
      DrawText(cat.desc, x + slotW * 0.5, y - 18, 13, 0.95, 0.9, 0.75, 1, true)
    end
  end

  local hints = {
    "左键 放置   右键/X 拆除   R 旋转",
    "Tab 地板/车顶   G 网格   工作台修车",
    "B 退出改装   T 上路   M 地图   F5/F9 存/读",
  }
  for i, h in ipairs(hints) do
    DrawText(h, vw - 14, 14 + (i - 1) * 20, 14, 0.85, 0.84, 0.80, 0.9, true, true)
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

  DrawText(curLayer == "roof" and "【车顶层】" or "【地板层】",
           math.floor(vw * 0.5), 14, 16, 0.55, 0.85, 0.98, 1, true)

  if hover and hover.inst then
    local c = hover.inst.cat
    DrawText(c.name .. "  ·  " .. (c.desc or "") .. "  ·  " .. costText(c.cost or {}),
             math.floor(vw * 0.5), vh - rowsN * (slotH + gap) - 40,
             14, 0.95, 0.90, 0.78, 1, true)
  end

  if toast.t > 0 then
    DrawText(toast.text, math.floor(vw * 0.5), 120, 16, 0.98, 0.93, 0.75, 1, true)
  end
end

function drawDriveHud()
  local vp = GetViewportSize()
  local vw = (vp and vp.w) or 1280
  local vh = (vp and vp.h) or 720
  local kmh = math.floor(math.abs(rv.speed) * 3.6)
  local ratio = stats.weight / WEIGHT_MAX
  local over = ratio > 1.0

  local pw, ph = 210, 166
  local px, py = 10, vh - ph - 10
  DrawRect(px, py, pw, ph, 0.08, 0.09, 0.10, 0.72)
  DrawRectOutline(px, py, pw, ph, 0.55, 0.52, 0.42, 0.9)
  local gear = (rv.speed > 0.05) and "D" or ((rv.speed < -0.05) and "R" or "N")
  DrawText(string.format("%d", kmh), px + 14, py + 10, 34,
           over and 1.0 or 0.95, over and 0.5 or 0.92, over and 0.35 or 0.78, 1)
  DrawText("km/h", px + 84, py + 26, 15, 0.7, 0.68, 0.62, 1)
  DrawText("挡位 " .. gear, px + 140, py + 26, 15, 0.6, 0.85, 0.95, 1)
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
  DrawText(string.format("载重 %.0f / %d kg", stats.weight, WEIGHT_MAX),
           px + 14, py + 124, 12, 0.8, 0.8, 0.78, 1)
  DrawText(string.format("引擎 %.0f%%  轮胎 %.0f%%  电瓶 %.0f%%",
           (parts and parts.engine) or 100, (parts and parts.tires) or 100,
           (parts and parts.battery) or 100),
           px + 14, py + 142, 11, 0.74, 0.74, 0.70, 1)

  drawNeedsPanel(vw - 230, 10, 220)

  local hints = {
    "W 油门   S 刹车/倒车   A/D 转向",
    "鼠标 张望   空格 停车",
    "C " .. (driveView == "cab" and "追逐视角" or "驾驶席视角") .. "   M 世界地图",
  }
  for i, h in ipairs(hints) do
    DrawText(h, vw - 260, vh - 74 + (i - 1) * 19, 12, 0.80, 0.79, 0.74, 0.9, true)
  end

  local top = string.format("第 %d 天  %s  ·  %s", day, timeString(), weatherName())
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

function drawNightOverlay()
  local a = nightAmount()
  if a <= 0.01 then return end
  local vp = GetViewportSize()
  local vw = (vp and vp.w) or 1280
  local vh = (vp and vp.h) or 720
  DrawRect(0, 0, vw, vh, 0.02, 0.04, 0.10, 0.46 * a)
  if isNight() then
    DrawText("夜深了 — 回车里睡觉，或放慢车速", math.floor(vw * 0.5), vh - 34,
             13, 0.72, 0.78, 0.95, 0.85 * a, true)
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
  DrawText("M 关闭", px + pw - 16, py + 13, 13, 0.6, 0.6, 0.55, 1, true)

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
    DrawCircle(mx2, my2, cur and 7 or 5, cur and 2 or 1.5,
               0.45 + danger * 0.17, 0.85 - danger * 0.18, 0.32, 1, true)
    DrawText(nd.name .. (danger > 0 and string.rep("!", danger) or ""),
             mx2, my2 - 20, 12, cur and 1 or 0.82,
             cur and 0.9 or 0.78, cur and 0.6 or 0.68, 1, true)
  end
  local rx, ry = mapXY(rv.x, rv.z)
  DrawLine(rx, ry, rx + math.sin(rv.yaw) * 20, ry + math.cos(rv.yaw) * 20,
           2, 0.98, 0.85, 0.4, 1)
  DrawCircle(rx, ry, 4, 2, 0.98, 0.85, 0.4, 1, true)
  DrawText("● 房车    ● 地点（越红越危险）    — 公路", px + 16, py + ph - 24,
           12, 0.7, 0.7, 0.65, 1)
  DrawText(string.format("已发现 %d 个地点", #NODES), px + pw - 16, py + ph - 24,
           12, 0.7, 0.7, 0.65, 1, true)
end
