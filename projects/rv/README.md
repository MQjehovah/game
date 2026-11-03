# Nomad Caravan（房车生存）

第一人称房车生存模拟，改造自原《Last Caravan》俯视网格沙盒。

## 核心循环

```
搜刮公路地点 / 湖边钓鱼 → 带回食材、水、咖啡、种子、材料
        ↓
车里做饭、煮咖啡、喝水、睡觉、种菜、修车
        ↓
第一人称自由摆放家具（B）→ 上路（T）→ 发现下一个地点
        ↓
天气/昼夜变化；夜里偶发零散感染者（低强度威胁，不强制撤离）
```

## 操作

| 按键 | 作用 |
|---|---|
| W A S D | 第一人称移动（A/D 横移） |
| 鼠标 | 视角 |
| Shift | 疾跑 |
| E | 交互：搜物资箱 / 用家具 / 钓鱼 / 篝火烤鱼 / 驾驶房车 |
| 左键 | 有感染者时射击；靠近物资箱时搜索；摆放模式下放置家具 |
| B | 进入/退出**第一人称自由摆放** |
| T | 在房车旁上路 |
| 空格 | 驾驶时停车 |
| C | 驾驶席 / 追逐视角 |
| M | 公路地图 |
| 1-0 | 快速使用物品（摆放模式下切换家具） |
| I | 打开/关闭背包面板（物品、效果、材料） |
| F5 / F9 | 保存 / 读取（`saves/layout.json`） |

### 自由摆放模式（B）

- 准星对准车内地面/车顶，显示家具预览（绿=可放，红=冲突/越界/材料不足）。
- 左键放置，右键或 `X` 拆除准星指向的家具。
- `R` 每次旋转 15°，`滚轮`/`1-0` 切换家具，`Tab` 切换地板/车顶。
- `B` 或 `E` 退出。家具不再吸附网格，位置/角度按准星自由摆放。

## 生存系统

- **生命 / 饥饿 / 口渴 / 体力 / 卫生**：随时间衰减；饿/渴归零会掉血。
- **背包面板（I）**：显示食物/水/材料数量与效果，数字键快速使用；种子/咖啡豆/修理包需去对应家具。
- **做饭**：灶台打开配方菜单：`1` 热饭（生食材+水）、`2` 鱼汤（烤鱼+水）、`3` 大餐（热饭+生食材+水）。
- **喝水**：水槽（水箱或瓶装水）。
- **咖啡**：咖啡机（咖啡豆 + 水 + 电）→ 热咖啡，恢复体力。
- **睡觉**：床上睡到第二天 07:00，恢复体力。
- **种植**：种植箱播种 → 浇水/等待 → 收获生食材。
- **洗漱**：卫生间消耗水，恢复卫生与少量体力。
- **修车/升级**：工作台打开菜单：`1` 车体、`2` 引擎、`3` 轮胎、`4` 电瓶；零件修满后，引擎/轮胎可继续升级 Lv1–3（每级 +4% 性能），消耗金属/电子件/布料。
- **油/水补给**：在营地或废弃加油站停靠自动补满。
- **钓鱼**：走到水塘边按 `E` 抛竿，等 5–9 秒；不要走动，否则收线。钓到的是**生鱼**，回营地篝火按 `E` 烤成烤鱼。
- **天气**：晴 / 阴 / 雨 / 雾，随游戏时间变化；雨雾天驾驶极速下降，雨天有 3D 雨丝、太阳能降到 25%、水箱缓慢接雨水。
- **真实昼夜**：场景太阳光的强度/颜色随游戏时间变化（黄昏变暖、夜晚变暗），不是单纯屏幕遮罩。
- **零件磨损**：引擎/轮胎/电瓶按里程磨损；引擎影响极速与加速，轮胎影响转向；引擎低于 10% 只能龟速，轮胎低于 10% 转向严重迟钝，并弹出警告。

## 公路与地点

- 公路是**世界固定**的：营地南侧主路 + 各地点之间的连接路，车头转向不会带着路面转。
- 公路由 10m 分段 + 独立黄色虚线组成，可开离公路越野；主路两侧有电线杆/横担装饰。
- 开局一次性生成 6 个额外地点（加油站/废宅/农场/诊所/商场/仓库/营地/湖边营地），并自动铺连接到最近地点的路；行驶中不再新增/改向公路。
- 非营地停靠时，物资箱刷新在地点建筑内；废宅/商店的 6 个箱子固定落在店内货架/柜台旁，
  玩家必须从正门进入搜刮；其他地点仍散落在屋外。
- 地点带危险度、材料与物品战利品表、节点标记柱和简易废弃建筑：加油站有雨棚/油泵，仓库是大厂房，农场是红顶谷仓，商场是更大的废弃商铺；湖边地点额外生成水塘。
- **可进入的废宅商店**（`rv_store.gltf`）：实心墙体（内外两面可见）、1.6m 门洞 +
  门框/雨棚/红招牌、两扇透明可破碎橱窗（窗内展示商品）；店内四层商品墙（成排彩色货盒 +
  价签）、中央双层货架、玻璃门冷柜、果蔬木箱、带屏幕/键盘/小票的收银机、挂式价牌、
  壁钟、购物篮堆、地毯、吊灯（真实点光源）和物资箱；侧/后墙与门洞由
  `collidePoiWalls()` 阻挡，玩家只能从正门进出。`coop_ruin.txt` + `coop_poiview.txt`
  可复现双人进门搜刮。
- **可进入的废弃加油站**（`rv_gasstation.gltf`）：实心墙体、1.6m 门洞、两扇透明可破碎橱窗
  （窗内展示商品），四柱雨棚 + 两台加油机；店内四层商品墙（成排彩色货盒 + 价签）、
  中央双层零食货架、玻璃门冷柜、咖啡机 + 杯堆 + 调料架、带屏幕/键盘/小票的收银机、
  挂式价牌、壁钟、购物篮堆、2 盏点光源和 6 个物资箱，同样由 `collidePoiWalls()`
  限制只能从正门进出。`coop_gas.txt` + `coop_poiext.txt` 可复现双人进出和店外全景。
- **可破碎橱窗**（`rv_window_glass.gltf` / `rv_glass_frag1..12.gltf`）：商店/加油站/仓库/
  商场/湖边木屋/诊所的橱窗都是独立半透明玻璃面板（BLEND 材质，可透出店内陈设）；
  破碎时按 seed 选一个**冲击点**，再从该点向外生长 4–6 条**径向裂纹轨迹**：
  每条裂纹逐环游走、在内环按 seed 分裂出分支裂纹，面板被这些裂纹切成
  4 个同心环带内的楔形碎片（裂纹角度/长度/分支由 seed 决定）。
  越靠近冲击点的环带越密、碎片越小，离冲击点越远碎片越大 —— 用简单的
  `stress = 1 / (1 + 3.2·dist)` 衰减驱动碎片数量、初速与自旋；
  再从 **12 种预生成碎片网格**池中取网格并按碎片包围盒缩放、沿径向旋转。
  碎片带向外 + 重力速度，落地弹跳后**永久留在地面成为碎屑**（不清除）。
  服务器在玩家进入 POI 时选一个 seed，权威地打破并广播 `glass=<id>:<seed>`，
  两个客户端用同一 seed 重建完全相同的裂纹网络（不做本地预测）。日志
  `[glass] broke windows at ruin/... (enter, N stress shards, M cracks, seed=N)`；
  `coop_glassview.txt` 提供门窗特写镜头。
- **商品包装图集**（`rv_products.png`）：`tools/gen_rv_product_atlas.py` 生成 4×4
  包装图集（品牌条/标签文字/色带/条码）；`tools/rv_atlas.py` 为每个商品盒提供 UV，
  商店/加油站/商场的商品墙、仓库的托盘/散货、谷仓的货架木箱、诊所的药盒、湖边木屋的
  货架与补给箱都显示为带包装/标签的商品而非纯色块（`coop_propview.txt` 提供道具特写镜头）。
- **可进入的物流仓库**（`rv_warehouse.gltf`）：高棚 + 宽装卸门/月台（黄色警示线、红白防撞柱、
  钢制升降台板）、**可开合卷帘门**（玩家靠近月台时服务器权威开门、离开关门，双端同步
  缩放卷起动画；未完全打开时门洞参与碰撞，玩家会被挡住）、两扇可破碎前橱窗、
  店内两侧 + 后墙钢制货架与黄底白字货架标识、
  标签托盘堆（带包装图集的彩色货箱 + 胶带封箱 + 发货标签）、
  叉车（车身/护顶架/门架/货叉/车轮/警灯）、发货台（显示器 + 标签打印机 + 键盘 + 凳子）、
  带包装图集的散装货箱、地面黄色通道线、顶部高窗、2 盏点光源和 6 个物资箱；
  墙体碰撞与门洞限制同商店。`coop_depot.txt` + `coop_poiview/poiext` 可复现双人进出。
- **可进入的旧农场**（`rv_barn.gltf`）：红谷仓 + 山墙屋顶 + 干草阁楼、**可开合滑动门**
  （玩家靠近谷仓门口时服务器权威开门、离开关门，双端同步横向滑动；未完全滑开时门洞
  参与碰撞，玩家会被挡住）、
  内部白灰墙衬、左右畜栏（前挂干草槽 + 饮水桶 + 栏号牌）、干草堆、饲料槽、工作台
  （红色工具箱 + 台钳 + 挂板工具）、马具墙（木挂板 + 挂钩 + 缰绳 + 马蹄铁）、
  马鞍架、两只奶桶、粮袋、带白色标签的木箱、后墙货架与**带包装图集的货箱**
  （3 层货架摆绿/品红/琥珀/蓝色包装箱），4 盏点光源和 6 个物资箱；
  门外有干草堆/水槽/木栅栏。`coop_farm.txt` + `coop_poiview/poiext` 可复现双人进出。
- **可进入的购物中心**（`rv_mall.gltf`）：高挑可破碎玻璃店面、红蓝 `MALL` 招牌、收银台
  （收银机屏幕/键盘/小票 + 刷卡器 + 购物袋）、四层商品墙（成排彩色商品 + 价签条）、
  折叠衣物展示台、配件展示台、左侧挂衣架（衣架 + SALE 吊牌）、右侧圆形挂衣架、
  穿色衣的橱窗模特、商场导视牌、悬挂 SALE 牌、入口指示牌、屋顶空调、门外花箱/
  长椅/购物车、4 盏点光源和 6 个物资箱。`coop_mall.txt` + `coop_poiview/poiext`
  可复现双人进出。
- **可进入的湖边木屋**（`rv_cabin.gltf`）：原木墙 + 山墙屋顶 + 带柱门廊、两扇可破碎窗、
  壁炉/烟囱、
  床与蓝被、餐桌椅、厨房柜台、**三层货架与带包装图集的补给箱**（货架商品 + 叠放木箱）、
  门廊补给箱、地毯、2 盏点光源和 6 个物资箱；门外有柴堆/
  木桶/鱼箱/灯柱，旁边生成水塘。`coop_lake.txt` + `coop_poiview/poiext` 可复现双人进出。
- **可进入的路边诊所**（`rv_clinic.gltf`）：白/青色外墙 + 平屋顶、玻璃双开门 + 红十字，
  两扇可破碎橱窗与青色墙裙；店内接待台（蓝图显示器/键盘/电话/单据）、候诊椅、诊床（床单/
  枕头/纸卷）、带玻璃门和白色标签的三层药柜（**带包装图集的药盒**）、X 光/检查壁灯、
  生命体征监护仪、输液架、器械托盘、隐私窗帘、`RECEPTION` 与 `EXAM` 标识、壁钟、
  3 盏点光源（含药柜前补光）和 6 个物资箱；门外有长椅/花箱/轮椅推车。
  `coop_clinic.txt` + `coop_poiview/poiext` 可复现双人进出。
- `M` 地图动态显示所有已发现地点。

## 低强度威胁

- 搜索物资箱时，夜里或危险地点有小概率惊动 1 只感染者。
- 场上最多 3 只；不刷新波次、不强制撤离、不抢搜获袋。
- 靠近会攻击玩家；左键射击，武器架让伤害翻倍，炮塔会自动防御。

## 表现

- 车内补了内墙板、车顶、仪表台、A 柱、暖色点光源，驾驶席视角隐藏车头盒体。
- 房车外壳由 `tools/gen_rv_body.py` 生成：倒角车顶/门槛、带窗框的内凹侧窗、
  前后轮拱（20 段圆弧 + 双层轮拱唇）+ 轮拱内衬、前后保险杠、侧防擦条、
  车顶行李架/空调/爬梯/遮阳篷盒、后视镜、灯组、格栅、排气管；
  车内新增多段式仪表台、方向盘、驾驶席（座垫/靠背/侧翼/头枕/滑轨）与两侧吊柜
  （柜门 + 把手）+ 驾驶室顶架。门洞/座椅/碰撞保持不变。
- **倒角几何**：`tools/rv_geom.bevel_box()` 把每个盒体替换为
  6 个内缩面 + 12 条 45° 棱边条 + 8 个角三角（小件/薄板自动回退为普通盒），
  `gen_rv_body.py` 与 `gen_rv_furniture.py` 的 `box()` 默认走它，
  所以车身、家具的轮廓都有可见的倒角高光，而不是硬直角。
- 可摆放家具**全部 20 类**改由 `tools/gen_rv_furniture.py` 生成多部件 glTF：
  单人/双人床（木框/床垫/被子/枕头）、灶台（烤箱门/玻璃窗/旋钮/双灶头）、
  冰箱（双门/把手/散热格栅）、水槽（柜体/台面/内嵌水盆/龙头）、工作台
  （桌面/腿/抽屉/挂板工具/台钳/工具箱）、储物箱（板条/包角/箱盖/提手/标签）、
  电视柜（柜门/电视/屏幕/天线/音箱）、卫浴间（底盆/围板/马桶/洗手盆/镜子/花洒）、
  药柜（玻璃门/层板/彩色药盒/红十字）、武器架（背板/立柱/两支步枪/弹药箱）、
  暖器（机身/格栅/顶部散热片/旋钮/指示灯）、净水箱（阶梯箱体/加注口/绑带/液位窗/
  阀门/标签）、电池组（双壳体/极柱/压条/线缆/标签）、发电机（框架/机身/红色油箱/
  控制面板/排气管/散热格栅/提手）、炮塔（底板/立柱/炮塔壳/双枪管/护盾/弹药箱）、
  太阳能板（边框/蓝色电池片/栅线/支架/接线盒）、通风扇（法兰/扇框/防护网/顶盖/
  摇柄）、咖啡机（机身/水箱/冲煮头/滴水盘/杯子/蒸汽棒/按键）、种植箱（木箱/土壤/
  幼苗/果实/支架）；预制体名与 `use` 交互钩子保持不变。
- **表面细节贴图**：`tools/gen_rv_surfaces.py` 为 12 种表面各生成 **3 张** 256×256 贴图
  （浅/深木纹、拉丝金属、深色金属、织物编织、塑料、玻璃、轮胎橡胶、车顶面板、
  车身横向瓦楞、太阳能栅格、叶片斑驳）：
  `rv_tex_<name>.png` 中性灰度底色细节、`rv_tex_<name>_n.png` 由高度场 Sobel 得到的
  切线空间法线图、`rv_tex_<name>_orm.png`（R=AO、G=粗糙度、B=金属度）；
  另有两张自发光贴图 `rv_tex_lamp.png` / `rv_tex_tail.png` 点亮大灯与尾灯。
  `tools/rv_surfaces.py` 为每个盒体面提供平面投影 UV（按面法线选轴、世界尺度平铺），
  glTF 材质同时追加 `baseColorTexture` / `normalTexture` / `metallicRoughnessTexture` /
  `occlusionTexture` / `emissiveTexture`，由原 `baseColorFactor` 着色 ——
  车体与全部 20 类家具因此有了木纹/拉丝/编织/瓦楞的表面明暗与凹凸，而调色板不变。
- 车内保留独立内饰板/仪表台/暖光点光源；驾驶席视角隐藏外壳，追逐视角显示完整车体。
- 公路改为 20 段 10m 深灰立方体 + 独立黄色虚线几何体：抬高路面并加 polygon offset，
  避免远距离 z-fighting 导致的整条路发白；不再依赖贴图/深度投影。
- **画风：低多边形草甸 + 起伏地形**：`tools/gen_rv_ground.py` 生成可平铺的
  `rv_ground_meadow.png`（草绿基底 + 深浅草丛条纹 + 泥土斑块 + 小花点）与
  `rv_dirt.png`，场景 `Ground` 改为 `albedoTex` + `uvRepeat 160`，从沙地变为草地；
  `tools/gen_rv_terrain.py` 生成营地级高度场（见下），其中保留了
  主路右侧 5–38m、z -24–46m 的土坡（近路侧泥土斜坡 + 顶部草地，
  高度沿 z 起伏、两端收口），对应参考图里的土坡/路肩。
  `tools/gen_rv_vegetation.py` 生成 `rv_meadow_a/b/c`（6×6m 草甸拼块，每块含
  18 簇草 + 2 丛花）、`rv_grass_tuft` / `rv_grass_tuft_b` / `rv_fern` /
  `rv_flower_white/blue/yellow/red` / `rv_bush`；`lc_config.MEADOW` 用 60 个
  拼块 + 约 130 株近景草花 + 34 丛灌木铺满营地与公路两侧（避免路面、水塘、
  POI 与土坡），并按 RV 位置回收复用。
  `updateMeadowWind()` 让草/花以 20Hz、分 4 批绕根部轻摆（草叶统一倾斜方向，
  偏航摆动看起来像随风倒伏）；土坡顶部另种 90 株草花灌木。
- **营地级高度场 + 四轮接地 / 车身俯仰侧倾**：`tools/gen_rv_terrain.py` 生成
  144×144m 的通用高度场（4m 网格），同时写出
  `assets/data/rv_terrain.json` 和可见网格 `rv_terrain.gltf`，两者出自同一
  高度函数：保留原土坡截面，并在营地西/南侧加起伏草丘；营地、水塘、
  固定公路走廊与 POI 占地全部压平，因此道路和建筑仍然水平。
  `lc_terrain.lua` 对 JSON 做双线性采样，`terrainHeight(x,z)` 给出任意点
  高度、`terrainSlope(x,z)` 给出梯度；公路段按高度抬升并按坡度倾斜
  （5m 分段、坡度限幅 10°，像切进坡里的土路），新生成的地点会拒绝落在
  坡面上。步行时 `player.groundY`、眼高、角色模型与枪械都跟随地形；
  驾驶/驻车时 `updateRvSuspension()` 在四个车轮接地点
  （±1.26m 轮距、±2.92m 轴距）分别采样 `terrainHeight`，每个接地点用
  近临界阻尼弹簧跟随地面（刚度 110、阻尼 0.85、行程限幅 ±0.30m），
  再由四点拟合车身平面得到 `rv.groundY` 与 `rv.pitch/rv.roll`
  （限幅 ±0.35rad、10Hz 平滑）。刚性车身在扭曲地面上只能三点着地，
  因此把拟合平面抬高到最深的接地点以上，宁可让对角车轮轻微下垂也不让
  车轮插进土里。`applyRvTransform()` 用 `rvOffset()` 对每个部件的局部偏移做
  roll→pitch→yaw 三维旋转并用
  **引擎新增的 `SetRotationEuler(entity,pitch,roll,yaw)`** 设置朝向，
  外壳/车门/内饰/家具/网格与物理刚体一起倾斜，房车不再插进坡体。
  车内第一人称站立高度由 `rvFloorOffset()` 取倾斜地板高度，镜头随车厢一起
  俯仰/侧倾。合作模式把每个槽位的 `gy` 与房车 `rv.gy` 写入 `game_state`
  （量化到 3 位小数），两端各自用同一高度场计算 `pitch/roll`，
  服务器代理、客户端预测、旁观相机都一致。
- **操作**：A/D 横移已修正为 A 向左、D 向右（右向量取 `forward × up`）；
  驾驶时 A 左转、D 右转，合作模式服务器转向方向同步修正。
- 新增做饭、倒水、咖啡、修理、拾取、钓鱼、受伤、枪声、脚步等音效。
- 驻车/荒野有低频风声循环，上路切换到引擎低频循环。
- HUD 中文文本改为手动 UTF-8 宽度居中/右对齐，避免非 1280 宽窗口下偏左/被裁。
- 车内墙板内缩、车窗改哑光深色并移除重叠侧门，消除墙板交会 z-fighting 与视角变色。

## 运行 / 验证

```powershell
# 直接跑 loose scene（最快）
.\build\neon_game.exe --scene projects\rv\assets\scenes\garage.json --scripts projects\rv

# 截图
.\build\neon_game.exe --scene projects\rv\assets\scenes\garage.json --scripts projects\rv `
  --smoke-test 120 --screenshot out.png 90
```

验证钩子（放在 `projects/rv/` 下）：

- `selftest_on.txt`：启动时跑完整系统自检，结果写到 `selftest_result.txt`。
- `autopilot_on.txt`：自动上路、靠站、搜箱、收工。
- `coop_clamp.txt`：合作测试时把玩家限制在房车前方（便于截图/稳定复现）。
- `coop_drive.txt`：合作测试时玩家 1 在 2 秒后自动坐上驾驶席。
- `coop_actions.txt`：合作测试时 6 秒后自动搜一个物资箱并切换灶台状态。
- `coop_cook.txt`：合作测试时 2 秒后给一份生食材并调用灶台 `cookRecipe`，
  日志 `[coop-cook] stove meal 0->1 ok=true`，用于验证新灶台模型的交互钩子。
- `coop_build.txt`：合作测试时槽位 1 自动放置太阳能板/储物箱并拆除储物箱。
- `coop_buildmode.txt`：客户端启动后直接进入建造模式（UI/截图验证）。
- `coop_board.txt`：槽位 1 在 5s 上车、9s 下车，用于验证进出状态同步。
- `coop_sitdemo.txt`：在房车旁生成一个播放 `Sit` 骨骼片段的预览角色。
- `coop_animdemo.txt`：内容 `sit` / `board` / `stand` / `dooropen` / `doorclose` /
  `step` / `carry`，生成一个循环播放对应片段的预览角色（近距离固定镜头），
  用于检查手工关键帧姿态。
- `coop_sitview.txt`：从房车驾驶室看向乘客席的固定镜头，用于双人验证坐姿。
- `coop_carryview.txt`：槽位 2 的固定镜头跟随槽位 1，用于验证搜刮时的 `Carry` 片段。
- `coop_glassview.txt`：槽位 2 的固定镜头贴近当前 POI 正门，用于验证可破碎橱窗。
- `coop_propview.txt`：槽位 2 的固定镜头对准当前 POI 的货物/药柜（仓库托盘、
  谷仓货架木箱、诊所药柜），用于验证包装图集。
- `coop_doortest.txt`：内容 `depot` 或 `farm`；把两名玩家放到对应门口，先顶着关闭的
  门走（碰撞应挡住），5s 后服务器开门并继续走（应能穿过），日志
  `[door-test] closed/crossed/open`。
- `coop_doorview.txt`：槽位 2 使用车外固定镜头，便于检查真实门洞、车门开合和门口坐姿乘客。
- `coop_ruin.txt`：合作测试传送到废宅商店门口，两名玩家 3–5s 进门、5–8s 在店内、
  8–10s 出门，用于验证可进入建筑与共享物资箱。
- `coop_gas.txt`：同上，但传送到废弃加油站商店门口（含雨棚/油泵）。
- `coop_depot.txt`：同上，但传送到物流仓库装卸门门口（货架/托盘/月台）。
- `coop_farm.txt`：同上，但传送到旧农场谷仓门口（畜栏/干草/饲料槽）。
- `coop_mall.txt`：同上，但传送到购物中心店门口（玻璃店面/货架/服装架）。
- `coop_lake.txt`：同上，但传送到湖边木屋门廊（壁炉/床/桌椅/门廊）。
- `coop_clinic.txt`：同上，但传送到路边诊所门口（红十字/玻璃门/药柜/诊床）。
- `coop_poiview.txt`：店内固定镜头（框住两名玩家、货架、柜台和物资箱）。
- `coop_poiext.txt`：店外全景固定镜头（商店 + 雨棚/油泵/装卸月台/谷仓等）。
- `start_mode.txt`：`inside` / `inside_night` / `night` / `place` / `place_night` /
  `drive` / `map` / `look_back` / `look_left` / `look_right` / `look_down` / `look_up` /
  `furnview`（车内家具墙）/ `furnall`（电视/卫浴/药柜/武器架/暖器）/
  `utilall`（水箱/电池/发电机/炮塔/种植箱/咖啡机）/ `utilclose`（电池/发电机/炮塔特写）/
  `utilroof`（车顶太阳能板/通风扇，驾驶追逐视角）/
  `rvfront`（房车正面）/ `side`（房车侧面）/
  `poi`（废宅商店门口）/ `gas`（加油站门口）/ `warehouse`（仓库月台）/
  `farm`（旧农场谷仓）/ `mall`（购物中心店门口）/ `lake`（湖边木屋）/
  `clinic`（路边诊所）。

自检覆盖 34 项：生存、做饭/鱼汤、咖啡、钓鱼、修车/升级引擎、自由摆放、昼夜灯光、天气、
高度场剖面/草丘/道路压平/地面跟随/坡面倾斜/车内倾斜/四轮接地、废宅商店/加油站/物流仓库/旧农场/购物中心/湖边木屋/路边诊所店内物资箱（6/6）、
POI 墙体碰撞/门洞通行等。

## 网络合作（主世界 2–4 人）

服务器权威的合作模式已并入 `garage.json` 主世界：同一辆房车、同一组需求/家具/搜刮物资，
支持 4 个玩家槽位。`coop_test.json` 仍保留为更轻量的 2 人协议调试场景。

```powershell
# 终端 1：权威服务器（主世界）
.\build-msvc\Release\neon_server.exe `
  --scene projects\rv\assets\scenes\garage.json --scripts projects\rv --port 26050

# 终端 2：玩家 1（先加入 → 槽位 1）
.\build\neon_game.exe --connect 127.0.0.1:26050 `
  --scene projects\rv\assets\scenes\garage.json --scripts projects\rv --name alice

# 终端 3：玩家 2（后加入 → 槽位 2）
.\build\neon_game.exe --connect 127.0.0.1:26050 `
  --scene projects\rv\assets\scenes\garage.json --scripts projects\rv --name bob

# 终端 4/5：玩家 3/4（槽位 3/4，可选）
```

- 操作：`WASD` 移动、鼠标视角、`F` 上/下驾驶席、`L` 车内灯、`G` 灶台开关、
  `B` 合作建造、`E` 搜附近物资箱/上下车、`1` 吃饭、`2` 喝水、`3` 睡觉。
- **进出房车**：走到车门旁按 `E` 上车（乘客席）；车内按 `E` 下车；按 `F` 直接坐上
  驾驶席。服务器记录每名玩家的 `inside/seat` 并广播；客户端在车内切第一人称、隐藏
  自己的角色，车外恢复第三人称。车体右侧已切出真实门洞（车身壳体 + 右侧内墙板
  同步开口），独立 `rv_door.gltf` 网格按后缘铰链开合。角色播放**手工关键帧**
  上车链：`DoorOpen`（够门把手→拉开，0.9s）→ `Step`（抬脚踩上车踏板，0.7s）→
  `Board`（站姿→抬步→落座，0.9s）→ `Sit` 循环；下车链：`Stand`（坐姿→起身→迈步，
  0.9s）→ `DoorClose`（再够把手→推合，0.9s）→ `Idle`。搜刮物资箱时服务器广播
  `carry` 槽位，远端角色播放 `Carry`（抱箱循环，1.2s）后回到 `Idle`。
  门板本身由 `in1..in4/door` 状态驱动同步摆动。
- **上行指令**：客户端 `Rpc("game_cmd", {...})`；服务器 `game_cmd` 处理器按
  `clientId` 路由到槽位，Lua 用 `NetCommand()` 消费并做距离/字段校验。
- **下行状态**：`game_state` 10Hz 广播位姿/需求/背包/家具开关；`game_world` 在家具
  变化时按 4 条/块分块发送完整布局，避免超过 RPC 字符串上限；`game_crates` 发送物资箱
  位置。三者都进入客户端脚本队列。`game_state` 里的玩家坐标/朝向取 3 位小数、
  需求/时钟/油水取 1 位小数，留出余量给 `glass=<id>:<seed>` 列表
  （否则 JSON 会顶到引擎 1KB 的 RPC 字符串上限，破碎后的状态会被截断）。
- **合作建造/拆除**：客户端 `B` 进入网格建造，`1-9` 选家具、`Tab` 地板/车顶、
  `J/L/I/K` 移动光标、`R` 旋转、`E` 放置、`X` 拆除。客户端只发
  `game_cmd {type="place"/"remove", ...}`；服务器用 `fits`、`canAfford`、`place`、
  `removeAt` 校验边界、重叠、材料和层，成功后才更新 `instances` 并广播 `game_world`。
- **位姿复制**：服务器生成 4 个 `CTransformBind` 玩家代理；客户端按相同顺序生成同名
  代理，引擎快照复制位姿，客户端 glTF 士兵可视实体跟随代理。
- **每客户端所有权**：服务器把每个代理的 `EntityKey` 随状态下发；客户端用
  `netControlKey` 认领自己的实体，四名玩家都能本地预测 + 服务器纠正。
- **同场景优化**：合作脚本设置 `netReplicateStatic=0`，不再把主世界数百个静态实体
  逐个走可靠通道 Spawn；客户端本来就加载了同一场景，只复制 4 个玩家代理。
- **加入时序**：`on_player_join` 在 `MsgWelcome` 之后触发；服务器在 Login 阶段就完成
  Welcome + 槽位分配，客户端的 Join 包延迟/重试也不会把客户端卡在“已登录但未入局”。
- **持久化**：服务器每 3 秒 + 正常关闭时把权威状态写入 `saves/layout.json`，包括标准
  房车/需求/背包/家具字段，以及 `coop` 扩展（物资箱位置、车内灯、槽位名字/最后位置）。
  服务器重启时自动读取并恢复房车姿态、燃油、需求、家具布局/开关、物资箱。
- **断线重连**：`on_player_leave` 只把槽位标记为离线并保留名字/位置；同名客户端
  用相同 `--name` 重连时回收原槽位和 `EntityKey`，服务器重新下发
  `game_state`/`game_world`/`game_crates`。槽位名字目前是局域网内的轻量身份，没有账号鉴权。
- **客户端握手修复**：Welcome 之前不再发送输入包；`SendJoin` 失败会重试而不是永久
  锁定；服务器在 LoginOk 后提前发送 Welcome，避免主世界首帧负载导致的 Join 竞态。
- **共享搜刮**：服务器 `searchCrate()` + `bankStopBag()` 后广播 `crates`/`materials`；
  两端 HUD/日志显示相同的剩余物资箱与材料。`coop_actions.txt` 可自动验证这条链路。
- **合作建造验证**：`coop_build.txt` 让槽位 1 自动放置车顶太阳能板、储物箱，再拆除
  储物箱；服务器日志 `furn=9→11→10`、`metal=12→10`，两端都出现
  `furniture rebuilt (10)` / `(11)` / `(10)`，截图 HUD 显示“家具 10 件”。
- **进出房车验证**：`coop_board.txt` 让槽位 1 在 5s 上车、9s 下车；服务器日志
  `alice 上车（乘客席）`、`in1=1 door=1`，随后 `alice 下车`、`in1=0 door=0`；
  两端同一 `seq` 都显示一致的 `in1/door`。截图：`rv_final_interior3.png`
  （车内第一人称透过门洞看到外部）、`rv_final_seat_visible5.png`
  （车外透过门洞看到坐在门口的乘客）。
- **车门/坐姿资产验证**：`rv_door_real_side.png` 显示独立车门网格绕铰链打开；
  `rv_sit_authored4.png` 显示手工关键帧 `Sit` 坐姿（髋部下沉、屈膝、双手前伸），
  `rv_board_pose.png` / `rv_board_pose2.png` 显示 `Board` 的伸手/落座关键帧，
  `rv_stand_pose.png` 显示 `Stand` 的起身关键帧，
  `rv_dooropen_pose.png` / `rv_doorclose_pose.png` 显示 `DoorOpen` / `DoorClose`
  够门把手的关键帧，`rv_step_pose.png` 显示 `Step` 抬脚上车踏板，
  `rv_carry_pose.png` / `rv_carry_flow.png` 显示 `Carry` 抱箱姿态。
  上下车过渡日志：
  上车 `player 1 reaches for the door handle (DoorOpen ok=true)` →
  `player 1 steps up into the RV (Step ok=true)` →
  `player 1 boards (Board -> Sit)` → `player 1 sits down (Sit loop)`；
  下车 `player 1 exits (Stand -> DoorClose -> Idle)` →
  `player 1 pushes the door shut (DoorClose ok=true)` →
  `player 1 stands up (Idle loop)`。搜刮日志：`player 1 carries a crate (Carry loop)` →
  `player 1 finishes carrying (Idle loop)`。各过渡由 `AnimationFinished` 触发切换，
  门板同步摆动；`rv_authboard_sitview.png` 为双人合作时从驾驶室看向乘客席的坐姿截图。
- **RV 车体与门洞验证**：`tools/gen_rv_body.py` 重新生成 `rv_body.gltf`，车身右侧
  仍保留完全一致的乘客门开口（`z=-3.54..-2.76, y=0.24..1.86`），车门仍是独立的
  `rv_door.gltf`（铰链在 `(1.205,-3.54)`）；`coop_board.txt` 双客户端验证
  `in1=1 door=1` → `in1=0 door=0`，并完整播放
  `DoorOpen → Step → Board → Sit → Stand → DoorClose → Idle`。
  截图：`rv_hi_rear34b.png`（后视：后窗/尾灯/爬梯/保险杠/车顶设备）、
  `rv_hi_side3.png`（侧视：内凹车窗/轮拱/防擦条/遮阳篷盒/行李架）、
  `rv_hi_front2.png`（前视：挡风玻璃/格栅/大灯/前保险杠）、
  `rv_hi_interior2.png`（车内：吊柜/柜门把手/驾驶室顶架/仪表台/方向盘/驾驶席）、
  `rv_hi_board.png`（车门打开、乘客坐在门口）。
- **家具替换验证**：`tools/gen_rv_furniture.py` 生成 20 个多部件模型
  （`rv_furn_bed_single/bed_double/stove/fridge/sink/workbench/storage/tv/bathroom/
  med_cabinet/gun_rack/heater/water_tank/battery/generator/turret/solar/roof_vent/
  coffee/plant`），`rv_bed_*`/`rv_tv`/`rv_water_tank`/`rv_solar`/`rv_coffee` 等预制体
  改为引用它们（尺寸/层/`use` 不变）。
  单机 `start_mode=furnview` 截图 `rv_furn_view.png` 可见床（木框/蓝被/枕头）、
  水槽（台面/水盆/龙头）、灶台（烤箱门/玻璃/旋钮/灶头）；`start_mode=furnall`
  截图 `rv_furn_all2.png` 可见电视柜、卫浴间、玻璃门药柜、双步枪武器架、暖器；
  `start_mode=utilall` 截图 `rv_util_all5.png` 可见蓝阶梯水箱、电池组、红色油箱
  发电机、炮塔、种植箱与咖啡机；`start_mode=utilclose` 截图 `rv_util_close2.png`
  为电池/发电机/炮塔特写；`start_mode=utilroof`（驾驶追逐视角）截图
  `rv_util_roof.png` 可见车顶太阳能板与通风扇。
  `coop_cook.txt` 双客户端跑 `[coop-cook] stove meal 0->1 ok=true`；
  自检的 `cook at stove` / `drink at sink` / `repair at workbench` 仍通过。
- **表面贴图验证**：`tools/gen_rv_surfaces.py` 生成 **38 张** `rv_tex_*.png`
  （12 种表面 × 底色/法线/ORM + 2 张自发光），`rv_body.gltf` 与 20 个
  `rv_furn_*.gltf` 的每个 primitive 都带 `TEXCOORD_0`，材质同时引用
  `baseColorTexture` / `normalTexture` / `metallicRoughnessTexture` /
  `occlusionTexture`（启动日志可见 38 条 `loaded texture 'assets/models/rv/rv_tex_*'`）。
  对比截图：上一版 `rv_tex_furn2.png` / `rv_tex_util.png` 只有底色明暗；
  本版 `rv_tex_normal_furn.png`（工作台/吊柜木纹出现凹陷与高光，红色工具箱有塑料高光）、
  `rv_tex_normal_util.png`（工作台挂板木纹浮雕、蓝色水箱面板缝）、
  `rv_tex_normal_body.png`（车身横向瓦楞的凹凸阴影）、`rv_tex_side3.png`
  （车身瓦楞 + 纵向板缝）、`rv_tex_front2.png`（驾驶室侧面）。
  自发光：`rv_tex_emit_front.png`（大灯发亮）、`rv_tex_emit_tail.png`（夜间尾灯红色发光）。
  `rv_tex_board.png` 双人上车截图中车门与车身同样带瓦楞纹理。
- **倒角几何验证**：`tools/rv_geom.py` 的 `bevel_box()` 为每个盒体生成
  6 面 + 12 棱边 + 8 角三角；`gen_rv_body.py` 顶点数从 8600 增至 26888，
  家具从约 360–480 增至 600–1600。近景对比：
  `rv_tex_normal_postcommit.png`（上一版）中工作台立柱、台钳座、红色工具箱都是硬直角；
  本版 `rv_bevel_furn.png` / `rv_bevel_util.png` 可见工作台立柱的竖向倒角高光、
  台钳座的八边形轮廓、工具箱盖沿的 45° 棱边、蓝色水箱/床垫的圆角；
  `rv_bevel_side.png` 显示车窗框与轮拱的倒角（轮拱改 20 段圆弧 + 双层轮唇）；
  `rv_bevel_front.png` 显示保险杠/格栅/车顶盖的倒角。
  门洞（`z=-3.54..-2.76, y=0.24..1.86`）、座椅坐标与物理碰撞盒未变。
- **草甸画风验证**：`Ground` 材质带 `albedoTex=assets/models/rv/rv_ground_meadow.png`
  （512×512，`uvRepeat=160`），启动日志可见该纹理加载；`MEADOW` 用
  60 个 6×6m 草甸拼块 + 约 130 株近景草花 + 34 丛灌木（共约 570 个实体），
  土坡顶部另有 90 株，单株 grass/flower 关闭投影。
  截图：`rv_final_drive2.png`（车顶追逐视角：公路两侧密集草丛、灌木、彩色野花，
  HUD 约 **91 FPS**）、`rv_patch_back.png`（回望公路，右侧土坡与草地）、
  `rv_hilltop.png`（从西南草丘回望营地：起伏草甸 + 水塘 + 房车）、
  `rv_patch_right.png`（草甸与池塘）。
  对照参考图，地面由原来的沙土色变为饱和草绿，并出现高矮草丛、灌木团、
  蓝/黄/白/红野花与沿路土坡，形成低多边形草甸风格。
- **营地高度场 / 四轮接地验证**：`assets/data/rv_terrain.json`（37×37，4m 网格，
  144×144m）与 `rv_terrain.gltf` 由 `tools/gen_rv_terrain.py` 同源生成，
  `lc_terrain.terrainHeight()` 双线性采样。自检新增
  `terrain height profile`（flat=0.00 mid=2.10 top=3.55）、
  `terrain ground follow`（follow=3.55 top=3.55）、
  `terrain heightfield`（37×37 step=4）、`terrain knoll`（knoll=2.09）、
  `terrain flat corridor`（camp/gas/ruin/clinic/road/edge 均 ≤0.06）、
  `rv slope tilt`（pitch=-0.12 roll=0.32）、
  `rv tilted interior`（nose=-0.60 right=0.60）与
  `rv four wheel contact`（gy=2.34 h0=2.34 clearance=0.00
  pitch=-0.28 roll=0.25 spread=4.01），单机自检 **34/34 PASS**。
  截图 `rv_rvhill.png`：房车停在草丘上，四轮接地点拟合出的车身随坡抬起；
  `rv_4wheel2.png`：土坡斜停时车轮不再半埋（最深接地点被抬到地面以上，
  对角车轮轻微下垂）；`rv_hilltop.png`：站在营地西南草丘回望，起伏草甸、
  水塘、房车与土坡；`rv_lookback_hf.png`：营地视角；`rv_susp_drive.png`：
  自动驾驶停靠时车身贴合坡面（HUD 约 100 FPS）。
  合作模式 `game_state` 带每位玩家 `gy` 与房车 `rv.gy`，两端用同一高度场
  算出相同 `pitch/roll`。
- **可破碎橱窗验证**：`rv_window_glass.gltf` 使用 BLEND 材质，商店/加油站/仓库/商场/
  湖边木屋/诊所橱窗初始为半透明蓝色面板（`rv_glass_intact_close.png` 中窗内商品呈蓝色调，
  `rv_clinic_glass_intact.png` 为诊所橱窗）；玩家进门后服务器打印
  `[glass] broke windows at ruin (enter, N stress shards, M cracks, seed=N)` 并把
  `glass=ruin:N` 写入 `game_state`，两个客户端都打印相同 seed、碎片数与裂纹数后重建
  完全一致的径向裂纹网络（一次双人验证为 `44 stress shards, 46 cracks, seed=985159`）。
  截图 `rv_crack_a.png` 可见碎片从冲击点向外呈扇形散开、近处小而密、远处大而疏；
  `rv_voronoi_a.png` / `rv_voronoi_debris.png` 为改造前的 Voronoi 布局对比，
  落地后碎片均保持不消失。
  像素对比同一位置从 `(27,50,73)`（玻璃蓝）变为 `(56,55,52)`（无玻璃），
  诊所窗为 `(64,86,106)` → `(95,99,101)`；单机左键攻击同样可击碎。
- **卷帘门验证**：`rv_rollup_door.gltf` 是 11 条板 + 导轨/底梁/卷轴的 2.6m 卷帘，
  `lc_scenery.updatePoiDoors()` 以 `scaleY = 1 - 0.94*cur` 绕顶边卷起。单机站在月台
  自动开门（`[depot-door] depot target=true` → `now open`，截图 `rv_rollup_open_fp.png`），
  离开自动关门。合作模式下服务器按玩家到月台的距离计算 `rollup` 并写入 `game_state`
  （`[depot-door] open/close (server)`），两个客户端都打印
  `target=true → now open (cur=1.00)` / `target=false → now closed (cur=0.00)`；
  `rv_rollup_coop_closed.png` 显示关闭的板条卷帘，`rv_rollup_coop_open.png` 显示卷起后
  可看到店内叉车/托盘与玩家。
- **谷仓滑动门验证**：`rv_sliding_door.gltf` 是 5 块红木板 + 白横梁/斜撑 + 顶滑轮 +
  把手/门牌（1.86×2.42m），`lc_scenery.updatePoiSlidingDoors()` 让它沿 X 滑动 2.05m
  停到右前墙上方。单机出生在门口自动开门（`[barn-door] farm target=true` →
  `now open`，截图 `rv_barn_door_open_fp.png`）；合作模式由服务器按玩家到谷仓门口
  的距离计算 `barndoor` 并写入 `game_state`（`[barn-door] open/close (server)`），
  两个客户端都打印 `target=true → now open (cur=1.00)` / `target=false → now closed`
  （状态日志 `barndoor=1.0` / `barndoor=0.0`）；`rv_barn_door_coop_closed.png` 显示
  关闭的红门，`rv_barn_door_coop_open.png` 显示门滑到右侧、门洞内可见玩家与干草/工作台。
- **关闭门碰撞验证**：`poiDoorBlocking()`（`lc_scenery`）在门未完全打开
  （`cur < 0.9`）时返回真，`collidePoiWalls()` 据此把整个前墙面当作实体；服务器在
  `serverUpdate` 里也调用 `setPoiDoorOpen/setPoiSlidingDoorOpen`，所以权威移动和客户端
  预测使用同一状态。`coop_doortest.txt=depot|farm` 驱动两名玩家先顶着关门走、5s 后
  开门继续走，服务器日志：

  ```text
  [door-test] closed: p1 z=-48.81 front=-48.95 blocked=true (door cur=0.00)
  [door-test] crossed: p1 z=-48.97 front=-48.95 (door cur=0.99)
  [door-test] open: p1 z=-53.91 inside=true (door cur=1.00)
  [door-test] closed: p1 z=53.19 front=53.05 blocked=true (door cur=0.00)
  [door-test] open: p1 z=48.09 inside=true (door cur=1.00)
  ```

  截图 `rv_door_closed_final.png` / `rv_barn_door_closed_block.png` 显示玩家被关着的门
  挡在门外，`rv_door_open_pass.png` / `rv_barn_door_open_pass.png` 显示门打开后玩家已
  进入室内（HUD 坐标 z 已越过前墙）。
- **包装图集验证**：`rv_products.png` 已被商店/加油站/商场/仓库/农场/诊所/湖边木屋引用；
  双客户端分别进入仓库、谷仓、诊所、湖边木屋（`shop=2`、两端 `smoke assertions passed`）。
  `coop_propview.txt` 特写显示：仓库托盘为紫/琥珀/红色包装箱（`rv_wh_atlas_final.png`）、
  谷仓三层货架为绿/品红/琥珀/蓝色包装箱（`rv_farm_atlas_final2.png`）、
  诊所药柜药盒带包装色块与白色标签（`rv_clinic_atlas_final.png`）、
  湖边木屋货架与叠放补给箱为红/紫/绿/蓝色包装箱（`rv_cabin_atlas_final.png`）。
  glTF 结构确认四处各有 1 个带 `TEXCOORD_0` 的 `*_product` primitive
  （仓库 312、农场 216、诊所 432、木屋 240 个 UV）。
- **验证**：客户端日志按服务器 `seq` 每 2 秒打印；同一 `seq` 的房车坐标、4 名玩家坐标、
  燃油/水/材料/物资箱/家具开关/饥饿完全一致。持久化/重连已用
  “保存 → 重启服务器 → 同名客户端加入” 和 “断开 → 同名重连” 两条链路验证。

## 已知差距（后续）

- 合作状态已写入 `saves/layout.json` 并支持同名重连；但还没有存档版本迁移/冲突合并，
  槽位身份基于 `--name`（局域网轻量身份，无账号鉴权）。
- 合作建造已支持网格放置/拆除（服务器校验边界/重叠/材料）；还不是单机的无网格自由摆放，
  也没有每名玩家的独立家具所有权/权限。家具仍没有物理碰撞。
- 玩家进出房车已支持第一/第三人称切换、角色显隐、真实门洞、独立车门网格铰链开合，
  以及手工关键帧 `DoorOpen`/`Step`/`Board`/`Stand`/`DoorClose`/`Carry` 过渡 +
  `Sit`/`Idle` 循环；门洞是矩形切面，
  切口边缘还没有门框/密封条细节，远端车内角色仍可能被门板/座椅家具部分遮挡。
- 所有 POI 类型（商店/加油站/仓库/农场/商场/湖边营地/诊所）都已映射到可进入的
  glTF 建筑；仅在名称完全无法匹配时才会回退到简易棚屋。
- 所有固定 POI（商店/加油站/仓库/农场/商场/湖边营地/诊所）的内饰都已升级
  （商品墙/货架标识/叉车/收银台/冷柜/药柜/医疗设备/马具/工具/服装架）；
  商店/加油站/仓库/商场/湖边木屋/诊所的前窗已支持透明玻璃，破碎时从 seed 冲击点
  生长径向 + 分支裂纹，并按应力衰减决定碎片大小/速度（每扇约 20–30 块），
  落地后保留为碎屑，破碎状态与 seed 由服务器权威广播；
  但仍是“径向裂环 + 预生成碎片网格”的近似，不是连续介质应力场/裂纹扩展模拟；
  物流仓库卷帘门与谷仓滑动门均可开合，且关闭时会阻挡玩家通行（服务器权威 + 客户端预测一致）；
  商店/加油站/商场/仓库/农场/诊所/湖边木屋的主要商品与补给箱已使用包装图集。
- 房车车体已由 `tools/gen_rv_body.py` 升级为倒角车身、内凹车窗、轮拱、保险杠/
  防擦条、车顶设备与多段式内饰；20 类可摆放家具全部改由
  `tools/gen_rv_furniture.py` 生成多部件 glTF。每个盒体经
  `tools/rv_geom.bevel_box()` 倒角（6 面 + 12 棱边 + 8 角三角），
  两者都已接入 `tools/gen_rv_surfaces.py` 生成 12 种表面 ×（底色 + 法线 + ORM）+
  2 张自发光贴图，共 38 张，每个 primitive 带平面投影 UV 并接入
  法线/粗糙度/金属度/AO/自发光；但几何仍是“倒角盒体/棱柱”组合，
  没有曲面细分、视差贴图或手工建模的高精度资产。
- 地形已有 144×144m 的营地级高度场（4m 网格，文件仅约 7.5KB）与土坡，
  但超出该范围仍是平地，网格分辨率也较粗（4m 面片）；
  房车已按四个车轮接地点采样高度并用弹簧阻尼贴合坡面，
  但四个车轮仍烘焙在同一个车身网格里，无法逐轮独立上下运动/转向，
  刚性车身在扭曲地面上也只能三点着地（第四轮轻微下垂）；
  植被风摆是整体偏航而非逐叶片顶点动画。
- 车辆零件是整体状态，不是可拆装的独立部件。
- 音效是程序合成的占位音，缺少更丰富的环境声/音乐。
