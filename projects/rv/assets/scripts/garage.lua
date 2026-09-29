-- ===========================================================================
-- Last Caravan（末路房车）v2 — 全新架构
--   phase = "parked" | "driving"（驻车随时可建造；停靠节点 = 附加搜刮/夜袭层）
--   主角：F 下车行走/上车；步行可射击夜袭尸群；C 切换驾驶视角（追逐/驾驶席）
--   山林越野场景：松林/橡树/悬崖岩石/仙人掌/篝火营地（Kenney nature 套件）
--   网格：4 x 14 格，格 0.5m；车头在 +z
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

-- 模块目录（layer = "floor"/"roof"；mh=高度米；powerGen/Draw kW、water L、weight kg）
local CATALOG = {
  { id="bed_double", name="双人床",   layer="floor", w=3, h=4, mh=0.45, color="#8A5A44", weight=60,  comfort=30, modelScale=1.0, desc="睡眠质量 +30" , cost={wood=4,cloth=4}},
  { id="bed_single", name="单人床",   layer="floor", w=2, h=3, mh=0.40, color="#A0714F", weight=35,  comfort=15, modelScale=1.0, desc="睡眠质量 +15" , cost={wood=2,cloth=2}},
  { id="stove",      name="灶台",     layer="floor", w=2, h=2, mh=0.50, color="#7A7F87", weight=40,  comfort=5,  powerDraw=0.5, waterDraw=2, modelScale=1.0, desc="耗电 0.5kW 耗水 2L/h" , cost={metal=3,electronics=1}},
  { id="fridge",     name="冰箱",     layer="floor", w=1, h=2, mh=1.10, color="#9FB4C7", weight=30,              powerDraw=0.3, modelScale=1.0, desc="耗电 0.3kW" , cost={metal=2,electronics=3}},
  { id="water_tank", name="净水箱",   layer="floor", w=2, h=2, mh=0.90, color="#4F8FBF", weight=160,             waterStore=120, modelScale=1.0, desc="储水 120L" , cost={metal=4}},
  { id="battery",    name="电池组",   layer="floor", w=2, h=1, mh=0.50, color="#D8B23A", weight=80,  battery=5,  modelScale=1.0, desc="储能 5kWh" , cost={metal=2,electronics=4}},
  { id="workbench",  name="工作台",   layer="floor", w=2, h=3, mh=0.55, color="#8C6239", weight=70,  modelScale=1.2, desc="改装检修" , cost={metal=2,wood=3}},
  { id="storage",    name="储物箱",   layer="floor", w=1, h=1, mh=0.60, color="#6E7B52", weight=15,  storage=50,  modelScale=1.0, desc="储物 50L" , cost={wood=2}},
  { id="heater",     name="电暖器",   layer="floor", w=1, h=1, mh=0.55, color="#C96B3F", weight=12,  comfort=10, powerDraw=0.8, modelScale=1.0, desc="耗电 0.8kW 舒适 +10" , cost={metal=1,electronics=2}},
  { id="tv",         name="娱乐柜",   layer="floor", w=2, h=1, mh=0.90, color="#5D5366", weight=28,  comfort=8,  powerDraw=0.2, modelScale=1.0, desc="耗电 0.2kW 舒适 +8" , cost={electronics=3,cloth=1}},
  { id="sink",       name="水槽",     layer="floor", w=1, h=1, mh=0.35, color="#B8C4CE", weight=12,  waterDraw=1, comfort=2, modelScale=1.0, desc="耗水 1L/h 舒适 +2" , cost={metal=2}},
  { id="bathroom",   name="卫生间",   layer="floor", w=2, h=3, mh=1.00, color="#7F9BA8", weight=120, comfort=12, waterDraw=3, modelScale=1.0, desc="耗水 3L/h 舒适 +12" , cost={metal=3,wood=2}},
  { id="generator",  name="发电机",   layer="floor", w=2, h=2, mh=0.70, color="#C7B45A", weight=180, powerGen=2.0, comfort=-5, modelScale=1.0, desc="发电 2.0kW 噪音 舒适 -5" , cost={metal=6,electronics=3}},
  { id="med_cabinet",name="药柜",     layer="floor", w=1, h=1, mh=0.50, color="#D8E8E0", weight=20,  comfort=6,  modelScale=1.0, desc="医疗 舒适 +6" , cost={metal=1,cloth=2}},
  { id="gun_rack",   name="武器架",   layer="floor", w=2, h=1, mh=0.50, color="#6B4F3A", weight=40,  modelScale=1.0, desc="夜袭时单发伤害翻倍" , cost={wood=3,metal=1}},
  { id="turret",     name="炮塔底座", layer="floor", w=2, h=2, mh=0.60, color="#5A5A5A", weight=90,  modelScale=1.0, desc="夜袭时自动射击 18m 内尸群" , cost={metal=8,electronics=4}},
  { id="solar",      name="太阳能板", layer="roof",  w=2, h=3, mh=0.15, color="#3E6FB0", weight=25,  powerGen=1.8, modelScale=1.0, desc="发电 1.8kW（车顶限定）" , cost={metal=2,electronics=4}},
  { id="roof_vent",  name="通风扇",   layer="roof",  w=1, h=1, mh=0.25, color="#8891A0", weight=8,   comfort=4,  powerDraw=0.1, modelScale=1.0, desc="耗电 0.1kW 舒适 +4" , cost={metal=1,electronics=1}},
  { id="roof_rack",  name="车顶行李架", layer="roof", w=2, h=4, mh=0.35, color="#5C6670", weight=30, storage=150, modelScale=1.0, desc="车载储物 150L" , cost={metal=3}},
}
local HOTKEYS = { "1","2","3","4","5","6","7","8","9","0" }

local DRIVE = {
  accel = 4.5, brake = 9.0, drag = 0.8, reverseMax = 3.0,
  maxSpeed = 9.0, steerRate = 1.4,
  camDist = 10.5, camPitch = 0.40,
}
-- 车辆：燃油 / 车体耐久
local VEH = {
  tank = 60, burn = 0.75, limpSpeed = 1.5,
  durMax = 100, impactK = 1.5, repairAmt = 25,
  repairCost = { metal = 2, wood = 1 },
  eventMin = 16, eventMax = 32,
}
-- 停靠搜刮 / 夜袭
local STOP = {
  crates = 5, threatMax = 6, evacTime = 15, searchThreat = 1,
  keepRatio = 0.6, pickRadius = 30,
}
local RAID = {
  spawnEvery = 1.1, aliveMax = 9, speed = 1.8, hpBase = 2,
  attackDist = 2.3, stealEvery = 0.9, hitRadius = 30,
  fireCd = 0.22, turretCd = 0.65, turretRange = 18,
}
-- 途中事件（权重）：尸群横穿 / 路边补给 / 燃油桶
local EVENTS = {
  { id = "horde",   w = 4 },
  { id = "supply",  w = 3 },
  { id = "fuelcan", w = 2 },
}
-- worldmap.json 节点的世界坐标（营地=原点，地图 1.0 ≈ 160m）
local NODES = {
  { id = "camp",    name = "营地",       x = 0,   z = 0 },
  { id = "gasstop", name = "废弃加油站", x = 0,  z = 60 },  -- TMP
  { id = "ruin",    name = "郊区废宅",   x = 38,  z = 27 },
  { id = "mall",    name = "购物中心",   x = 100, z = -73 },
  { id = "town",    name = "小镇主街",   x = 131, z = -8 },
  { id = "depot",   name = "物流仓库",   x = 154, z = -58 },
}
local MAT_NAMES = { metal="金属片", electronics="电子件", cloth="布料", wood="木材" }
local materials = { metal=12, electronics=8, cloth=6, wood=10 }

-- 山林/越野场景池（Kenney nature 套件模型 + 地面灌木色块）
local SCENERY = {
  { pre = "rv_pine_a",     n = 10, kind = "tree" },
  { pre = "rv_pine_b",     n = 10, kind = "tree" },
  { pre = "rv_pine_c",     n = 10, kind = "tree" },
  { pre = "rv_oak",        n = 8,  kind = "tree" },
  { pre = "rv_deadtree",   n = 8,  kind = "tree" },
  { pre = "rv_rock_big",   n = 6,  kind = "rock" },
  { pre = "rv_rock_block", n = 10, kind = "rock" },
  { pre = "rv_cactus",     n = 8,  kind = "cactus" },
  { pre = "rv_scrub_a",    n = 34, kind = "scrub" },
  { pre = "rv_scrub_b",    n = 26, kind = "scrub" },
}

-- ---------------------------------------------------------------------------
-- 状态
-- ---------------------------------------------------------------------------
local selected, rot, curLayer = 1, 0, "floor"
local occupancy, roofOcc = {}, {}
local instances, nextId = {}, 1
local hover = nil
local ghostEnt, hoverDecalEnt, gridFloorEnt, gridRoofEnt = nil, nil, nil, nil
local gridVisible = true
local ghostValidLast = nil
local toast = { text = "", t = 0 }
local stats = {}
local unreachableNames = {}

-- 阶段：驻车（建造+停靠层）/ 行驶；onFoot = 主角下车
local phase, onFoot = "parked", false
local driveView = "chase"          -- 行驶视角 "chase" | "cab"
local atNode = nil                 -- 驻车点在节点旁 = 节点 id（搜刮层激活）
local rv = { x = 0, z = 0, yaw = 0, speed = 0 }
local curNode, day = "camp", 1
local rvBody = nil                 -- RV 动态刚体（Jolt）
local rvParts, camEnt, groundEnt = nil, nil, nil
local camSmooth = nil
local impactCd = 0
local fuel, dur = 60, 100
local fuelWarned = false
local dustAcc = 0
local mapOpen = false

-- 搜刮 / 夜袭
local stopBag, stopThreat, evacT = {}, 0, -1
local crates = {}
local horde, hordeT = {}, 0
local fireT, turretT, killCount, raidT = 0, 0, 0, 0

-- 途中事件
local nextEventT = 20
local driveProps, driveHorde = {}, {}

-- 主角
local player = { active = false, x = 0, z = 0, yaw = 0, body = nil, head = nil, gun = nil }

-- 验证钩子
local autoPilot, driveT, autoT, autoTarget = false, 0, 0, nil
local nearNode = nil

-- ---------------------------------------------------------------------------
-- 小工具
-- ---------------------------------------------------------------------------
local function cellIndex(cx, cz) return cz * GW + cx end

-- 世界点 -> 房车局部（逆旋转 -yaw）再落格：房车停在任意位置/朝向都能放置
local function worldToCell(x, z)
  local dx, dz = x - rv.x, z - rv.z
  local cy, sy = math.cos(rv.yaw), math.sin(rv.yaw)
  local lx = dx * cy - dz * sy
  local lz = dx * sy + dz * cy
  local cx = math.floor((lx - ORIGIN_X) / CELL)
  local cz = math.floor((lz - ORIGIN_Z) / CELL)
  if cx < 0 or cx >= GW or cz < 0 or cz >= GH then return nil end
  return cx, cz
end

-- 房车局部点 -> 世界坐标（前进 = (sin yaw, cos yaw)）
local function rotOf(lx, lz)
  local cy, sy = math.cos(rv.yaw), math.sin(rv.yaw)
  return rv.x + lx * cy + lz * sy, rv.z - lx * sy + lz * cy
end

-- 方向向量 -> yaw（前进 = (sin yaw, cos yaw)）
local function dirYaw(dx, dz)
  if dz > 0 then return math.atan(dx / dz)
  elseif dz < 0 then return math.atan(dx / dz) + (dx >= 0 and math.pi or -math.pi)
  else return (dx >= 0) and (math.pi * 0.5) or (-math.pi * 0.5) end
end

local function footprint(cat, r)
  if r % 2 == 1 then return cat.h, cat.w end
  return cat.w, cat.h
end

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

local function canAfford(cost)
  for k, v in pairs(cost) do
    if (materials[k] or 0) < v then return false end
  end
  return true
end

local function payCost(cost)
  for k, v in pairs(cost) do materials[k] = (materials[k] or 0) - v end
end

local function refundCost(cost)
  for k, v in pairs(cost) do materials[k] = (materials[k] or 0) + v end
end

local function costText(cost)
  local parts = {}
  for k, v in pairs(cost) do parts[#parts + 1] = MAT_NAMES[k] .. "x" .. v end
  return table.concat(parts, " ")
end

local function hexColor(s)
  local n = tonumber(string.sub(s, 2), 16)
  if not n then return 1, 1, 1 end
  local b = n % 256
  local g = math.floor(n / 256) % 256
  local r = math.floor(n / 65536) % 256
  return r / 255, g / 255, b / 255
end

local function nodeById(id)
  for _, nd in ipairs(NODES) do
    if nd.id == id then return nd end
  end
  return nil
end

local function hasModule(id)
  for _, inst in pairs(instances) do
    if inst.cat.id == id then return inst end
  end
  return nil
end

local function nodeName(id)
  local nd = nodeById(id)
  return nd and nd.name or "荒野"
end

-- ---------------------------------------------------------------------------
-- 走道连通性：从驾驶舱门（前排整行）洪泛填充自由格
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
    for _, dv in ipairs({ {1,0}, {-1,0}, {0,1}, {0,-1} }) do
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
        for _, c in ipairs(cells) do
          for _, dv in ipairs({ {1,0}, {-1,0}, {0,1}, {0,-1} }) do
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
  local ent = SpawnPrefab("rv_" .. cat.id, { x = 0, y = 0, z = 0 })
  if ent == nil then return nil end
  local w, h = footprint(cat, r)
  local baseY = (cat.layer == "roof") and ROOF_Y or FLOOR_Y
  local ox = ORIGIN_X + cx * CELL + w * CELL * 0.5
  local oz = ORIGIN_Z + cz * CELL + h * CELL * 0.5
  local ly = cat.modelScale and (baseY + 0.01) or (baseY + cat.mh * 0.5 + 0.005)
  local wx, wz = rotOf(ox, oz)
  if cat.modelScale then
    SetPosition(ent, { x = wx, y = ly, z = wz })
    local sc = cat.modelScale
    SetScale(ent, sc, sc, sc)
  else
    SetPosition(ent, { x = wx, y = ly, z = wz })
    SetScale(ent, w * CELL - 0.03, cat.mh, h * CELL - 0.03)
  end
  local lrot = (r % 2 == 1) and (math.pi * 0.5) or 0
  SetRotationY(ent, rv.yaw + lrot)
  local sx, sy, sz
  if cat.modelScale then
    sx, sy, sz = cat.modelScale, cat.modelScale, cat.modelScale
  else
    sx, sy, sz = w * CELL - 0.03, cat.mh, h * CELL - 0.03
  end
  Tween(ent, 2, { x = sx * 0.6, y = sy * 0.6, z = sz * 0.6 },
              { x = sx, y = sy, z = sz }, 0.18, 1)
  return ent, ox, ly, oz, lrot
end

local function place(cat, cx, cz, r, free)
  if cat.layer == "roof" and curLayer ~= "roof" then return false end
  if cat.layer ~= "roof" and curLayer == "roof" then return false end
  if not fits(cat, cx, cz, r) then return false end
  if not free and not canAfford(cat.cost or {}) then return false end
  local ent, ox, ly, oz, lrot = spawnModule(cat, cx, cz, r)
  if ent == nil then return false end
  if not free then payCost(cat.cost or {}) end
  local id = nextId
  nextId = nextId + 1
  local w, h = footprint(cat, r)
  local occ = occGrid(cat.layer)
  for dz = 0, h - 1 do
    for dx = 0, w - 1 do
      occ[cellIndex(cx + dx, cz + dz)] = id
    end
  end
  instances[id] = { cat = cat, ent = ent, cx = cx, cz = cz, rot = r, layer = cat.layer,
                    lx = ox, ly = ly, lz = oz, lrot = lrot }
  computeReachability()
  return true
end

local function removeAt(cx, cz)
  local inst = findInstanceAt(cx, cz)
  if not inst then
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
              refundCost(cand.cat.cost or {})
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
  refundCost(inst.cat.cost or {})
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
    s.comfort    = s.comfort    + ((c.comfort or 0) * (inst.reachable == false and 0 or 1))
    s.count      = s.count + 1
  end
  stats = s
end

-- ---------------------------------------------------------------------------
-- JSON（存读档）
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
    pos = pos + 1
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
        pos = pos + 1
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

-- ---------------------------------------------------------------------------
-- 房车装配：车体部件 + 模块 + 网格贴花随车位姿整体变换
-- ---------------------------------------------------------------------------
local function collectRvParts()
  if rvParts then return end
  rvParts = {}
  local names = { "RV_Floor", "RV_WallL", "RV_WallR", "RV_RearWall", "RV_Cab",
                  "RV_Windshield", "Wheel_FL", "Wheel_FR", "Wheel_RL", "Wheel_RR" }
  for _, n in ipairs(names) do
    local e = FindNamedEntity(n)
    if e ~= nil then
      local p = GetPosition(e)
      rvParts[#rvParts + 1] = { ent = e, lx = p.x, ly = p.y, lz = p.z }
    end
  end
end

local function applyRvTransform()
  if rvParts then
    for _, p in ipairs(rvParts) do
      local wx, wz = rotOf(p.lx, p.lz)
      SetPosition(p.ent, { x = wx, y = p.ly, z = wz })
      SetRotationY(p.ent, rv.yaw)
    end
  end
  for _, inst in pairs(instances) do
    local wx, wz = rotOf(inst.lx, inst.lz)
    SetPosition(inst.ent, { x = wx, y = inst.ly, z = wz })
    SetRotationY(inst.ent, rv.yaw + (inst.lrot or 0))
  end
  if gridFloorEnt ~= nil then
    SetPosition(gridFloorEnt, { x = rv.x, y = FLOOR_Y, z = rv.z })
    SetRotationY(gridFloorEnt, rv.yaw)
  end
  if gridRoofEnt ~= nil then
    SetPosition(gridRoofEnt, { x = rv.x, y = ROOF_Y, z = rv.z })
    SetRotationY(gridRoofEnt, rv.yaw)
  end
end

-- ---------------------------------------------------------------------------
-- 山林越野场景：松林/橡树/枯树/岩石/仙人掌/灌木（回收循环）+ 节点标记
-- ---------------------------------------------------------------------------
local function sceneryScale(kind)
  if kind == "rock" then
    return 1.2 + math.random() * 1.6
  elseif kind == "cactus" then
    return 0.8 + math.random() * 0.9
  elseif kind == "tree" then
    return 0.9 + math.random() * 0.9
  end
  return 1
end

local function placeScenery(p, fx, fz, fMin, fMax)
  for _ = 1, 6 do
    local f = fMin + math.random() * (fMax - fMin)
    local lat = (math.random() * 2 - 1) * 44
    if math.abs(lat) >= 6 or fMin < 0 then
      local wx = rv.x + fx * f - fz * lat
      local wz = rv.z + fz * f + fx * lat
      local blocked = false
      local rdx, rdz = wx - rv.x, wz - rv.z
      if rdx * rdx + rdz * rdz < 100 then blocked = true end  -- 房车 10m 内
      for _, nd in ipairs(NODES) do
        local ddx, ddz = wx - nd.x, wz - nd.z
        if ddx * ddx + ddz * ddz < 121 then blocked = true break end  -- 节点 11m
      end
      if not blocked then
        local s = sceneryScale(p.kind)
        local y = 0
        if p.kind == "scrub" then
          y = 0.015
          SetScale(p.ent, 0.5 + math.random(), 0.03 + math.random() * 0.04, 0.5 + math.random())
        else
          SetScale(p.ent, s, s, s)
          if p.kind == "rock" then y = s * 0.55 end
        end
        SetPosition(p.ent, { x = wx, y = y, z = wz })
        SetRotationY(p.ent, math.random() * math.pi * 2)
        p.x, p.z = wx, wz
        -- 树/岩石 = 静态碰撞体（越野障碍），回收 = 传送刚体
        if p.kind == "tree" or p.kind == "rock" then
          local half = (p.kind == "rock") and s * 0.7 or 0.3
          local cy = (p.kind == "rock") and s * 0.55 or 1.0
          if p.body then
            PhysicsSetPosition(p.body, { x = wx, y = cy, z = wz })
          else
            p.body = PhysicsAddBox({ x = wx, y = cy, z = wz },
                                   { x = half, y = cy, z = half }, false, {})
          end
        end
        return
      end
    end
  end
  local wx = rv.x + fx * (fMax + 20) - fz * 30
  local wz = rv.z + fz * (fMax + 20) + fx * 30
  SetPosition(p.ent, { x = wx, y = 0.3, z = wz })
  p.x, p.z = wx, wz
end

local scenery = {}
local function spawnScenery()
  if #scenery > 0 then return end
  for _, t in ipairs(SCENERY) do
    for _ = 1, t.n do
      local ent = SpawnPrefab(t.pre, { x = 0, y = -50, z = 0 })
      if ent ~= nil then
        local p = { ent = ent, kind = t.kind, x = 0, z = 0, body = nil }
        placeScenery(p, 0, 1, -45, 70)  -- 初始绕营地铺开
        scenery[#scenery + 1] = p
      end
    end
  end
end

local function recycleScenery(fx, fz)
  for _, p in ipairs(scenery) do
    local dx, dz = p.x - rv.x, p.z - rv.z
    local f = dx * fx + dz * fz
    local lat = dx * (-fz) + dz * fx
    if f < -40 or f > 95 or math.abs(lat) > 55 then
      placeScenery(p, fx, fz, 25, 75)
    end
  end
end

-- 节点标记柱 + 营地篝火 + 加油站油桶
local function spawnMarkers()
  for _, nd in ipairs(NODES) do
    if nd.id ~= "camp" then
      local ent = SpawnPrefab("rv_marker", { x = nd.x, y = 1.6, z = nd.z })
      if ent ~= nil then
        SetScale(ent, 0.16, 3.2, 0.16)
        PhysicsAddBox({ x = nd.x, y = 1.6, z = nd.z },
                      { x = 0.09, y = 1.6, z = 0.09 }, false, {})
      end
    end
  end
  local fire = SpawnPrefab("rv_campfire", { x = 1.8, y = 0.12, z = 2.0 })
  if fire ~= nil then SetScale(fire, 0.9, 0.9, 0.9) end
  local gas = nodeById("gasstop")
  if gas then
    for _, off in ipairs({ {2.0, 1.0}, {2.6, -0.4}, {1.2, -0.6} }) do
      local ent = SpawnPrefab("rv_fuelcan",
                              { x = gas.x + off[1], y = 0.35, z = gas.z + off[2] })
      if ent ~= nil then SetScale(ent, 0.5, 0.7, 0.5) end
    end
  end
end

-- ---------------------------------------------------------------------------
-- 主角：F 下车/上车；步行 WASD（W/S 前后、A/D 转向）；可射击夜袭尸群
-- ---------------------------------------------------------------------------
local function spawnPlayer()
  if player.active then return end
  local bx, bz = rotOf(1.7, -1.0)
  player.x, player.z, player.yaw = bx, bz, rv.yaw + math.pi * 0.5
  player.body = SpawnPrefab("rv_player_body", { x = bx, y = 0.53, z = bz })
  player.head = SpawnPrefab("rv_player_head", { x = bx, y = 1.32, z = bz })
  player.gun  = SpawnPrefab("rv_player_gun",  { x = bx, y = 0.95, z = bz })
  player.active = (player.body ~= nil)
  if not player.active then return end
  onFoot = true
end

local function despawnPlayer()
  if player.body ~= nil then Despawn(player.body) end
  if player.head ~= nil then Despawn(player.head) end
  if player.gun ~= nil then Despawn(player.gun) end
  player.body, player.head, player.gun = nil, nil, nil
  player.active = false
  onFoot = false
end

local function updatePlayer(dt)
  if not onFoot or player.body == nil then return end
  -- 坦克式行走：W/S 前后，A/D 转向（与驾驶操作一致）
  local mv = (ActionDown("w") and 1 or 0) - (ActionDown("s") and 1 or 0)
  local turn = (ActionDown("a") and 1 or 0) - (ActionDown("d") and 1 or 0)
  player.yaw = player.yaw + turn * 2.4 * dt
  local fx, fz = math.sin(player.yaw), math.cos(player.yaw)
  local sp = (mv >= 0) and 3.2 or 1.6
  player.x = player.x + fx * sp * mv * dt
  player.z = player.z + fz * sp * mv * dt
  -- 车内行走：进入车厢范围则夹在 4x14 格内部（不出墙）
  local dx, dz = player.x - rv.x, player.z - rv.z
  local cy, sy = math.cos(rv.yaw), math.sin(rv.yaw)
  local lx = dx * cy - dz * sy
  local lz = dx * sy + dz * cy
  if math.abs(lx) < 1.5 and math.abs(lz) < 4.3 then
    lx = math.max(-1.0, math.min(1.0, lx))
    lz = math.max(-3.5, math.min(3.5, lz))
    player.x = rv.x + lx * cy + lz * sy
    player.z = rv.z - lx * sy + lz * cy
  end
  local bob = math.abs(math.sin(raidT * 9 + 1)) * (mv ~= 0 and 0.05 or 0.015)
  SetPosition(player.body, { x = player.x, y = 0.53 + bob * 0.3, z = player.z })
  SetPosition(player.head, { x = player.x, y = 1.32 + bob, z = player.z })
  SetPosition(player.gun, { x = player.x + fx * 0.3, y = 0.95, z = player.z + fz * 0.3 })
  SetRotationY(player.body, player.yaw)
  SetRotationY(player.head, player.yaw)
  SetRotationY(player.gun, player.yaw)
end

local function playerMuzzle()
  if onFoot and player.active then
    local fx, fz = math.sin(player.yaw), math.cos(player.yaw)
    return player.x + fx * 0.35, 0.95, player.z + fz * 0.35
  end
  local turret = hasModule and hasModule("turret") or nil
  if turret then
    local mx, mz = rotOf(turret.lx, turret.lz)
    return mx, (turret.ly or 2.6) + 0.5, mz
  end
  local mx, mz = rotOf(0, 1.5)
  return mx, 2.35, mz
end

-- ---------------------------------------------------------------------------
-- 停靠搜刮（驻车在节点旁时的附加层；建造不受影响）
-- ---------------------------------------------------------------------------
local function nodeLootRoll(nd)
  local out = {}
  for mat, range in pairs(nd.loot or {}) do
    local lo, hi = range[1] or 0, range[2] or 0
    if hi > lo then
      local n = lo + math.random(0, hi - lo)
      if math.random() < 0.12 then n = n + hi end
      if n > 0 then out[mat] = (out[mat] or 0) + n end
    end
  end
  if not next(out) then out.cloth = 1 end
  return out
end

local function despawnCrates()
  for _, c in ipairs(crates) do Despawn(c.ent) end
  crates = {}
end

local function spawnCrates()
  despawnCrates()
  for i = 1, STOP.crates do
    local ent = SpawnPrefab("rv_loot_crate", { x = 0, y = -50, z = 0 })
    if ent ~= nil then
      local cx, cz
      for _ = 1, 12 do
        local a = math.random() * math.pi * 2
        local r = 4.0 + math.random() * 5.0
        local wx, wz = rotOf(math.sin(a) * r, math.cos(a) * r)
        local rdx, rdz = wx - rv.x, wz - rv.z
        if rdx * rdx + rdz * rdz >= 9 then
          local ok = true
          for _, c in ipairs(crates) do
            local ddx, ddz = wx - c.x, wz - c.z
            if ddx * ddx + ddz * ddz < 1.96 then ok = false break end
          end
          if ok then cx, cz = wx, wz break end
        end
      end
      if cx == nil then cx, cz = rotOf(3.6, -2.0 + 1.4 * (i - 1)) end
      SetPosition(ent, { x = cx, y = 0.28, z = cz })
      SetScale(ent, 0.55, 0.55, 0.55)
      SetRotationY(ent, math.random() * math.pi * 2)
      crates[#crates + 1] = { ent = ent, x = cx, z = cz }
    end
  end
end

local function searchCrate(idx)
  local c = crates[idx]
  if c == nil then return end
  local nd = nodeById(atNode or curNode) or { loot = {}, danger = 0 }
  local loot = nodeLootRoll(nd)
  local parts = {}
  for mat, n in pairs(loot) do
    stopBag[mat] = (stopBag[mat] or 0) + n
    parts[#parts + 1] = MAT_NAMES[mat] .. "+" .. n
  end
  SpawnFloatText(c.x, 0.9, c.z, table.concat(parts, "  "),
                 false, 1.2, 1.0, 0.85, 0.4)
  Despawn(c.ent)
  table.remove(crates, idx)
  stopThreat = math.min(STOP.threatMax,
                        stopThreat + STOP.searchThreat + (nd.danger or 0))
  if stopThreat >= STOP.threatMax and evacT < 0 then
    evacT = STOP.evacTime
    toast.text = "尸群被惊动了！赶紧撤（E 收藏撤离 / T 上路）"
    toast.t = 3.5
  end
end

local function bankStopBag(keepRatio)
  local got = {}
  for mat, n in pairs(stopBag) do
    local v = math.floor(n * (keepRatio or 1) + 0.5)
    if v > 0 then
      materials[mat] = (materials[mat] or 0) + v
      got[#got + 1] = MAT_NAMES[mat] .. "+" .. v
    end
  end
  stopBag = {}
  return table.concat(got, " ")
end

local despawnHorde  -- 前置声明
local setPhase      -- 前置声明（endScavenge 引用）
local function endScavenge(keepRatio)
  local got = bankStopBag(keepRatio)
  despawnCrates()
  despawnHorde()
  stopThreat = 0
  evacT = -1
  day = day + 1
  atNode = nil
  toast.text = (got == "" and "空手而归" or "物资入库: " .. got)
               .. " — 第 " .. day .. " 天"
  toast.t = 4.0
end

-- ---------------------------------------------------------------------------
-- 夜袭：尸群逼近 / 偷物资 / 射击 / 炮塔
-- ---------------------------------------------------------------------------
function despawnHorde()
  for _, z in ipairs(horde) do Despawn(z.ent) end
  horde = {}
end

local function spawnZombie()
  local ent = SpawnPrefab("rv_zombie", { x = 0, y = -50, z = 0 })
  if ent == nil then return end
  local a = math.random() * math.pi * 2
  local r = 14 + math.random() * 7
  local wx, wz = rotOf(math.sin(a) * r, math.cos(a) * r)
  local nd = nodeById(atNode or curNode)
  local danger = nd and nd.danger or 0
  SetPosition(ent, { x = wx, y = 0.8, z = wz })
  SetScale(ent, 0.5, 1.6, 0.4)
  horde[#horde + 1] = { ent = ent, x = wx, z = wz,
                        hp = RAID.hpBase + math.floor(danger / 2),
                        speed = RAID.speed * (0.85 + math.random() * 0.4)
                                + danger * 0.12,
                        phase = math.random() * 6.28, stealT = 0 }
end

local function killZombie(idx)
  local z = horde[idx]
  EmitParticles({ pos = { x = z.x, y = 1.0, z = z.z }, count = 14,
                  speedMin = 1.0, speedMax = 3.5, lifeMin = 0.3, lifeMax = 0.7,
                  sizeStart = 0.22, sizeEnd = 0.05,
                  color = { r = 0.35, g = 0.12, b = 0.1, a = 0.9 },
                  colorEnd = { r = 0.35, g = 0.12, b = 0.1, a = 0.0 },
                  additive = false })
  Despawn(z.ent)
  table.remove(horde, idx)
  killCount = killCount + 1
  if math.random() < 0.2 then
    local mats = { "metal", "electronics", "cloth", "wood" }
    local m = mats[math.random(1, 4)]
    stopBag[m] = (stopBag[m] or 0) + 1
    SpawnFloatText(z.x, 1.4, z.z, MAT_NAMES[m] .. "+1",
                   false, 1.2, 1.0, 0.85, 0.4)
  end
end

local function updateHorde(dt)
  raidT = raidT + dt
  if evacT >= 0 then
    hordeT = hordeT - dt
    if hordeT <= 0 and #horde < RAID.aliveMax then
      hordeT = RAID.spawnEvery
      spawnZombie()
    end
  end
  -- 主角下车时尸群优先扑向主角（更危险），否则奔着物资/房车去
  local tx, tz = rv.x, rv.z
  if onFoot and player.active then tx, tz = player.x, player.z end
  for i = #horde, 1, -1 do
    local z = horde[i]
    local dx, dz = tx - z.x, tz - z.z
    local dist = math.sqrt(dx * dx + dz * dz)
    if dist > 0.01 then
      z.x = z.x + dx / dist * z.speed * dt
      z.z = z.z + dz / dist * z.speed * dt
    end
    local sway = math.sin(raidT * 5.0 + z.phase)
    SetPosition(z.ent, { x = z.x, y = 0.8 + math.abs(sway) * 0.06, z = z.z })
    SetRotationY(z.ent, dirYaw(dx, dz) + sway * 0.15)
    if dist < RAID.attackDist then
      z.stealT = z.stealT - dt
      if z.stealT <= 0 then
        z.stealT = RAID.stealEvery
        local mats, n = {}, 0
        for m, vv in pairs(stopBag) do
          if vv > 0 then n = n + 1 mats[n] = m end
        end
        if n > 0 then
          local m = mats[math.random(1, n)]
          stopBag[m] = stopBag[m] - 1
          if stopBag[m] <= 0 then stopBag[m] = nil end
          SpawnFloatText(z.x, 1.5, z.z, "-" .. MAT_NAMES[m],
                         false, 1.2, 1, 0.3, 0.25)
        end
      end
    else
      z.stealT = 0
    end
  end
end

-- 射击：屏幕空间命中最近丧尸；枪口 = 主角（下车）/ 炮塔（有则）/ 车顶
local function shootAt(mx, my)
  local best, bestD = nil, RAID.hitRadius * RAID.hitRadius
  for i, z in ipairs(horde) do
    local sp = WorldToScreen(z.x, 1.0, z.z)
    if sp then
      local ddx, ddz = sp.x - mx, sp.y - my
      local d2 = ddx * ddx + ddz * ddz
      if d2 < bestD then best, bestD = i, d2 end
    end
  end
  local mzx, mzy, mzz = playerMuzzle()
  EmitParticles({ pos = { x = mzx, y = mzy, z = mzz }, count = 4,
                  speedMin = 2, speedMax = 5, lifeMin = 0.05, lifeMax = 0.12,
                  sizeStart = 0.16, sizeEnd = 0.03,
                  color = { r = 1, g = 0.8, b = 0.3, a = 0.95 },
                  colorEnd = { r = 1, g = 0.6, b = 0.1, a = 0.0 },
                  additive = true })
  if best then
    local z = horde[best]
    z.hp = z.hp - (hasModule("gun_rack") and 2 or 1)
    EmitParticles({ pos = { x = z.x, y = 1.0 + math.random() * 0.4, z = z.z },
                    count = 6, speedMin = 0.5, speedMax = 2,
                    lifeMin = 0.2, lifeMax = 0.45,
                    sizeStart = 0.14, sizeEnd = 0.03,
                    color = { r = 0.4, g = 0.1, b = 0.08, a = 0.9 },
                    colorEnd = { r = 0.4, g = 0.1, b = 0.08, a = 0.0 },
                    additive = false })
    if z.hp <= 0 then killZombie(best) end
  else
    local g = PickGround({ x = mx, y = my })
    if g then
      EmitParticles({ pos = { x = g.x, y = 0.08, z = g.z }, count = 8,
                      speedMin = 0.5, speedMax = 2, lifeMin = 0.25, lifeMax = 0.5,
                      sizeStart = 0.15, sizeEnd = 0.04,
                      color = { r = 0.62, g = 0.57, b = 0.47, a = 0.7 },
                      colorEnd = { r = 0.62, g = 0.57, b = 0.47, a = 0.0 },
                      additive = false })
    end
  end
end

local function turretFire(dt)
  if turretT > 0 then
    turretT = turretT - dt
    return
  end
  local turret = hasModule("turret")
  if not turret then return end
  local best, bestD = nil, RAID.turretRange * RAID.turretRange
  for i, z in ipairs(horde) do
    local ddx, ddz = z.x - rv.x, z.z - rv.z
    local d2 = ddx * ddx + ddz * ddz
    if d2 < bestD then best, bestD = i, d2 end
  end
  if not best then return end
  turretT = RAID.turretCd
  local z = horde[best]
  local tx, tz = rotOf(turret.lx, turret.lz)
  EmitParticles({ pos = { x = tx, y = (turret.ly or 2.6) + 0.5, z = tz }, count = 4,
                  speedMin = 2, speedMax = 5, lifeMin = 0.05, lifeMax = 0.1,
                  sizeStart = 0.14, sizeEnd = 0.03,
                  color = { r = 1, g = 0.9, b = 0.4, a = 0.95 },
                  colorEnd = { r = 1, g = 0.7, b = 0.1, a = 0.0 },
                  additive = true })
  EmitParticles({ pos = { x = z.x, y = 1.0, z = z.z }, count = 8,
                  speedMin = 0.8, speedMax = 2.5, lifeMin = 0.2, lifeMax = 0.4,
                  sizeStart = 0.16, sizeEnd = 0.04,
                  color = { r = 1, g = 0.75, b = 0.25, a = 0.9 },
                  colorEnd = { r = 1, g = 0.6, b = 0.15, a = 0.0 },
                  additive = true })
  z.hp = z.hp - 1
  if z.hp <= 0 then killZombie(best) end
end

-- ---------------------------------------------------------------------------
-- 途中随机事件：尸群横穿 / 路边补给 / 燃油桶
-- ---------------------------------------------------------------------------
local function pickEvent()
  local total = 0
  for _, e in ipairs(EVENTS) do total = total + e.w end
  local r = math.random() * total
  for _, e in ipairs(EVENTS) do
    r = r - e.w
    if r <= 0 then return e.id end
  end
  return "supply"
end

local function despawnDriveWorld()
  for _, p in ipairs(driveProps) do Despawn(p.ent) end
  driveProps = {}
  for _, z in ipairs(driveHorde) do Despawn(z.ent) end
  driveHorde = {}
end

local function triggerEvent()
  local id = pickEvent()
  if id == "horde" then
    local n = 4 + math.random(0, 2)
    local base = 25 + math.random() * 10
    local lat0 = (math.random() * 2 - 1) * 8
    for _ = 1, n do
      local ent = SpawnPrefab("rv_zombie", { x = 0, y = -50, z = 0 })
      if ent ~= nil then
        local wx, wz = rotOf(lat0 + (math.random() * 2 - 1) * 3,
                             base + (math.random() * 2 - 1) * 6)
        SetPosition(ent, { x = wx, y = 0.8, z = wz })
        SetScale(ent, 0.5, 1.6, 0.4)
        driveHorde[#driveHorde + 1] = { ent = ent, x = wx, z = wz,
            speed = 1.6 + math.random() * 0.6,
            phase = math.random() * 6.28, life = 22 }
      end
    end
    toast.text = "尸群横穿！绕开它们——撞上掉车体耐久"
  elseif id == "supply" then
    local ent = SpawnPrefab("rv_loot_crate", { x = 0, y = -50, z = 0 })
    if ent ~= nil then
      local wx, wz = rotOf((math.random() * 2 - 1) * 6, 22 + math.random() * 10)
      SetPosition(ent, { x = wx, y = 0.28, z = wz })
      SetScale(ent, 0.55, 0.55, 0.55)
      driveProps[#driveProps + 1] = { ent = ent, x = wx, z = wz, kind = "supply" }
    end
    toast.text = "发现路边补给！开过去拾取"
  else
    local ent = SpawnPrefab("rv_fuelcan", { x = 0, y = -50, z = 0 })
    if ent ~= nil then
      local wx, wz = rotOf((math.random() * 2 - 1) * 6, 22 + math.random() * 10)
      SetPosition(ent, { x = wx, y = 0.35, z = wz })
      SetScale(ent, 0.5, 0.7, 0.5)
      driveProps[#driveProps + 1] = { ent = ent, x = wx, z = wz, kind = "fuelcan" }
    end
    toast.text = "路边有燃油桶！开过去拾取"
  end
  toast.t = 3.5
end

local function collectDriveProp(p)
  if p.kind == "fuelcan" then
    local v = 10 + math.random(0, 8)
    fuel = math.min(VEH.tank, fuel + v)
    fuelWarned = fuel > VEH.tank * 0.5 and false or fuelWarned
    SpawnFloatText(p.x, 1.0, p.z, "燃料 +" .. v .. "L",
                   false, 1.2, 1.0, 0.85, 0.4)
  else
    local mats = { "metal", "electronics", "cloth", "wood" }
    local got = {}
    for _ = 1, 2 + math.random(0, 2) do
      local m = mats[math.random(1, 4)]
      local n = 1 + math.random(0, 2)
      materials[m] = (materials[m] or 0) + n
      got[#got + 1] = MAT_NAMES[m] .. "+" .. n
    end
    SpawnFloatText(p.x, 1.0, p.z, table.concat(got, " "),
                   false, 1.2, 1.0, 0.85, 0.4)
  end
  Despawn(p.ent)
end

local function updateDriveWorld(dt)
  raidT = raidT + dt
  nextEventT = nextEventT - dt
  if nextEventT <= 0 then
    nextEventT = VEH.eventMin + math.random() * (VEH.eventMax - VEH.eventMin)
    if math.abs(rv.speed) > 3 then triggerEvent() end
  end
  local fx, fz = math.sin(rv.yaw), math.cos(rv.yaw)
  for i = #driveProps, 1, -1 do
    local p = driveProps[i]
    local dx, dz = p.x - rv.x, p.z - rv.z
    local d2 = dx * dx + dz * dz
    local fwd = dx * fx + dz * fz
    if d2 < 9 then
      collectDriveProp(p)
      table.remove(driveProps, i)
    elseif fwd < -30 then
      Despawn(p.ent)
      table.remove(driveProps, i)
    end
  end
  for i = #driveHorde, 1, -1 do
    local z = driveHorde[i]
    z.life = z.life - dt
    local dx, dz = rv.x - z.x, rv.z - z.z
    local dist = math.sqrt(dx * dx + dz * dz)
    if dist > 0.01 then
      z.x = z.x + dx / dist * z.speed * dt
      z.z = z.z + dz / dist * z.speed * dt
    end
    SetPosition(z.ent, { x = z.x, y = 0.8 + math.abs(math.sin(raidT * 5 + z.phase)) * 0.06, z = z.z })
    SetRotationY(z.ent, dirYaw(dx, dz))
    if dist < 2.0 and math.abs(rv.speed) > 1 then
      dur = math.max(0, dur - 3)
      EmitParticles({ pos = { x = z.x, y = 1.0, z = z.z }, count = 12,
                      speedMin = 1, speedMax = 3.5, lifeMin = 0.3, lifeMax = 0.6,
                      sizeStart = 0.2, sizeEnd = 0.05,
                      color = { r = 0.35, g = 0.12, b = 0.1, a = 0.9 },
                      colorEnd = { r = 0.35, g = 0.12, b = 0.1, a = 0.0 },
                      additive = false })
      SpawnFloatText(z.x, 1.3, z.z, "车体 -3", false, 1.2, 1, 0.35, 0.3)
      Despawn(z.ent)
      table.remove(driveHorde, i)
    elseif z.life <= 0 or dist > 70 then
      Despawn(z.ent)
      table.remove(driveHorde, i)
    end
  end
end

-- ---------------------------------------------------------------------------
-- 行驶物理：Jolt 速度驱动 + 朝向覆盖；燃油/撞击/事件
-- ---------------------------------------------------------------------------
local function updateDrive(dt)
  local throttle = (ActionDown("w") and 1 or 0) - (ActionDown("s") and 1 or 0)
  local steer = (ActionDown("a") and 1 or 0) - (ActionDown("d") and 1 or 0)
  if autoPilot then
    driveT = driveT + dt
    if nearNode and nearNode.id ~= "camp" and driveT > 3.0 then
      setPhase("parked")  -- 靠站（autoPilot 保持 true：搜刮接管）
      return
    end
    if driveT > 45.0 then
      autoPilot = false
      setPhase("parked")
      return
    end
    throttle = 1
    if autoTarget == nil then
      local bestD = 1e18
      for _, nd in ipairs(NODES) do
        if nd.id ~= "camp" then
          local ddx, ddz = nd.x - rv.x, nd.z - rv.z
          local d2 = ddx * ddx + ddz * ddz
          if d2 < bestD then bestD = d2 autoTarget = nd end
        end
      end
    end
    if autoTarget then
      local dx, dz = autoTarget.x - rv.x, autoTarget.z - rv.z
      local desired = dirYaw(dx, dz)
      local err = desired - rv.yaw
      while err > math.pi do err = err - 2 * math.pi end
      while err < -math.pi do err = err + 2 * math.pi end
      steer = math.max(-1, math.min(1, err * 1.5))
    end
  end

  -- 读回上一物理步；意图速度被物理骤降 = 撞击 -> 掉耐久（冷却防研磨连扣）
  local bodyVy = 0
  if rvBody then
    local p = PhysicsGetPosition(rvBody)
    if p then rv.x, rv.z = p.x, p.z end
    local v = PhysicsGetVelocity(rvBody)
    if v then
      bodyVy = v.y
      local fxr, fzr = math.sin(rv.yaw), math.cos(rv.yaw)
      local proj = v.x * fxr + v.z * fzr
      if math.abs(proj) < math.abs(rv.speed) then
        local impact = math.abs(rv.speed) - math.abs(proj)
        if impact > 1.5 and impactCd <= 0 then
          impactCd = 0.5
          dur = math.max(0, dur - math.min(8, impact * VEH.impactK))
        end
        rv.speed = proj
      end
    end
  end
  impactCd = impactCd - dt

  -- 超载/车况：极速与加速打折
  local ratio = stats.weight / WEIGHT_MAX
  local maxSp, acc = DRIVE.maxSpeed, DRIVE.accel
  if ratio > 1.0 then
    local over = math.min(1.0, ratio - 1.0)
    maxSp = maxSp * (1.0 - over * 0.45)
    acc = acc * (1.0 - over * 0.5)
  end
  -- 燃油：踩油门燃烧（超载更费油）；没油 = 龟速挪窝
  if throttle > 0 and fuel > 0 then
    fuel = math.max(0, fuel - VEH.burn * (ratio > 1.0 and 1.5 or 1) * dt)
  end
  if fuel <= 0 then
    maxSp = math.min(maxSp, VEH.limpSpeed)
    if rv.speed > VEH.limpSpeed then rv.speed = VEH.limpSpeed end
  end
  maxSp = maxSp * (0.5 + 0.5 * dur / VEH.durMax)
  if fuel <= VEH.tank * 0.2 and not fuelWarned then
    fuelWarned = true
    toast.text = "油量低！加油站加满 / 路边捡燃油桶"
    toast.t = 3.5
  elseif fuel > VEH.tank * 0.5 then
    fuelWarned = false
  end

  if throttle > 0 then
    rv.speed = math.min(maxSp, rv.speed + acc * dt)
  elseif throttle < 0 then
    if rv.speed > 0.05 then
      rv.speed = math.max(0.0, rv.speed - DRIVE.brake * dt)
    else
      rv.speed = math.max(-DRIVE.reverseMax, rv.speed - acc * 0.6 * dt)
    end
  else
    local mag = math.abs(rv.speed) - DRIVE.drag * dt
    if mag < 0 then mag = 0 end
    rv.speed = (rv.speed >= 0) and mag or -mag
  end

  local sf = math.min(1.0, math.abs(rv.speed) / 3.0)
  if sf > 0.01 and steer ~= 0 then
    local dir = (rv.speed < -0.05) and -1 or 1
    rv.yaw = rv.yaw + steer * DRIVE.steerRate * sf * dt * dir
  end

  local fx, fz = math.sin(rv.yaw), math.cos(rv.yaw)
  if rvBody then
    PhysicsSetVelocity(rvBody, { x = fx * rv.speed, y = bodyVy, z = fz * rv.speed })
    local half = rv.yaw * 0.5
    PhysicsSetRotation(rvBody, { x = 0, y = math.sin(half), z = 0, w = math.cos(half) })
  else
    rv.x = rv.x + fx * rv.speed * dt
    rv.z = rv.z + fz * rv.speed * dt
  end

  applyRvTransform()
  if groundEnt ~= nil then
    SetPosition(groundEnt, { x = rv.x, y = -0.05, z = rv.z })
  end
  recycleScenery(fx, fz)
  updateDriveWorld(dt)

  if math.abs(rv.speed) > 2.5 then
    dustAcc = dustAcc + dt * (6.0 + math.abs(rv.speed) * 1.6)
    while dustAcc >= 1.0 do
      dustAcc = dustAcc - 1.0
      local s = (math.random() < 0.5) and -1.3 or 1.3
      local wx, wz = rotOf(s, -2.9)
      EmitParticles({
        pos = { x = wx, y = 0.12, z = wz }, count = 2,
        vel = { x = -fx * 1.2, y = 0.9, z = -fz * 1.2 },
        speedMin = 0.3, speedMax = 1.2, lifeMin = 0.5, lifeMax = 1.1,
        sizeStart = 0.35, sizeEnd = 1.5,
        color = { r = 0.58, g = 0.53, b = 0.44, a = 0.45 },
        colorEnd = { r = 0.58, g = 0.53, b = 0.44, a = 0.0 },
        additive = false, gravity = 0,
      })
    end
  else
    dustAcc = 0
  end

  -- 临近节点（12m 内可停靠）
  nearNode = nil
  local best = 144
  for _, nd in ipairs(NODES) do
    local ddx, ddz = rv.x - nd.x, rv.z - nd.z
    local d2 = ddx * ddx + ddz * ddz
    if d2 < best then best = d2 nearNode = nd end
  end
end

-- ---------------------------------------------------------------------------
-- 相机：驻车=车库机位；行驶=追逐或驾驶席(C)；步行=主角身后
-- ---------------------------------------------------------------------------
local function updateCamera(dt)
  if camEnt == nil then return end
  local tx, ty, tz, lookx, looky, lookz
  if onFoot and player.active then
    local fx, fz = math.sin(player.yaw), math.cos(player.yaw)
    tx = player.x - fx * 3.6
    ty = 2.3
    tz = player.z - fz * 3.6
    lookx, looky, lookz = player.x, 1.3, player.z
  elseif phase == "driving" and driveView == "cab" then
    local fx, fz = math.sin(rv.yaw), math.cos(rv.yaw)
    local cx2, cz2 = rotOf(0.45, 1.4)
    tx, ty, tz = cx2, 1.55, cz2
    lookx, looky, lookz = rv.x + fx * 12, 1.1, rv.z + fz * 12
  elseif phase == "driving" then
    local fx, fz = math.sin(rv.yaw), math.cos(rv.yaw)
    local cp, sp = math.cos(DRIVE.camPitch), math.sin(DRIVE.camPitch)
    tx = rv.x - fx * DRIVE.camDist * cp
    ty = 1.4 + sp * DRIVE.camDist
    tz = rv.z - fz * DRIVE.camDist * cp
    lookx, looky, lookz = rv.x + fx * 2.5, 0.9, rv.z + fz * 2.5
  else
    tx, ty, tz = rv.x, 11.5, rv.z - 9.2
    lookx, looky, lookz = rv.x, 0.0, rv.z
  end
  if camSmooth == nil then camSmooth = { x = tx, y = ty, z = tz } end
  local k = 1 - math.exp(-5.0 * dt)
  camSmooth.x = camSmooth.x + (tx - camSmooth.x) * k
  camSmooth.y = camSmooth.y + (ty - camSmooth.y) * k
  camSmooth.z = camSmooth.z + (tz - camSmooth.z) * k
  local sx, sy, sz = camSmooth.x, camSmooth.y, camSmooth.z
  if phase == "driving" and driveView == "chase" then
    local amp = math.min(0.05, math.abs(rv.speed) * 0.005)
    sx = sx + (math.random() - 0.5) * amp
    sy = sy + (math.random() - 0.5) * amp
  end
  SetPosition(camEnt, { x = sx, y = sy, z = sz })
  local dx, dy, dz = lookx - sx, looky - sy, lookz - sz
  local len = math.sqrt(dx * dx + dy * dy + dz * dz)
  if len > 1e-4 then SetLook(camEnt, dx / len, dy / len, dz / len) end
end

-- ---------------------------------------------------------------------------
-- 阶段切换：驻车 ⇄ 行驶；停靠节点 = 搜刮层叠加在驻车上
-- ---------------------------------------------------------------------------
function setPhase(m)
  if phase == m then return end
  local prev = phase
  phase = m
  if m == "parked" then
    rv.speed = 0
    if rvBody then PhysicsSetVelocity(rvBody, { x = 0, y = 0, z = 0 }) end
    if prev == "driving" then
      if nearNode then
        atNode = nearNode.id
        curNode = atNode
        stopBag, stopThreat, evacT = {}, 0, -1
        killCount, hordeT, fireT, turretT, raidT = 0, 0, 0, 0, 0
        spawnCrates()
        local nd = nodeById(atNode)
        local stars = string.rep("★", nd and nd.danger or 0)
        toast.text = "停靠 " .. nodeName(atNode)
                     .. (stars ~= "" and ("（危险 " .. stars .. "）") or "")
                     .. " — 左键搜箱  建造随时可用"
        if atNode == "gasstop" and fuel < VEH.tank - 0.5 then
          fuel = VEH.tank
          fuelWarned = false
          toast.text = toast.text .. " · ⛽ 已加满油"
        end
      else
        atNode = nil
        toast.text = "荒野停车 — 可改装"
      end
    end
    toast.t = 4.5
  else  -- driving
    if atNode then
      -- 开走 = 收工：物资入库 + 天数 +1
      despawnCrates()
      despawnHorde()
      day = day + 1
      local got = bankStopBag(1.0)
      stopThreat = 0
      evacT = -1
      atNode = nil
      toast.text = (got == "" and "启程" or "物资入库: " .. got)
                   .. " — 第 " .. day .. " 天"
    else
      toast.text = "上路！W/S 油门·刹车  A/D 转向  空格 停车  C 切换视角"
    end
    toast.t = 4.5
  end
  if ghostEnt ~= nil then SetVisible(ghostEnt, false) end
  if hoverDecalEnt ~= nil then SetVisible(hoverDecalEnt, false) end
  hover = nil
  if gridFloorEnt ~= nil then
    SetVisible(gridFloorEnt, (m == "parked") and gridVisible or false)
  end
  if gridRoofEnt ~= nil then
    SetVisible(gridRoofEnt, (m == "parked") and gridVisible and curLayer == "roof" or false)
  end
end

-- ---------------------------------------------------------------------------
-- 存读档 v5：布局 + 房车位姿 + 车辆 + 天数
-- ---------------------------------------------------------------------------
local function saveLayout()
  local rows = {}
  for _, inst in pairs(instances) do
    rows[#rows + 1] = { id = inst.cat.id, cx = inst.cx, cz = inst.cz,
                        rot = inst.rot, layer = inst.layer }
  end
  local data = { cell = CELL, gw = GW, gh = GH, materials = materials,
                 rv = { x = rv.x, z = rv.z, yaw = rv.yaw, node = atNode or curNode },
                 vehicle = { fuel = fuel, dur = dur },
                 day = day, modules = rows }
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
  if type(data.materials) == "table" then
    for k, v in pairs(data.materials) do
      materials[k] = math.max(materials[k] or 0, v or 0)
    end
  end
  clearAll()
  local byId = {}
  for _, cat in ipairs(CATALOG) do byId[cat.id] = cat end
  for _, row in ipairs(data.modules) do
    local cat = byId[row.id]
    if cat then
      local saved = curLayer
      curLayer = row.layer or cat.layer or "floor"
      place(cat, math.floor(row.cx), math.floor(row.cz), math.floor(row.rot or 0), true)
      curLayer = saved
    end
  end
  computeReachability()
  if type(data.rv) == "table" then
    rv.x = tonumber(data.rv.x) or 0
    rv.z = tonumber(data.rv.z) or 0
    rv.yaw = tonumber(data.rv.yaw) or 0
    curNode = tostring(data.rv.node or "camp")
    atNode = nil  -- 读档回到纯建造状态
    applyRvTransform()
    if rvBody then
      PhysicsSetPosition(rvBody, { x = rv.x, y = 0.9, z = rv.z })
      PhysicsSetVelocity(rvBody, { x = 0, y = 0, z = 0 })
    end
  end
  if type(data.vehicle) == "table" then
    fuel = math.min(VEH.tank, tonumber(data.vehicle.fuel) or VEH.tank)
    dur = math.min(VEH.durMax, tonumber(data.vehicle.dur) or VEH.durMax)
  end
  day = tonumber(data.day) or 1
  toast.text = "布局已读取"
  toast.t = 2.5
end

-- ---------------------------------------------------------------------------
-- 幽灵 / 网格 / 悬停高亮
-- ---------------------------------------------------------------------------
local function setGhostColor(valid)
  if ghostEnt ~= nil then Despawn(ghostEnt) end
  ghostEnt = SpawnPrefab(valid and "rv_ghost_ok" or "rv_ghost_bad",
                         { x = 0, y = 0, z = 0 })
end

local function ensureHelpers()
  if ghostEnt == nil then
    setGhostColor(true)
    SetVisible(ghostEnt, false)
  end
  if gridFloorEnt == nil then
    gridFloorEnt = SpawnDecal("assets/sprites/grid_4x14.png", 0, FLOOR_Y, 0,
                              1.0, 0.55, 1, 1, 1, true, 0.1)
    SetScale(gridFloorEnt, 2.0, 1.0, 7.0)
  end
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
-- HUD
-- ---------------------------------------------------------------------------
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
    onFoot and "W/S 行走   A/D 转向" or "左键 放置   右键/X 拆除",
    onFoot and "F 上车" or "F 下车   T 上路   C 视角(行驶)",
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

  if toast.t > 0 then
    DrawText(toast.text, math.floor(vw * 0.5), 120, 16, 0.98, 0.93, 0.75, 1, true)
  end
end

local function drawDriveHud()
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
local function drawScavengeOverlay()
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

local function drawMapHud()
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

-- ---------------------------------------------------------------------------
-- 生命周期
-- ---------------------------------------------------------------------------
function on_start()
  camEnt = FindNamedEntity("Main Camera")
  groundEnt = FindNamedEntity("Ground")
  collectRvParts()
  -- 合并 worldmap.json：名字/危险度/战利品表/链接
  do
    local text = ReadText("assets/data/worldmap.json")
    if text and text ~= "" then
      local data = jsonDecode(text)
      if type(data) == "table" and type(data.nodes) == "table" then
        for _, nd in ipairs(data.nodes) do
          local mine = nodeById(nd.id)
          if mine then
            mine.name = nd.name or mine.name
            mine.danger = nd.danger or 0
            mine.loot = nd.loot or {}
            mine.links = nd.links or {}
          end
        end
      end
    end
  end
  if groundEnt ~= nil then SetScale(groundEnt, 800, 0.1, 800) end
  spawnScenery()
  spawnMarkers()
  do  -- 验证钩子：scriptBaseDir 下放 autopilot_on.txt 即自动驾驶全循环
    local flag = ReadText("autopilot_on.txt")
    autoPilot = (flag ~= nil and flag ~= "")
  end
  ensureHelpers()
  -- 初始示例布局
  place(CATALOG[1], 1, 0, 0, true)
  place(CATALOG[3], 0, 4, 0, true)
  place(CATALOG[5], 0, 6, 0, true)
  place(CATALOG[9], 3, 6, 0, true)
  local saved = curLayer
  curLayer = "roof"
  place(CATALOG[17], 0, 8, 0, true)
  place(CATALOG[19], 2, 9, 0, true)
  curLayer = saved
  computeReachability()
  recomputeStats()
  local text = ReadText("saves/layout.json")
  if text and text ~= "" then loadLayout() end
  applyRvTransform()
  -- RV 动态刚体：质量随载重
  rvBody = PhysicsAddBox({ x = rv.x, y = 0.9, z = rv.z },
                         { x = 1.2, y = 0.9, z = 4.3 }, true,
                         { mass = math.max(800, 1200 + stats.weight * 0.8),
                           friction = 0.4, restitution = 0.05 })
  if autoPilot then setPhase("driving") end  -- 验证钩子：直接上路
end

function on_update(ent, dt)
  if toast.t > 0 then toast.t = toast.t - dt end
  ensureHelpers()
  if ActionPressed("m") then mapOpen = not mapOpen end

  -- ======================= 行驶 =======================
  if phase == "driving" then
    if ActionPressed("c") then
      driveView = (driveView == "chase") and "cab" or "chase"
    end
    if ActionPressed("space") then
      setPhase("parked")
    end
    updateDrive(dt)
    updateCamera(dt)
    return
  end

  -- ======================= 驻车（建造 + 停靠层 + 主角） =======================
  if ActionPressed("t") and not onFoot then setPhase("driving") end
  if ActionPressed("f") then
    if onFoot then
      local dx, dz = player.x - rv.x, player.z - rv.z
      if dx * dx + dz * dz < 12 then
        despawnPlayer()
        toast.text = "上车 — 可改装"
        toast.t = 2.5
      else
        toast.text = "走回到房车旁才能上车（F）"
        toast.t = 2.0
      end
    else
      spawnPlayer()
      if onFoot then
        toast.text = "下车 — W/S 行走  A/D 转向  F 上车"
        toast.t = 3.5
      end
    end
  end
  if ActionPressed("h") and dur < VEH.durMax and not onFoot then
    if canAfford(VEH.repairCost) then
      payCost(VEH.repairCost)
      dur = math.min(VEH.durMax, dur + VEH.repairAmt)
      recomputeStats()
      toast.text = string.format("修理车体 +%.0f（车况 %.0f%%）", VEH.repairAmt, dur)
    else
      toast.text = "材料不足: 金属x2 木材x1"
    end
    toast.t = 2.2
  end
  -- 停靠层（驻车在节点旁时）：撤离 / 夜袭 / 搜箱
  if atNode then
    if ActionPressed("e") and evacT < 0 then
      endScavenge(1.0)
    end
    if evacT >= 0 then
      evacT = evacT - dt
      if evacT <= 0 then
        toast.text = "尸群冲垮了停靠点！丢下部分物资…"
        toast.t = 3.0
        endScavenge(STOP.keepRatio)
      end
    end
    -- 夜袭期：尸群逼近 + 炮塔 + 左键射击（准星附近优先，不与建造抢点击）
    if evacT >= 0 then
      updateHorde(dt)
      turretFire(dt)
      fireT = fireT - dt
      if InputMouseDown(0) and fireT <= 0 then
        local mp = InputMousePos()
        local nearest = false
        for _, z in ipairs(horde) do
          local sp = WorldToScreen(z.x, 1.0, z.z)
          if sp then
            local ddx, ddz = sp.x - mp.x, sp.y - mp.y
            if ddx * ddx + ddz * ddz < RAID.hitRadius * RAID.hitRadius then
              nearest = true
              break
            end
          end
        end
        if nearest then
          fireT = RAID.fireCd
          shootAt(mp.x, mp.y)
        end
      end
    end
    -- 自动化钩子：搜箱 / 夜袭射击
    if autoPilot then
      if evacT < 0 then
        autoT = autoT + dt
        if autoT > 1.2 then
          autoT = 0
          if #crates > 0 then
            searchCrate(1)
          else
            autoPilot = false
            endScavenge(1.0)
          end
        end
      else
        autoT = autoT + dt
        if autoT > 0.35 then
          autoT = 0
          if #horde > 0 then
            local sp = WorldToScreen(horde[1].x, 1.0, horde[1].z)
            if sp then shootAt(sp.x, sp.y) end
          end
        end
      end
    end
  end
  updatePlayer(dt)
  updateCamera(dt)

  -- ---------- 建造交互（驻车时永远可用；下车也能造） ----------
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

  local mp = InputMousePos()
  local hitSlot = hotbarHit(mp.x, mp.y)
  local overUi = hitSlot ~= nil
  local clickUsed = false

  -- 停靠层点击优先级：搜箱 -> 夜袭射击 -> 否则落入建造
  if atNode and InputMousePressed(0) then
    local best, bestD = nil, STOP.pickRadius * STOP.pickRadius
    for i, c in ipairs(crates) do
      local sp = WorldToScreen(c.x, 0.5, c.z)
      if sp then
        local ddx, ddz = sp.x - mp.x, sp.y - mp.y
        local d2 = ddx * ddx + ddz * ddz
        if d2 < bestD then best, bestD = i, d2 end
      end
    end
    if best then
      searchCrate(best)
      clickUsed = true
    elseif evacT >= 0 then
      for _, z in ipairs(horde) do
        local sp = WorldToScreen(z.x, 1.0, z.z)
        if sp then
          local ddx, ddz = sp.x - mp.x, sp.y - mp.y
          if ddx * ddx + ddz * ddz < RAID.hitRadius * RAID.hitRadius then
            if fireT <= 0 then
              fireT = RAID.fireCd
              shootAt(mp.x, mp.y)
            end
            clickUsed = true
            break
          end
        end
      end
    end
  end

  if not clickUsed then
    local pick = PickGround(mp)
    hover = nil
    if pick then
      local cx, cz = worldToCell(pick.x, pick.z)
      if cx then
        local inst = findInstanceAt(cx, cz)
        local cat = CATALOG[selected]
        local layerOk = (cat.layer == "roof") == (curLayer == "roof")
        local valid = layerOk and fits(cat, cx, cz, rot) and not overUi
        hover = { cx = cx, cz = cz, valid = valid, inst = inst, uiSlot = hitSlot }
        local w, h = footprint(cat, rot)
        local baseY = (curLayer == "roof") and ROOF_Y or FLOOR_Y
        local ox = ORIGIN_X + cx * CELL + w * CELL * 0.5
        local oz = ORIGIN_Z + cz * CELL + h * CELL * 0.5
        local gwx, gwz = rotOf(ox, oz)
        SetVisible(ghostEnt, true)
        SetPosition(ghostEnt, { x = gwx, y = baseY + 0.035, z = gwz })
        SetRotationY(ghostEnt, rv.yaw)
        SetScale(ghostEnt, w * CELL - 0.04, 0.06, h * CELL - 0.04)
        if ghostValidLast ~= valid then
          setGhostColor(valid)
          ghostValidLast = valid
        end
        SetVisible(hoverDecalEnt, inst ~= nil)
        if inst then
          local iw, ih = footprint(inst.cat, inst.rot)
          local baseYI = (inst.layer == "roof") and ROOF_Y or FLOOR_Y
          local hlx = ORIGIN_X + inst.cx * CELL + iw * CELL * 0.5
          local hlz = ORIGIN_Z + inst.cz * CELL + ih * CELL * 0.5
          local hwx, hwz = rotOf(hlx, hlz)
          SetPosition(hoverDecalEnt, { x = hwx, y = baseYI, z = hwz })
          SetRotationY(hoverDecalEnt, rv.yaw)
          SetScale(hoverDecalEnt, iw * CELL, 1.0, ih * CELL)
          SetDecal(hoverDecalEnt, 1.0, 0.45, inst.cat.mh + 0.25)
        end
      end
    end
    if hover == nil then
      SetVisible(ghostEnt, false)
      SetVisible(hoverDecalEnt, false)
    end

    if InputMousePressed(0) then
      if hitSlot then
        selected = hitSlot
      elseif hover and hover.valid then
        if place(CATALOG[selected], hover.cx, hover.cz, rot) then
          recomputeStats()
        end
      elseif hover and not hover.valid and not overUi then
        local cat = CATALOG[selected]
        if fits(cat, hover.cx, hover.cz, rot) and not canAfford(cat.cost or {}) then
          toast.text = "材料不足: 需要 " .. costText(cat.cost or {})
          toast.t = 1.6
        end
      end
    end
    if InputMousePressed(1) and not overUi and hover then
      if removeAt(hover.cx, hover.cz) then recomputeStats() end
    end
    if ActionPressed("x") and hover then
      if removeAt(hover.cx, hover.cz) then recomputeStats() end
    end
  else
    hover = nil
    SetVisible(ghostEnt, false)
    SetVisible(hoverDecalEnt, false)
  end
end

-- 2D HUD 必须在 on_render 里画：draw2d 上下文只在渲染期接线
function on_render()
  if phase == "driving" then
    drawDriveHud()
  else
    drawHud()
    if atNode then drawScavengeOverlay() end
  end
  if mapOpen then drawMapHud() end
end
