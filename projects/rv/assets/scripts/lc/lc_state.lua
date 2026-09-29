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

-- 阶段：驻车（建造+停靠层）/ 行驶；onFoot = 主角下车
phase, onFoot = "parked", false
driveView = "chase"          -- 行驶视角 "chase" | "cab"
atNode = nil                 -- 驻车点在节点旁 = 节点 id（搜刮层激活）
rv = { x = 0, z = 0, yaw = 0, speed = 0 }
curNode, day = "camp", 1
rvBody = nil                 -- RV 动态刚体（Jolt）
rvParts, camEnt, groundEnt = nil, nil, nil
camSmooth = nil
impactCd = 0
fuel, dur = 60, 100
fuelWarned = false
dustAcc = 0
mapOpen = false

-- 搜刮 / 夜袭
stopBag, stopThreat, evacT = {}, 0, -1
crates = {}
horde, hordeT = {}, 0
fireT, turretT, killCount, raidT = 0, 0, 0, 0

-- 途中事件
nextEventT = 20
driveProps, driveHorde = {}, {}

-- 主角
player = { active = false, x = 0, z = 0, yaw = 0, ent = nil, anim = "",
                 mag = 30, reloadT = 0, nadeCd = 0 }
grenades = {}
hitMarkT, recoilT = 0, 0

-- 验证钩子
autoPilot, driveT, autoT, autoTarget = false, 0, 0, nil
nearNode = nil
