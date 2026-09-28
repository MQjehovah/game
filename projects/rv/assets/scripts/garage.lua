-- ===========================================================================
-- Last Caravan（末路房车）车库沙盒 v0.2 — P1 布局深度
-- 网格模块化改装：双层放置（地板/车顶）/ 走道连通性 / 四资源+容量 / 存读档
-- 网格：4 x 14 格，格 0.5m；地板 y≈0.03，车顶 y≈2.25；车头在 +z（画面远端）
-- ===========================================================================

-- ---------------------------------------------------------------------------
-- 配置
-- ---------------------------------------------------------------------------
local CELL      = 0.5
local GW, GH    = 4, 14              -- 网格宽(x) x 长(z)，单位格
local ORIGIN_X  = -1.0               -- 格 (0,0) 的西边缘
local ORIGIN_Z  = -3.5               -- 格 (0,0) 的后边缘（镜头侧）
local FLOOR_Y   = 0.03               -- 车厢地板顶面
local ROOF_Y    = 2.25               -- 车顶承载面（墙顶 2.2）
local WEIGHT_MAX = 1500              -- 底盘载重 kg

-- 模块目录（v0.2：layer = "floor"/"roof"；w/h 单位=格，mh=高度米）
-- powerGen/powerDraw kW；waterStore/waterDraw L、L/h；weight kg；comfort 点数；
-- storage L（载物容量）。layer="roof" 的模块只能放车顶层。
-- modelScale: Kenney GLB 的统一缩放（模型原生 ~1 单位=1 米）
local CATALOG = {
  -- 地板层
  { id="bed_double", name="双人床",   layer="floor", w=3, h=4, mh=0.45, color="#8A5A44", weight=60,  comfort=30, modelScale=1.0, desc="睡眠质量 +30" },
  { id="bed_single", name="单人床",   layer="floor", w=2, h=3, mh=0.40, color="#A0714F", weight=35,  comfort=15, modelScale=1.0, desc="睡眠质量 +15" },
  { id="stove",      name="灶台",     layer="floor", w=2, h=2, mh=0.50, color="#7A7F87", weight=40,  comfort=5,  powerDraw=0.5, waterDraw=2, modelScale=1.0, desc="耗电 0.5kW 耗水 2L/h" },
  { id="fridge",     name="冰箱",     layer="floor", w=1, h=2, mh=1.10, color="#9FB4C7", weight=30,              powerDraw=0.3, modelScale=1.0, desc="耗电 0.3kW" },
  { id="water_tank", name="净水箱",   layer="floor", w=2, h=2, mh=0.90, color="#4F8FBF", weight=160,             waterStore=120, modelScale=1.0, desc="储水 120L" },
  { id="battery",    name="电池组",   layer="floor", w=2, h=1, mh=0.50, color="#D8B23A", weight=80,  battery=5,  modelScale=1.0, desc="储能 5kWh" },
  { id="workbench",  name="工作台",   layer="floor", w=2, h=3, mh=0.55, color="#8C6239", weight=70,  modelScale=1.2, desc="改装检修（P1 后开放）" },
  { id="storage",    name="储物箱",   layer="floor", w=1, h=1, mh=0.60, color="#6E7B52", weight=15,  storage=50,  modelScale=1.0, desc="储物 50L" },
  { id="heater",     name="电暖器",   layer="floor", w=1, h=1, mh=0.55, color="#C96B3F", weight=12,  comfort=10, powerDraw=0.8, modelScale=1.0, desc="耗电 0.8kW 舒适 +10" },
  { id="tv",         name="娱乐柜",   layer="floor", w=2, h=1, mh=0.90, color="#5D5366", weight=28,  comfort=8,  powerDraw=0.2, modelScale=1.0, desc="耗电 0.2kW 舒适 +8" },
  { id="sink",       name="水槽",     layer="floor", w=1, h=1, mh=0.35, color="#B8C4CE", weight=12,  waterDraw=1, comfort=2, modelScale=1.0, desc="耗水 1L/h 舒适 +2" },
  { id="bathroom",   name="卫生间",   layer="floor", w=2, h=3, mh=1.00, color="#7F9BA8", weight=120, comfort=12, waterDraw=3, modelScale=1.0, desc="耗水 3L/h 舒适 +12" },
  { id="generator",  name="发电机",   layer="floor", w=2, h=2, mh=0.70, color="#C7B45A", weight=180, powerGen=2.0, comfort=-5, modelScale=1.0, desc="发电 2.0kW 噪音 舒适 -5" },
  { id="med_cabinet",name="药柜",     layer="floor", w=1, h=1, mh=0.50, color="#D8E8E0", weight=20,  comfort=6,  modelScale=1.0, desc="医疗 舒适 +6" },
  { id="gun_rack",   name="武器架",   layer="floor", w=2, h=1, mh=0.50, color="#6B4F3A", weight=40,  modelScale=1.0, desc="武器存放（P2 夜袭）" },
  { id="turret",     name="炮塔底座", layer="floor", w=2, h=2, mh=0.60, color="#5A5A5A", weight=90,  modelScale=1.0, desc="预留（P2 夜袭开放）" },
  -- 车顶层（太阳能上车顶发电 +50%）
  { id="solar",      name="太阳能板", layer="roof",  w=2, h=3, mh=0.15, color="#3E6FB0", weight=25,  powerGen=1.8, modelScale=1.0, desc="发电 1.8kW（车顶限定）" },
  { id="roof_vent",  name="通风扇",   layer="roof",  w=1, h=1, mh=0.25, color="#8891A0", weight=8,   comfort=4,  powerDraw=0.1, modelScale=1.0, desc="耗电 0.1kW 舒适 +4" },
  { id="roof_rack",  name="车顶行李架", layer="roof", w=2, h=4, mh=0.35, color="#5C6670", weight=30, storage=150, modelScale=1.0, desc="车载储物 150L" },
}
local HOTKEYS = { "1","2","3","4","5","6","7","8","9","0" }  -- 前 10 件绑定热键

-- ---------------------------------------------------------------------------
-- 状态
-- ---------------------------------------------------------------------------
local selected  = 1          -- 当前选择的目录索引
local rot       = 0          -- 幽灵旋转 0/1（1 = 交换 w/h）
local curLayer  = "floor"    -- 当前放置层 "floor" / "roof"
local occupancy = {}         -- 地板层: cellIndex = cz*GW+cx -> 实例 id
local roofOcc   = {}         -- 车顶层占用
local instances = {}         -- id -> { cat=目录项, ent=实体, cx=, cz=, rot=, layer= }
local nextId    = 1
local hover     = nil        -- 当前悬停格 { cx=, cz=, valid= }
local ghostEnt, hoverDecalEnt
local gridFloorEnt, gridRoofEnt
local gridVisible = true
local ghostValidLast = nil
local toast     = { text="", t=0 }
local stats     = {}
local unreachableNames = {}  -- 不可达模块名列表（HUD 警告）

-- ---------------------------------------------------------------------------
-- 小工具
-- ---------------------------------------------------------------------------
local function cellIndex(cx, cz) return cz * GW + cx end

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

local function occGrid(layer)
  return layer == "roof" and roofOcc or occupancy
end

local function fits(cat, cx, cz, r)
  local cells = cellsOf(cat, cx, cz, r)
  if not cells then return false end
  local occ = occGrid(cat.layer)
  for _, c in ipairs(cells) do
    if occ[cellIndex(c.x, c.z)] ~= nil then return false end
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
-- 走道连通性：从驾驶舱门（前排整行）洪泛填充自由格；
-- 模块可达 = 占格四邻存在可达自由格。不可达模块舒适度不计并列入警告。
-- ---------------------------------------------------------------------------
local function computeReachability()
  local reach = {}
  local queue = {}
  for cx = 0, GW - 1 do
    local idx = cellIndex(cx, GH - 1)
    if occupancy[idx] == nil then
      reach[idx] = true
      queue[#queue + 1] = { cx = cx, cz = GH - 1 }
    end
  end
  local head = 1
  while queue[head] do
    local c = queue[head]
    head = head + 1
    local dirs = { {1,0}, {-1,0}, {0,1}, {0,-1} }
    for _, dv in ipairs(dirs) do
      local nx, nz = c.cx + dv[1], c.cz + dv[2]
      if nx >= 0 and nx < GW and nz >= 0 and nz < GH then
        local idx = cellIndex(nx, nz)
        if occupancy[idx] == nil and not reach[idx] then
          reach[idx] = true
          queue[#queue + 1] = { cx = nx, cz = nz }
        end
      end
    end
  end
  unreachableNames = {}
  for _, inst in pairs(instances) do
    if inst.cat.layer == "roof" then
      inst.reachable = true
    else
      local cells = cellsOf(inst.cat, inst.cx, inst.cz, inst.rot)
      inst.reachable = false
      if cells then
        local dirs = { {1,0}, {-1,0}, {0,1}, {0,-1} }
        for _, c in ipairs(cells) do
          for _, dv in ipairs(dirs) do
            local nx, nz = c.x + dv[1], c.z + dv[2]
            if nx >= 0 and nx < GW and nz >= 0 and nz < GH then
              if reach[cellIndex(nx, nz)] then inst.reachable = true end
            end
          end
        end
      end
      if not inst.reachable then
        unreachableNames[#unreachableNames + 1] = inst.cat.name
      end
    end
  end
  table.sort(unreachableNames)
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
  local baseY = (cat.layer == "roof") and ROOF_Y or FLOOR_Y
  -- 以锚点格(左上)为基准摆放整个占格范围
  local ox = ORIGIN_X + cx * CELL + w * CELL * 0.5
  local oz = ORIGIN_Z + cz * CELL + h * CELL * 0.5
  if cat.modelScale then
    -- 真模型（Kenney GLB）：统一缩放保持比例，不平展到占格盒
    SetPosition(ent, { x = ox, y = baseY + 0.01, z = oz })
    local sc = cat.modelScale
    SetScale(ent, sc, sc, sc)
  else
    SetPosition(ent, { x = ox, y = baseY + cat.mh * 0.5 + 0.005, z = oz })
    SetScale(ent, w * CELL - 0.03, cat.mh, h * CELL - 0.03)
  end
  if r % 2 == 1 then SetRotationY(ent, math.pi * 0.5) end
  return ent
end

local function place(cat, cx, cz, r)
  if cat.layer == "roof" and curLayer ~= "roof" then return false end
  if cat.layer ~= "roof" and curLayer == "roof" then return false end
  if not fits(cat, cx, cz, r) then return false end
  local ent = spawnModule(cat, cx, cz, r)
  if ent == nil then return false end
  local id = nextId
  nextId = nextId + 1
  local w, h = footprint(cat, r)
  local occ = occGrid(cat.layer)
  for dz = 0, h - 1 do
    for dx = 0, w - 1 do
      occ[cellIndex(cx + dx, cz + dz)] = id
    end
  end
  instances[id] = { cat = cat, ent = ent, cx = cx, cz = cz, rot = r, layer = cat.layer }
  computeReachability()
  return true
end

local function removeAt(cx, cz)
  local inst = findInstanceAt(cx, cz)
  if not inst then
    -- 车顶层拆除（悬停拾取仍走地面格）
    local occ = roofOcc
    for idx, id in pairs(occ) do
      local cand = instances[id]
      if cand then
        local w, h = footprint(cand.cat, cand.rot)
        for dz = 0, h - 1 do
          for dx = 0, w - 1 do
            if cand.cx + dx == cx and cand.cz + dz == cz then
              Despawn(cand.ent)
              for dz2 = 0, h - 1 do
                for dx2 = 0, w - 1 do
                  occ[cellIndex(cand.cx + dx2, cand.cz + dz2)] = nil
                end
              end
              instances[id] = nil
              computeReachability()
              return true
            end
          end
        end
      end
    end
    return false
  end
  local w, h = footprint(inst.cat, inst.rot)
  for dz = 0, h - 1 do
    for dx = 0, w - 1 do
      occupancy[cellIndex(inst.cx + dx, inst.cz + dz)] = nil
    end
  end
  Despawn(inst.ent)
  instances[inst.id] = nil
  computeReachability()
  return true
end

local function recomputeStats()
  local s = { powerGen = 0, powerDraw = 0, battery = 0, waterStore = 0, waterDraw = 0,
              weight = 0, comfort = 0, count = 0, storage = 0 }
  for _, inst in pairs(instances) do
    local c = inst.cat
    s.powerGen   = s.powerGen   + (c.powerGen   or 0)
    s.powerDraw  = s.powerDraw  + (c.powerDraw  or 0)
    s.battery    = s.battery    + (c.battery    or 0)
    s.waterStore = s.waterStore + (c.waterStore or 0)
    s.waterDraw  = s.waterDraw  + (c.waterDraw  or 0)
    s.weight     = s.weight     + (c.weight     or 0)
    s.storage    = s.storage    + (c.storage    or 0)
    -- 不可达模块的舒适度不计（够不着用不了）
    s.comfort    = s.comfort    + ((c.comfort or 0) * (inst.reachable == false and 0 or 1))
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
    rows[#rows + 1] = { id = inst.cat.id, cx = inst.cx, cz = inst.cz,
                        rot = inst.rot, layer = inst.layer }
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
  roofOcc = {}
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
    if cat then
      local saveLayer = row.layer or cat.layer or "floor"
      if saveLayer == "roof" then
        place(cat, math.floor(row.cx), math.floor(row.cz), math.floor(row.rot or 0))
      else
        -- 地板层放置需要临时切层（place 校验当前层）
        local saved = curLayer
        curLayer = "floor"
        place(cat, math.floor(row.cx), math.floor(row.cz), math.floor(row.rot or 0))
        curLayer = saved
      end
    end
  end
  computeReachability()
  toast.text = "布局已读取"
  toast.t = 2.5
end

-- ---------------------------------------------------------------------------
-- 幽灵 / 高亮 / 网格
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
  -- 地板网格：标准贴花用法（同 moba spawnGroundQuad）：组件 size 保持 1.0，
  -- 投影盒长宽/高度用实体 SetScale 控制；size 与 SetScale 叠乘会放大 size 倍。
  if gridFloorEnt == nil then
    gridFloorEnt = SpawnDecal("assets/sprites/grid_4x14.png", 0, FLOOR_Y, 0,
                              1.0, 0.55, 1, 1, 1, true, 0.1)
    SetScale(gridFloorEnt, 2.0, 1.0, 7.0)
  end
  -- 车顶网格：独立贴花，随层显隐
  if gridRoofEnt == nil then
    gridRoofEnt = SpawnDecal("assets/sprites/grid_4x14.png", 0, ROOF_Y, 0,
                             1.0, 0.55, 1, 1, 1, true, 0.1)
    SetScale(gridRoofEnt, 2.0, 1.0, 7.0)
    SetVisible(gridRoofEnt, false)
  end
  if hoverDecalEnt == nil then
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
  local pw, ph = 250, 236
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
  drawStatBar(10 + pad, rowY, innerW, "储物",
    string.format("%d L", s.storage),
    math.min(1, s.storage / 400),
    false)
  rowY = rowY + 34
  drawStatBar(10 + pad, rowY, innerW, "舒适",
    string.format("%d%%", math.min(100, s.comfort)),
    math.min(1, s.comfort / 100),
    false)

  -- 不可达警告
  if #unreachableNames > 0 then
    DrawText("不可达: " .. table.concat(unreachableNames, "、"),
             10 + pad, 10 + ph - 10, 13, 1, 0.45, 0.3, 1)
  end

  -- 底部热栏：3 行 x 7 列（19 模块）
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

  -- 右上提示
  local hints = {
    "左键 放置   右键/X 拆除",
    "R 旋转   Tab 地板/车顶",
    "G 网格   F5/F9 存/读",
  }
  for i, h in ipairs(hints) do
    DrawText(h, vw - 14, 14 + (i - 1) * 20, 14, 0.85, 0.84, 0.80, 0.9, true, true)
  end

  -- 当前层指示
  DrawText(curLayer == "roof" and "【车顶层】" or "【地板层】",
           math.floor(vw * 0.5), 14, 16, 0.55, 0.85, 0.98, 1, true)

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

-- ---------------------------------------------------------------------------
-- 生命周期
-- ---------------------------------------------------------------------------
function on_start()
  ensureHelpers()
  -- 初始示例布局（有存档时会被覆盖）：一眼看到“布置好的房车”
  place(CATALOG[1], 1, 0, 0)    -- 双人床（贴后墙右侧，3x4 格）
  place(CATALOG[3], 0, 4, 0)    -- 灶台（左舷中段）
  place(CATALOG[5], 0, 6, 0)    -- 净水箱（灶台后方）
  place(CATALOG[9], 3, 6, 0)    -- 储物箱
  local saved = curLayer
  curLayer = "roof"
  place(CATALOG[17], 0, 8, 0)   -- 太阳能板（车顶）
  place(CATALOG[19], 2, 9, 0)   -- 车顶行李架
  curLayer = saved
  computeReachability()
  recomputeStats()
  local placed = 0
  for _ in pairs(instances) do placed = placed + 1 end
  toast.text = string.format("示例布局: 放置 %d 件, 载重 %.0f kg", placed, stats.weight)
  toast.t = 4.0
  -- 尝试自动读取上次布局
  local text = ReadText("saves/layout.json")
  if text and text ~= "" then loadLayout() end
end

function on_update(ent, dt)
  if toast.t > 0 then toast.t = toast.t - dt end
  ensureHelpers()

  -- 键盘
  for i, key in ipairs(HOTKEYS) do
    if ActionPressed(key) then selected = i end
  end
  if ActionPressed("r") then rot = (rot + 1) % 2 end
  if ActionPressed("tab") then
    curLayer = (curLayer == "roof") and "floor" or "roof"
    SetVisible(gridFloorEnt, curLayer == "floor" and gridVisible or false)
    SetVisible(gridRoofEnt, curLayer == "roof" and gridVisible or false)
  end
  if ActionPressed("g") then
    gridVisible = not gridVisible
    SetVisible(gridFloorEnt, gridVisible)
    SetVisible(gridRoofEnt, gridVisible and curLayer == "roof")
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
      -- 目录条目的层必须与当前层一致才可放置
      local layerOk = (cat.layer == "roof") == (curLayer == "roof")
      local valid = layerOk and fits(cat, cx, cz, rot) and not overUi
      hover = { cx = cx, cz = cz, valid = valid, inst = inst, uiSlot = hitSlot }
      -- 幽灵（当前层高度）
      local w, h = footprint(cat, rot)
      local baseY = (curLayer == "roof") and ROOF_Y or FLOOR_Y
      local ox = ORIGIN_X + cx * CELL + w * CELL * 0.5
      local oz = ORIGIN_Z + cz * CELL + h * CELL * 0.5
      SetVisible(ghostEnt, true)
      SetPosition(ghostEnt, { x = ox, y = baseY + 0.035, z = oz })
      SetScale(ghostEnt, w * CELL - 0.04, 0.06, h * CELL - 0.04)
      if ghostValidLast ~= valid then
        setGhostColor(valid)
        ghostValidLast = valid
      end
      -- 悬停实例高亮（贴花投影盒覆盖模块 -> 光斑贴在模块顶面）
      SetVisible(hoverDecalEnt, inst ~= nil)
      if inst then
        local iw, ih = footprint(inst.cat, inst.rot)
        local baseYI = (inst.layer == "roof") and ROOF_Y or FLOOR_Y
        SetPosition(hoverDecalEnt, {
          x = ORIGIN_X + inst.cx * CELL + iw * CELL * 0.5,
          y = baseYI,
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
