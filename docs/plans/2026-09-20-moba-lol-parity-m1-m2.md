# NeonMOBA 高仿 LoL —— M1+M2 实施计划

> **For Claude:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task.

**Goal:** 给 MOBA demo 加上 LoL 式相机操作手感（M1）与核心对局规则（M2）。

**Architecture:** 全部在 `projects/moba/assets/scripts/moba.lua`（+ `projects/moba/input.json`）实现，不改引擎。验证用编辑器自动 Play（`--play --frames`）+ `neon_log.log` + 截图。

**Tech Stack:** Lua 脚本（引擎绑定）、ImGui 无关、粒子 telegraph 做指示器。

---

## 约定 / 命令

- 构建目录：`build/mingw-ninja`
- 自动验证（脚本无语法/运行错误）：
  `build\mingw-ninja\neon_editor.exe --project projects\moba --scene assets/scenes/moba.json --play --frames 320`
  然后检查 `neon_log.log` 是否出现 `moba.lua` / `attempt to`。
- 提交：每个任务一次 commit。

---

## M1 相机与操作手感

### Task 1: 相机输入动作

**Files:**
- Modify: `projects/moba/input.json`

**Step 1:** 增加动作
```json
    "attack": ["A"],
    "ping": ["G"],
    "camera": ["Y"]
```

**Step 2:** 提交
```
git add projects/moba/input.json
git commit -m "feat(moba): input 增加 camera(Y) 动作"
```

### Task 2: 锁定/解锁相机 + 边缘平移

**Files:** `projects/moba/assets/scripts/moba.lua`（`updateCameraFollow`）

- 顶部常量区加 `local camFollow = true`。
- `updateCameraFollow(dt)` 改为：
  - `camFollow` 为真：聚焦向英雄插值（现状）。
  - `ActionPressed("camera")` 切换 `camFollow`。
  - `ActionPressed("center")`：`camFollow=true` 且聚焦回英雄。
  - 非锁定：读鼠标位置，进入边缘带按其屏幕方向平移 `camFocus`；方向用 yaw 基
    `rx,rz=(-cosφ, sinφ)`、`ux,uz=(sinφ, cosφ)`；`camFocus` clamp 到 `[-70,70]`。
- 末尾保留 `CAM.dist` 滚轮缩放 + `applyCamera()`。

**验证:** 自动 Play 无脚本错误；解锁后拖到边缘视角移动、`Y` 切回跟随、`空格` 归位。

**提交:** `git commit -m "feat(moba): Y 解锁/锁定相机 + 屏幕边缘平移"`

### Task 3: 施法指示器（按住显示，松开释放）

**Files:** `moba.lua`（`updatePlayer` 施法段 + 新增 `drawSpellIndicator`）

- 新增 helper：按住时按技能类型发地面 telegraph 粒子（低数量、短生命，每帧重发）：
  - `projectile`/`melee`：从英雄朝指针方向铺一条粒子带（长度 `ab.range`）。
  - `aoe_target`/`aoe_self`：在指针/英雄处画半径圆（沿圆周发一圈粒子）。
- `updatePlayer` 中把 `ActionPressed("spell"..i)` 改为：
  - `ActionDown("spell"..i)` → `drawSpellIndicator(h,i,aimX,aimZ)`
  - `ActionReleased("spell"..i)` → `castAbility(h,i,aimX,aimZ)`

**验证:** 按住 Q 出现指示线；松开在指针处放技能；点击视野外回退前向。

**提交:** `git commit -m "feat(moba): QWER 施法指示器（按住显示/松开释放）"`

### Task 4: A 显示攻击范围 + 目标选中框

**Files:** `moba.lua`（`updatePlayer`、`drawWorldPlates` 或 `on_render`）

- 按住 `ActionDown("attack")` 时绕英雄画攻击范围圈（telegraph 粒子）。
- `h.target` 有效时，在其屏幕位置画 `DrawRectOutline` 小方框（选中框）。

**验证:** 按住 A 出现范围圈；点敌人后其头顶有框。

**提交:** `git commit -m "feat(moba): A 攻击范围圈 + 目标选中框"`

---

## M2 对局核心规则

### Task 5: 技能加点

**Files:** `moba.lua`（`spawnHero`、`grantXp`、`castAbility`、`updatePlayer`、`drawHud`）

- `spawnHero`：`skillPoints = 1, ranks = {0,0,0,0}`。
- `grantXp` 升级时 `h.skillPoints = h.skillPoints + 1`。
- `updatePlayer`：`InputKey("ctrl") == 1 and ActionPressed("spell"..i)` → 加点
  （`skillPoints>0` 且 `ranks[i] < maxRank`，R 上限 3 其他 5）；加点时不施法。
- `castAbility`：`ranks[idx] <= 0` → 提示"未学习"，直接 return。
- 数值随等级缩放：`dmg = ab.dmg * (0.6 + 0.4*rank)`、`cd = ab.cd * (1 - 0.05*(rank-1))`。
- `drawHud`：技能图标角标显示 `rank/maxRank`，未学习置灰；在技能栏上方显示剩余技能点。

**验证:** 开局 1 点；Ctrl+Q 学会 Q 并消耗 1 点；未学技能按 Q 提示未学习；升级得点。

**提交:** `git commit -m "feat(moba): 技能加点（Ctrl+QWER）+ 等级缩放 + HUD"`

### Task 6: 防御塔仇恨

**Files:** `moba.lua`（`damage`、新增 `updateTower`、`on_update` 塔分支）

- `damage(target, amount, source)`：当 `source.isHero and target.isHero and source.team~=target.team`
  时，给 `target.team` 方、范围内且存活的塔设 `towerTarget=source, towerAggroT=3`。
- 新增 `updateTower(u, dt)`：
  - `towerAggroT` 递减；目标死亡/出范围则清。
  - `u.target = u.towerTarget`（若有），否则 `nearestEnemy(u, u.range, true)`（优先小兵）。
  - `autoAttack(u, dt)`。
- `on_update` 里塔的分支由 `autoAttack(u,dt)` 改为 `updateTower(u,dt)`。

**验证:** 英雄攻击敌方英雄后，在塔范围内会被塔转火；否则塔只打小兵。

**提交:** `git commit -m "feat(moba): 防御塔仇恨优先级（小兵优先，攻击我方英雄才转火英雄）"`

### Task 7: 回城被打断 + 进度条

**Files:** `moba.lua`（`damage`、`drawHud`）

- `damage`：`target.isHero and (target.channel or 0) > 0 and amount > 0` → `target.channel = 0`。
- `drawHud`：`h.channel > 0` 时在中下方画进度条（1.4s 归一）。

**验证:** 按 B 读条，期间受击立即取消并提示；进度条可见。

**提交:** `git commit -m "feat(moba): 回城受击打断 + 读条进度条"`

### Task 8: 连杀悬赏 + 复活倒计时

**Files:** `moba.lua`（`killUnit`、`respawnHero`、`drawHud`）

- 英雄击杀英雄：`source.streak = (source.streak or 0) + 1`；
  `gold += 300 + math.min(source.streak - 1, 5) * 50`。
- 被击杀方：`victim.streak = 0`。
- `drawHud` 死亡态已有"复活中"，补：`h.kills` 连杀提示（如 `x3`）。

**验证:** 连杀金币递增；死亡复活倒计时显示。

**提交:** `git commit -m "feat(moba): 连杀悬赏金币 + 复活倒计时"`

---

## 完成标准

- 自动 Play 无脚本/运行错误；`neon_log.log` 无 `moba.lua`/`attempt to`。
- M1/M2 全部任务独立提交。
- 引擎未改动（无需重编译）。
