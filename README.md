# NeonEngine

一个面向大型 3D 网络游戏（类《魔兽世界》）设计的 C++17 游戏引擎。本项目包含：

- **引擎**：平台无关的模块化核心（数学库、ECS、场景、物理、资源、UI、音频）+ 可插拔的平台/渲染后端。
- **Demo**：第三人称动作游戏《NeonRealm》，用于验证引擎全链路（程序化美术、程序化音效、波次战斗、HUD、存档）。
- **编辑器**：3D 场景编辑器（`neon_editor.exe`），Dear ImGui 停靠式工具 UI，内置场景/资产/资源/属性/日志五面板，并带"引擎 UI 演示"窗口对照自研控件树。
- **文档**：架构说明、实现路线图、代码规范（见 `docs/`）。

## 特性

| 模块 | 状态 |
| --- | --- |
| 自研数学库（Vec2/3/4、Mat4、Quat、Transform、Ray/AABB） | ✅ |
| ECS（SparseSet 实体-组件-系统） | ✅ |
| 固定步长游戏循环（60Hz）+ 可变渲染 | ✅ |
| 跨平台窗口/输入抽象（`IWindow`/`IInput`） | ✅ Win32 已实测；X11/Cocoa 代码就绪，CI 验证 |
| OpenGL 3.3/4.x 渲染后端（自研 GL 加载器，无 GLEW/glad） | ✅ 已实测（Intel 4.6） |
| Vulkan 渲染后端 | ⚠️ 实验性：可构建/运行/截图，但渲染结果为灰度（顶点色/材质未正确应用），待修复；GL 为默认后端 |
| 3D 渲染：相机、方向光+点光、雾、网格、调试线框 | ✅ |
| PBR 材质：Cook-Torrance BRDF（金属度/粗糙度/AO/自发光） | ✅ |
| 阴影：投影阴影（CPU 接触阴影，兼容损坏深度缓冲的驱动） | ✅ |
| 实例化渲染 + 视锥剔除（大批树木/岩石单次 draw call） | ✅ |
| 高度图地形网格（程序化噪声地形 + 顶点色） | ✅ |
| 深度缓冲可用性检测，不可用时自动降级画家算法排序 | ✅（兼容问题驱动） |
| 字体渲染（stb_truetype 图集，像素字体内嵌 + 系统 CJK 字体） | ✅ 中文 UI |
| 控件树 UI 系统（布局/命中测试/焦点/拖拽/Window/List/TextField/Slider） | ✅ 编辑器实测 |
| 2D 立即模式 UI（按钮/面板/进度条，HUD） | ✅ |
| Dear ImGui 工具层（docking 停靠布局，CJK 中文） | ✅ `neon_imgui` 模块，编辑器实测 |
| 编辑器五面板：场景/资产/资源/属性/日志 | ✅ 停靠、可开关、位置持久化 |
| 资产面板：目录浏览/模型导入/图片预览（跨平台 UTF-8） | ✅ |
| 资源面板：已加载纹理/网格/字体统计 | ✅ `AssetManager::Stats` |
| 日志面板：引擎日志环形缓冲 + 分级过滤 | ✅ `core::GetRecentLogs` |
| 3D 场景编辑器（场景 JSON 保存加载 + 拾取/相机 + glTF/OBJ 导入） | ✅ `neon_editor.exe` |
| 引擎控件树深化（TreeView/ComboBox/TabBar/DockLayout/ScrollArea） | ✅ 编辑器演示窗口 |
| UI 自动化冒烟测试（ImGui + 自研控件命中/点击/状态校验） | ✅ `--smoke-test` |
| 粒子系统（3D 公告板粒子） | ✅ |
| 物理（动态球 vs 静态 AABB + 重力 + 射线） | ✅ 简易内置，可替换 Jolt/Bullet |
| 资源管线（stb_image、OBJ+MTL、glTF 2.0 导入、Kenney CC0、程序化生成） | ✅ |
| glTF 2.0：JSON DOM 解析器、PBR 材质、节点变换（Khronos 样例模型验证） | ✅ |
| 单元测试（13 项：数学/ECS/配置/RNG） | ✅ 现 461 项 |
| 截图/冒烟测试（`--smoke-test` / `--screenshot`） | ✅ |
| 网络：UDP 传输 + 可靠通道（ACK/重传/乱序重排）、版本化消息编解码 | ✅ `neon::net` |
| 网络：无头权威服务器 `neon_server`（固定 60Hz + 确定性沙箱） | ✅ |
| 网络：快照插值 + 预测回滚 + AOI 九宫格兴趣管理 | ✅ |
| 网络：v0 匿名登录 + 角色选择占位 | ✅ |
| 网络：确定性模拟验收（服务器权威模拟 ≡ 客户端本地预测） | ✅ `tests/test_determinism.cpp` |
| 网络：多玩家输入模型（每客户端一个角色；场景定义 `on_player_join` + `BindPlayerToClient`） | ✅ |
| 骨骼动画：glTF 蒙皮导入 + 动画状态机（含过渡/混合）+ GPU 蒙皮着色 | ✅ |
| 阴影映射：方向光 CSM + 点光 cubemap（颜色编码深度，兼容损坏深度缓冲的驱动） | ✅ |
| 后处理：HDR 浮点管线 + Bloom + ACES 色调映射 + MSAA | ✅ |
| IBL 环境光（天空梯度 → 辐照度/预过滤/BRDF LUT） | ✅ |
| LOD 资产链（按距离切换网格，数据驱动） | ✅ |
| 世界分区流式（chunk 3×3 窗口异步加载/卸载） | ✅ |
| ECS 批量迭代 + 确定性并行 job（串行/并行逐位一致） | ✅ |
| 脚本：Lua 5.4 沙箱（确定性 RNG/时钟）+ 引擎绑定 + 行为树引擎 | ✅ `neon::script`/`neon::bt` |
| 脚本：按实例函数捕获（跨 chunk 不再互相遮蔽）+ `Spawn(kind, pos, script)` 动态控制器 | ✅ |
| 战斗核心（M2）：状态效果（燃烧/中毒/回血，确定性 tick）+ 数据驱动技能表（投射物/近战扇形/朝向攻击盒，冷却/法力/命中附加效果） | ✅ |
| 2D 脚本画布：Lua `on_render()` + `DrawRect/DrawRectOutline/DrawText/DrawSprite`（1280x720 设计坐标 + 贴图），数据驱动 2D 游戏零 C++ 玩法代码 | ✅ |
| 编辑器 2D 模式：9x5 草坪画布（5 种植物/橡皮/3 种僵尸笔刷），保存/加载关卡 JSON；试玩按钮/F5 统一（3D 与 2D 同一入口） | ✅ `neon_editor` |
| 数据驱动 3D 游戏：`projects/neon_realm`（魔兽风格 demo 移植：村庄/狼群/波次/任务对话/存档，全 Lua） | ✅ |
| 输入映射（Godot 式）：动作名→按键 JSON（项目 input.json）+ `ActionDown/ActionPressed/ActionAxis` 绑定 + 编辑器改键面板 | ✅ `neon::script::InputMap` |
| 组件 Schema（Godot @export / UE UPROPERTY 风格）：组件字段元数据 → 属性面板自动生成编辑器，任意自定义组件（含 plant/zombie）可编辑 | ✅ `neon::scene::ComponentSchema` |
| 预置体工作流（Godot 场景实例化风格）：`prefabs/*.json` 模板 + 编辑器"插入预置体/另存为预置体"，场景实体带 prefab 引用与实例覆盖 | ✅ 冒烟覆盖 |
| 导航寻路：2D 可行走网格 `NavGrid` + A*（八方向、绕墙、JSON 资产往返）+ 编辑器导航面板（格子编辑/起点终点/路径预览） | ✅ `neon::nav` |
| 本地化（Godot 风格）：多语言字符串表 JSON + 脚本 `Loc(key)`（激活→默认→键 回退链）+ 编辑器本地化面板 | ✅ `neon::core::Localization` |
| 导出配置：`game.json` 的 `export` 预设（platform/icon/description）+ 打包面板可视化编辑；manifest 接受 `editor` 元数据字段 | ✅ |
| 材质球资产（Unity .mat / Godot Material 风格）：`materials/*.mat.json`（颜色/金属度/粗糙度/AO/自发光/四张贴图），资产面板新建/过滤/拖拽到实体，属性面板"另存为材质球"，场景导出带引用并展开 | ✅ `materialRef` |
| 内置脚本编辑器（Godot 风格）：资产面板/脚本面板双击 `.lua` 打开，多行编辑 + Ctrl+S 保存 + 实时语法检查（行号/错误信息），一键"外部编辑器打开"（系统默认编辑器） | ✅ `脚本编辑器` 面板 |
| 场景树（Godot 式）：实体 `transform.parent` 父子变换继承 + 编辑器树形层级面板 + 拖拽排父子 | ✅ |
| 信号系统：`SignalConnect(name, fn)` / `SignalEmit(name, arg)`（支持局部闭包）+ 多场景 `ChangeScene(path)` | ✅ |
| 数据驱动场景：组件化 JSON + 预制体 + `game.json` 清单 | ✅ |
| 编辑器深化：gizmo / 撤销重做 / 材质编辑器 / 行为树可视化 / 脚本面板 / 缩略图 / 多相机 / 热重载 / 性能面板 | ✅ |
| 一键打包 + 通用播放器：`neon_editor --package` → `game.pack` → `neon_game` 运行数据驱动游戏 | ✅ 编辑→打包→运行闭环 |
| 音频：miniaudio 软件混音器 + 程序化音效/音乐 | ✅ 三平台统一，Windows 失败回退 WinMM |

## 构建

依赖只有：**CMake ≥ 3.15** 与 **支持 C++17 的编译器**。第三方库（stb、字体）已 vendored 进仓库。

### Windows（MinGW 或 MSVC）

```bat
cmake -S . -B build -G "MinGW Makefiles" -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
.\build\neon_rush.exe
```

MSVC：把 `-G "MinGW Makefiles"` 换成 `-G "Visual Studio 17 2022"` 即可。

### macOS

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
./build/neon_rush
```

### Linux

需要 X11 与 GL 开发包（Debian/Ubuntu）：

```bash
sudo apt install build-essential cmake libx11-dev libgl1-mesa-dev
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
./build/neon_rush
```

### 测试

```bash
cmake --build build --target neon_tests -j
./build/neon_tests
```

### 编辑器

```bash
./build/neon_editor                    # 启动场景编辑器
./build/neon_editor --smoke-test 120   # 运行 UI 交互冒烟测试（失败退出码为 1）
./build/neon_editor --screenshot out.png 60
```

编辑器采用停靠式布局：左侧"场景/资产/资源"页签，右侧"属性/日志"页签，中央 3D 视口（右键旋转、中键平移、滚轮缩放、左键拾取）。场景面板增删/复制/排序实体；资产面板浏览项目文件、双击导入 OBJ/glTF、预览贴图；属性面板编辑变换/颜色/金属度/粗糙度；日志面板分级过滤引擎日志。

**项目切换（Godot 风格）**：工具栏左侧是项目选择器，自动扫描 `projects/` 下带 `game.json` 的项目（如 NeonRealm 3D、NeonPvZ 2D），右侧是场景选择器。切换项目会按 `game.json` 的 `editor.mode` 进入对应编辑视图（3D 场景树或 2D 画布）并加载起始场景；上次打开的项目会从 `neon_editor_config.json` 恢复。试玩（`▶ 试玩`）直接运行当前项目的场景。"项目"菜单可手输任意项目目录、重新加载或导出场景。

**2D 与 3D 统一由场景维护**：场景文件（`scenes/*.json`）是唯一的编辑/运行单元，与 2D/3D 无关——一关就是一个场景。植物、僵尸和 3D 实体一样是场景里的实体，带 `plant`/`zombie` 组件；编辑器 2D 画布用格子视图编辑这些实体并写回场景文件，运行时 `pvz.lua` 从同一个场景文件读取——"关卡"是游戏概念（关卡列表、波次表由项目脚本/数据表达），引擎只有场景。

## 运行与验证

```bash
# 正常运行
./build/neon_rush

# 无头冒烟测试（跑 N 个固定步长后自动退出，0 表示成功）
./build/neon_rush --smoke-test 600

# 指定帧截图（PNG）
./build/neon_rush --smoke-test 400 --screenshot shot.png 300
```

### Demo《NeonRealm》操作

| 输入 | 动作 |
| --- | --- |
| WASD / 方向键 | 移动（相对相机） |
| 鼠标拖动 | 环绕相机 |
| 左键 | 近战攻击（60° 扇形） |
| 右键 | 冲刺（带无敌帧） |
| 空格 | 跳跃 |
| Esc | 暂停 / 返回 |

目标：在开放世界中帮助村长猎杀野狼、完成主线任务，升级并收集金币。
存档自动写入 `neon_realm_save.dat`（等级/经验/金币）。

### 素材与许可

- 模型：Kenney Nature Kit（CC0）+ DamagedHelmet（Khronos glTF 样例，CC-BY 4.0）。
- 中文 UI 字体：运行时加载系统字体（Windows 微软雅黑 / macOS 苹方 / Linux Noto CJK）。

### 局域网双进程 demo（一服务器 + 两客户端）

```bat
:: 终端 1：服务器（默认即 --host 模式）
build\neon_server.exe --scene tests\data\neon_server_sample\scene.json
:: 终端 2：客户端 A（先登录 → 输入控制器）
build\neon_game.exe --connect 127.0.0.1:26000 --scene tests\data\neon_server_sample\scene.json --name alice
:: 终端 3：客户端 B（后登录 → 观察者，只收快照）
build\neon_game.exe --connect 127.0.0.1:26000 --scene tests\data\neon_server_sample\scene.json --name bob
```

### 数据驱动 2D 游戏（NeonPvZ，植物大战僵尸原型）

玩法与绘制全部在 Lua 脚本里（`projects/pvz/scripts/pvz.lua`），精灵贴图在 `projects/pvz/assets/sprites/`，关卡布局由编辑器 2D 模式以场景实体（`plant`/`zombie` 组件）摆放在场景文件 `projects/pvz/scenes/pvz.json`：

```bat
:: 1) 打开编辑器，工具栏点"2D模式"（或 --2d 启动）：摆植物/僵尸刷怪点，F5 或 ▶ 试玩直接开打
.\build\neon_editor.exe --2d
:: 2) 打包项目
.\build\neon_editor.exe --package projects/pvz build\pvz_out
:: 3) 用通用播放器运行
.\build\neon_game.exe --pack build\pvz_out\game.pack
```

操作：点击卡牌选择植物（向日葵/豌豆/寒冰/坚果/樱桃炸弹）→ 点击草坪格子种植；点击阳光收集；
僵尸（普通/路障/铁桶）到达房屋左侧时该行推草机触发，无推草机则失败；胜负后按 Enter 重新开始。

### 数据驱动 3D 游戏（NeonRealm，魔兽风格 demo 移植）

村庄、狼群、波次、对话与 HUD 全部在 `projects/neon_realm`（场景 JSON + Lua），
程序化 mesh 由引擎生成，运行方式与 PvZ 完全一致：

```bat
:: 编辑器打开项目场景并 F5 试玩（统一试玩按钮）
.\build\neon_editor.exe --project projects/neon_realm
:: 或直接打包运行
.\build\neon_editor.exe --package projects/neon_realm build\realm_out
.\build\neon_game.exe --pack build\realm_out\game.pack
```

操作：WASD 移动（相机相对）、左键近战、1 火球、2 治疗、右键冲刺、空格跳跃；
靠近村长按 F 对话；击败狼群获得经验/金币并触发下一波。

核心承诺：**确定性模拟**——服务器权威模拟与客户端本地预测在相同输入流下逐位一致（`tests/test_determinism.cpp` 哈希验收）。详见 [docs/NETWORKING.md](docs/NETWORKING.md)。

## 项目结构

```
engine/
  include/neon/        # 引擎公共头文件（按模块分层）
  src/                 # 引擎实现
    platform/win32     # Win32 窗口/输入/GL 上下文
    platform/x11       # X11/GLX（Linux）
    platform/cocoa     # Cocoa/NSOpenGL（macOS）
    gfx/gl             # OpenGL 后端 + 自研 GL 加载器
    gfx/vulkan         # Vulkan 后端（占位）
    audio/miniaudio      # miniaudio 混音器（T5.1，跨平台）
    audio/winmm          # WinMM 混音器（Windows 回退）
    net/               # UDP 传输 + 可靠通道 + 协议编解码
game/
  src/                 # NeonRealm demo + 数据驱动玩家（neon_game，含网络客户端）
server/
  src/                 # 无头权威服务器 neon_server（GameServer + AOI）
editor/
  src/                 # 场景编辑器 neon_editor（ImGui 工具 UI）
tests/                 # 单元测试
third_party/           # stb、字体、lua、imgui（均已 vendored）
docs/                  # 架构/路线图/网络说明/规范文档
```

详细设计见 [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md)，路线图见 [docs/ROADMAP.md](docs/ROADMAP.md)。

## 许可

- 引擎与 demo 代码：MIT（见 [LICENSE](LICENSE)）。
- 第三方：stb（public domain / MIT）、Press Start 2P（SIL OFL 1.1，见 `third_party/fonts/`）。
