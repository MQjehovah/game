-- lc_state.lua
-- 运行时状态（全局：各模块共享读写）

selected, rot, curLayer = 1, 0, "floor"
occupancy, roofOcc = {}, {}
instances, nextId = {}, 1
hover = nil
ghostEnt, hoverDecalEnt, gridFloorEnt, gridRoofEnt = nil, nil, nil, nil
gridVisible = true
ghostValidLast = nil
toast = { text = "", t = 0 }
stats = {}
unreachableNames = {}

-- 阶段：驻车 / 行驶；onFoot = 主角在第一人称
phase, onFoot = "parked", false
buildMode = false             -- 改装模式（第一人称自由摆放）
placeRot = 0                  -- 当前家具旋转（弧度）
placeValid = false
placeLx, placeLz = 0, 0
driveView = "cab"             -- 行驶视角 "cab" | "chase"
atNode = nil                  -- 驻车点在节点旁 = 节点 id（搜刮层激活）
rv = { x = 0, z = 0, yaw = 0, speed = 0, groundY = 0, pitch = 0, roll = 0 }
curNode, day, clock = "camp", 1, 8 * 60
rvBody = nil                  -- RV 动态刚体（Jolt）
rvParts, rvShell, camEnt, groundEnt = nil, nil, nil, nil
rvWheels = nil               -- 四个独立车轮实体（分轮悬挂/转向）
rvDoor, rvDoorAngle = nil, 0
roadEnt, roadSegments, roadPhase, roadScroll, roadDashes = nil, {}, 0, 0, {}
camSmooth = nil
impactCd = 0
engineWarned, tireWarned = false, false
fuel, dur = 60, 100
parts = { engine = 100, tires = 100, battery = 100 }
upgrades = { engine = 0, tires = 0, cargo = 0, rod = 0 }
fuelWarned = false
dustAcc = 0
mapOpen = false

-- 第一人称 / 生存
needs = { health = 100, hunger = 85, thirst = 90, energy = 90, hygiene = 80 }
inv = {}
water = 80                    -- 净水箱当前水量（L）
nearAction = nil              -- 当前可交互物（物资箱/家具/房车）
stationUI = nil               -- 打开的家具菜单（灶台/工作台）
invOpen = false               -- 背包面板
useAnimT = 0
hurtT = 0                     -- 受伤红屏
lastWarn = {}                 -- 需求低警告去重
sleeping = false
sleepT = 0
poiDistAcc = 0                -- 程序化地点生成里程
generatedPois = {}

-- 顺路货运合同（营地接单 / 到点结算）
job = nil                     -- 当前合同 {destId, cargo, crates, weight, pay, fuel}
jobOffers = {}                -- 板上可接的合同
jobDelivered = 0              -- 已完成的货运单数
jobBoard = nil                -- 营地货运板世界坐标
jobCargoEnts = {}             -- 车顶货物可视化
jobUI = false                 -- 货运板菜单是否打开
jobResult = nil               -- 交付/失败结算卡（3.5s）
lastJobDay = 0                -- 最近一次完成货运的日期（每日首单加成）
jobHistory = {}               -- 最近完成的货运（最多 5 条，地图展示）
dailyGoal = nil               -- 每日目标 {id,name,target,progress,done,reward,day}
roadMarkers = {}              -- 公路轮廓桩（反光桩）
rvHeadlights = {}             -- 夜间前大灯（聚光灯，随车身旋转）
rvTaillights = {}             -- 刹车/倒车尾灯
rvReverselights = {}          -- 倒车白灯
campProps = {}                -- 营地陈设（遮阳棚/木桌/物资箱/营地灯）
roadWires = {}                -- 电线杆之间的电线
roadShoulders = {}            -- 主路两侧的土肩
roadDebris = {}               -- 主路两侧的废弃车/石块
roadDebrisColliders = 0       -- 已添加的静态碰撞盒数量
hayBales = {}                 -- 农场谷仓外的干草捆
poiSigns = {}                 -- POI 门头招牌
poiSignsLit = nil
campfireLight = nil           -- 营火暖光（只保留 light 组件）
campfireFlames = {}           -- 营火可见火苗
birds = {}                    -- 白天绕营地/房车飞行的鸟群
puddles = {}                  -- 雨天积水
campfireLightT = 0
visitedPois = {}              -- 已首次抵达的地点（探索奖励去重）
visitedCount = 0
interiorLightsOn = true       -- 车内灯开关（L）
highBeam = false             -- 远光灯（H）
flashlightOn = true           -- 手电筒开关（F）
flashlightEnt = nil           -- 手电筒聚光灯实体
tripDistance, tripFuel = 0, 0 -- 行程统计：米 / 升
batteryCharge = 0             -- 电池当前电量 kWh
bestFish, fishCaught = 0, 0   -- 钓鱼纪录：最大尺寸 cm / 累计条数

-- 搜刮 / 低强度威胁
stopBag, stopThreat, evacT = {}, 0, -1
stopItems = {}
crates = {}
horde, hordeT = {}, 0
fireT, turretT, killCount, raidT = 0, 0, 0, 0
ambientT = 0
fishingT, fishingTotal = 0, 0
fishingBait = false
rawFoodAge = 0                 -- 生食材累计腐败进度（游戏分钟）
rawSpoilWarned = false
ponds = {}
campfire = nil
weather, weatherT, weatherNext = "clear", 0, 15

-- 途中事件
nextEventT = 20
driveProps, driveHorde = {}, {}

-- 主角
player = { active = false, x = 0, z = 0, yaw = 0, pitch = 0, ent = nil, anim = "",
                 mag = 30, reloadT = 0, nadeCd = 0 }
grenades = {}
hitMarkT, recoilT = 0, 0

-- 验证钩子
autoPilot, driveT, autoT, autoTarget = false, 0, 0, nil
nearNode = nil
startMode = "fp"
