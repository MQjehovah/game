# 房车改造：从 Last Caravan 到 Nomad Caravan

日期：2026-11-03  
工程：`projects/rv`

## 目标

把原来的「俯视网格建造 + 丧尸夜袭」改造成 Nomad Drive 式体验：

- 第一人称，鼠标视角，人在车里走动。
- 生存需求：生命 / 饥饿 / 口渴 / 体力 / 卫生。
- 可交互家具：灶台、冰箱、水槽、咖啡机、床、卫生间、工作台、种植箱。
- 日常：做饭、煮咖啡、喝水、睡觉、种菜、修车。
- 程序化公路世界：跟随房车的公路 + 每约 130m 生成新地点。
- 保留**低强度威胁**：零散感染者，不强制撤离、不成波刷新。

网格建造没有删除，而是收进 `B` 改装模式，作为房车定制层。

## 模式与输入

| 模式 | 相机 | 主要输入 |
|---|---|---|
| 第一人称驻车 | 眼睛高度 + 鼠标视角 | WASD / E / 左键 / 1-8 |
| 驾驶 | 驾驶席或追逐 | W/S/A/D / 空格 / C |
| 改装 | 俯视 | 左键放置 / 右键拆除 / R / Tab / G |

引擎侧的 FPS 相机通过 `cameraMouseLock` + `cameraFocus/cameraYaw/cameraPitch/cameraDist` GameVar 接管，脚本每帧写入（与 `projects/fps` 同一套约定）。

## 新增/改写的脚本

| 文件 | 职责 |
|---|---|
| `lc_survival.lua` | 时间/昼夜、需求衰减、物品、做饭/咖啡、睡觉、种植、受伤/昏倒 |
| `lc_interact.lua` | 第一人称交互检测（物资箱/家具/房车）与 E 分发 |
| `lc_camera.lua` | 第一人称 / 驾驶席 / 追逐 / 改装俯视 |
| `lc_player.lua` | 第一人称移动、鼠标视角、动画、隐藏第一人称身体 |
| `lc_hud.lua` | 生存面板、交互提示、驾驶仪表、改装热栏、地图 |
| `lc_raid.lua` | 低强度感染者：偶发、最多 3 只、贴身攻击、左键射击 |
| `lc_scenery.lua` | 程序化地点生成 + 节点标记 |
| `lc_vehicle.lua` | 车内衬板/车窗、驾驶席隐藏车头几何、公路贴花滚动 |
| `lc_selftest.lua` | 无头系统自检 |
| `garage.lua` | 三模式状态机、输入编排、存读档 |

## 关键实现

### 第一人称

- 引擎的 FPS 相机路径：`cameraFocus = 眼睛 + 视线 * 2`，`cameraDist = 2`，`cameraYaw = yaw + π`（引擎前向为 -Z 约定）。
- 第一人称隐藏 Soldier 身体，避免头部网格罩住镜头；射击/交互仍按位置计算。
- 鼠标捕获：`SetVar("cameraMouseLock", 1)`，改装模式置 0 让光标回来。

### 房车内部

- 原场景的墙体/车头是单面 cube，从车内看到背面会变成黑色。
- 补了一层朝内的浅色墙板、后墙、仪表台、A 柱、地毯；外侧补了车窗/侧门/后窗贴片。
- 驾驶席视角隐藏 `RV_Cab` / `RV_Windshield`，由仪表台 + A 柱取景框代替，避免车头盒体挡死视野。

### 生存

- `SURVIVAL.timeScale = 3`：1 现实秒 = 3 游戏分钟。
- 需求按游戏分钟衰减；饿/渴归零扣血；饱食/水分充足缓慢回血。
- 睡觉直接推进到第二天 07:00。
- 改装模式暂停时间与需求，避免在车库里饿死。

### 物品与战利品

- `ITEMS`：瓶装水、罐头、生食材、热饭、咖啡豆、热咖啡、种子、修理包。
- 物资箱同时 Roll 材料与物品；物品即时进背包，材料进搜获袋，离开停靠点时入库。
- 物品可在对应家具使用，也可用 1-8 快速使用（种子/咖啡豆/修理包会提示去对应家具）。

### 程序化地点

- 初始 6 个固定地点（营地/加油站/废宅/商场/小镇/仓库）。
- 行驶里程累计每 130m 调用 `trySpawnPoi()`：前方 110–180m、随机横向偏移生成一个新地点，带危险度与战利品表。
- 地点写入存档 `pois`，读档时补节点/标记；地图边界动态计算，不裁剪新地点。

### 低强度威胁

- 搜箱时按「危险度 + 是否夜晚」小概率刷 1 只，场上上限 3。
- 不再有 `evac` 倒计时、波次、抢物资；感染者只贴身攻击玩家。
- 保留射击、武器架增伤、炮塔自动防御。
- `stop_cycle` 流程图仍负责生成物资箱/入库/推进时间，但不再触发夜袭波次。

### 公路

- 早期用贴花/跟随路面的方案有投影、剔除、z-fighting 问题，已废弃。
- 现在是 `spawnRoadNetwork()` 一次性生成的**世界固定公路网**：深灰 10m 分段
  （`rv_road_seg`，y=0.08，polygonOffset -2）+ 黄色虚线（`rv_road_dash`，y=0.12）。
- 营地南侧有主路，worldmap 的每条 link 铺一条连接路；POI 在**开局一次性生成**
  （以现有地点为锚点，不用车头方向），并自动连到最近地点。行驶中不再新增/改向公路。
- 房车转向/离开公路时，路面保持世界固定；树木/岩石生成时避开公路 6m。
- 主路两侧生成电线杆 + 横担（`rv_powerpole`），强化公路感。

### 草甸画风与地形

- `tools/gen_rv_ground.py` 生成可平铺的 `rv_ground_meadow.png`（草绿基底 +
  深浅草丛条纹 + 泥土斑块 + 小花点）和 `rv_dirt.png`；场景 `Ground` 使用
  `albedoTex` 与 `uvRepeat=160`，世界从沙地变为饱和的草地。
- `tools/gen_rv_terrain.py` 生成营地级高度场（保留主路右侧 5–38m、
  z -24–46m 的土坡：靠路侧为泥土斜坡、顶部为草地，高度沿 z 起伏、两端收口，
  并在营地西/南侧加草丘），对应参考图的土坡/路肩与起伏草甸。
- `tools/gen_rv_vegetation.py` 生成低多边形植被：`rv_meadow_a/b/c`
  （6×6m 拼块，每块 18 簇草 + 2 丛花）、`rv_grass_tuft` / `rv_grass_tuft_b` /
  `rv_fern`（多片弯曲草叶）、`rv_flower_white/blue/yellow/red`（三朵一簇）、
  `rv_bush`（多球灌木）。
- `lc_config.MEADOW` 用 60 个拼块 + 约 130 株近景草花 + 34 丛灌木铺满营地与
  公路两侧，按 RV 位置回收复用；生成时避开路面、水塘、POI 占地、土坡与 RV 车体。
- `updateMeadowWind()` 以 20Hz、分 4 批让草/花绕根部轻摆（草叶统一倾斜方向），
  土坡顶部另种 90 株草花灌木。实测车顶追逐视角约 **91 FPS**。
- `tools/gen_rv_terrain.py` 生成营地级高度场：144×144m、4m 网格，同时写出
  `assets/data/rv_terrain.json`（供 Lua 采样）与 `rv_terrain.gltf`（可见网格），
  两者同源。高度场保留路旁土坡，并在营地西/南侧加起伏草丘；营地、水塘、
  固定公路走廊和 POI 占地都被压平，超出范围保持平地。
- `lc_terrain.lua` 对网格做双线性采样，提供
  `terrainHeight(x,z)` / `terrainFollow()` / `terrainSlope(x,z)`；步行时角色眼高、
  模型与枪械、驾驶时 `rv.groundY` + 外壳/内饰/家具/相机全部跟随地形高度。
  公路段按高度抬升并按坡度倾斜（5m 分段、10° 限幅），新 POI 会避开坡面。
  悬挂、轮胎抓地与转向由 Jolt 多体物理求解：单机为四个车轮创建独立刚体，
  用软限位滑轨关节（`PhysicsAddSliderJoint(..., 8Hz, 1.0)`）连接到车体，
  每个车轮用 `terrainHeight` 采样并施加弹簧/阻尼冲量
  （k=90000 N/m、c=11000 N·s/m、力限幅 12kN），车体通过四个滑轨被支撑。
  轮胎侧向冲量（`tireGripImpulse`）与滑移率牵引（`slipTraction`）直接作用于
  车轮刚体；`surfaceGrip()` 给出沥青 0.95 / 土坡 0.72 / 草地 0.60；
  纵向在后轮牵引、四轮制动。A/D 只给前轮转角（±0.5rad），
  `ackermannAngles()` 让内侧前轮（0.55rad）比外侧（0.45rad）转得更多；
  偏航由轮胎横向力矩积分得到（5% 角速度阻尼），每帧用
  `rvYawPitchRollFromQuat()` 从车体四元数读回 yaw/pitch/roll；
  因此高度、加速、刹车、转向、侧滑都来自求解器。
  引擎新增 `PhysicsApplyImpulse` / `PhysicsGetPointVelocity` /
  `PhysicsGetAngularVelocity` / `PhysicsAddAngularImpulse` /
  `PhysicsAddSliderJoint` / `PhysicsActivate`。
  合作服务器/无物理后端时回退到 `updateRvSuspension()` 的四点平面拟合与脚本偏航。
  视觉车轮直接读取各自车轮刚体的位置，滚动角按每轮接触速度累积
  （`rv.wheelSpinI`），前轮按 Ackermann 左右夹角偏转。
  `applyRvTransform()` 用三维旋转摆放每个部件并通过引擎新增的
  `SetRotationEuler(entity,pitch,roll,yaw)` 设置朝向，物理刚体也用同一四元数；
  车内第一人称的站立高度由 `rvFloorOffset()` 取倾斜地板高度。
  合作 `game_state` 广播每位玩家的 `gy` 与房车 `rv.gy`，两端各自计算相同的倾斜。

## 验证

`selftest_on.txt` 自检覆盖：

- 初始家具齐全
- 灶台做饭（消耗水、产出热饭）
- 水槽喝水
- 咖啡机煮 + 喝
- 床上睡觉（第二天 07:00）
- 种植播种 + 收获
- 工作台修车
- 低强度感染者生成/击杀
- 交互检测（站到灶台前识别“做饭”）
- 程序化地点生成
- 昼夜
- 存档写入

另外 `autopilot_on.txt` 可跑「上路 → 靠站 → 搜箱 → 收工」全循环。

## 第二轮补强（同日）

- **自由摆放**：`B` 改为第一人称摆放；准星拾取地面/车顶，家具可 15° 旋转、无网格吸附，
  AABB 碰撞 + 车内边界校验；存档同时支持网格旧档与自由摆放新档。
- **路面修复**：旧实现用深度投影贴花，会把路面画到房车/家具上；中间版改用贴图
  立方体后又出现斜视角发白。最终改为 20 段 10m 深灰立方体（`rv_road_seg`，抬高到
  y=0.08 + polygonOffset -2）+ 独立黄色虚线（y=0.12，按里程滚动）。
  分段解决单条 200m 长条被整体视锥剔除的问题，抬高 + 偏移解决远距离 z-fighting 发白。
- **钓鱼**：营地水塘 + 湖边地点；`E` 抛竿 5–9 秒，等待期间移动会收线；产出**生鱼**，
  回营地篝火按 `E` 烤成烤鱼；鱼汤配方消耗烤鱼。
- **灶台菜单**：`E` 打开配方面板，`1` 热饭 / `2` 鱼汤 / `3` 大餐，按当前食材/水/电力标红；
  新增 `fishsoup`、`feast` 两种食物，冰箱会优先吃更好的食物。
- **背包面板**：`I` 打开，列出物品数量/效果与材料库存，数字键快速使用。
- **天气**：晴/阴/雨/雾随游戏时间变化；雨雾有屏幕叠层、3D 雨丝和驾驶极速惩罚。
  太阳能按天气打折（雨 25% / 雾 50% / 阴 70%），雨天水箱缓慢接雨水。
- **真实昼夜光照**：每分钟改一次场景 `Sun` 的 `light.intensity/color/ambientStrength`
  （黄昏暖色 → 夜晚 0.13 强度），屏幕暗角只作为辅助。
- **地点搜刮**：非营地停靠时物资箱刷在废弃小屋周围 1.4–3.2m，玩家需要下车进屋搜刮；
  营地仍刷在房车周围。自检新增 `crates at POI ruin` 与 `day/night sun light`。
- **车辆零件**：引擎/轮胎/电瓶按里程磨损；引擎影响极速/加速，轮胎影响转向；
  引擎 <10% 只能龟速、轮胎 <10% 转向严重迟钝，并弹警告；
  工作台改为菜单式维修（1 车体 / 2 引擎 / 3 轮胎 / 4 电瓶），显示百分比与材料成本，
  有修理包时优先消耗；引擎/轮胎修满后可继续升级 Lv1–3（每级 +4%），升级存档。
- **车舱表现**：内墙板、车顶板、驾驶室顶、仪表台/A 柱、两盏暖色点光源；
  驾驶席隐藏车头盒体，视野从驾驶席向前。
- **音效**：新增程序合成的脚步/点击/做饭/喝水/咖啡/修理/拾取/钓鱼/受伤/枪声；
  驻车播放 wind 循环，上路播放 engine 循环（PlayMusic 单声道替换）。
- **路边建筑**：每个地点生成一个带门洞/窗的简易废弃小屋（立方体拼装原型）；
  加油站额外有雨棚+油泵，仓库是大厂房，农场是红顶谷仓，商场是更大的废弃商铺。
- **UI 居中**：引擎 `DrawText` 的 CJK Measure 在非 1280 宽窗口会偏左；HUD 改为
  脚本内 UTF-8 宽度估算 + 左对齐绘制，统一居中/右对齐。
- **车内破面**：内墙板从 ±0.99 内缩到 ±0.94，车窗外移到 ±1.335 并改哑光深色，
  删除与长车窗重叠的侧门板；修复墙板 z-fighting 和不同视角反光变色。

## 网络合作（主世界 2–4 人）

`garage.json` + `assets/scripts/lc/lc_coop.lua` 已把服务器权威合作并入主世界：
同一辆房车、同一组需求/家具/搜刮物资，最多 4 个玩家槽位。
`coop_test.json` 仍作为更轻量的 2 人协议调试场景保留。

- **服务器权威状态**：房车 `x/z/yaw/speed/fuel`、4 名玩家位姿、五项需求、水箱/存粮、
  材料与共享背包、家具布局/开关、车内灯、游戏时间、物资箱列表。
- **启动角色**：引擎在 `on_start` 之前把 `netRole` 写入 GameVar（server/client/spectate）；
  `garage.lua` 据此走合作初始化，跳过单机存档、第一人称出生和单机 HUD。
- **上行指令**：客户端 `Rpc("game_cmd", {...})`；服务器 `game_cmd` 处理器把
  `clientId + args` 放进 runtime 的 NetCommand 队列，Lua 用 `NetCommand()` 按客户端
  路由到槽位（第 1–4 个加入 → 槽位 1–4）。
- **下行状态**：`game_state` 10Hz 广播位姿/需求/背包/家具开关；`game_world` 在家具
  变化时按 4 条/块分块发送完整布局（避免超过 RPC 字符串上限）；`game_crates` 发送
  物资箱位置。三者都进入客户端 NetCommand 队列。
- **合作建造/拆除**：客户端 `B` 进入网格建造，`1-9` 选家具、`Tab` 地板/车顶、
  `J/L/I/K` 移动光标、`R` 旋转、`E` 放置、`X` 拆除；只发
  `game_cmd {type="place"/"remove", cx, cz, rot, layer}`。服务器用 `fits` / `canAfford`
  校验边界、重叠、材料和层，再调用 `place` / `removeAt` 更新 `instances` 与共享材料，
  成功后广播分块 `game_world`。`coop_build.txt` 自动验证：
  服务器 `furn=9→11→10`、`metal=12→10`，两端 `furniture rebuilt (10/11/10)`。
- **进出房车**：靠近车门按 `E` 发 `game_cmd {type="board"}`（乘客席），车内按 `E`
  发 `exit`，`F` 直接坐驾驶席。服务器记录 `inside/seat`，广播 `in1..in4/door`；
  客户端车内切第一人称、隐藏自身角色、隐藏外壳，车外恢复第三人称；车门是独立的
  `rv_door.gltf` 网格（后缘铰链），按 `doorAngle` 真实旋转。车体右侧已切出真实门洞
  （`z=-3.54..-2.76, y=0.25..1.86`），右侧内墙板同步分成上/后/前三块；打开车门
  即可透过开口看到内饰和坐在门口的乘客。角色播放**手工关键帧**上车链：
  `DoorOpen`（站姿→够门把手→拉开，5 个关键帧，0.9s）→ `Step`（抬脚踩踏板，
  4 个关键帧，0.7s）→ `Board`（站姿→抬步→落座，5 个关键帧，0.9s）→
  `Sit` 循环（坐姿 + 1s 呼吸关键帧）；下车链：
  `Stand`（坐姿→前倾→起身→迈步，5 个关键帧，0.9s）→ `DoorClose`（够把手→推合，
  5 个关键帧，0.9s）→ `Idle`。搜刮物资箱时服务器广播 `carry`，远端角色播放
  `Carry`（抱箱循环，1.2s）后回到 `Idle`。门板与 `door` 状态同步摆动。
  `coop_board.txt` 验证 5s 上车、9s 下车，两端同一 `seq` 都是 `in1=1 door=1`
  再回到 `in1=0 door=0`；日志 `player 1 reaches for the door handle (DoorOpen ok=true)` /
  `player 1 steps up into the RV (Step ok=true)` / `player 1 boards (Board -> Sit)` /
  `player 1 sits down (Sit loop)` / `player 1 exits (Stand -> DoorClose -> Idle)` /
  `player 1 pushes the door shut (DoorClose ok=true)` /
  `player 1 stands up (Idle loop)`；搜刮日志
  `player 1 carries a crate (Carry loop)` / `player 1 finishes carrying (Idle loop)`。
  `coop_sitview.txt` 从驾驶室拍坐姿，`coop_animdemo.txt` 可预览
  `dooropen` / `doorclose` / `step` / `carry`，`coop_carryview.txt` 跟随搜刮角色。
- **房车车体/内饰**：`tools/gen_rv_body.py` 生成倒角车身、内凹车窗 + 窗框、
  20 段轮拱（双层轮唇）/轮拱内衬、前后保险杠/防擦条、行李架/空调/爬梯/遮阳篷、
  后视镜/灯组/格栅，以及多段式仪表台、方向盘、驾驶席与两侧吊柜；
  门洞/座椅坐标/物理碰撞保持不变。
- **倒角几何**：`tools/rv_geom.bevel_box()` 把每个盒体拆成
  6 个内缩面 + 12 条 45° 棱边条 + 8 个角三角（小件/薄片自动回退为普通盒），
  `gen_rv_body.py` 与 `gen_rv_furniture.py` 的 `box()` 默认使用它，
  因此车身、车窗框、保险杠、家具台面/柜体/工具箱等轮廓都有可见倒角。
- **可摆放家具**：全部 20 类（单人/双人床、灶台、冰箱、水槽、工作台、储物箱、电视柜、
  卫浴间、药柜、武器架、暖器、净水箱、电池组、发电机、炮塔、太阳能板、通风扇、
  咖啡机、种植箱）均由 `tools/gen_rv_furniture.py` 生成多部件 glTF
  （木框/床垫/被子/枕头、烤箱门/玻璃/旋钮/灶头、冰箱双门/把手/格栅、水槽柜/台盆/
  龙头、工作台/挂板工具/台钳、木箱板条/包角/提手/标签、电视/屏幕/天线、卫浴底盆/
  马桶/洗手盆/镜子/花洒、玻璃门药柜/药盒/红十字、双步枪武器架/弹药箱、暖器格栅/
  散热片/旋钮、阶梯水箱/加注口/绑带/液位窗/阀门、电池双壳体/极柱/压条/线缆、
  发电机框架/红色油箱/控制面板/排气管/格栅、炮塔底板/立柱/双枪管/护盾/弹药箱、
  太阳能边框/蓝色电池片/栅线/支架、通风扇法兰/防护网/顶盖/摇柄、咖啡机水箱/
  冲煮头/滴水盘/杯子/蒸汽棒、种植箱土壤/幼苗/果实/支架）；
  预制体名、网格槽位与 `use` 钩子不变，`start_mode=furnview` / `furnall` /
  `utilall` / `utilclose` / `utilroof` 提供车内/车顶检查机位。
- **表面细节贴图**：`tools/gen_rv_surfaces.py` 为 12 种表面各生成 3 张 256×256 贴图
  （`rv_tex_<name>.png` 底色细节、`_n.png` 由高度场 Sobel 得到的切线空间法线、
  `_orm.png` 的 R=AO / G=粗糙度 / B=金属度），另有两张自发光图
  `rv_tex_lamp.png` / `rv_tex_tail.png`；`tools/rv_surfaces.py` 提供 `face_uvs()`
  （按面法线选投影轴、世界尺度平铺）与 `surface_extras()`（38 张 images/textures）；
  `rv_body.gltf` 与 20 个 `rv_furn_*.gltf` 的每个 primitive 都带 `TEXCOORD_0`，
  材质在保留 `baseColorFactor` 的同时追加 `baseColorTexture` / `normalTexture` /
  `metallicRoughnessTexture` / `occlusionTexture` / `emissiveTexture`，
  因此木纹/拉丝金属/织物编织/车身瓦楞既有底色也有凹凸与高光。
- **位姿快照**：两侧按相同顺序 `Spawn("player")` … `Spawn("player4")`；4 个代理都带
  `CTransformBind`。客户端 glTF 士兵可视实体每帧跟随代理，房车仍使用主世界 glTF 模型。
- **每客户端实体所有权**：`EntityKey(entity)` + `netControlKey` GameVar；服务器把每个
  代理的 key 放进 `game_state`，客户端认领自己的 key，4 名玩家都能本地预测 + 服务器纠正。
- **同场景复制优化**：合作脚本设置 `netReplicateStatic=0`。主世界有数百个静态实体，
  客户端本来已加载同一场景；不逐个走可靠通道 Spawn，避免 Welcome/状态包被挤出窗口。
- **加入时序**：`on_player_join` 在 `MsgWelcome` 之后触发；服务器在 LoginOk 后提前
  Welcome 并完成槽位分配，客户端的 Join 包延迟时也不会卡在“已登录未入局”。
- **持久化**：服务器每 3s + `Shutdown` 时调用 `saveLayout()` 写 `saves/layout.json`；
  标准字段保存房车/需求/背包/家具，`coop` 扩展保存物资箱、车内灯、槽位名字与最后位置。
  启动时 `coopInitWorld` 读取存档并恢复，日志 `[coop-main] loaded save ...`。
- **断线重连**：`on_player_leave(clientId, name)` 只标记槽位离线并保留名字/位置；
  同名客户端重连时 `on_player_join` 回收原槽位/`EntityKey`，并重发
  `game_state`/`game_world`/`game_crates`。日志示例：
  `player_join client=2 slot=1 ... (reconnect, old client=1)`。
- **共享搜刮/家具**：服务器 `searchCrate()` + `bankStopBag()` 后广播 `crates/materials`；
  两端 HUD/日志显示相同剩余物资箱与材料。`coop_actions.txt` 可自动搜箱并切换灶台，
  `game_world` 重新下发后客户端日志出现 `furniture synced (9)` / `crates synced (5)`。
- **验证**：双方日志按服务器 `seq` 每 2s 输出同一行；同一 `seq` 的房车坐标、4 名玩家
  坐标、燃油/水/材料/物资箱/家具开关/饥饿完全一致。已用 2、3、4 个客户端分别验证
  （4 客户端时服务器日志 `joined=4/4`，四个客户端都 `smoke assertions passed`）。
  持久化/重连另行验证：保存后重启服务器能恢复 `rv/crates/stove/fuel`，同名客户端
  重连回收 `slot=1` 并收到 `crates synced (5)`。

## 可进入的废宅商店、加油站、仓库、农场、购物中心、湖边木屋与诊所

- `assets/models/rv/rv_store.gltf` + `rv_store.bin`（`tools/gen_rv_store.py`）：
  实心墙体、1.6m 门洞、门框/雨棚/红招牌、两扇透明可破碎橱窗（窗内展示商品）；店内
  四层商品墙（成排彩色货盒 + 价签）、中央双层货架、玻璃门冷柜、果蔬木箱、
  带屏幕/键盘/小票的收银机、挂式价牌、壁钟、购物篮堆、地毯和物资箱；
  `ruin` 和所有“废宅/商店”地点生成该 glTF。
- `assets/models/rv/rv_gasstation.gltf` + `rv_gasstation.bin`（`tools/gen_rv_gasstation.py`）：
  实心墙体、1.6m 门洞、两扇透明可破碎橱窗；店内四层商品墙（成排彩色货盒 + 价签）、
  中央双层零食货架、玻璃门冷柜、咖啡机 + 杯堆 + 调料架、带屏幕/键盘/小票的收银机、
  挂式价牌、壁钟、购物篮堆；四柱雨棚 + 两台加油机；`gasstop` 和所有“加油站”地点
  生成该 glTF。
- `assets/models/rv/rv_window_glass.gltf` / `rv_glass_frag1..12.gltf`
  （`tools/gen_rv_windows.py` / `tools/gen_rv_glass_frags.py`）：半透明 BLEND 玻璃面板 +
  12 种不规则碎片网格（随机点云凸包，三角/四边/五边/六边形薄棱柱，带轻微倾角）；
  `lc_scenery.spawnPoiWindows()` 按 POI 类型（商店/加油站/仓库/商场/湖边木屋/诊所）
  生成对应尺寸的两扇前窗面板；`breakPoiWindowsAt()` 在玩家进入或攻击时做**应力驱动的
  径向裂纹扩展**：按 seed 选冲击点，从该点生长 4–6 条逐环游走的径向裂纹并在内环
  按 seed 分裂出分支，面板被切成 4 个同心环带内的楔形碎片；越靠近冲击点的环带越密，
  `stress = 1 / (1 + 3.2·dist)` 同时驱动碎片初速与自旋。再从 12 种网格池中取网格、
  按单元包围盒缩放并沿径向旋转；`updatePoiWindows()` 施加向外 + 重力速度、落地弹跳
  并让碎片永久留在地面；服务器把 `glass=<id>:<seed>` 写入 `game_state`，客户端用
  同一 seed 重建完全相同的裂纹网络（不做本地预测）。
- `assets/models/rv/rv_products.png`（`tools/gen_rv_product_atlas.py`）：4×4 商品包装
  图集（品牌条/标签文字/色带/条码）；`tools/rv_atlas.py` 提供 UV 映射，商店/加油站/
  商场/仓库/农场/诊所/湖边木屋的商品盒/托盘/货箱/药盒/补给箱使用 `*_product` 材质 +
  图集 UV，商品显示为包装而不是纯色块；`coop_propview.txt` 提供道具特写镜头。
- `assets/models/rv/rv_warehouse.gltf` + `rv_warehouse.bin`（`tools/gen_rv_warehouse.py`）：
  3.6m 高棚、宽装卸门 + 月台（黄色警示线/防撞柱/升降台板）、两扇可破碎前橱窗、
  顶部高窗、两侧与后墙
  钢制货架、黄底白字货架标识、带包装图集的标签托盘堆（胶带封箱）、叉车、发货台（显示器/标签
  打印机/键盘/凳子）、散装货箱、地面通道线；`depot` 和所有“仓库/物流”地点生成该 glTF。
- `assets/models/rv/rv_rollup_door.gltf` + `rv_rollup_door.bin`（`tools/gen_rv_rollup_door.py`）：
  11 条钢/深色板条 + 两侧导轨 + 底梁 + 顶部卷轴（2.6m 高）；`lc_scenery.spawnPoiDoor()`
  在物流仓库装卸门处生成，`updatePoiDoors()` 用 `scaleY = 1 - 0.94*cur` 绕顶边卷起；
  合作模式由服务器按玩家到月台的距离计算 `rollup`，随 `game_state` 广播，双端同步。
  `poiDoorBlocking()` 在 `cur < 0.9` 时让 `collidePoiWalls()` 把门洞当作实体，关闭时
  挡住玩家（服务器与客户端预测一致）。
- `assets/models/rv/rv_barn.gltf` + `rv_barn.bin`（`tools/gen_rv_barn.py`）：
  红谷仓 + 山墙屋顶、干草阁楼与顶部滑轨、白灰内衬、畜栏（干草槽/饮水桶/栏号牌）、
  饲料槽、工作台（工具箱/台钳/挂板工具）、马具墙（挂钩/缰绳/马蹄铁）、马鞍架、
  奶桶、粮袋、带标签木箱、后墙货架（摆带包装图集的木箱），门外干草堆/水槽/木栅栏；
  `farm` 和所有“农场”地点生成该 glTF。
- `assets/models/rv/rv_sliding_door.gltf` + `rv_sliding_door.bin`
  （`tools/gen_rv_sliding_door.py`）：5 块红木板 + 白横梁/斜撑 + 顶滑轮 + 把手/门牌
  （1.86×2.42m）；`lc_scenery.spawnPoiSlidingDoor()` 在谷仓门洞生成，
  `updatePoiSlidingDoors()` 沿 X 滑动 2.05m 停到右前墙上方；合作模式由服务器按
  玩家到谷仓门口的距离计算 `barndoor`，随 `game_state` 广播，双端同步；未完全滑开时
  同样由 `poiDoorBlocking()` 参与碰撞。
- `assets/models/rv/rv_mall.gltf` + `rv_mall.bin`（`tools/gen_rv_mall.py`）：
  4m 高可破碎玻璃店面、红蓝 `MALL` 招牌；店内收银台（收银机屏幕/键盘/小票 + 刷卡器 +
  购物袋）、四层商品墙（成排彩色商品 + 价签条）、折叠衣物展示台、配件展示台、
  左墙挂衣架 + 圆形挂衣架、穿色衣的橱窗模特、商场导视牌、悬挂 SALE 牌、入口指示牌；
  屋顶空调、门外花箱/长椅/购物车；`mall` 和所有“商场”地点生成该 glTF。
- `assets/models/rv/rv_cabin.gltf` + `rv_cabin.bin`（`tools/gen_rv_cabin.py`）：
  原木墙、山墙屋顶、带柱门廊、两扇可破碎窗、壁炉/烟囱、床/蓝被、餐桌椅、厨房柜台、
  货架（摆带包装图集的商品）、叠放补给箱、地毯、柴堆/木桶/鱼箱（带包装）/灯柱；
  `lakecamp` 和所有“营地”地点生成该 glTF，旁边生成水塘。
- `assets/models/rv/rv_clinic.gltf` + `rv_clinic.bin`（`tools/gen_rv_clinic.py`）：
  白/青色外墙、平屋顶、玻璃双开门 + 红十字、两扇可破碎橱窗；店内接待台（蓝图显示器/
  键盘/电话/单据）、候诊椅、诊床（床单/枕头/纸卷）、三层玻璃门药柜（带包装图集的药盒 +
  白色标签）、检查壁灯、生命体征监护仪、输液架、器械托盘、隐私窗帘、`RECEPTION`/
  `EXAM` 标识、壁钟与药柜补光，门外长椅/花箱/轮椅推车；`clinic` 和所有“诊所/医院”地点
  生成该 glTF。
- `lc_scenery.spawnRuinFor()` 对 `ruin`/`gasstop`/`depot`/`farm`/`mall`/`lakecamp`/
  `clinic` 和对应名称的地点直接生成对应 glTF，不再使用缩放立方体拼装。
- `collidePoiWalls()`（`lc_util.lua`）在**服务器和客户端预测**同时生效：侧/后墙
  与门洞两侧的前墙会挡住玩家，只有门洞可穿过。
- 各处店内的 6 个物资箱固定落在货架/柜台/托盘/畜栏/药柜旁；`E` 走近
  即可搜刮（共享搜刮状态和其他合作物资一致）。
- 验证钩子：`coop_ruin.txt` / `coop_gas.txt` / `coop_depot.txt` / `coop_farm.txt` /
  `coop_mall.txt` / `coop_lake.txt` / `coop_clinic.txt` 把房车与两名玩家放到对应
  店门口，3–5s 进门、5–8s 店内、8–10s 出门；`coop_poiview.txt` 店内镜头，
  `coop_poiext.txt` 店外全景；`start_mode.txt=poi/gas/warehouse/farm/mall/lake/clinic`
  提供单机出生点。
- 双端验证日志：`entered ruin/gasstop/depot/farm/mall/lakecamp/clinic`、`shop=2` →
  `exited`、`shop=0`；远端客户端同一 `seq` 的 `shop` 和玩家坐标一致。
  截图：`rv_store_*` / `rv_gas_*` / `rv_wh_*` / `rv_final_farm_*` / `rv_mall_*` /
  `rv_lake_*` / `rv_clinic_*`（店内）、`rv_final_gas_exterior.png`、
  `rv_wh_exterior.png`、`rv_final_farm_exterior.png`、`rv_mall_exterior.png`、
  `rv_lake_exterior.png`、`rv_clinic_exterior.png`。

## 房车 glTF 模型

- `assets/models/rv/rv_body.gltf` + `rv_body.bin`：脚本生成的连续车身、驾驶室、
  斜挡风玻璃、两扇侧窗、后窗、车门、前后保险杠、侧裙、灯组、车顶空调、四个圆柱轮胎 + 轮毂。
- 旧场景盒体（墙/后墙/驾驶室/挡风玻璃）在 `collectRvParts()` 中隐藏，只保留地板。
- 驾驶席视角隐藏整个外壳；驻车/追逐视角显示完整模型。
- 车内仍用独立内饰板、仪表台、A 柱、车顶板、暖色点光源。

## 与 Nomad Drive 的差距

1. **多人合作**：已并入 `garage.json` 主世界，支持 2–4 人、共享房车/需求/家具/物资箱，
   权威状态会写入 `saves/layout.json` 并支持同名断线重连；真实门洞 + 独立车门 +
   手工关键帧 `DoorOpen`/`Step`/`Board`/`Stand`/`DoorClose`/`Carry` + `Sit` 循环已具备，
   但还没有存档版本迁移/冲突合并，槽位身份基于 `--name`（无账号鉴权），
   合作建造仍是网格模式（还不是单机的无网格自由摆放），也还没有每名玩家的独立物品所有权。
2. **家具自由摆放**：已改为第一人称无网格自由放置 + AABB/车内边界校验。
3. **美术**：房车车体已由 `tools/gen_rv_body.py` 升级为倒角车身、内凹车窗、轮拱、
   保险杠/防擦条、车顶设备与多段式仪表台/方向盘/驾驶席/吊柜；全部 20 类可摆放家具
   （床/灶台/冰箱/水槽/工作台/储物箱/电视/卫浴/药柜/武器架/暖器/水箱/电池/发电机/
   炮塔/太阳能板/通风扇/咖啡机/种植箱）也改由 `tools/gen_rv_furniture.py` 生成
   多部件模型，每个盒体经 `tools/rv_geom.bevel_box()` 倒角，并接入 12 种表面 ×
   （底色 + 法线 + ORM 贴图）+ 2 张自发光贴图
   （木纹/拉丝金属/织物/橡胶/车身瓦楞/太阳能栅格），每个 primitive 有平面投影 UV；
   但几何仍是“倒角盒体/棱柱”组合，法线图只是平面凸起近似，
   没有曲面细分/视差贴图；世界画风已转为低多边形草甸 + 起伏草丘/土坡，
   玩家/房车/合作代理按营地级高度场跟随坡面高度，
   房车已有四个独立车轮刚体，通过软限位滑轨弹簧支撑车体；
   高度/俯仰/侧倾/侧滑/偏航来自求解器，前轮有阿克曼夹角，
   路面摩擦区分沥青/土坡/草地，纵向使用滑移率曲线；
   但车轮刚体是方形代理（无轮胎碰撞形状/独立滚动约束），
   没有轮胎温度/磨损模型，合作服务器仍走脚本偏航回退；
   高度场只覆盖营地周边 144×144m，范围外仍是平地；
   商店/加油站/仓库/商场/湖边木屋/诊所前窗已支持透明玻璃，破碎时从 seed 冲击点
   生长径向 + 分支裂纹，按应力衰减决定碎片大小/数量/速度（每扇约 20–30 块），
   落地后保留为永久碎屑（状态与 seed 由服务器权威广播），但仍是“径向裂环 +
   预生成碎片网格”的近似，不是连续介质应力场/裂纹扩展模拟；
   物流仓库卷帘门与谷仓滑动门均已可开合，且关闭时由 `poiDoorBlocking()` 参与碰撞阻挡通行；
   商店/加油站/商场/仓库/农场/诊所/湖边木屋的主要商品与补给箱已使用包装图集，
   角色动画也仍是手工关键帧。
4. **世界**：已有世界固定公路网 + 15 个地点；废宅/商店 → `rv_store.gltf`、
   废弃加油站 → `rv_gasstation.gltf`、物流仓库 → `rv_warehouse.gltf`、
   旧农场 → `rv_barn.gltf`、购物中心 → `rv_mall.gltf`、湖边营地 →
   `rv_cabin.gltf`、路边诊所 → `rv_clinic.gltf` 均可进入，含店内物资箱和墙体碰撞；
   未匹配名称的程序化地点仍会回退到简易棚屋。
5. **玩法**：钓鱼、天气、昼夜、零件磨损/升级、灶台菜单、背包、合作均已具备；
   还缺更深的搜刮/建造/驾驶成长。

## 下一步建议

- P1：合作存档版本迁移/冲突合并 + 账号级槽位身份（替换当前 `--name` 轻量身份）。
- P1：把可进入商店的内饰替换为更精细的家具/商品模型，并给其余 POI 加室内。
- P1：门洞加门框/密封条和登车踏步，替换当前矩形切面。
- P1：更多角色动作（开门、搬家具、上车踏步）与驾驶/乘客座位区分。
- P1：合作建造升级为无网格自由摆放 + 每名玩家的独立家具所有权/权限。
- P2：POI 建筑内部（可进入的便利店/加油站/农舍），替换立方体拼装。
- P2：更高精度的房车模型与车内材质/灯光。
- P3：更多 NPC 事件、道路随机事件、长途旅行目标。
