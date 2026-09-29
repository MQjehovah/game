-- lc_hud.lua
-- HUD：状态面板/热栏/速度表/停靠叠加层/世界地图/准星

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
  DrawText(label, x, y, 15, 0.92, 0.90, 0.84, 1)
  DrawText(valueText, x + w, y, 15, warn and 1 or 0.92, warn and 0.42 or 0.90,
           warn and 0.36 or 0.84, 1, true)
  local barW, barH = w, 6
  DrawRect(x, y + 20, barW, barH, 0.13, 0.13, 0.14, 0.9)
  local f = math.max(0, math.min(1, frac or 0))
  local br, bg, bb = 0.45, 0.78, 0.44
  if warn then br, bg, bb = 0.88, 0.28, 0.24 end
  DrawRect(x, y + 20, barW * f, barH, br, bg, bb, 1)
end

function drawHud()
  local sprinting = onFoot and (player.anim == "Run")
  local vp = GetViewportSize()
  local vw = (vp and vp.w) or 1280
  local vh = (vp and vp.h) or 720
  local pw, ph = 250, 270
  DrawRect(10, 10, pw, ph, 0.08, 0.09, 0.10, 0.72)
  DrawRectOutline(10, 10, pw, ph, 0.55, 0.52, 0.42, 0.9)
  local title = atNode and ("停靠 · " .. nodeName(atNode)) or "驻车 · Last Caravan"
  DrawText(title, 20, 16, 17, 0.95, 0.88, 0.70, 1)
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
    string.format("%d L%s", s.waterStore,
                  s.waterDraw > 0 and string.format(" (耗%d/h)", s.waterDraw) or ""),
    math.min(1, s.waterStore / 300), s.waterDraw > s.waterStore)
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

  -- 底部热栏：3 行 x 7 列
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
    onFoot and "W/S 行走   A/D 转向   Shift 疾跑" or "左键 放置   右键/X 拆除",
    onFoot and "F 上车   Q 手雷" or "F 下车   T 上路   C 视角(行驶)",
    "M 地图   H 修理   F5/F9 存/读",
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

  if onFoot then
    local mp = InputMousePos()
    if mp then
      -- 准星
      DrawRect(mp.x - 6, mp.y - 0.5, 12, 1, 0.92, 0.9, 0.85, 0.85)
      DrawRect(mp.x - 0.5, mp.y - 6, 1, 12, 0.92, 0.9, 0.85, 0.85)
      -- 命中标记（X 四瓣，命中后短暂闪现）
      if hitMarkT > 0 then
        local a = hitMarkT / 0.12
        DrawRect(mp.x - 10, mp.y - 0.75, 6, 1.5, 1, 0.3, 0.25, a)
        DrawRect(mp.x + 4, mp.y - 0.75, 6, 1.5, 1, 0.3, 0.25, a)
        DrawRect(mp.x - 0.75, mp.y - 10, 1.5, 6, 1, 0.3, 0.25, a)
        DrawRect(mp.x - 0.75, mp.y + 4, 1.5, 6, 1, 0.3, 0.25, a)
      end
    end
    local by2 = vh - 205
    DrawRect(vw * 0.5 - 70, by2 - 6, 140, 64, 0.06, 0.07, 0.08, 0.55)
    local ammoText = (player.reloadT > 0) and "换弹中…" or (player.mag .. " / 30")
    local aCol = (player.mag <= 6 or player.reloadT > 0) and 1 or 0.9
    DrawText("弹药 " .. ammoText, math.floor(vw * 0.5), by2, 14,
             aCol, player.reloadT > 0 and 0.6 or 0.9, 0.5, 1, true)
    local nadeText = (player.nadeCd > 0) and string.format("Q 手雷 %.1fs", player.nadeCd)
                     or "Q 手雷 就绪"
    DrawText(nadeText, math.floor(vw * 0.5), by2 + 20, 13,
             player.nadeCd > 0 and 0.55 or 0.65, 0.85, 0.55, 1, true)
    if sprinting then
      DrawText("疾跑中", math.floor(vw * 0.5), by2 + 40, 13,
               0.6, 0.85, 0.55, 1, true)
    end
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

  local pw, ph = 210, 146
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
  DrawText(string.format("车体 %.0f%%%s", dur, dur < 40 and "  需修理(H)" or ""),
           px + 14, py + 92, 13, dur < 40 and 1 or 0.85,
           dur < 40 and 0.4 or 0.84, dur < 40 and 0.3 or 0.8, 1)
  DrawRect(px + 14, py + 110, pw - 28, 5, 0.13, 0.13, 0.14, 0.9)
  DrawRect(px + 14, py + 110, (pw - 28) * math.max(0, dur / VEH.durMax), 5,
           dur < 40 and 0.88 or 0.45, dur < 40 and 0.28 or 0.78,
           dur < 40 and 0.24 or 0.44, 1)
  DrawText(string.format("载重 %.0f / %d kg", stats.weight, WEIGHT_MAX),
           px + 14, py + 124, 12, 0.8, 0.8, 0.78, 1)

  local hints = {
    "W 油门   S 刹车/倒车   A/D 转向",
    "空格 停车   C " .. (driveView == "cab" and "追逐视角" or "驾驶席视角"),
    "M 世界地图",
  }
  for i, h in ipairs(hints) do
    DrawText(h, vw - 14, 14 + (i - 1) * 20, 14, 0.85, 0.84, 0.80, 0.9, true, true)
  end

  local top = "第 " .. day .. " 天"
  if nearNode then
    top = top .. "   ▶ 临近 " .. nearNode.name .. "（空格停靠）"
  end
  DrawText(top, math.floor(vw * 0.5), 14, 16, 0.95, 0.9, 0.75, 1, true)

  if toast.t > 0 then
    DrawText(toast.text, math.floor(vw * 0.5), 120, 16, 0.98, 0.93, 0.75, 1, true)
  end
end

-- 停靠节点叠加面板（威胁 / 搜获 / 撤离警报）——画在驻车 HUD 之上
function drawScavengeOverlay()
  local vp = GetViewportSize()
  local vw = (vp and vp.w) or 1280
  local vh = (vp and vp.h) or 720
  local nd = nodeById(atNode)

  local px, py, pw = 10, 290, 216
  local ph = (evacT >= 0) and 88 or 64
  DrawRect(px, py, pw, ph, 0.08, 0.09, 0.10, 0.72)
  DrawRectOutline(px, py, pw, ph, 0.55, 0.52, 0.42, 0.9)
  DrawText("尸群威胁", px + 12, py + 7, 14, 0.9, 0.62, 0.5, 1)
  local gap2 = 5
  local segW = (pw - 24 - (STOP.threatMax - 1) * gap2) / STOP.threatMax
  for i = 1, STOP.threatMax do
    local sx = px + 12 + (i - 1) * (segW + gap2)
    local t = i / STOP.threatMax
    DrawRect(sx, py + 32, segW, 12, 0.12, 0.12, 0.13, 0.9)
    if stopThreat >= i - 0.01 then
      DrawRect(sx, py + 32, segW, 12, 0.35 + 0.62 * t, 0.75 - 0.55 * t, 0.25, 1)
    end
  end
  if evacT >= 0 then
    DrawText("夜袭! 歼灭 x " .. killCount .. "   场上 x " .. #horde,
             px + 12, py + 56, 14, 1, 0.5, 0.4, 1)
  end

  local cats = 0
  for _ in pairs(stopBag) do cats = cats + 1 end
  local bw = 190
  local bh = 34 + 21 * math.max(1, cats)
  local bx, by = 10, vh - bh - 10
  DrawRect(bx, by, bw, bh, 0.08, 0.09, 0.10, 0.72)
  DrawRectOutline(bx, by, bw, bh, 0.55, 0.52, 0.42, 0.9)
  DrawText("本次搜获", bx + 12, by + 7, 14, 0.85, 0.8, 0.62, 1)
  if cats == 0 then
    DrawText("（还没搜到东西）", bx + 12, by + 30, 13, 0.55, 0.55, 0.52, 1)
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

  if evacT >= 0 then
    local flash = (math.floor(evacT * 4) % 2 == 0)
    DrawRect(0, 0, vw, vh, 0.7, 0.05, 0.02, flash and 0.10 or 0.04)
    DrawText(string.format("尸群逼近！%.0f 秒内撤离", math.ceil(evacT)),
             math.floor(vw * 0.5), math.floor(vh * 0.22), 26,
             1, flash and 0.25 or 0.5, 0.2, 1, true)
    local mp = InputMousePos()
    if mp then
      DrawRect(mp.x - 1, mp.y - 9, 2, 18, 1, 0.45, 0.3, 0.9)
      DrawRect(mp.x - 9, mp.y - 1, 18, 2, 1, 0.45, 0.3, 0.9)
    end
  end

  local stars = string.rep("★", nd and nd.danger or 0)
  DrawText("停靠 · " .. nodeName(atNode)
           .. (stars ~= "" and ("  危险 " .. stars) or "") .. "   第 " .. day .. " 天",
           math.floor(vw * 0.5), 34, 15, 0.95, 0.88, 0.7, 1, true)
end

function drawMapHud()
  local vp = GetViewportSize()
  local vw = (vp and vp.w) or 1280
  local vh = (vp and vp.h) or 720
  DrawRect(0, 0, vw, vh, 0.02, 0.03, 0.04, 0.55)
  local pw, ph = 470, 380
  local px = math.floor((vw - pw) * 0.5)
  local py = math.floor((vh - ph) * 0.5)
  DrawRect(px, py, pw, ph, 0.07, 0.08, 0.09, 0.93)
  DrawRectOutline(px, py, pw, ph, 0.55, 0.52, 0.42, 0.95)
  DrawText("世界地图", px + 16, py + 10, 16, 0.95, 0.88, 0.7, 1)
  DrawText("M 关闭", px + pw - 16, py + 13, 13, 0.6, 0.6, 0.55, 1, true)
  local wx0, wx1, wz0, wz1 = -190, 200, -125, 215
  local function mapXY(x, z)
    return px + 26 + (x - wx0) / (wx1 - wx0) * (pw - 52),
           py + 44 + (z - wz0) / (wz1 - wz0) * (ph - 84)
  end
  for _, nd in ipairs(NODES) do
    for _, lid in ipairs(nd.links or {}) do
      if nd.id < lid then
        local o = nodeById(lid)
        if o then
          local ax, ay = mapXY(nd.x, nd.z)
          local bx, by = mapXY(o.x, o.z)
          DrawLine(ax, ay, bx, by, 2, 0.42, 0.46, 0.4, 0.85)
        end
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
             mx2, my2 - 20, 13, cur and 1 or 0.82,
             cur and 0.9 or 0.78, cur and 0.6 or 0.68, 1, true)
  end
  local rx, ry = mapXY(rv.x, rv.z)
  DrawLine(rx, ry, rx + math.sin(rv.yaw) * 20, ry + math.cos(rv.yaw) * 20,
           2, 0.98, 0.85, 0.4, 1)
  DrawCircle(rx, ry, 4, 2, 0.98, 0.85, 0.4, 1, true)
  DrawText("● 房车    ● 节点（越红越危险）    — 道路",
           px + 16, py + ph - 24, 12, 0.7, 0.7, 0.65, 1)
end
