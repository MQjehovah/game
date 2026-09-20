# NeonMOBA 服务器多局并行 + 大厅打磨 设计

日期：2026-09-20
状态：待实施（大改服务器）

## 现状

`GameServer` 是**单局**：`runtime_` / `kernel_` / `controllerInput_` /
`entityClientIds_` / `grid_` / `tick_` / `packVfs_` 全是单实例；客户端连上即被这一个
runtime 服务。大厅（房间）只是标签，`StartRoomMatch` 用唯一 runtime 开局。

`tests/test_server.cpp` 依赖"启动即有单局、Step 即产出快照"的行为，重构必须保留。

## 目标

1. **多局并行**：一台服务器同一进程内跑多局，每局一个独立模拟（runtime+物理+
   脚本+AOI+快照+输入），按房间隔离。
2. **大厅打磨**：点击整行加入、自动刷新、房主踢人/转让、准备状态等。

## 多局并行设计

### 核心：抽出 `Match`（每局一份）
把现有单局内部状态封装为 `Match`：
- `scene::GameRuntime runtime`
- `std::unique_ptr<kernel::Kernel> kernel`（物理+脚本服务，脚本钩子按本局房间
  作用域：`rpcCall` 只广播给本局客户端，`inputForEntity`/`bindPlayerToClient`
  用本局的 `entityClientIds`）
- `NetInput controllerInput` + `std::map<uint64_t, NetInput> clientInputs`
- `std::unordered_map<uint64_t,uint64_t> entityClientIds`
- `AoiGrid grid`
- `uint32_t tick`、`uint64_t accumulator`
- `std::set<uint64_t> clients`（clientId 集合，房间成员）
- `std::string room`

方法：`Start(sceneJson, scenePath, cfg)`、`Step(dt)`、`BroadcastSnapshot(clients)`、
`SetClientInput(clientId, MsgInput)`、`CallOnMatchStart(blue, red)`。

### `GameServer` 变成管理器
- `clients_`（全局，按地址）。
- `std::unique_ptr<Match> defaultMatch_`：`Start` 时按 cfg 场景创建，**保留给
  非房间客户端/测试**（行为与旧版一致）。
- `std::map<std::string, std::unique_ptr<Match>> matches_`：房间 → 局。
- 客户端"归属局"：`Client.room` 有对应 match 则用它，否则 `defaultMatch_`。
- `Step(nowMs)`：推进 `defaultMatch_` 与每个房间 match（各自 60Hz 累加器），
  各自把快照/生成/销毁发给自己的客户端。
- 输入：收到某客户端的 `MsgInput` → 路由到其归属局的 `clientInputs[clientId]`。
- `StartRoomMatch(room)`：为该房间创建 `Match`（同一场景，房间内 clientId 集合），
  调 `on_match_start(blue, red|0)`；房间满员或房主点"和 AI 开始"触发。
- 生命周期：房间空 → 销毁该 `Match`；`--ticks` 结束/`Stop` 回收所有 match。
- AOI/快照、反作弊限流、lagcomp 均按局独立。

### 兼容与迁移
- `defaultMatch_` 保证旧测试与"不进房直接玩"仍工作。
- `AoiGrid`、`entityClientIds_`、`controllerInput_` 从 GameServer 移到 Match。
- 现有 `BroadcastSnapshot/ApplyControllerInput/SendDespawn/EntityKey` 等改为
  Match 上的方法，GameServer 转发。
- 单局相关函数签名尽量不变，降低对 `tests/test_server.cpp` 的冲击。

### 风险/工作量
- 大：游戏循环、快照、输入、AOI、反作弊限流、断线回收都要按局重排。
- 需保证 `ctest` 不回归（`test_server.cpp` 单局用例必须过）。
- 建议分阶段：S1 抽 `Match` 且仅 `defaultMatch_`（行为等价）→ 跑测试；
  S2 房间 match 并行 + 路由；S3 大厅打磨。

## 大厅打磨（可先做，小-中）
- 点击**整行**加入（现在要点"加入"按钮）。
- 房间列表自动刷新（已有 1s）+ 人数/已开始显示（已有）。
- 房主：踢人、转让房主、房间内"准备"状态（准备齐才允许开始）。
- 离开/断线时正确回收房间席位与广播。

## 验证
- `ctest`（`test_server.cpp` 等）单局用例不回归。
- 手动：起服 → 2~3 个客户端各建/进不同房间 → 各局独立推进、互不影响；
  大厅：建/刷/整行加入/踢人/转让/和 AI 开始。
- `--ticks` 下多局并行不泄漏（Stop 回收）。

## 交付顺序
S1 `Match` 抽取（等价重构，测过） → S2 房间多局并行 + 路由 → S3 大厅打磨。
