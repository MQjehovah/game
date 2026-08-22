# 路线图：从当前地基到大型 3D MMO

目标游戏形态参考《魔兽世界》：大世界、海量实体、客户端/服务器分离、海量资产、持续运营。以下按里程碑组织，每阶段都有可验证的验收标准。

## 当前状态（已交付）

- 分层引擎：core / interface / platform / gfx / audio / physics / assets / ui / scene。
- OpenGL 3.3/4.x 后端（Windows 实测），Win32 窗口实测；X11/Cocoa 代码就绪由 CI 编译。
- ECS、固定步长循环、第三人称 demo《NeonRealm》、13 项单元测试、截图与冒烟测试、三平台 CI。
- PBR 材质（Cook-Torrance）、实例化渲染 + 视锥剔除、glTF 2.0 导入（Khronos 样例验证）、系统 CJK 字体。
- **场景编辑器**（`neon_editor.exe`）：Dear ImGui docking 停靠布局 + 场景/资产/资源/属性/日志五面板 + 3D 视口拾取相机 + OBJ/glTF 导入 + 场景 JSON 保存加载 + UI 交互冒烟测试。
- **控件树 UI 系统**：相对坐标布局、命中测试、焦点/拖拽、Window/List/TextField/Slider/TreeView/ComboBox/TabBar/DockLayout/ScrollArea。
- **Dear ImGui 集成**（`neon_imgui`）：自研 IRenderBackend 适配层、引擎输入桥接、系统 CJK 字体，仅编辑器链接。
- **网络层（M4）**：UDP 传输 + 可靠通道（ACK/重传）、版本化消息编解码、无头权威服务器 `neon_server`、快照插值 + 预测回滚、AOI 九宫格、v0 匿名登录/角色选择占位，以及**确定性模拟验收**（服务器权威模拟与客户端本地预测对同一脚本输入流逐位一致，见 `docs/NETWORKING.md`）。

## M1：渲染质量（近期）

- [ ] **Vulkan 后端**：实例→设备→交换链→RenderPass→Pipeline→命令缓冲→帧同步（详细步骤见 `docs/VULKAN_ROADMAP.md`）。
- [ ] 阴影映射（方向光 CSM；点光 cubemap）。
- [x] PBR 材质（metallic/roughness/法线贴图）。
- [x] 网格实例化绘制（大量草/树/怪物）、视锥剔除。
- [ ] IBL 环境光。
- [x] 后处理：HDR + Bloom、色调映射（ACES）、MSAA。
- [ ] 地形：高度图 + 分块 LOD（四叉树），纹理 splatting。

## M2：角色与战斗

- [ ] 骨骼动画管线：glTF 蒙皮导入（glTF 静态网格已支持）、动画状态机（idle/run/attack）、混合树。
- [ ] 技能/状态效果（buff/debuff）框架，数值表数据驱动（JSON）。
- [ ] 命中检测与碰撞细化：胶囊体、攻击判定盒、击退。
- [ ] 摄像机碰撞与智能跟随（第三人称 MMO 手感）。

## M3：大世界与流式加载

- [ ] 世界分区（chunk）：按玩家位置异步加载/卸载。
- [x] 资产管线：glTF 2.0 导入（静态网格 + PBR 材质）。
- [ ] 纹理压缩（BC/ASTC）、LOD 资产链、异步解码。
- [ ] ECS 演进：archetype 存储、批迭代、job/并行调度、确定性快照。
- [ ] 对象池与内存 arena，控制 GC 停顿与分配。

## M4：网络化（客户端/服务器同构）

- [x] 传输层：UDP + 重传/ACK、序列号、防作弊随机种子（确定性模拟）。
- [x] 服务器：无渲染目标（headless）、权威物理/战斗、固定 tick。
- [x] 状态同步：快照插值/预测回滚、兴趣管理（AOI 九宫格/视野）。
- [x] 账号/登录/角色选择（v0 匿名占位）；**分区与副本服务器**（world server / instance server）仍待实现。

## M5：工具与运营

- [ ] 地图编辑器进阶：地形编辑、碰撞/出生点/NPC 放置、世界分区数据格式。
- [x] 场景编辑器基础：ImGui 工具 UI、视口导航/拾取、实体增删改、属性面板、场景 JSON 保存加载。
- [x] 编辑器面板：场景/资产/资源/属性/日志（停靠、过滤、统计、导入）。
- [ ] 编辑器进阶：视口多相机、gizmo（平移/旋转/缩放手柄）、撤销/重做、资产预览缩略图、材质编辑器（贴图槽）。
- [ ] 热重载（shader/资产/脚本），引擎内调试菜单。
- [ ] 性能分析：CPU/GPU 时间线、实体统计、内存跟踪；自动化基准。
- [ ] 包体与更新管线、崩溃上报、日志汇聚。

## 横向工程债

- [ ] Linux/macOS 音频后端（ALSA/CoreAudio 或 miniaudio）。
- [x] 中文/CJK 字体（系统字体图集 + 码点采样，demo/编辑器全量覆盖）。
- [ ] CJK 字形自动收集（从 UI 字符串静态分析，替代手工维护 cjkSamples）。
- [ ] 序列化：稳定、带版本的二进制/JSON 格式用于存档与网络。
- [ ] 渲染资源生命周期管理（引用计数/GPU 资源缓存），当前 demo 生命周期即应用生命周期。
- [x] 网络协议编解码、可靠传输、确定性模拟单测（M4）；[ ] 更多物理 / OBJ / 其余单元测试。

## 里程碑依赖关系

```
M1 渲染质量 ──► M3 大世界 ──► M4 网络
        │            ▲
        └──► M2 角色 ┘        └──► M5 工具与运营
```

M1/M2 可并行；M4 依赖 M2 的确定性战斗与 M3 的分区；M5 依赖 M1-M3 稳定。
