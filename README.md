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
| Vulkan 渲染后端 | ⏳ 接口已定义，实现见 [VULKAN_ROADMAP.md](docs/VULKAN_ROADMAP.md) |
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
| 音频（miniaudio 软件混音器 + 程序化生成音效/音乐） | ✅ miniaudio（三平台统一；Windows 失败回退 WinMM，其余 Null） |
| 资源管线（stb_image、OBJ+MTL、glTF 2.0 导入、Kenney CC0、程序化生成） | ✅ |
| glTF 2.0：JSON DOM 解析器、PBR 材质、节点变换（Khronos 样例模型验证） | ✅ |
| 单元测试（13 项：数学/ECS/配置/RNG） | ✅ |
| 截图/冒烟测试（`--smoke-test` / `--screenshot`） | ✅ |

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

编辑器采用停靠式布局：左侧"场景/资产/资源"页签，右侧"属性/日志"页签，中央 3D 视口（右键旋转、中键平移、滚轮缩放、左键拾取）。场景面板增删/复制/排序实体；资产面板浏览项目文件、双击导入 OBJ/glTF、预览贴图；属性面板编辑变换/颜色/金属度/粗糙度；日志面板分级过滤引擎日志。工具栏保存/加载场景（`editor_scene.json`）、快速添加实体、播放动画。视图菜单可开关各面板、"引擎 UI 演示"（自研控件树）与 ImGui Demo。

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
game/
  src/                 # NeonRealm demo（ECS 玩法系统/程序化美术/音效）
tests/                 # 单元测试
third_party/           # stb、字体（均已 vendored）
docs/                  # 架构/路线图/规范文档
```

详细设计见 [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md)，路线图见 [docs/ROADMAP.md](docs/ROADMAP.md)。

## 许可

- 引擎与 demo 代码：MIT（见 [LICENSE](LICENSE)）。
- 第三方：stb（public domain / MIT）、Press Start 2P（SIL OFL 1.1，见 `third_party/fonts/`）。
