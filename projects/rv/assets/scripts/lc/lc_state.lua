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
upgrades = { engine = 0, tires = 0 }
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

-- 搜刮 / 低强度威胁
stopBag, stopThreat, evacT = {}, 0, -1
stopItems = {}
crates = {}
horde, hordeT = {}, 0
fireT, turretT, killCount, raidT = 0, 0, 0, 0
ambientT = 0
fishingT, fishingTotal = 0, 0
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
