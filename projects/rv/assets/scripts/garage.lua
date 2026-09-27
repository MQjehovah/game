-- ===========================================================================
-- Last Caravan（末路房车）车库沙盒 v0.1
-- 网格模块化改装：放置 / 旋转 / 拆除 / 占用判定 / 四资源结算 / HUD / 存读档
-- 网格：4 x 14 格，格 0.5m，车内区域 x[-1,1] z[-3.5,3.5]，车头在 +z（画面远端）
-- ===========================================================================

-- ---------------------------------------------------------------------------
-- 配置
-- ---------------------------------------------------------------------------
local CELL      = 0.5
local GW, GH    = 4, 14              -- 网格宽(x) x 长(z)，单位格
local ORIGIN_X  = -1.0               -- 格 (0,0) 的西边缘
local ORIGIN_Z  = -3.5               -- 格 (0,0) 的后边缘（镜头侧）
local FLOOR_Y   = 0.03               -- 车厢地板顶面
local WEIGHT_MAX = 1500              -- 底盘载重 kg

-- 模块目录（v0.1：程序化色块占位；w/h 单位=格，mh=高度米）
-- powerGen/powerDraw kW；waterStore L、waterDraw L/h；weight kg；comfort 点数
local CATALOG = {
  { id="bed_double", name="双人床",   w=3, h=4, mh=0.45, color="#8A5A44", weight=60,  comfort=30, desc="睡眠质量 +30" },
  { id="bed_single", name="单人床",   w=2, h=3, mh=0.40, color="#A0714F", weight=35,  comfort=15, desc="睡眠质量 +15" },
  { id="stove",      name="灶台",     w=2, h=2, mh=0.50, color="#7A7F87", weight=40,  comfort=5,  powerDraw=0.5, waterDraw=2, desc="耗电 0.5kW 耗水 2L/h" },
  { id="fridge",     name="冰箱",     w=1, h=2, mh=1.10, color="#9FB4C7", weight=30,              powerDraw=0.3, desc="耗电 0.3kW" },
  { id="water_tank", name="净水箱",   w=2, h=2, mh=0.90, color="#4F8FBF", weight=160,             waterStore=120, desc="储水 120L" },
  { id="battery",    name="电池组",   w=2, h=1, mh=0.50, color="#D8B23A", weight=80,  battery=5,  desc="储能 5kWh" },
  { id="solar",      name="太阳能板", w=2, h=3, mh=0.15, color="#3E6FB0", weight=25,  powerGen=1.2, desc="发电 1.2kW（后续上车顶）" },
  { id="workbench",  name="工作台",   w=2, h=3, mh=0.55, color="#8C6239", weight=70,  desc="改装检修（P1 开放功能）" },
  { id="storage",    name="储物箱",   w=1, h=1, mh=0.60, color="#6E7B52", weight=15,  desc="储物 50L" },
  { id="heater",     name="电暖器",   w=1, h=1, mh=0.55, color="#C96B3F", weight=12,  comfort=10, powerDraw=0.8, desc="耗电 0.8kW 舒适 +10" },
  { id="tv",         name="娱乐柜",   w=2, h=1, mh=0.90, color="#5D5366", weight=28,  comfort=8,  powerDraw=0.2, desc="耗电 0.2kW 舒适 +8" },
  { id="turret",     name="炮塔底座", w=2, h=2, mh=0.60, color="#5A5A5A", weight=90,  desc="预留（P2 夜袭开放）" },
}
local HOTKEYS = { "1","2","3","4","5","6","7","8","9","0" }  -- 前 10 件绑定热键

-- ---------------------------------------------------------------------------
-- 状态
-- ---------------------------------------------------------------------------
local selected  = 1          -- 当前选择的目录索引
local rot       = 0          -- 幽灵旋转 0/1（1 = 交换 w/h）
local occupancy = {}         -- cellIndex = cz*GW+cx -> 实例 id
local instances = {}         -- id -> { cat=目录项, ent=实体, cx=, cz=, rot= }
local nextId    = 1
local hover     = nil        -- 当前悬停格 { cx=, cz=, valid= }
local ghostEnt, gridEnt, hoverDecalEnt
local gridVisible = true
local ghostValidLast = nil
local toast     = { text="", t=0 }
local stats     = {}

-- ---------------------------------------------------------------------------
-- 小工具
-- ---------------------------------------------------------------------------
local function cellIndex(cx, cz) return cz * GW + cx end

local function cellCenter(cx, cz)
  return ORIGIN_X + (cx + 0.5) * CELL, ORIGIN_Z + (cz + 0.5) * CELL
end

local function worldToCell(x, z)
  local cx = math.floor((x - ORIGIN_X) / CELL)
  local cz = math.floor((z - ORIGIN_Z) / CELL)
  if cx < 0 or cx >= GW or cz < 0 or cz >= GH then return nil end
  return cx, cz
end

-- 旋转后的占格尺寸
local function footprint(cat, r)
  if r % 2 == 1 then return cat.h, cat.w end
  return cat.w, cat.h
end

-- 该实例覆盖的全部格子（越界返回 nil）
local function cellsOf(cat, cx, cz, r)
  local w, h = footprint(cat, r)
  local cells = {}
  for dz = 0, h - 1 do
    for dx = 0, w - 1 do
      local x, z = cx + dx, cz + dz
      if x < 0 or x >= GW or z < 0 or z >= GH then return nil end
      cells[#cells + 1] = { x = x, z = z }
    end
  end
  return cells
end

local function fits(cat, cx, cz, r)
  local cells = cellsOf(cat, cx, cz, r)
  if not cells then return false end
  for _, c in ipairs(cells) do
    if occupancy[cellIndex(c.x, c.z)] ~= nil then return false end
  end
  return true
end

local function findInstanceAt(cx, cz)
  local id = occupancy[cellIndex(cx, cz)]
  return id and instances[id] or nil
end

-- #RRGGBB -> r,g,b (0..1)
local function hexColor(s)
  local n = tonumber(string.sub(s, 2), 16)
  if not n then return 1, 1, 1 end
  local b = n % 256
  local g = math.floor(n / 256) % 256
  local r = math.floor(n / 65536) % 256
  return r / 255, g / 255, b / 255
end

-- ---------------------------------------------------------------------------
-- 放置 / 拆除 / 统计
-- ---------------------------------------------------------------------------
local function spawnModule(cat, cx, cz, r)
  -- 运行时可见实体必须走预制体路径：SetEntityComponent 只写 SceneData JSON，
  -- 不会物化成 SceneMesh（DrawSystem 只扫 SceneMesh），Spawn+组件画不出来。
  local ent = SpawnPrefab("rv_" .. cat.id, { x = 0, y = 0, z = 0 })
  if ent == nil then return nil end
  local w, h = footprint(cat, r)
  local mx, mz = cellCenter(cx, cz)
  -- 以锚点格(左上)为基准摆放整个占格范围
  local ox = ORIGIN_X + cx * CELL + w * CELL * 0.5
  local oz = ORIGIN_Z + cz * CELL + h * CELL * 0.5
  SetPosition(ent, { x = ox, y = FLOOR_Y + cat.mh * 0.5 + 0.005, z = oz })
  SetScale(ent, w * CELL - 0.03, cat.mh, h * CELL - 0.03)
  if r % 2 == 1 then SetRotationY(ent, math.pi * 0.5) end
  return ent
end

local function place(cat, cx, cz, r)
  if not fits(cat, cx, cz, r) then return false end
  local ent = spawnModule(cat, cx, cz, r)
  if ent == nil then return false end
  local id = nextId
  nextId = nextId + 1
  local w, h = footprint(cat, r)
  for dz = 0, h - 1 do
    for dx = 0, w - 1 do
      occupancy[cellIndex(cx + dx, cz + dz)] = id
    end
  end
  instances[id] = { cat = cat, ent = ent, cx = cx, cz = cz, rot = r }
  return true
end

local function removeAt(cx, cz)
  local inst = findInstanceAt(cx, cz)
  if not inst then return false end
  local w, h = footprint(inst.cat, inst.rot)
  for dz = 0, h - 1 do
    for dx = 0, w - 1 do
      occupancy[cellIndex(inst.cx + dx, inst.cz + dz)] = nil
    end
  end
  Despawn(inst.ent)
  instances[inst.id] = nil
  return true
end

local function recomputeStats()
  local s = { powerGen = 0, powerDraw = 0, battery = 0,
              waterStore = 0, waterDraw = 0, weight = 0, comfort = 0, count = 0 }
  for _, inst in pairs(instances) do
    local c = inst.cat
    s.powerGen   = s.powerGen   + (c.powerGen   or 0)
    s.powerDraw  = s.powerDraw  + (c.powerDraw  or 0)
    s.battery    = s.battery    + (c.battery    or 0)
    s.waterStore = s.waterStore + (c.waterStore or 0)
    s.waterDraw  = s.waterDraw  + (c.waterDraw  or 0)
    s.weight     = s.weight     + (c.weight     or 0)
    s.comfort    = s.comfort    + (c.comfort    or 0)
    s.count      = s.count + 1
  end
  stats = s
end

-- ---------------------------------------------------------------------------
-- JSON（存读档用的极简实现：编码 + 递归下降解析）
-- ---------------------------------------------------------------------------
local function jsonEncode(v)
  local t = type(v)
  if t == "table" then
    local isArr = #v > 0
    local parts = {}
    if isArr then
      for _, item in ipairs(v) do parts[#parts + 1] = jsonEncode(item) end
      return "[" .. table.concat(parts, ",") .. "]"
    end
    for k, item in pairs(v) do
      parts[#parts + 1] = '"' .. k .. '":' .. jsonEncode(item)
    end
    return "{" .. table.concat(parts, ",") .. "}"
  elseif t == "number" then
    return string.format("%.4g", v)
  elseif t == "string" then
    return '"' .. v .. '"'
  elseif t == "boolean" then
    return v and "true" or "false"
  end
  return "null"
end

local function jsonDecode(s)
  local pos = 1
  local function skipWs()
    while pos <= #s do
      local c = string.sub(s, pos, pos)
      if c == " " or c == "\t" or c == "\n" or c == "\r" then pos = pos + 1
      else break end
    end
  end
  local parseValue
  local function parseString()
    pos = pos + 1  -- 跳过 "
    local out = {}
    while pos <= #s do
      local c = string.sub(s, pos, pos)
      if c == '"' then pos = pos + 1 return table.concat(out) end
      if c == "\\" then
        pos = pos + 1
        local e = string.sub(s, pos, pos)
        if e == "n" then out[#out + 1] = "\n"
        elseif e == "t" then out[#out + 1] = "\t"
        else out[#out + 1] = e end
        pos = pos + 1
      else
        out[#out + 1] = c
        pos = pos + 1
      end
    end
    return table.concat(out)
  end
  local function parseNumber()
    local start = pos
    while pos <= #s do
      local c = string.sub(s, pos, pos)
      if c == "-" or c == "+" or c == "." or (c >= "0" and c <= "9")
         or c == "e" or c == "E" then pos = pos + 1
      else break end
    end
    return tonumber(string.sub(s, start, pos - 1)) or 0
  end
  parseValue = function()
    skipWs()
    local c = string.sub(s, pos, pos)
    if c == "{" then
      pos = pos + 1
      local obj = {}
      skipWs()
      if string.sub(s, pos, pos) == "}" then pos = pos + 1 return obj end
      while true do
        skipWs()
        local key = parseString()
        skipWs()
        pos = pos + 1  -- 跳过 :
        obj[key] = parseValue()
        skipWs()
        local sep = string.sub(s, pos, pos)
        pos = pos + 1
        if sep == "}" then return obj end
      end
    elseif c == "[" then
      pos = pos + 1
      local arr = {}
      skipWs()
      if string.sub(s, pos, pos) == "]" then pos = pos + 1 return arr end
      while true do
        arr[#arr + 1] = parseValue()
        skipWs()
        local sep = string.sub(s, pos, pos)
        pos = pos + 1
        if sep == "]" then return arr end
      end
    elseif c == '"' then
      return parseString()
    elseif string.sub(s, pos, pos + 3) == "true" then
      pos = pos + 4 return true
    elseif string.sub(s, pos, pos + 4) == "false" then
      pos = pos + 5 return false
    elseif string.sub(s, pos, pos + 3) == "null" then
      pos = pos + 4 return nil
    else
      return parseNumber()
    end
  end
  return parseValue()
end

local function saveLayout()
  local rows = {}
  for _, inst in pairs(instances) do
    rows[#rows + 1] = { id = inst.cat.id, cx = inst.cx, cz = inst.cz, rot = inst.rot }
  end
  local data = { cell = CELL, gw = GW, gh = GH, modules = rows }
  local ok = WriteText("saves/layout.json", jsonEncode(data))
  toast.text = ok and "布局已保存 (saves/layout.json)" or "保存失败"
  toast.t = 2.5
end

local function clearAll()
  for _, inst in pairs(instances) do Despawn(inst.ent) end
  instances = {}
  occupancy = {}
end

local function loadLayout()
  local text = ReadText("saves/layout.json")
  if not text or text == "" then
    toast.text = "没有存档 (saves/layout.json)"
    toast.t = 2.5
    return
  end
  local data = jsonDecode(text)
  if type(data) ~= "table" or type(data.modules) ~= "table" then
    toast.text = "存档格式无效"
    toast.t = 2.5
    return
  end
  clearAll()
  local byId = {}
  for _, cat in ipairs(CATALOG) do byId[cat.id] = cat end
  for _, row in ipairs(data.modules) do
    local cat = byId[row.id]
    if cat then place(cat, math.floor(row.cx), math.floor(row.cz), math.floor(row.rot or 0)) end
  end
  toast.text = "布局已读取"
  toast.t = 2.5
end

-- ---------------------------------------------------------------------------
-- 幽灵 / 高亮
-- ---------------------------------------------------------------------------
local function setGhostColor(valid)
  -- 绿=可放，红=冲突（换色=换预制体实体；运行时改组件颜色不生效）
  if ghostEnt ~= nil then Despawn(ghostEnt) end
  ghostEnt = SpawnPrefab(valid and "rv_ghost_ok" or "rv_ghost_bad",
                         { x = 0, y = 0, z = 0 })
end

local function ensureHelpers()
  if ghostEnt == nil then
    setGhostColor(true)
    SetVisible(ghostEnt, false)
  end
  if gridEnt == nil then
    -- 标准贴花用法（同 moba spawnGroundQuad）：组件 size 保持 1.0，
    -- 投影盒长宽/高度用实体 SetScale 控制；size 与 SetScale 叠乘会放大 size 倍。
    gridEnt = SpawnDecal("assets/sprites/grid_4x14.png", 0, FLOOR_Y, 0,
                         1.0, 0.55, 1, 1, 1, true, 0.1)
    SetScale(gridEnt, 2.0, 1.0, 7.0)
  end
  if hoverDecalEnt == nil then
    -- 悬停高亮：圆形贴花，投影盒高度覆盖被悬停模块 -> 光斑贴在模块顶面
    hoverDecalEnt = SpawnDecal("assets/sprites/decal_disc.png", 0, FLOOR_Y, 0,
                               1.0, 0.45, 1, 0.95, 0.75, true, 0.6)
    SetVisible(hoverDecalEnt, false)
  end
end

-- ---------------------------------------------------------------------------
-- HUD（设计坐标，1 世界单位 = 1 设计像素）
-- ---------------------------------------------------------------------------
local function drawStatBar(x, y, w, label, valueText, frac, warn)
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

local function drawHud()
  local vp = GetViewportSize()
  local vw = (vp and vp.w) or 1280
  local vh = (vp and vp.h) or 720
  -- 顶部面板
  local pw, ph = 250, 190
  DrawRect(10, 10, pw, ph, 0.08, 0.09, 0.10, 0.72)
  DrawRectOutline(10, 10, pw, ph, 0.55, 0.52, 0.42, 0.9)
  DrawText("车库 · Last Caravan", 20, 16, 17, 0.95, 0.88, 0.70, 1)
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
    math.min(1, s.waterStore / 300),
    s.waterDraw > s.waterStore)
  rowY = rowY + 34
  drawStatBar(10 + pad, rowY, innerW, "载重",
    string.format("%.0f / %d kg", s.weight, WEIGHT_MAX),
    s.weight / WEIGHT_MAX,
    s.weight > WEIGHT_MAX)
  rowY = rowY + 34
  drawStatBar(10 + pad, rowY, innerW, "舒适",
    string.format("%d%%", math.min(100, s.comfort)),
    math.min(1, s.comfort / 100),
    false)

  -- 底部热栏：2 行 x 6 列
  local slotW, slotH, gap = 96, 46, 6
  local cols = 6
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
    DrawRect(x, y, slotW, slotH, sel and 0.20 or 0.10, sel and 0.18 or 0.10,
             sel and 0.12 or 0.11, 0.85)
    DrawRectOutline(x, y, slotW, slotH, sel and 0.98 or 0.45, sel and 0.85 or 0.42,
                    sel and 0.55 or 0.38, 1)
    local mr, mg, mb = hexColor(cat.color)
    DrawRect(x + 6, y + 6, 10, slotH - 12, mr, mg, mb, 1)
    DrawText(cat.name, x + 22, y + 5, 14, 0.95, 0.92, 0.86, 1)
    local key = HOTKEYS[i]
    DrawText(key and ("[" .. key .. "]") or "点击", x + 22, y + 23, 12,
             0.65, 0.65, 0.62, 1)
    if hover and hover.uiSlot == i then
      DrawText(cat.desc, x + slotW * 0.5, y - 18, 13, 0.95, 0.9, 0.75, 1, true)
    end
  end

  -- 右上提示
  local hints = {
    "左键 放置   右键/X 拆除",
    "R 旋转   G 网格   F5/F9 存/读",
  }
  for i, h in ipairs(hints) do
    DrawText(h, vw - 14, 14 + (i - 1) * 20, 14, 0.85, 0.84, 0.80, 0.9, true)
  end

  -- 悬停实例信息
  if hover and hover.inst then
    local c = hover.inst.cat
    DrawText(c.name .. "  ·  " .. (c.desc or ""), math.floor(vw * 0.5), vh - rowsN * (slotH + gap) - 40,
             14, 0.95, 0.90, 0.78, 1, true)
  end

  -- toast
  if toast.t > 0 then
    DrawText(toast.text, math.floor(vw * 0.5), 120, 16, 0.98, 0.93, 0.75, 1, true)
  end
end

-- 热栏区域命中（返回槽位索引或 nil）
local function hotbarHit(mx, my)
  local vp = GetViewportSize()
  local vw = (vp and vp.w) or 1280
  local vh = (vp and vp.h) or 720
  local slotW, slotH, gap = 96, 46, 6
  local cols = 6
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

-- ---------------------------------------------------------------------------
-- 生命周期
-- ---------------------------------------------------------------------------
function on_start()
  ensureHelpers()
  recomputeStats()
  -- 初始示例布局（有存档时会被覆盖）：一眼看到“布置好的房车”
  place(CATALOG[1], 1, 0, 0)    -- 双人床（贴后墙右侧，3x4 格）
  place(CATALOG[3], 0, 4, 0)    -- 灶台（左舷中段）
  place(CATALOG[5], 0, 6, 0)    -- 净水箱（灶台后方）
  place(CATALOG[7], 2, 10, 0)   -- 太阳能板
  recomputeStats()
  local placed = 0
  for _ in pairs(instances) do placed = placed + 1 end
  toast.text = string.format("示例布局: 放置 %d 件, 载重 %.0f kg", placed, stats.weight)
  toast.t = 4.0
  -- 尝试自动读取上次布局
  local text = ReadText("saves/layout.json")
  if text and text ~= "" then loadLayout() end
end

function on_update(dt)
  if toast.t > 0 then toast.t = toast.t - dt end
  ensureHelpers()

  -- 键盘
  for i, key in ipairs(HOTKEYS) do
    if ActionPressed(key) then selected = i end
  end
  if ActionPressed("r") then rot = (rot + 1) % 2 end
  if ActionPressed("g") then
    gridVisible = not gridVisible
    if gridEnt ~= nil then SetVisible(gridEnt, gridVisible) end
  end
  if ActionPressed("f5") then saveLayout() end
  if ActionPressed("f9") then loadLayout() end

  -- 鼠标拾取
  local mp = InputMousePos()
  local hitSlot = hotbarHit(mp.x, mp.y)
  local overUi = hitSlot ~= nil
  local pick = PickGround(mp)
  hover = nil
  if pick then
    local cx, cz = worldToCell(pick.x, pick.z)
    if cx then
      local inst = findInstanceAt(cx, cz)
      local cat = CATALOG[selected]
      local valid = fits(cat, cx, cz, rot) and not overUi
      hover = { cx = cx, cz = cz, valid = valid, inst = inst, uiSlot = hitSlot }
      -- 幽灵
      local w, h = footprint(cat, rot)
      local ox = ORIGIN_X + cx * CELL + w * CELL * 0.5
      local oz = ORIGIN_Z + cz * CELL + h * CELL * 0.5
      SetVisible(ghostEnt, true)
      SetPosition(ghostEnt, { x = ox, y = FLOOR_Y + 0.035, z = oz })
      SetScale(ghostEnt, w * CELL - 0.04, 0.06, h * CELL - 0.04)
      if ghostValidLast ~= valid then
        setGhostColor(valid)
        ghostValidLast = valid
      end
      -- 悬停实例高亮（贴花投影盒覆盖模块 -> 光斑贴在模块顶面）
      SetVisible(hoverDecalEnt, inst ~= nil)
      if inst then
        local iw, ih = footprint(inst.cat, inst.rot)
        SetPosition(hoverDecalEnt, {
          x = ORIGIN_X + inst.cx * CELL + iw * CELL * 0.5,
          y = FLOOR_Y,
          z = ORIGIN_Z + inst.cz * CELL + ih * CELL * 0.5,
        })
        SetScale(hoverDecalEnt, iw * CELL, 1.0, ih * CELL)
        SetDecal(hoverDecalEnt, 1.0, 0.45, inst.cat.mh + 0.25)
      end
    end
  end
  if hover == nil then
    SetVisible(ghostEnt, false)
    SetVisible(hoverDecalEnt, false)
  end

  -- 鼠标操作
  if InputMousePressed(0) then
    if hitSlot then
      selected = hitSlot
    elseif hover and hover.valid then
      if place(CATALOG[selected], hover.cx, hover.cz, rot) then
        recomputeStats()
      end
    end
  end
  if InputMousePressed(1) and not overUi and hover then
    if removeAt(hover.cx, hover.cz) then recomputeStats() end
  end
  if ActionPressed("x") and hover then
    if removeAt(hover.cx, hover.cz) then recomputeStats() end
  end
end

-- 2D HUD 必须在 on_render 里画：draw2d 上下文只在渲染期接线，
-- on_update 里的 DrawRect/DrawText 会被静默丢弃。
function on_render()
  drawHud()
end
