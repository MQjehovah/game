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
  { id="bed_double", name="双人床",   layer="floor", w=3, h=4, mh=0.45, color="#8A5A44", weight=60,  comfort=30, modelScale=1.0, desc="睡眠质量 +30" , cost={wood=4,cloth=4}},
  { id="bed_single", name="单人床",   layer="floor", w=2, h=3, mh=0.40, color="#A0714F", weight=35,  comfort=15, modelScale=1.0, desc="睡眠质量 +15" , cost={wood=2,cloth=2}},
  { id="stove",      name="灶台",     layer="floor", w=2, h=2, mh=0.50, color="#7A7F87", weight=40,  comfort=5,  powerDraw=0.5, waterDraw=2, modelScale=1.0, desc="耗电 0.5kW 耗水 2L/h" , cost={metal=3,electronics=1}},
  { id="fridge",     name="冰箱",     layer="floor", w=1, h=2, mh=1.10, color="#9FB4C7", weight=30,              powerDraw=0.3, modelScale=1.0, desc="耗电 0.3kW" , cost={metal=2,electronics=3}},
  { id="water_tank", name="净水箱",   layer="floor", w=2, h=2, mh=0.90, color="#4F8FBF", weight=160,             waterStore=120, modelScale=1.0, desc="储水 120L" , cost={metal=4}},
  { id="battery",    name="电池组",   layer="floor", w=2, h=1, mh=0.50, color="#D8B23A", weight=80,  battery=5,  modelScale=1.0, desc="储能 5kWh" , cost={metal=2,electronics=4}},
  { id="workbench",  name="工作台",   layer="floor", w=2, h=3, mh=0.55, color="#8C6239", weight=70,  modelScale=1.2, desc="改装检修（P1 后开放）" , cost={metal=2,wood=3}},
  { id="storage",    name="储物箱",   layer="floor", w=1, h=1, mh=0.60, color="#6E7B52", weight=15,  storage=50,  modelScale=1.0, desc="储物 50L" , cost={wood=2}},
  { id="heater",     name="电暖器",   layer="floor", w=1, h=1, mh=0.55, color="#C96B3F", weight=12,  comfort=10, powerDraw=0.8, modelScale=1.0, desc="耗电 0.8kW 舒适 +10" , cost={metal=1,electronics=2}},
  { id="tv",         name="娱乐柜",   layer="floor", w=2, h=1, mh=0.90, color="#5D5366", weight=28,  comfort=8,  powerDraw=0.2, modelScale=1.0, desc="耗电 0.2kW 舒适 +8" , cost={electronics=3,cloth=1}},
  { id="sink",       name="水槽",     layer="floor", w=1, h=1, mh=0.35, color="#B8C4CE", weight=12,  waterDraw=1, comfort=2, modelScale=1.0, desc="耗水 1L/h 舒适 +2" , cost={metal=2}},
  { id="bathroom",   name="卫生间",   layer="floor", w=2, h=3, mh=1.00, color="#7F9BA8", weight=120, comfort=12, waterDraw=3, modelScale=1.0, desc="耗水 3L/h 舒适 +12" , cost={metal=3,wood=2}},
  { id="generator",  name="发电机",   layer="floor", w=2, h=2, mh=0.70, color="#C7B45A", weight=180, powerGen=2.0, comfort=-5, modelScale=1.0, desc="发电 2.0kW 噪音 舒适 -5" , cost={metal=6,electronics=3}},
  { id="med_cabinet",name="药柜",     layer="floor", w=1, h=1, mh=0.50, color="#D8E8E0", weight=20,  comfort=6,  modelScale=1.0, desc="医疗 舒适 +6" , cost={metal=1,cloth=2}},
  { id="gun_rack",   name="武器架",   layer="floor", w=2, h=1, mh=0.50, color="#6B4F3A", weight=40,  modelScale=1.0, desc="武器存放（P2 夜袭）" , cost={wood=3,metal=1}},
  { id="turret",     name="炮塔底座", layer="floor", w=2, h=2, mh=0.60, color="#5A5A5A", weight=90,  modelScale=1.0, desc="预留（P2 夜袭开放）" , cost={metal=8,electronics=4}},
  -- 车顶层（太阳能上车顶发电 +50%）
  { id="solar",      name="太阳能板", layer="roof",  w=2, h=3, mh=0.15, color="#3E6FB0", weight=25,  powerGen=1.8, modelScale=1.0, desc="发电 1.8kW（车顶限定）" , cost={metal=2,electronics=4}},
  { id="roof_vent",  name="通风扇",   layer="roof",  w=1, h=1, mh=0.25, color="#8891A0", weight=8,   comfort=4,  powerDraw=0.1, modelScale=1.0, desc="耗电 0.1kW 舒适 +4" , cost={metal=1,electronics=1}},
  { id="roof_rack",  name="车顶行李架", layer="roof", w=2, h=4, mh=0.35, color="#5C6670", weight=30, storage=150, modelScale=1.0, desc="车载储物 150L" , cost={metal=3}},
}
local HOTKEYS = { "1","2","3","4","5","6","7","8","9","0" }  -- 前 10 件绑定热键

-- 驾驶（P2.5）：房车真的能在荒漠里开。T 上路 / WASD 驾驶 / 空格停靠。
local DRIVE = {
  accel = 4.5, brake = 9.0, drag = 0.8, reverseMax = 3.0,
  maxSpeed = 9.0,   -- m/s ≈ 32 km/h；超载按超出比例打折
  steerRate = 1.4,  -- rad/s（3 m/s 以上打满，速度越低越难打方向）
  camDist = 10.5, camPitch = 0.40,
}
-- 停靠搜刮（P2）：停在节点后搜物资箱，威胁随搜刮上涨，满值触发撤离时限
local STOP = {
  crates = 5,        -- 每个停靠点刷新的物资箱数
  threatMax = 6,     -- 威胁满值 → 触发撤离时限
  evacTime = 15,     -- 撤离时限（秒）
  searchThreat = 1,  -- 每次搜刮基础威胁（+ 节点危险度）
  keepRatio = 0.6,   -- 超时被袭保留下来的物资比例（损失 40%）
  pickRadius = 30,   -- 点击搜箱的屏幕命中半径（px）
}
-- worldmap.json 节点的世界坐标（驾驶目的地；营地=原点）。地图 1.0 ≈ 160m。
local NODES = {
  { id = "camp",    name = "营地",       x = 0,   z = 0 },
  { id = "gasstop", name = "废弃加油站", x = 46,  z = -42 },
  { id = "ruin",    name = "郊区废宅",   x = 38,  z = 27 },
  { id = "mall",    name = "购物中心",   x = 100, z = -73 },
  { id = "town",    name = "小镇主街",   x = 131, z = -8 },
  { id = "depot",   name = "物流仓库",   x = 154, z = -58 },
}

-- 材料经济：搜刮获得（P2），建造消耗；拆除全额退还
local MAT_NAMES = { metal="金属片", electronics="电子件", cloth="布料", wood="木材" }
local materials = { metal=12, electronics=8, cloth=6, wood=10 }

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

-- 驾驶状态
local mode = "garage"            -- "garage" / "drive"
local rv = { x = 0, z = 0, yaw = 0, speed = 0 }
local curNode = "camp"
local rvParts, camEnt, groundEnt -- 车体部件 / 相机 / 地面（on_start 收集）
local props, markersSpawned = {}, false  -- 荒漠循环道具 / 节点标记
local camSmooth, nearNode        -- 相机平滑位置 / 临近节点
local dustAcc = 0
local autoPilot, driveT = false, 0  -- RV_AUTOPILOT=1 自动驾驶（视觉验收钩子）
local rvBody = nil               -- RV 动态刚体（Jolt；位姿/碰撞由物理解算）

-- 停靠搜刮状态
local stopBag = {}     -- 本次停靠已搜获（撤离/上车时入库）
local stopThreat = 0   -- 尸群威胁 0..threatMax
local evacT = -1       -- 撤离倒计时（<0 = 未触发）
local crates = {}      -- 场上物资箱 { ent=, x=, z= }
local day = 1          -- 旅途天数（每次启程 +1）
local mapOpen = false  -- 世界地图开关（M）
local autoT = 0        -- autopilot 停靠自动搜箱计时
local autoTarget = nil -- autopilot 寻的目的地（最近的非营地节点）

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

-- 房车局部点 -> 世界坐标（R(yaw) 旋转 + 平移；与 SetRotationY 同约定：
-- 局部 +z 轴转向世界 (sin yaw, cos yaw)，车头朝向即前进方向）
local function rotOf(lx, lz)
  local cy, sy = math.cos(rv.yaw), math.sin(rv.yaw)
  return rv.x + lx * cy + lz * sy, rv.z - lx * sy + lz * cy
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

local function canAfford(cat)
  if not cat.cost then return true end
  for k, v in pairs(cat.cost) do
    if (materials[k] or 0) < v then return false end
  end
  return true
end

local function payCost(cat)
  if not cat.cost then return end
  for k, v in pairs(cat.cost) do materials[k] = (materials[k] or 0) - v end
end

local function refundCost(cat)
  if not cat.cost then return end
  for k, v in pairs(cat.cost) do materials[k] = (materials[k] or 0) + v end
end

local function costText(cat)
  if not cat.cost then return "" end
  local parts = {}
  for k, v in pairs(cat.cost) do
    parts[#parts + 1] = MAT_NAMES[k] .. "x" .. v
  end
  return table.concat(parts, " ")
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
  local ly = cat.modelScale and (baseY + 0.01) or (baseY + cat.mh * 0.5 + 0.005)
  local wx, wz = rotOf(ox, oz)
  if cat.modelScale then
    -- 真模型（Kenney GLB）：统一缩放保持比例，不平展到占格盒
    SetPosition(ent, { x = wx, y = ly, z = wz })
    local sc = cat.modelScale
    SetScale(ent, sc, sc, sc)
  else
    SetPosition(ent, { x = wx, y = ly, z = wz })
    SetScale(ent, w * CELL - 0.03, cat.mh, h * CELL - 0.03)
  end
  -- 朝向 = 房车朝向 + 自身 90°（旋转占格）
  local lrot = (r % 2 == 1) and (math.pi * 0.5) or 0
  SetRotationY(ent, rv.yaw + lrot)
  -- 放置弹入动画：缩放从 0.65 倍 tween 到目标（prop 2 = scale）
  local sx, sy, sz
  if cat.modelScale then
    sx, sy, sz = cat.modelScale, cat.modelScale, cat.modelScale
  else
    sx, sy, sz = w * CELL - 0.03, cat.mh, h * CELL - 0.03
  end
  Tween(ent, 2, { x = sx * 0.6, y = sy * 0.6, z = sz * 0.6 },
              { x = sx, y = sy, z = sz }, 0.18, 1)
  return ent, ox, ly, oz, lrot  -- 局部位姿供 place 记账（驾驶模式整体变换用）
end

local function place(cat, cx, cz, r, free)
  if cat.layer == "roof" and curLayer ~= "roof" then return false end
  if cat.layer ~= "roof" and curLayer == "roof" then return false end
  if not fits(cat, cx, cz, r) then return false end
  if not free and not canAfford(cat) then return false end
  local ent, ox, ly, oz, lrot = spawnModule(cat, cx, cz, r)
  if ent == nil then return false end
  if not free then payCost(cat) end
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
              refundCost(cand.cat)
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
  refundCost(inst.cat)
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
-- 驾驶模式：车体/模块/网格随 rv 位姿整体变换 + 无限荒漠 + 追逐相机
-- ---------------------------------------------------------------------------
local setMode  -- 前置声明：updateDrive 的自动停车回路在定义前引用
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

-- 把车体部件 / 模块 / 网格贴花摆到当前车位姿（局部 -> 世界）
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

-- 荒漠道具池：出视野（车后 40m / 横向 55m 外）回收，车前 25..70m 重生，
-- 正前方 5m 与节点 10m 内留空。地面/道具不随车移动——位移感全靠道具倒退。
local PROP_TYPES = {
  { pre = "rv_scrub_a", n = 34 },
  { pre = "rv_scrub_b", n = 26 },
  { pre = "rv_rock",    n = 14 },
  { pre = "rv_tree",    n = 9 },
  { pre = "rv_wreck",   n = 5 },
}
local function propScale(kind)
  if kind == "rv_rock" then
    return 0.6 + math.random() * 1.4, 0.35 + math.random() * 0.85, 0.6 + math.random() * 1.4
  elseif kind == "rv_tree" then
    return 0.4 + math.random() * 0.5, 2.0 + math.random() * 1.6, 0.4 + math.random() * 0.5
  elseif kind == "rv_wreck" then
    return 1.6 + math.random() * 0.8, 0.6 + math.random() * 0.5, 2.8 + math.random() * 1.4
  end
  return 0.5 + math.random(), 0.03 + math.random() * 0.04, 0.5 + math.random()
end
local function placeProp(p, fx, fz, fMin, fMax)
  for _ = 1, 6 do
    local f = fMin + math.random() * (fMax - fMin)
    local lat = (math.random() * 2 - 1) * 42
    if math.abs(lat) >= 5 or fMin < 0 then
      local wx = rv.x + fx * f - fz * lat
      local wz = rv.z + fz * f + fx * lat
      local blocked = false
      do  -- 不压在房车身上（防止初始穿透被 Jolt 弹飞）
        local rdx, rdz = wx - rv.x, wz - rv.z
        if rdx * rdx + rdz * rdz < 81 then blocked = true end
      end
      for _, nd in ipairs(NODES) do
        local ddx, ddz = wx - nd.x, wz - nd.z
        if ddx * ddx + ddz * ddz < 100 then blocked = true break end
      end
      if not blocked then
        local sx, sy, sz = propScale(p.kind)
        local y = (p.kind == "rv_scrub_a" or p.kind == "rv_scrub_b") and 0.015 or sy * 0.5
        SetPosition(p.ent, { x = wx, y = y, z = wz })
        SetScale(p.ent, sx, sy, sz)
        SetRotationY(p.ent, math.random() * math.pi * 2)
        p.x, p.z = wx, wz
        -- 物理体：岩石/枯树/残骸是动态刚体（可被撞开）；灌木贴地无碰撞；
        -- 回收复用已有刚体（传送 + 清速度），避免增删抖动
        if p.body then
          PhysicsSetPosition(p.body, { x = wx, y = y, z = wz })
          PhysicsSetVelocity(p.body, { x = 0, y = 0, z = 0 })
        elseif p.kind == "rv_rock" or p.kind == "rv_tree" or p.kind == "rv_wreck" then
          local dens = (p.kind == "rv_rock") and 500 or 220
          p.body = PhysicsAddBox({ x = wx, y = y, z = wz },
                                 { x = sx * 0.5, y = sy * 0.5, z = sz * 0.5 },
                                 true, { mass = math.max(60, sx * sy * sz * dens),
                                         friction = 0.6, restitution = 0.05 })
        end
        return
      end
    end
  end
  -- 重试失败：扔到更远处，保证不卡死循环
  local wx = rv.x + fx * (fMax + 20) - fz * 30
  local wz = rv.z + fz * (fMax + 20) + fx * 30
  SetPosition(p.ent, { x = wx, y = 0.3, z = wz })
  p.x, p.z = wx, wz
end
local function spawnProps()
  if #props > 0 then return end
  for _, t in ipairs(PROP_TYPES) do
    for _ = 1, t.n do
      local ent = SpawnPrefab(t.pre, { x = 0, y = -50, z = 0 })
      if ent ~= nil then
        local p = { ent = ent, kind = t.pre, x = 0, z = 0 }
        placeProp(p, 0, 1, -45, 65)  -- 初始绕营地环形铺满
        props[#props + 1] = p
      end
    end
  end
end
local function recycleProps(fx, fz)
  for _, p in ipairs(props) do
    -- 被撞开的道具：视觉跟随物理体（位置以 Jolt 解算为准）
    if p.body then
      local pp = PhysicsGetPosition(p.body)
      if pp then
        local mx, mz = pp.x - p.x, pp.z - p.z
        if mx * mx + mz * mz > 1e-6 then
          p.x, p.z = pp.x, pp.z
          SetPosition(p.ent, { x = pp.x, y = pp.y, z = pp.z })
        end
      end
    end
    local dx, dz = p.x - rv.x, p.z - rv.z
    local f = dx * fx + dz * fz
    local lat = dx * (-fz) + dz * fx
    if f < -40 or f > 95 or math.abs(lat) > 55 then
      placeProp(p, fx, fz, 25, 70)
    end
  end
end

-- 节点标记柱（世界坐标）；名字牌在 drawDriveHud 里用 WorldToScreen 投影绘制
-- （引擎的 EntityPlates 只是数据 API，不主动渲染）
local function spawnMarkers()
  if markersSpawned then return end
  markersSpawned = true
  for _, nd in ipairs(NODES) do
    if nd.id ~= "camp" then  -- 营地=原点，柱子会插在房车里
      local ent = SpawnPrefab("rv_marker", { x = nd.x, y = 1.6, z = nd.z })
      if ent ~= nil then
        SetScale(ent, 0.16, 3.2, 0.16)
        -- 静态碰撞柱（撞上会停车，穿不过去）
        PhysicsAddBox({ x = nd.x, y = 1.6, z = nd.z },
                      { x = 0.09, y = 1.6, z = 0.09 }, false, {})
      end
    end
  end
end

local function nodeName(id)
  for _, nd in ipairs(NODES) do
    if nd.id == id then return nd.name end
  end
  return "荒野"
end

local function nodeById(id)
  for _, nd in ipairs(NODES) do
    if nd.id == id then return nd end
  end
  return nil
end

-- ---------------------------------------------------------------------------
-- 停靠搜刮：物资箱 / 战利品 Roll / 尸群威胁 / 撤离
-- ---------------------------------------------------------------------------
local function nodeLootRoll(nd)
  local out = {}
  for mat, range in pairs(nd.loot or {}) do
    local lo, hi = range[1] or 0, range[2] or 0
    if hi > lo then
      local n = lo + math.random(0, hi - lo)
      if math.random() < 0.12 then n = n + hi end  -- 12% 意外大件
      if n > 0 then out[mat] = (out[mat] or 0) + n end
    end
  end
  if not next(out) then out.cloth = 1 end  -- 保底
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
      -- 环形随机落位（房车局部系）：3..8m，避开车体 2.6m / 其他箱 1.4m
      local cx, cz
      for _ = 1, 12 do
        local a = math.random() * math.pi * 2
        local r = 3.0 + math.random() * 5.0
        local wx, wz = rotOf(math.sin(a) * r, math.cos(a) * r)
        local rdx, rdz = wx - rv.x, wz - rv.z
        if rdx * rdx + rdz * rdz >= 6.75 then
          local ok = true
          for _, c in ipairs(crates) do
            local ddx, ddz = wx - c.x, wz - c.z
            if ddx * ddx + ddz * ddz < 1.96 then ok = false break end
          end
          if ok then cx, cz = wx, wz break end
        end
      end
      if cx == nil then  -- 兜底：右侧排开
        cx, cz = rotOf(3.2, -2.0 + 1.4 * (i - 1))
      end
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
  local nd = nodeById(curNode) or { loot = {}, danger = 0 }
  local loot = nodeLootRoll(nd)
  local parts = {}
  for mat, n in pairs(loot) do
    stopBag[mat] = (stopBag[mat] or 0) + n
    parts[#parts + 1] = MAT_NAMES[mat] .. "+" .. n
  end
  SpawnFloatText(c.x, 0.9, c.z, table.concat(parts, "  "),
                 false, 1.0, 0.85, 0.45, 1.0)
  Despawn(c.ent)
  table.remove(crates, idx)
  -- 尸群被搜刮声惊动：+1 基础 + 节点危险度
  stopThreat = math.min(STOP.threatMax,
                        stopThreat + STOP.searchThreat + (nd.danger or 0))
  if stopThreat >= STOP.threatMax and evacT < 0 then
    evacT = STOP.evacTime
    toast.text = "尸群被惊动了！赶紧撤（E 收藏撤离 / T 直接上路）"
    toast.t = 3.5
  end
end

-- 物资入库；keepRatio < 1 表示被袭折损。返回入库摘要文本。
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

-- 结束停靠（keepRatio = 1 主动撤离 / STOP.keepRatio 被袭折损）→ 车库模式
local function endStop(keepRatio)
  local got = bankStopBag(keepRatio)
  despawnCrates()
  stopThreat = 0
  evacT = -1
  day = day + 1
  setMode("garage")
  toast.text = (got == "" and "空手而归" or "物资入库: " .. got)
               .. " — 第 " .. day .. " 天"
  toast.t = 4.0
end

local function updateDrive(dt)
  local throttle = (ActionDown("w") and 1 or 0) - (ActionDown("s") and 1 or 0)
  local steer = (ActionDown("a") and 1 or 0) - (ActionDown("d") and 1 or 0)
  if autoPilot then
    driveT = driveT + dt
    if nearNode and nearNode.id ~= "camp" and driveT > 3.0 then
      setMode("stop")  -- 靠站（营地是家，不停；autoPilot 保持 true 接管搜刮）
      return
    end
    if driveT > 45.0 then  -- 45s 没靠上任何节点：荒野停车兜底
      autoPilot = false
      setMode("garage")
      return
    end
    throttle = 1
    -- 朝最近的非营地节点寻的（验证钩子）：误差角 → 转向
    --（yaw 约定：前进 = (sin yaw, cos yaw)，故目标角 = atan2(dx, dz)）
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
    local target = autoTarget
    if target then
      local dx, dz = target.x - rv.x, target.z - rv.z
      local desired
      if dz > 0 then desired = math.atan(dx / dz)
      elseif dz < 0 then desired = math.atan(dx / dz) + (dx >= 0 and math.pi or -math.pi)
      else desired = (dx >= 0) and (math.pi * 0.5) or (-math.pi * 0.5) end
      local err = desired - rv.yaw
      while err > math.pi do err = err - 2 * math.pi end
      while err < -math.pi do err = err + 2 * math.pi end
      steer = math.max(-1, math.min(1, err * 1.5))
    end
  end

  -- 先读回上一物理步：位置/速度以 Jolt 解算为准。被障碍挡住时速度投影
  -- 小于意图值 -> 写回 rv.speed，撞墙自然减速。必须在油门积分之前读，
  -- 否则读到的永远是上帧刚设进去的值（会死锁在 0）。
  local bodyVy = 0
  if rvBody then
    local p = PhysicsGetPosition(rvBody)
    if p then rv.x, rv.z = p.x, p.z end
    local v = PhysicsGetVelocity(rvBody)
    if v then
      bodyVy = v.y
      local fxr, fzr = math.sin(rv.yaw), math.cos(rv.yaw)
      local proj = v.x * fxr + v.z * fzr
      if math.abs(proj) < math.abs(rv.speed) then rv.speed = proj end
    end
  end

  -- 超载惩罚：极速/加速按超出比例打折
  local ratio = stats.weight / WEIGHT_MAX
  local maxSp, acc = DRIVE.maxSpeed, DRIVE.accel
  if ratio > 1.0 then
    local over = math.min(1.0, ratio - 1.0)
    maxSp = maxSp * (1.0 - over * 0.45)
    acc = acc * (1.0 - over * 0.5)
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

  -- 转向：速度越低越难打方向；倒车反向（贴真实驾驶感）
  local sf = math.min(1.0, math.abs(rv.speed) / 3.0)
  if sf > 0.01 and steer ~= 0 then
    local dir = (rv.speed < -0.05) and -1 or 1
    rv.yaw = rv.yaw + steer * DRIVE.steerRate * sf * dt * dir
  end

  local fx, fz = math.sin(rv.yaw), math.cos(rv.yaw)

  -- 施加本帧意图：速度驱动（碰撞/推挤由物理在下一步解算），朝向每帧覆盖
  -- 以保持简化驾驶模型（不会翻车）。无刚体时退化为直接积分。
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
  recycleProps(fx, fz)

  -- 后轮扬尘
  if math.abs(rv.speed) > 2.5 then
    dustAcc = dustAcc + dt * (6.0 + math.abs(rv.speed) * 1.6)
    while dustAcc >= 1.0 do
      dustAcc = dustAcc - 1.0
      local s = (math.random() < 0.5) and -1.3 or 1.3
      local wx, wz = rotOf(s, -2.9)
      EmitParticles({
        pos = { x = wx, y = 0.12, z = wz }, count = 2,
        vel = { x = -fx * 1.2, y = 0.9, z = -fz * 1.2 },
        speedMin = 0.3, speedMax = 1.2,
        lifeMin = 0.5, lifeMax = 1.1,
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

-- 相机：车库=场景原始机位（随 rv 平移）；驾驶=车后追逐（平滑 + 高速微震）
local function updateCamera(dt)
  if camEnt == nil then return end
  local fx, fz = math.sin(rv.yaw), math.cos(rv.yaw)
  local tx, ty, tz, lookx, lookz, looky
  if mode == "drive" then
    local cp, sp = math.cos(DRIVE.camPitch), math.sin(DRIVE.camPitch)
    tx = rv.x - fx * DRIVE.camDist * cp
    ty = 1.4 + sp * DRIVE.camDist
    tz = rv.z - fz * DRIVE.camDist * cp
    lookx, lookz, looky = rv.x + fx * 2.5, rv.z + fz * 2.5, 0.9
  else
    tx, ty, tz = rv.x, 11.5, rv.z - 9.2
    lookx, lookz, looky = rv.x, rv.z, 0.0
  end
  if camSmooth == nil then camSmooth = { x = tx, y = ty, z = tz } end
  local k = 1 - math.exp(-5.0 * dt)
  camSmooth.x = camSmooth.x + (tx - camSmooth.x) * k
  camSmooth.y = camSmooth.y + (ty - camSmooth.y) * k
  camSmooth.z = camSmooth.z + (tz - camSmooth.z) * k
  local sx, sy, sz = camSmooth.x, camSmooth.y, camSmooth.z
  if mode == "drive" then
    local amp = math.min(0.05, math.abs(rv.speed) * 0.005)
    sx = sx + (math.random() - 0.5) * amp
    sy = sy + (math.random() - 0.5) * amp
  end
  SetPosition(camEnt, { x = sx, y = sy, z = sz })
  local dx, dy, dz = lookx - sx, looky - sy, lookz - sz
  local len = math.sqrt(dx * dx + dy * dy + dz * dz)
  if len > 1e-4 then SetLook(camEnt, dx / len, dy / len, dz / len) end
end

function setMode(m)
  if mode == m then return end
  local prev = mode
  mode = m
  if m == "garage" then
    rv.speed = 0
    if rvBody then PhysicsSetVelocity(rvBody, { x = 0, y = 0, z = 0 }) end
    if prev == "stop" then despawnCrates() end
    toast.text = "荒野停车 — 车库模式（可改装）"
    toast.t = 3.5
  elseif m == "stop" then
    rv.speed = 0
    if rvBody then PhysicsSetVelocity(rvBody, { x = 0, y = 0, z = 0 }) end
    if nearNode then curNode = nearNode.id end
    stopBag, stopThreat, evacT = {}, 0, -1
    spawnCrates()
    local nd = nodeById(curNode)
    local stars = string.rep("★", nd and nd.danger or 0)
    toast.text = "停靠 " .. nodeName(curNode) .. (stars ~= "" and ("（危险 " .. stars .. "）") or "")
                 .. " — 左键搜箱  E 收藏撤离  T 上路"
    toast.t = 5.0
  else  -- drive
    if prev == "stop" then
      -- 开走 = 收工：物资入库 + 天数 +1（物资箱自然残留在这片荒野）
      despawnCrates()
      day = day + 1
      local got = bankStopBag(1.0)
      stopThreat = 0
      evacT = -1
      toast.text = (got == "" and "启程" or "物资入库: " .. got)
                   .. " — 第 " .. day .. " 天"
      toast.t = 4.0
    else
      toast.text = "上路！W/S 油门·刹车  A/D 转向  空格 停车"
      toast.t = 4.5
    end
  end
  if ghostEnt ~= nil then SetVisible(ghostEnt, false) end
  if hoverDecalEnt ~= nil then SetVisible(hoverDecalEnt, false) end
  hover = nil
  if gridFloorEnt ~= nil then
    SetVisible(gridFloorEnt, (m == "garage") and gridVisible or false)
  end
  if gridRoofEnt ~= nil then
    SetVisible(gridRoofEnt, (m == "garage") and gridVisible and curLayer == "roof" or false)
  end
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
  local data = { cell = CELL, gw = GW, gh = GH, materials = materials,
                 rv = { x = rv.x, z = rv.z, yaw = rv.yaw, node = curNode },
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
    -- 沙盒阶段：材料只补足到初始值（不因旧存档残量锁死建造）
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
      local saveLayer = row.layer or cat.layer or "floor"
      -- 重放免费：材料状态由存档 materials 字段整体恢复
      if saveLayer == "roof" then
        place(cat, math.floor(row.cx), math.floor(row.cz), math.floor(row.rot or 0), true)
      else
        local saved = curLayer
        curLayer = "floor"
        place(cat, math.floor(row.cx), math.floor(row.cz), math.floor(row.rot or 0), true)
        curLayer = saved
      end
    end
  end
  -- 房车世界位姿（旧存档无 rv 字段 = 原点）
  if type(data.rv) == "table" then
    rv.x = tonumber(data.rv.x) or 0
    rv.z = tonumber(data.rv.z) or 0
    rv.yaw = tonumber(data.rv.yaw) or 0
    curNode = tostring(data.rv.node or "camp")
    day = tonumber(data.day) or 1
    applyRvTransform()
    if rvBody then  -- 刚体跟着传送，避免读档后物理把车拽回旧位置
      PhysicsSetPosition(rvBody, { x = rv.x, y = 0.9, z = rv.z })
      PhysicsSetVelocity(rvBody, { x = 0, y = 0, z = 0 })
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
    "R 旋转   Tab 地板/车顶   G 网格",
    "T 上路   M 地图   F5/F9 存/读",
  }
  for i, h in ipairs(hints) do
    DrawText(h, vw - 14, 14 + (i - 1) * 20, 14, 0.85, 0.84, 0.80, 0.9, true, true)
  end

  -- 右上材料库存面板
  local mw, mh2 = 150, 118
  local mx0 = vw - mw - 10
  local my0 = 78
  DrawRect(mx0, my0, mw, mh2, 0.08, 0.09, 0.10, 0.72)
  DrawRectOutline(mx0, my0, mw, mh2, 0.45, 0.52, 0.42, 0.9)
  DrawText("材料库存", mx0 + 10, my0 + 6, 14, 0.85, 0.80, 0.62, 1)
  local mats = {
    { MAT_NAMES.metal,     materials.metal },
    { MAT_NAMES.electronics, materials.electronics },
    { MAT_NAMES.cloth,     materials.cloth },
    { MAT_NAMES.wood,      materials.wood },
  }
  for i, mv in ipairs(mats) do
    local yy = my0 + 30 + (i - 1) * 21
    DrawText(mv[1], mx0 + 10, yy, 14, 0.88, 0.86, 0.80, 1)
    DrawText(tostring(mv[2]), mx0 + mw - 14, yy, 14, 0.95, 0.92, 0.82, 1, true)
  end

  -- 当前层指示
  DrawText(curLayer == "roof" and "【车顶层】" or "【地板层】",
           math.floor(vw * 0.5), 14, 16, 0.55, 0.85, 0.98, 1, true)

  -- 悬停实例信息
  if hover and hover.inst then
    local c = hover.inst.cat
    DrawText(c.name .. "  ·  " .. (c.desc or "") .. "  ·  " .. costText(c),
             math.floor(vw * 0.5), vh - rowsN * (slotH + gap) - 40,
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
  camEnt = FindNamedEntity("Main Camera")
  groundEnt = FindNamedEntity("Ground")
  collectRvParts()
  -- 地面放大到 800x800：追逐相机高度下 60x60 的地面边缘会露馅
  if groundEnt ~= nil then SetScale(groundEnt, 800, 0.1, 800) end
  -- 合并 worldmap.json：名字/危险度/战利品表/链接（本文件 NODES 只持世界坐标）
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
  spawnProps()
  spawnMarkers()
  do  -- 验证钩子：工作目录放 autopilot_on.txt 即自动上路巡航（截图验收用；
      -- Lua 沙箱无 os 库，读不了环境变量）
    local flag = ReadText("autopilot_on.txt")
    autoPilot = (flag ~= nil and flag ~= "")
  end
  ensureHelpers()
  -- 初始示例布局（有存档时会被覆盖）：一眼看到“布置好的房车”
  place(CATALOG[1], 1, 0, 0, true)    -- 双人床（贴后墙右侧，3x4 格）
  place(CATALOG[3], 0, 4, 0, true)    -- 灶台（左舷中段）
  place(CATALOG[5], 0, 6, 0, true)    -- 净水箱（灶台后方）
  place(CATALOG[9], 3, 6, 0, true)    -- 储物箱
  local saved = curLayer
  curLayer = "roof"
  place(CATALOG[17], 0, 8, 0, true)   -- 太阳能板（车顶）
  place(CATALOG[19], 2, 9, 0, true)   -- 车顶行李架
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
  applyRvTransform()
  -- RV 动态刚体：半长 (1.2, 0.9, 4.3)，底面落在 y=0 隐式地面上；
  -- 载重影响质量（超载的车更沉、更难被推动，也推东西更狠）
  rvBody = PhysicsAddBox({ x = rv.x, y = 0.9, z = rv.z },
                         { x = 1.2, y = 0.9, z = 4.3 }, true,
                         { mass = math.max(800, 1200 + stats.weight * 0.8),
                           friction = 0.4, restitution = 0.05 })
  if autoPilot then setMode("drive") end
end

function on_update(ent, dt)
  if toast.t > 0 then toast.t = toast.t - dt end
  ensureHelpers()
  if ActionPressed("m") then mapOpen = not mapOpen end

  -- 驾驶模式：车库交互（放置/拆除/热栏）全部挂起
  if mode == "drive" then
    if ActionPressed("space") then
      -- 靠在节点 12m 内 = 停靠搜刮，荒野 = 车库改装
      if nearNode then setMode("stop") else setMode("garage") end
    end
    updateDrive(dt)
    updateCamera(dt)
    return
  end

  -- 停靠搜刮：搜箱 / 尸群威胁 / 撤离时限
  if mode == "stop" then
    if ActionPressed("e") then endStop(1.0) end
    if mode == "stop" and ActionPressed("t") then setMode("drive") end
  end
  if mode == "stop" and evacT >= 0 then
    evacT = evacT - dt
    if evacT <= 0 then
      toast.text = "尸群冲垮了停靠点！丢下部分物资…"
      toast.t = 3.0
      endStop(STOP.keepRatio)
    end
  end
  if mode == "stop" then
    -- 点击搜箱（屏幕空间命中最近的箱）
    if InputMousePressed(0) then
      local mp = InputMousePos()
      local best, bestD = nil, STOP.pickRadius * STOP.pickRadius
      for i, c in ipairs(crates) do
        local sp = WorldToScreen(c.x, 0.5, c.z)
        if sp then
          local dx, dy = sp.x - mp.x, sp.y - mp.y
          local d2 = dx * dx + dy * dy
          if d2 < bestD then best, bestD = i, d2 end
        end
      end
      if best then searchCrate(best) end
    end
    -- 自动化钩子：每 1.2s 搜最近一箱，搜完主动撤离（截图验收用）
    if autoPilot then
      autoT = autoT + dt
      if autoT > 1.2 then
        autoT = 0
        if #crates > 0 then
          searchCrate(1)
        elseif evacT < 0 then
          autoPilot = false
          endStop(1.0)
        end
      end
    end
    updateCamera(dt)
    return
  end

  if ActionPressed("t") then setMode("drive") end
  updateCamera(dt)

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
      local gwx, gwz = rotOf(ox, oz)
      SetVisible(ghostEnt, true)
      SetPosition(ghostEnt, { x = gwx, y = baseY + 0.035, z = gwz })
      SetRotationY(ghostEnt, rv.yaw)
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

  -- 鼠标操作
  if InputMousePressed(0) then
    if hitSlot then
      selected = hitSlot
    elseif hover and hover.valid then
      if place(CATALOG[selected], hover.cx, hover.cz, rot) then
        recomputeStats()
      end
    elseif hover and not hover.valid and not overUi then
      local cat = CATALOG[selected]
      if fits(cat, hover.cx, hover.cz, rot) and not canAfford(cat) then
        toast.text = "材料不足: 需要 " .. costText(cat)
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
end

-- 驾驶 HUD：速度表 / 档位 / 载重（超载变红）/ 临近节点提示
local function drawDriveHud()
  local vp = GetViewportSize()
  local vw = (vp and vp.w) or 1280
  local vh = (vp and vp.h) or 720
  local kmh = math.floor(math.abs(rv.speed) * 3.6)
  local ratio = stats.weight / WEIGHT_MAX
  local over = ratio > 1.0

  local pw, ph = 210, 100
  local px, py = 10, vh - ph - 10
  DrawRect(px, py, pw, ph, 0.08, 0.09, 0.10, 0.72)
  DrawRectOutline(px, py, pw, ph, 0.55, 0.52, 0.42, 0.9)
  local gear = (rv.speed > 0.05) and "D" or ((rv.speed < -0.05) and "R" or "N")
  DrawText(string.format("%d", kmh), px + 14, py + 10, 34,
           over and 1.0 or 0.95, over and 0.5 or 0.92, over and 0.35 or 0.78, 1)
  DrawText("km/h", px + 84, py + 26, 15, 0.7, 0.68, 0.62, 1)
  DrawText("挡位 " .. gear, px + 140, py + 26, 15, 0.6, 0.85, 0.95, 1)
  DrawText(string.format("载重 %.0f / %d kg%s", stats.weight, WEIGHT_MAX,
           over and "  超载! 极速受限" or ""),
           px + 14, py + 60, 13, over and 1 or 0.85, over and 0.45 or 0.84,
           over and 0.3 or 0.8, 1)
  DrawRect(px + 14, py + 80, pw - 28, 6, 0.13, 0.13, 0.14, 0.9)
  DrawRect(px + 14, py + 80, (pw - 28) * math.min(1, ratio), 6,
           over and 0.88 or 0.45, over and 0.28 or 0.78, over and 0.24 or 0.44, 1)

  local hints = { "W 油门   S 刹车/倒车", "A/D 转向   空格 停车" }
  for i, h in ipairs(hints) do
    DrawText(h, vw - 14, 14 + (i - 1) * 20, 14, 0.85, 0.84, 0.80, 0.9, true, true)
  end

  local top = "停靠点: " .. nodeName(curNode)
  if nearNode and nearNode.id ~= curNode then
    top = top .. "   ▶ 临近 " .. nearNode.name .. "（空格停靠）"
  end
  DrawText(top, math.floor(vw * 0.5), 14, 16, 0.95, 0.9, 0.75, 1, true)

  -- 节点名字牌：标记柱顶投影到屏幕
  for _, nd in ipairs(NODES) do
    if nd.id ~= "camp" then
      local sp = WorldToScreen(nd.x, 3.6, nd.z)
      if sp then
        DrawText("▲ " .. nd.name, math.floor(sp.x), math.floor(sp.y), 15,
                 0.95, 0.85, 0.45, 1, true)
      end
    end
  end

  if toast.t > 0 then
    DrawText(toast.text, math.floor(vw * 0.5), 120, 16, 0.98, 0.93, 0.75, 1, true)
  end
end

-- 停靠 HUD：尸群威胁条 / 本次搜获 / 撤离警报 / 物资箱标记
local function drawStopHud()
  local vp = GetViewportSize()
  local vw = (vp and vp.w) or 1280
  local vh = (vp and vp.h) or 720
  local nd = nodeById(curNode)

  -- 顶部：地点 + 危险度 + 天数
  local stars = string.rep("★", nd and nd.danger or 0)
  DrawText("停靠 · " .. nodeName(curNode)
           .. (stars ~= "" and ("  危险 " .. stars) or "")
           .. "   第 " .. day .. " 天",
           math.floor(vw * 0.5), 14, 16, 0.95, 0.88, 0.7, 1, true)

  -- 左上：尸群威胁条（分段，绿→红）
  local px, py, pw, ph = 10, 10, 216, 64
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

  -- 左下：本次搜获
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

  -- 撤离时限：全屏红闪 + 大字倒计时
  if evacT >= 0 then
    local flash = (math.floor(evacT * 4) % 2 == 0)
    DrawRect(0, 0, vw, vh, 0.7, 0.05, 0.02, flash and 0.13 or 0.05)
    DrawText(string.format("尸群逼近！%.0f 秒内撤离", math.ceil(evacT)),
             math.floor(vw * 0.5), math.floor(vh * 0.28), 30,
             1, flash and 0.25 or 0.5, 0.2, 1, true)
  end

  -- 场上物资箱标记
  for _, c in ipairs(crates) do
    local sp = WorldToScreen(c.x, 0.75, c.z)
    if sp then
      DrawText("□ 搜", math.floor(sp.x), math.floor(sp.y), 14,
               1, 0.75, 0.35, 1, true)
    end
  end

  local hints = { "左键 搜箱", "E 收藏撤离   T 上路", "M 世界地图" }
  for i, h in ipairs(hints) do
    DrawText(h, vw - 14, 14 + (i - 1) * 20, 14, 0.85, 0.84, 0.80, 0.9, true, true)
  end

  if toast.t > 0 then
    DrawText(toast.text, math.floor(vw * 0.5), 120, 16, 0.98, 0.93, 0.75, 1, true)
  end
end

-- 世界地图（M）：节点/链接/当前停靠点/房车位置，纯 2D 投影
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

  -- 世界坐标 → 面板（覆盖全部节点 + 边距）
  local wx0, wx1, wz0, wz1 = -190, 200, -125, 215
  local function mapXY(x, z)
    return px + 26 + (x - wx0) / (wx1 - wx0) * (pw - 52),
           py + 44 + (z - wz0) / (wz1 - wz0) * (ph - 84)
  end
  -- 道路（链接）
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
  -- 节点（危险度着色：绿→红）
  for _, nd in ipairs(NODES) do
    local mx2, my2 = mapXY(nd.x, nd.z)
    local cur = (nd.id == curNode)
    local danger = nd.danger or 0
    DrawCircle(mx2, my2, cur and 7 or 5, cur and 2 or 1.5,
               0.45 + danger * 0.17, 0.85 - danger * 0.18, 0.32, 1, true)
    DrawText(nd.name .. (danger > 0 and string.rep("!", danger) or ""),
             mx2, my2 - 20, 13, cur and 1 or 0.82,
             cur and 0.9 or 0.78, cur and 0.6 or 0.68, 1, true)
  end
  -- 房车位置 + 朝向
  local rx, ry = mapXY(rv.x, rv.z)
  DrawLine(rx, ry, rx + math.sin(rv.yaw) * 20, ry + math.cos(rv.yaw) * 20,
           2, 0.98, 0.85, 0.4, 1)
  DrawCircle(rx, ry, 4, 2, 0.98, 0.85, 0.4, 1, true)
  DrawText("● 房车    ● 节点（越红越危险）    — 道路",
           px + 16, py + ph - 24, 12, 0.7, 0.7, 0.65, 1)
end

-- 2D HUD 必须在 on_render 里画：draw2d 上下文只在渲染期接线，
-- on_update 里的 DrawRect/DrawText 会被静默丢弃。
function on_render()
  if mode == "drive" then drawDriveHud()
  elseif mode == "stop" then drawStopHud()
  else drawHud() end
  if mapOpen then drawMapHud() end
end
