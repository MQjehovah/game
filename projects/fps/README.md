# NeonOps

一个面向“可上线规模”展示的第一人称射击游戏样例，不是功能验证 demo。它把引擎的数据驱动、UI、输入映射、音频、存档、场景切换、投射物、敌人生成、HUD 和完整游戏流程串成一条可验收的产品切片。

## 游戏循环

- 主菜单：开始任务、设置（鼠标灵敏度、音乐、音效）、持久化到 `save.json`
- 三个关卡：`Perimeter Breach` -> `Sector Hold` -> `Command Core`
- 武器系统：突击步枪 / 战术 SMG / 霰弹枪 / 狙击枪，弹药、换弹、散布、射速、伤害均由 `assets/data/weapons.json` 驱动（第一人称模型由脚本程序化生成的 OBJ 提供）
- 敌人系统：无人机 / 炮台 / 重型单位 / 自爆 Sapper / 精英 Juggernaut，属性由 `assets/data/enemies.json` 驱动
- 波次导演：`assets/data/levels.json` 定义每关掩体、敌人生成节奏（最终关有 Boss 波）
- 拾取物：敌人概率掉落血包 / 弹药
- 打击感：爆头判定 ×2 伤害、命中伤害飘字、命中/受击反馈音、连杀 COMBO 倍率、击杀粒子/浮字
- 暂停、重开、任务失败、通关、主菜单返回
- 最高分和设置持久化

## 操作

- WASD / 方向键：移动
- 鼠标：视角
- 左键：射击
- R：换弹
- 1/2/3 或 Q：切换武器
- Space：跳跃
- Shift：冲刺
- P：暂停
- Enter：重开 / 继续

## 运行

```bat
.\build\neon_editor.exe --project projects\fps
```

在编辑器里如果直接按播放，会从主菜单场景开始；点击 `START MISSION` 进入第一关。若只想直接验证射击，先打开 `assets/scenes/level_01.json`，再按 `F5` 播放。

打包运行：

```bat
.\build\neon_editor.exe --package projects\fps build\fps_out
.\build\neon_game.exe --pack build\fps_out\game.pack
```

## 结构

- `assets/scenes/menu.json`：主菜单场景
- `assets/scenes/level_01.json` 等：三个关卡场景
- `assets/scripts/menu.lua`：菜单、设置、存档
- `assets/scripts/game.lua`：FPS 核心玩法
- `assets/data/*.json`：武器、敌人、关卡数据
- `assets/ui/*.ui.json`：菜单、HUD、暂停、结算界面
- `assets/prefabs/*.json`：敌人、掩体、拾取物预制体
