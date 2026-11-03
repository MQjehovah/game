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
upgrades = { engine = 0, tires = 0, cargo = 0, rod = 0, water = 0, fuel = 0 }
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
jobStreak = 0                 -- 连续送达单数（超时/放弃清零）
jobBoard = nil                -- 营地货运板世界坐标
jobCargoEnts = {}             -- 车顶货物可视化
jobUI = false                 -- 货运板菜单是否打开
jobResult = nil               -- 交付/失败结算卡（3.5s）
morningReport = nil           -- 睡醒晨报（天数/货运/今日目标/油量）
relayDone = false             -- 是否已在废弃小镇恢复信号中继
relaySupplyDay = 0            -- 最近一次领取救援补给的日期
relayCard = nil               -- 信号恢复总结卡
lastJobDay = 0                -- 最近一次完成货运的日期（每日首单加成）
jobHistory = {}               -- 最近完成的货运（最多 5 条，地图展示）
milestones = {}               -- 已领取的长期里程碑（key = "explore:3"）
journal = {}                  -- 旅行日志（最近事件，最多 30 条）
journalOpen = false           -- 旅行日志面板是否打开
loreNotes = {}                -- 路牌笔记实体 {ent,x,z,id}
notesFound = {}               -- 已读笔记 id
loreRewardDone = false        -- 是否已领取集齐笔记奖励
fishBookOpen = false          -- 鱼类图鉴面板是否打开
helpOpen = false              -- 操作说明面板是否打开
dailyGoal = nil               -- 每日目标 {id,name,target,progress,done,reward,day}
roadMarkers = {}              -- 公路轮廓桩（反光桩）
strandedCar = nil             -- 路边抛锚车事件 {x,z,body,lamp,looted}
vendingMachine = nil          -- 加油站自动售货机 {x,z,ent}
rvHeadlights = {}             -- 夜间前大灯（聚光灯，随车身旋转）
rvTaillights = {}             -- 刹车/倒车尾灯
rvReverselights = {}          -- 倒车白灯
campProps = {}                -- 营地陈设（遮阳棚/木桌/物资箱/营地灯）
roadWires = {}                -- 电线杆之间的电线
roadShoulders = {}            -- 主路两侧的土肩
roadDebris = {}               -- 主路两侧的废弃车/石块
roadDebrisColliders = 0       -- 已添加的静态碰撞盒数量
hayBales = {}                 -- 农场谷仓外的干草捆
farmScarecrow = {}            -- 农场稻草人（低多边形）
farmCrops = {}                -- 农场成排作物
docks = {}                    -- 钓鱼水塘的木栈桥
radioMast = nil               -- 仓库旁的无线电塔 {parts, beacon, t}
radioMastFixed = false        -- 无线电塔是否已修好（长线目标）
waterTowers = {}              -- 小镇水塔地标
windmill = nil                -- 农场风车 {x,y,z,angle,blades}
windmillWaterMin = 0          -- 风车水泵冷却（游戏分钟）
windmillUpgraded = false      -- 风车水泵是否已升级（冷却 60→30 分钟）
poiSigns = {}                 -- POI 门头招牌
poiSignsLit = nil
campfireLight = nil           -- 营火暖光（只保留 light 组件）
campfireFlames = {}           -- 营火可见火苗
campfireOn = true             -- 篝火是否在燃烧
campfireFuelMin = 720         -- 篝火剩余燃料（游戏分钟，约 12 游戏小时）
campfireWoodMax = 2160        -- 篝火燃料上限（游戏分钟，约 36 小时）
birds = {}                    -- 白天绕营地/房车飞行的鸟群
rabbits = {}                  -- 白天在草甸上蹦跳的野兔
butterflies = {}              -- 白天在花丛/草甸飞舞的蝴蝶
wildlifeAnchor = { x = 0, z = 0 }  -- 野生动物群所在区域（不跟车移动）
ambientDeer = {}              -- 农场附近白天活动的鹿群
ambientDeerT = 0              -- 鹿群动画计时
pondDucks = {}                -- 水塘上白天游动的野鸭
pondDucksT = 0                -- 野鸭动画计时
puddles = {}                  -- 雨天积水
snowPatches = {}              -- 雪天地面雪斑
snowCenter = nil              -- 雪斑铺设中心（房车开远后重铺）
berryBushes = {}              -- 营地附近可采摘的浆果丛 {ent,x,z,ready,t}
roadCaches = {}               -- 主路旁的补给箱 {ent,x,z,ready,t}
campfireLightT = 0
cricketT = 0                   -- 夜间蟋蟀鸣叫计时
cricketPlays = 0               -- 已播放次数（自检用）
birdChirpT = 0                 -- 白天鸟鸣计时
birdChirpPlays = 0             -- 白天鸟鸣次数（自检用）
howlT = 0                      -- 夜间远处狼嚎计时
howlPlays = 0                  -- 狼嚎次数（自检用）
fireCrackleT = 0               -- 篝火噼啪声计时
campfireSmokeT = 0             -- 炊烟发射计时
campfireSmokeCount = 0         -- 已发射炊烟粒子数（自检用）
emberT = 0                     -- 火星发射计时
emberCount = 0                 -- 已发射火星数（自检用）
fireCracklePlays = 0           -- 已播放次数（自检用）
visitedPois = {}              -- 已首次抵达的地点（探索奖励去重）
visitedCount = 0
interiorLightsOn = true       -- 车内灯开关（L）
highBeam = false             -- 远光灯（H）
flashlightOn = true           -- 手电筒开关（F）
flashlightEnt = nil           -- 手电筒聚光灯实体
ambientTrack = nil            -- 当前环境音乐（wind/engine/rain/radio）
radioOn = false               -- 营地收音机开关
radioStation = 1              -- 当前电台（1/2）
carRadio = false              -- 驾驶时是否用车载电台代替引擎声
cruiseOn = false              -- 定速巡航是否开启
cruiseSpeed = 0               -- 定速巡航目标车速（m/s）
radioBroadcastT = 0           -- 收音机广播计时（游戏分钟）
radioBroadcastNext = 120      -- 下一次广播阈值（游戏分钟）
campRadio = nil               -- 营地收音机世界坐标
tripDistance, tripFuel = 0, 0 -- 行程统计：米 / 升
batteryCharge = 0             -- 电池当前电量 kWh
bestFish, fishCaught = 0, 0   -- 钓鱼纪录：最大尺寸 cm / 累计条数
bestFishName = nil            -- 最大纪录对应的鱼种
fishRecords = {}              -- 各鱼种最大尺寸 cm
fishCounts = {}               -- 各鱼种累计条数
fishCollectionDone = false    -- 是否已集齐全部鱼种并领奖
bigFishRewardDone = false     -- 是否已领取 60cm 大鱼奖励
exploreAllDone = false        -- 是否已跑遍所有固定地点并领奖
newGameHintT = 0              -- 首次进入的操作提示剩余时间（秒）
autoSaveFlashT = 0            -- 自动存档提示剩余时间（秒）

-- 搜刮 / 低强度威胁
stopBag, stopThreat, evacT = {}, 0, -1
stopItems = {}
crates = {}
horde, hordeT = {}, 0
fireT, turretT, killCount, raidT = 0, 0, 0, 0
ambientT = 0
fishingT, fishingTotal = 0, 0
fishingBait = false
fishingDock = false           -- 本次抛竿是否在栈桥上
fishingBite = false           -- 是否处于“咬钩”窗口
fishingBiteT = 0              -- 咬钩窗口剩余秒数
pondTrap = nil                -- 湖边鱼笼 {x,z,t,ready}
fishingSplashCount = 0        -- 水花特效次数（自检用）
fishingBobber = nil           -- 钓鱼浮漂实体
fishingBobberT = 0            -- 浮漂上下浮动计时
fishingBobberBase = nil       -- 浮漂落点 {x,y,z}
rawFoodAge = 0                 -- 生食材累计腐败进度（游戏分钟）
engineTemp = 20               -- 引擎水温（°C）
brakeHeat = 0                 -- 刹车热衰减累积（0–1）
chainsOn = true               -- 防滑链是否装上（步行按 C 取下/装上）
oilLife = 100                 -- 机油寿命（%，随里程下降）
rawSpoilWarned = false
nightDriveMin = 0             -- 连续夜间驾驶累计（游戏分钟）
warmthMin = 0                 -- 热食保暖剩余（游戏分钟，抵消雪天寒冷）
restedMin = 0                 -- 睡饱状态剩余（游戏分钟，减缓夜间疲劳）
ponds = {}
campfire = nil
weather, weatherT, weatherNext = "clear", 0, 15
rainbowT = 0                  -- 雨停后彩虹剩余时间（秒）

-- 途中事件
nextEventT = 20
driveProps, driveHorde = {}, {}
driveDeer = {}                -- 横穿公路的野生动物（鹿/野兔放大）

-- 主角
player = { active = false, x = 0, z = 0, yaw = 0, pitch = 0, ent = nil, anim = "",
                 mag = 30, reloadT = 0, nadeCd = 0 }
grenades = {}
hitMarkT, recoilT = 0, 0

-- 验证钩子
autoPilot, driveT, autoT, autoTarget = false, 0, 0, nil
nearNode = nil
startMode = "fp"
