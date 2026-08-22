# 网络层（M4）：LAN demo + 确定性模拟

本文档说明 NeonEngine 的网络分层、如何跑一个真实的局域网双进程 demo（一个 `neon_server` + 两个 `neon_game --connect`），以及本层最重要的验收承诺：**确定性模拟**——服务器权威模拟与客户端本地预测，在相同输入流下逐位一致。

## 1. 网络分层总览

```
                    ┌──────────────────────────────────────────────────┐
  neon_game (client)│  client::ClientSync                                │
                    │   快照缓冲 + 插值 + 预测/回滚（v1: 分歧快照纠正）     │
                    │  PlayerApp --connect 每帧：发 MsgInput → 本地 Tick → │
                    │   收 MsgSnapshot → ClientSync → 纠正受控实体          │
                    └───────────────┬──────────────────────────────────┘
                                    │ UDP（loopback / 局域网）
                    ┌───────────────▼──────────────────────────────────┐
                    │  ReliableChannel（T6.2 传输层）                     │
                    │   帧 = magic+CRC+version+msgId+seq+payload         │
                    │   滑动窗口 ACK / 超时重传 / 乱序重排 / 断线判定       │
                    └───────────────┬──────────────────────────────────┘
                    ┌───────────────▼──────────────────────────────────┐
  neon_server (host)│  GameServer（T6.3 无头权威服务器）                  │
                    │   固定 60Hz Tick → 权威模拟 → 广播 MsgSnapshot      │
                    │   AOI 九宫格（T6.5）→ 每客户端只发其兴趣集内的实体     │
                    │   v1 输入模型：首个登录者 = 输入控制者                │
                    │   v0 匿名账号 + 角色列表占位（T6.6）                  │
                    └──────────────────────────────────────────────────┘
```

四个职责块：

- **传输层**（`neon::net`）：`UdpSocket` + `ReliableChannel`。自研帧格式（版本化序列化 + CRC），滑动窗口可靠投递。`MessageCodec` 对恶意/损坏输入做边界校验。
- **服务器**（`server::GameServer`）：headless、无渲染、权威。复用与客户端相同的 `scene::GameRuntime`（同一份确定性 Lua 沙箱）以固定 60Hz 步进；每 tick 收集控制器输入 → 步进模拟 → 广播快照。
- **兴趣管理（AOI）**（`server::AoiGrid`）：九宫格。客户端快照只含其受控实体周边 `(2r+1)^2` 格内的实体；进入/离开兴趣集以 `MsgSpawn`/`MsgDespawn` 增量通知。
- **客户端同步**（`client::ClientSync`）：快照环缓冲、双快照插值、预测回滚查询。v1 为“分歧即快照纠正”（snap-on-divergence），无多帧重放。

## 2. 局域网双进程 demo（一服务器 + 两客户端）

目录结构约定：服务器与客户端**加载同一个场景 JSON**（`--scene` 传同一文件），这是客户端/服务器同构（shared-state）模型的前提——两边跑同一份脚本/物理/确定性 RNG。

```bat
:: 终端 1：服务器（默认即 --host 模式：绑定并监听；--ticks 可选，跑满 N tick 后退出）
build\neon_server.exe --scene tests\data\neon_server_sample\scene.json --seed 20260821

:: 终端 2：客户端 A（第一个登录 → 输入控制器）
build\neon_game.exe --connect 127.0.0.1:26000 --scene tests\data\neon_server_sample\scene.json --seed 20260821 --name alice

:: 终端 3：客户端 B（第二个登录 → 观察者）
build\neon_game.exe --connect 127.0.0.1:26000 --scene tests\data\neon_server_sample\scene.json --seed 20260821 --name bob
```

- A 先登录成为 **输入控制器**（v1 单控制器模型）：A 的按键/WASD 驱动服务器上唯一的玩家。
- B 随后登录成为 **观察者**：同样收到快照流、能看到 A 的移动；但 B 的输入被服务器忽略（v1 未做多输入模型）。
- 换局域网：把 `127.0.0.1` 换成服务器内网 IP，并给 `neon_server` 去掉 `--loopback`（默认绑定 `0.0.0.0`）。防火墙需放行 UDP 端口（默认 26000）。

无头冒烟（回归）：

```bat
:: 服务器固定跑 120 tick 后退出
build\neon_server.exe --port 26000 --scene tests\data\neon_server_sample\scene.json --ticks 120
:: 客户端 --connect 跑 120 帧；仅当 welcomed + 收到快照 + 受控实体移动时才以 0 退出
build\neon_game.exe --connect 127.0.0.1:26000 --scene tests\data\neon_server_sample\scene.json --ticks 120 --seed 20260821
```

## 3. 确定性模拟验收（核心交付）

**承诺**：在相同输入流下，服务器权威模拟与客户端本地预测产生**完全相同**的状态。

做法（`tests/test_determinism.cpp`）：

1. **脚本化输入流**（`server/src/scripted_input.hpp`）：一个固定、文档化的 `ScriptedInput{tick, MsgInput}` 序列——`0..59` 持续 forward（`moveY=+1`），`60` 松手并按一次 jump（bit0），`90` 按一次 interact（bit3），其余 tick 空闲。该序列**同时**注入服务器（`GameServer::SetScriptedInputs`，无需 socket 客户端）与客户端本地预测（`GameRuntime` + `server::NetInput`）。
2. **纯双模拟对比**（bit-exact 证明）：两边各跑 120 个固定 60Hz tick，比较受控实体最终位置（逐位相等）与**状态哈希**（全部快照实体位置 + 脚本 GameVars，FNV-1a 对原始字节逐位哈希）。两侧哈希必须相等。
3. **活回环变体**（真实客户端 ↔ 真实服务器）：同一脚本流经 UDP 真实投递。因 v1 回滚是“分歧即纠正”（无重放），该变体的保证是**回滚阈值内一致**；在无丢包回环 + 相同输入下预测逐 tick 精确跟踪服务器，无纠正发生。**bit-exact 的证明以纯双模拟为准**——活回环在有网络抖动/丢包时会被纠正机制掩盖。

### 为什么能逐位一致

- **固定 tick**：服务器 `Step` 与客户端 `Tick(1/60)` 使用同一固定步长与累加器语义。
- **同一份确定性沙箱（T2.4）**：`math.random`/`NMath` 由同一确定性 RNG（`SetRngSeed`，固定默认种子 `20260821`）驱动；`os.time`/`os.clock` 为引擎注入时钟。服务器与客户端必须传**相同 `--seed`**。
- **同构运行**：客户端与服务器复用同一个 `scene::GameRuntime` + 同一场景 JSON + 同一 Lua 脚本。
- **稳定迭代序**：ECS 视图遍历与 AOI `InterestSet` 均为确定性顺序（升序实体 id / 行主序格子），快照流可复现。

> 服务器默认种子与客户端默认种子都是固定常量 `20260821`，但显式 `--seed` 可覆盖；demo 中两边务必一致。

### 生产化前仍缺（见 ROADMAP M4 之后的工程项）

- **增量/增量编码（delta encoding）**：当前每 tick 全量快照，带宽随实体数线性涨；生产需要按兴趣集做 delta/掩码。
- **真实认证**：v0 匿名账号 = 计数器自增，无凭据校验；需替换为 token/凭据交换。
- **多控制器/多玩家**：v1 只有一个输入控制器；多玩家需要每实体输入路由（`entityId` 维度的 `MsgInput`）。
- **RTT 自适应**：插值延迟（当前固定 6 tick）应按 RTT 动态调整；回滚应重放（多帧 re-simulation）而非快照纠正。
- **防作弊**：当前服务器接收任何控制器输入；确定性种子下发与输入时序校验留给后续。
