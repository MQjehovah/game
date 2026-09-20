# NeonMOBA 联机（C 观战 → A 指令操控）设计与计划

日期：2026-09-20
状态：方向已确认（先 C 后 A；指令走 Rpc；AI 与双真人都要）

## 现状（实测）

- `neon_server` 能直接跑 MOBA 场景（1200 tick 无脚本错误，自动选人并模拟）。
- `neon_game --connect host:port --scene <moba.json>` 能连上：收到 Welcome + 快照（实测 401 个、缓冲 8），但 `controlledMoved=0` 触发 connect smoke 断言失败。
- 引擎已有：权威 server（60Hz/UDP/AOI）、客户端快照插值+预测回滚（`client_sync`）、`Rpc` 双向通道、`MsgInput`（仅盘/轴，无鼠标点选）、快照只含 transform。
- 缺：观战模式；MOBA 指令模型；HUD 状态复制。

## C 观战（本阶段）

**目标**：一个客户端连到运行中的服务器，看到真实对局（服务器/AI 在打），不操控、不本地跑玩法。

**做法**：
1. `neon_game` 增加 `--spectate`：连接后只接收快照并插值渲染，**不发送输入**、**不做本地预测**、**不要求受控实体**、**跳过 connect smoke 断言**。
2. 客户端仍加载同一场景（地形/相机）以渲染；单位实体按快照 id 覆盖（与服务器同脚本生成的 id 对齐）。
3. 服务器：把每帧 `world.hash` 这类调试 RPC 降级/限流（现每帧刷屏），或客户端静音该类 RPC。

**验收**：`neon_server` 起一局；`neon_game --spectate --connect` 能看到小兵/英雄移动推进、塔攻击等（截图 + 日志）。

## A 指令操控

**目标**：真人客户端操控自己的英雄；可 1v1（对服务器 AI）或 2 个真人客户端同局。

**协议（走 `Rpc`，JSON）**
- 客户端→服务器：`moba_cmd`，字段 `seq, clientId, type,` 及按类型的 `{x,z}`（地面点）/`targetId`（实体）/`ability`（1-4）。
  - type: `move` `attackMove` `attackTarget` `cast` `ping` `recall` `levelup` `stop`
- 服务器→客户端：`moba_state`（周期），字段 `tick,` `heroes[]`（id、hp、maxHp、mana、level、kills、deaths、cs、gold、cd[4]、ranks[4]）、`kills[]`。
- 复用既有快照链路复制单位 transform（服务器权威）。

**服务器侧（moba.lua 网络模式）**
- 新增 `NET` 标记：`on_start` 读到网络模式时，`updatePlayer` 改为从**命令队列**取本客户端英雄的指令并执行（无指令则待机），不再读本地鼠标/键盘。
- 指令入队：服务器收到 `moba_cmd` → 存入 `pending[clientId]`；`updatePlayer` 消费。
- 服务器每 N tick 广播 `moba_state`。
- 其余（小兵、野怪、塔、AI 英雄）不变，服务器权威模拟。

**客户端侧（观战/操控）**
- 输入：右键/A/G/QWER/Ctrl 产生命令 → `Rpc("moba_cmd", ...)`。
- 显示：单位走快照插值；HUD（血/蓝/冷却/金币等）来自 `moba_state`。
- 预测：v1 先不预测（跟随服务器，~100ms 延迟可接受）；A2 再加本地预测+回滚（引擎已有 `NeedsReconcile`，可升级为多帧重放）。

**双真人**
- 服务器维护 `clientId → hero 实体`映射：Join 时分配一方（BLUE/RED）。第一名进 BLUE、第二名进 RED；只有 AI 时服务器自控 RED（现有 AI）。
- 服务器通过 `moba_cmd` 的 `clientId` 决定操控哪个英雄。

## 任务拆解

### C-1 观战模式
- 文件：`game/src/player.cpp`、`player.hpp`（PlayerConfig 加 `spectate`）、`game/src/player_main.cpp`（`--spectate` 解析）。
- 连接后不 `SendInputPacket`、不 `ReconcileControlled`、不要求受控实体；`connect smoke` 断言在 spectate 下跳过。
- 验收：`--spectate --connect` 收到快照且渲染推进。

### C-2 静音调试 RPC
- 服务器 `world.hash` 限流（每 N tick）或客户端 `--log-cat net:info` 降噪。

### A-1 指令协议
- 引擎：无需改协议（用 `Rpc` 名称+JSON）。
- moba.lua：`Rpc` 回调注册 `moba_cmd`（服务器）/发送（客户端）；新增 `NET` 模式分支。

### A-2 服务器命令队列 + 状态广播
- `updatePlayer` 网络分支消费命令；周期广播 `moba_state`。

### A-3 客户端 HUD 由 `moba_state` 驱动 + 命令发送
- 客户端 on_render 用 `moba_state` 画 HUD；输入转命令发送。

### A-4 双客户端分配
- Join 分配 BLUE/RED；`clientId → hero` 映射。

### A2（后续）本地预测 + 回滚重放

## 验证
- C：服务器 + `--spectate` 客户端，截图/日志确认对局推进。
- A：服务器 + 真人客户端，点击移动/技能生效；两客户端同局各控一方。
- `ctest` 不新增失败。
