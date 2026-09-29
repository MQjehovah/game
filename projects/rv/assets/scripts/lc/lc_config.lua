CELL = 0.5
GW, GH = 4, 14              -- 网格宽(x) x 长(z)，单位格
ORIGIN_X = -1.0             -- 格 (0,0) 的西边缘
ORIGIN_Z = -3.5             -- 格 (0,0) 的后边缘（镜头侧）
FLOOR_Y = 0.03              -- 车厢地板顶面
ROOF_Y = 2.25               -- 车顶承载面（墙顶 2.2）
WEIGHT_MAX = 1500           -- 底盘载重 kg

-- lc_config.lua
-- 配置：网格/模块目录/载具/夜袭/事件/节点/场景池（全局表，供各模块共享）

CATALOG = {
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
HOTKEYS = { "1","2","3","4","5","6","7","8","9","0" }

DRIVE = {
  accel = 4.5, brake = 9.0, drag = 0.8, reverseMax = 3.0,
  maxSpeed = 9.0, steerRate = 1.4,
  camDist = 10.5, camPitch = 0.40,
}
-- 车辆：燃油 / 车体耐久
VEH = {
  tank = 60, burn = 0.75, limpSpeed = 1.5,
  durMax = 100, impactK = 1.5, repairAmt = 25,
  repairCost = { metal = 2, wood = 1 },
  eventMin = 16, eventMax = 32,
}
-- 停靠搜刮 / 夜袭
STOP = {
  crates = 5, threatMax = 6, evacTime = 15, searchThreat = 1,
  keepRatio = 0.6, pickRadius = 30,
}
RAID = {
  spawnEvery = 1.1, aliveMax = 9, speed = 1.8, hpBase = 2,
  attackDist = 2.3, stealEvery = 0.9, hitRadius = 30,
  fireCd = 0.22, turretCd = 0.65, turretRange = 18,
}
-- 途中事件（权重）：尸群横穿 / 路边补给 / 燃油桶
EVENTS = {
  { id = "horde",   w = 4 },
  { id = "supply",  w = 3 },
  { id = "fuelcan", w = 2 },
}
-- worldmap.json 节点的世界坐标（营地=原点，地图 1.0 ≈ 160m）
NODES = {
  { id = "camp",    name = "营地",       x = 0,   z = 0 },
  { id = "gasstop", name = "废弃加油站", x = 0,  z = 60 },  -- TMP
  { id = "ruin",    name = "郊区废宅",   x = 38,  z = 27 },
  { id = "mall",    name = "购物中心",   x = 100, z = -73 },
  { id = "town",    name = "小镇主街",   x = 131, z = -8 },
  { id = "depot",   name = "物流仓库",   x = 154, z = -58 },
}
MAT_NAMES = { metal="金属片", electronics="电子件", cloth="布料", wood="木材" }
materials = { metal=12, electronics=8, cloth=6, wood=10 }

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
  { pre = "rv_scrub_a",    n = 34, kind = "scrub" },
  { pre = "rv_scrub_b",    n = 26, kind = "scrub" },
}
