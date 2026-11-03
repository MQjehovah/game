CELL = 0.5
GW, GH = 4, 14              -- 网格宽(x) x 长(z)，单位格
ORIGIN_X = -1.0             -- 格 (0,0) 的西边缘
ORIGIN_Z = -3.5             -- 格 (0,0) 的后边缘（镜头侧）
FLOOR_Y = 0.03              -- 车厢地板顶面
ROOF_Y = 2.25               -- 车顶承载面（墙顶 2.2）
WEIGHT_MAX = 1500           -- 底盘载重 kg
SENSITIVITY = 0.0022        -- 鼠标灵敏度（弧度/像素）

-- lc_config.lua
-- 配置：网格/模块目录/生存/载具/低强度威胁/程序化地点

CATALOG = {
  { id="bed_double", name="双人床",   layer="floor", w=3, h=4, mh=0.45, color="#8A5A44", weight=60,  comfort=30, modelScale=1.0, use="bed",      desc="睡一觉恢复体力" , cost={wood=4,cloth=4}},
  { id="bed_single", name="单人床",   layer="floor", w=2, h=3, mh=0.40, color="#A0714F", weight=35,  comfort=15, modelScale=1.0, use="bed",      desc="睡一觉恢复体力" , cost={wood=2,cloth=2}},
  { id="stove",      name="灶台",     layer="floor", w=2, h=2, mh=0.50, color="#7A7F87", weight=40,  comfort=5,  powerDraw=0.5, waterDraw=2, use="stove",  modelScale=1.0, desc="用生食材做饭" , cost={metal=3,electronics=1}},
  { id="fridge",     name="冰箱",     layer="floor", w=1, h=2, mh=1.10, color="#9FB4C7", weight=30,  powerDraw=0.3, use="fridge", modelScale=1.0, desc="吃东西 / 冷藏" , cost={metal=2,electronics=3}},
  { id="water_tank", name="净水箱",   layer="floor", w=2, h=2, mh=0.90, color="#4F8FBF", weight=160, waterStore=120, use="tank", modelScale=1.0, desc="储水 120L" , cost={metal=4}},
  { id="battery",    name="电池组",   layer="floor", w=2, h=1, mh=0.50, color="#D8B23A", weight=80,  battery=5,  modelScale=1.0, desc="储能 5kWh" , cost={metal=2,electronics=4}},
  { id="workbench",  name="工作台",   layer="floor", w=2, h=3, mh=0.55, color="#8C6239", weight=70,  use="workbench", modelScale=1.2, desc="修车 / 检修" , cost={metal=2,wood=3}},
  { id="storage",    name="储物箱",   layer="floor", w=1, h=1, mh=0.60, color="#6E7B52", weight=15,  storage=50, use="storage", modelScale=1.0, desc="查看随身物资" , cost={wood=2}},
  { id="heater",     name="电暖器",   layer="floor", w=1, h=1, mh=0.55, color="#C96B3F", weight=12,  comfort=10, powerDraw=0.8, use="heater", modelScale=1.0, desc="耗电 0.8kW 舒适 +10" , cost={metal=1,electronics=2}},
  { id="tv",         name="娱乐柜",   layer="floor", w=2, h=1, mh=0.90, color="#5D5366", weight=28,  comfort=8,  powerDraw=0.2, use="tv", modelScale=1.0, desc="耗电 0.2kW 舒适 +8" , cost={electronics=3,cloth=1}},
  { id="sink",       name="水槽",     layer="floor", w=1, h=1, mh=0.35, color="#B8C4CE", weight=12,  waterDraw=1, comfort=2, use="sink", modelScale=1.0, desc="喝水 / 灌水" , cost={metal=2}},
  { id="bathroom",   name="卫生间",   layer="floor", w=2, h=3, mh=1.00, color="#7F9BA8", weight=120, comfort=12, waterDraw=3, use="bathroom", modelScale=1.0, desc="洗漱提神" , cost={metal=3,wood=2}},
  { id="generator",  name="发电机",   layer="floor", w=2, h=2, mh=0.70, color="#C7B45A", weight=180, powerGen=2.0, comfort=-5, use="generator", modelScale=1.0, desc="发电 2.0kW 噪音 舒适 -5" , cost={metal=6,electronics=3}},
  { id="med_cabinet",name="药柜",     layer="floor", w=1, h=1, mh=0.50, color="#D8E8E0", weight=20,  comfort=6,  use="med", modelScale=1.0, desc="用布料包扎 +20 生命" , cost={metal=1,cloth=2}},
  { id="gun_rack",   name="武器架",   layer="floor", w=2, h=1, mh=0.50, color="#6B4F3A", weight=40,  use="gun", modelScale=1.0, desc="防御步枪伤害翻倍" , cost={wood=3,metal=1}},
  { id="turret",     name="炮塔底座", layer="floor", w=2, h=2, mh=0.60, color="#5A5A5A", weight=90,  use="turret", modelScale=1.0, desc="自动射击 16m 内感染者" , cost={metal=8,electronics=4}},
  { id="solar",      name="太阳能板", layer="roof",  w=2, h=3, mh=0.15, color="#3E6FB0", weight=25,  powerGen=1.8, modelScale=1.0, desc="发电 1.8kW（车顶限定）" , cost={metal=2,electronics=4}},
  { id="roof_vent",  name="通风扇",   layer="roof",  w=1, h=1, mh=0.25, color="#8891A0", weight=8,   comfort=4, powerDraw=0.1, modelScale=1.0, desc="耗电 0.1kW 舒适 +4" , cost={metal=1,electronics=1}},
  { id="roof_rack",  name="车顶行李架", layer="roof", w=2, h=4, mh=0.35, color="#5C6670", weight=30, storage=150, modelScale=1.0, desc="车载储物 150L" , cost={metal=3}},
  { id="coffee",     name="咖啡机",   layer="floor", w=1, h=1, mh=0.42, color="#3A332E", weight=8,  powerDraw=0.4, use="coffee", modelScale=1.0, desc="咖啡豆+水+电 = 热咖啡" , cost={metal=2,electronics=2}},
  { id="plant",      name="种植箱",   layer="floor", w=1, h=2, mh=0.38, color="#54703F", weight=20, comfort=4, use="plant", modelScale=1.0, desc="种下种子，成熟后收获" , cost={wood=2,cloth=1}},
}
HOTKEYS = { "1","2","3","4","5","6","7","8","9","0" }

-- 生存：时间流速 / 需求衰减（按游戏分钟）/ 睡眠
SURVIVAL = {
  timeScale = 3.0,             -- 1 现实秒 = 3 游戏分钟
  startClock = 8 * 60,
  hungerRate = 0.030,
  thirstRate = 0.060,
  energyRate = 0.022,
  hygieneRate = 0.012,
  healthDrain = 0.06,          -- 饥饿/口渴归零后按游戏分钟扣血
  healthRegen = 0.012,         -- 饱食/水分充足时缓慢回血
  sprintExtra = 0.030,         -- 疾跑额外体力消耗
  sleepHours = 8,
  nightStart = 20 * 60,
  nightEnd = 6 * 60,
  waterMax = 120,
  drink = 30,
  bathWater = 3.0,
  cookWater = 0.5,
  coffeeWater = 0.3,
  plantMinutes = 240,          -- 现实约 80 秒成熟
  playerDamageCd = 1.5,
}

-- 物品目录（吃的/喝的/材料）
ITEMS = {
  water    = { name = "瓶装水", kind = "drink", thirst = 28, desc = "口渴 -28" },
  canned   = { name = "罐头",   kind = "food",  hunger = 22, health = 1, desc = "饥饿 -22" },
  rawfood  = { name = "生食材", kind = "food",  hunger = 8, health = -2, desc = "可直接吃，但会掉血" },
  meal     = { name = "热饭",   kind = "food",  hunger = 42, health = 6, desc = "饥饿 -42 生命 +6" },
  coffee   = { name = "咖啡豆", kind = "brew",  desc = "咖啡机原料" },
  brew     = { name = "热咖啡", kind = "drink", thirst = 10, energy = 30, health = 2, desc = "体力 +30" },
  seed     = { name = "种子",   kind = "seed",  desc = "种进种植箱" },
  repairkit= { name = "修理包", kind = "repair", amount = 35, desc = "修车 +35" },
  rawfish  = { name = "生鱼",   kind = "food",  hunger = 8,  health = -2, desc = "需要篝火烤熟" },
  fish     = { name = "烤鱼",   kind = "food",  hunger = 30, health = 5, desc = "饥饿 -30 生命 +5" },
  fishsoup = { name = "鱼汤",   kind = "food",  hunger = 35, thirst = 20, health = 3, desc = "饥饿 -35 口渴 -20" },
  feast    = { name = "大餐",   kind = "food",  hunger = 65, health = 10, desc = "饥饿 -65 生命 +10" },
}
ITEM_ORDER = { "water", "canned", "rawfood", "meal", "fishsoup", "feast", "coffee", "brew", "rawfish", "fish", "seed", "repairkit" }

-- 钓鱼鱼种（权重 / 尺寸 cm）：越稀有越大，用来做纪录与风味
FISH_SPECIES = {
  { name = "鲫鱼", w = 50, min = 12, max = 28 },
  { name = "鲤鱼", w = 30, min = 25, max = 55 },
  { name = "鲶鱼", w = 15, min = 35, max = 75 },
  { name = "白鲢", w = 5,  min = 40, max = 80 },
}

-- 工作台可维修的车体/零件
REPAIR_PARTS = {
  { id = "body",    name = "车体" },
  { id = "engine",  name = "引擎" },
  { id = "tires",   name = "轮胎" },
  { id = "battery", name = "电瓶" },
}

-- 灶台配方
RECIPES = {
  { id = "meal",     name = "热饭", cost = { rawfood = 1 },               water = 0.5, out = "meal",     desc = "饥饿 -42  生命 +6" },
  { id = "fishsoup", name = "鱼汤", cost = { fish = 1 },                  water = 1.0, out = "fishsoup", desc = "饥饿 -35  口渴 -20" },
  { id = "feast",    name = "大餐", cost = { rawfood = 2, meal = 1 },     water = 0.5, out = "feast",    desc = "饥饿 -65  生命 +10" },
}

-- 物资箱里的非材料掉落（权重）
LOOT_ITEMS = {
  { id = "water",     w = 30, min = 1, max = 2 },
  { id = "canned",    w = 24, min = 1, max = 2 },
  { id = "rawfood",   w = 24, min = 1, max = 2 },
  { id = "coffee",    w = 14, min = 1, max = 1 },
  { id = "seed",      w = 12, min = 1, max = 1 },
  { id = "repairkit", w = 7,  min = 1, max = 1 },
  { id = "meal",      w = 4,  min = 1, max = 1 },
}

DRIVE = {
  accel = 4.5, brake = 9.0, drag = 0.8, reverseMax = 3.0,
  maxSpeed = 9.0, steerRate = 1.4,
  camDist = 10.5, camPitch = 0.40,
}
-- 车辆：燃油 / 车体耐久
VEH = {
  tank = 60, burn = 0.12, idleBurn = 0.010, limpSpeed = 1.5,
  pressureWear = 0.00002,   -- 冷胎压随里程下降（bar/m，约 0.02bar/1000m）
  gearUp = { 1.8, 3.4, 5.2, 7.0 },   -- 自动挡升挡车速（m/s，共 5 挡）
  gearRpmMin = 850, gearRpmMax = 2600,
  durMax = 100, impactK = 1.5, repairAmt = 25,
  partMax = 100,
  partWear = { engine = 0.030, tires = 0.050, battery = 0.006 },  -- 每米磨损
  repairCost = { metal = 2, wood = 1 },
  eventMin = 16, eventMax = 32,
}
-- 停靠搜刮
STOP = {
  crates = 6, threatMax = 3, evacTime = 15, searchThreat = 1,
  keepRatio = 1.0, pickRadius = 30,
}
-- 顺路货运合同：营地板子接单 -> 开到目标 POI -> 结算材料/燃料。
-- 货物重量计入 stats.weight，直接压加速度/极速/油耗（Nomad Drive 的
-- “顺路载货”循环：不新增地图，只把已有节点变成可跑的目标）。
JOBS = {
  offers = 3,
  minDistance = 45,
  boardRadius = 3.4,
  deliverRadius = 14,
  maxCargo = 4,
  fuelBase = 6,
  fuelPer100m = 5,
  cargo = {
    { id = "med",   name = "医疗箱", kg = 34, mat = "cloth" },
    { id = "parts", name = "机械件", kg = 46, mat = "metal" },
    { id = "elec",  name = "电子件", kg = 28, mat = "electronics" },
    { id = "grain", name = "粮袋",   kg = 40, mat = "wood" },
  },
}

-- 低强度威胁：夜里/危险地点才偶发 1-3 只，不强制撤离
RAID = {
  spawnEvery = 4.0, aliveMax = 3, speed = 1.15, hpBase = 2,
  attackDist = 1.5, attackCd = 1.6, attackDmg = 7, hitRadius = 26,
  fireCd = 0.28, turretCd = 0.8, turretRange = 16,
}
-- 途中事件（权重）：感染者横穿 / 路边补给 / 燃油桶
EVENTS = {
  { id = "horde",   w = 1 },
  { id = "supply",  w = 3 },
  { id = "fuelcan", w = 2 },
}
-- worldmap.json 节点的世界坐标（营地=原点，地图 1.0 ≈ 160m）
NODES = {
  { id = "camp",    name = "营地",       x = 0,   z = 0 },
  { id = "gasstop", name = "废弃加油站", x = 0,  z = 60 },
  { id = "ruin",    name = "郊区废宅",   x = 38,  z = 27 },
  { id = "mall",    name = "购物中心",   x = 100, z = -73 },
  { id = "town",    name = "小镇主街",   x = 131, z = -8 },
  { id = "depot",   name = "物流仓库",   x = 154, z = -58 },
  { id = "farm",    name = "旧农场",     x = -64, z = 44, danger = 1,
    loot = { wood = { 2, 5 }, cloth = { 1, 3 } }, links = { "camp" } },
  { id = "lakecamp", name = "湖边营地",   x = -52, z = 30, danger = 0, pond = true,
    loot = { wood = { 2, 4 }, cloth = { 1, 3 } }, links = { "camp" } },
  { id = "clinic",  name = "路边诊所",   x = 24,  z = -40, danger = 2,
    loot = { cloth = { 2, 4 }, electronics = { 1, 2 } }, links = { "camp" } },
}
MAT_NAMES = { metal="金属片", electronics="电子件", cloth="布料", wood="木材" }
materials = { metal=12, electronics=8, cloth=6, wood=10 }

-- 程序化公路地点：每隔一段里程在前方生成一个
POI_KINDS = {
  { name = "废弃加油站", danger = 1, loot = { metal={1,3}, electronics={0,2} } },
  { name = "路边废宅",   danger = 1, loot = { wood={2,4}, cloth={1,3} } },
  { name = "旧农场",     danger = 1, loot = { wood={2,5}, cloth={1,2} } },
  { name = "小诊所",     danger = 2, loot = { cloth={2,4}, electronics={1,2} } },
  { name = "废弃商场",   danger = 2, loot = { electronics={2,4}, cloth={1,3} } },
  { name = "物流仓库",   danger = 3, loot = { metal={3,6}, electronics={2,4} } },
  { name = "临时营地",   danger = 0, loot = { wood={1,3}, cloth={1,2} } },
  { name = "湖边营地",   danger = 1, loot = { wood={2,4}, cloth={1,2} }, pond = true },
}

-- 山林/越野场景池（Kenney nature 套件模型 + 地面灌木色块）
SCENERY = {
  { pre = "rv_pine_a",     n = 10, kind = "tree" },
  { pre = "rv_pine_b",     n = 10, kind = "tree" },
  { pre = "rv_pine_c",     n = 10, kind = "tree" },
  { pre = "rv_oak",        n = 8,  kind = "tree" },
  { pre = "rv_deadtree",   n = 8,  kind = "tree" },
  { pre = "rv_rock_big",   n = 6,  kind = "rock" },
  { pre = "rv_rock_block", n = 10, kind = "rock" },
  { pre = "rv_cactus",     n = 8,  kind = "cactus" },
  { pre = "rv_scrub_a",    n = 90, kind = "scrub" },
  { pre = "rv_scrub_b",    n = 70, kind = "scrub" },
}

-- Dense stylised meadow layered on top of the coarse scenery: grass tufts,
-- wildflowers and bushes follow the reference art direction.
MEADOW = {
  { pre = "rv_meadow_a",      n = 20,  kind = "patch" },
  { pre = "rv_meadow_b",      n = 20,  kind = "patch" },
  { pre = "rv_meadow_c",      n = 20,  kind = "patch" },
  { pre = "rv_grass_tuft",    n = 55,  kind = "grass" },
  { pre = "rv_grass_tuft_b",  n = 45,  kind = "grass" },
  { pre = "rv_fern",          n = 20,  kind = "grass" },
  { pre = "rv_flower_white",  n = 10,  kind = "flower" },
  { pre = "rv_flower_blue",   n = 12,  kind = "flower" },
  { pre = "rv_flower_yellow", n = 10,  kind = "flower" },
  { pre = "rv_flower_red",    n = 8,   kind = "flower" },
  { pre = "rv_bush",          n = 34,  kind = "bush" },
}
