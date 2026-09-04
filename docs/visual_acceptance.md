# 视觉品质实机验收（A1-A5）

本文件是 A1-A5 视觉批的**实机验收流程**。所有 A/B 对比都通过 `neon_game --scene
<场景> --screenshot <png> <帧>` 逐帧截图（跑的是完整 GameRuntime，应用 RenderStack /
灯光 / 天空盒，与编辑器"编辑视图"不同）。编辑器 `--screenshot` 只截编辑视图，**不能**看到
A1/A5 的 composite 效果。

## 准备
- 已有验收场景：`projects/default/assets/scenes/visual_acceptance.json`（地面 + DamagedHelmet +
  相机 + 太阳光 + **renderstack 组件**，开启 SSAO/体积光/bloom/调色/自动曝光/暗角/天空盒）。
- 用 `neon_game` 以 loose scene 模式运行（需脚本基目录）：

```bat
build\Release\neon_game.exe --scene projects\default\assets\scenes\visual_acceptance.json --scripts projects\default
```

## 截图命令（A/B 对比）
每个特性都截两张做对比，用图像 diff 工具（或肉眼看差异）。

### A1 颜色分级
没开启 = 去掉 `renderstack` 里的 `grade*` 字段（或临时把 `"grade": false`）。
- ON：`--screenshot shots\a1_on.png 60`
- OFF：`--screenshot shots\a1_off.png 60`
- 预期：ON 版饱和度/色温/对比度明显更"电影感"；OFF 版与旧管线一致。

### A2 法线贴图
DamagedHelmet 自带动法线贴图（`Default_Normal.jpg`）。对比 `castShadow`/材质。
- 预期：法线贴图在光照下产生表面凹凸细节（头盔铆钉/划痕立体感），无它则偏平。
- 排查：若看不出差异，确认 `neon_game` 已用 `NEON_NO_NORMAL` 无关；法线永远随材质导入。

### A3 半球光 + 探针 GI
- 半球光：改 `light.ambientStrength` 或加 `renderstack` 相关；晴天阴影区应呈"暗而非黑"。
- 探针 GI：场景需一个 `.navgrid.json` 声明 + `BakeLightProbes`。默认关闭。

### A4 程序化天空盒
`light.skybox: true` + `useAtmosphere: true` → 太阳圆盘/月亮/云。
- 预期：天空随相机转动（不再是屏幕固定渐变）；可见太阳光晕 + 程序云。

### A5 自动曝光 + 暗角
`renderstack` 里 `autoExposure: true` + `vignette: true`。
- 自动曝光：面向亮处 vs 暗处（移动相机）画面应自适应明暗，不再死白/死黑。
- 暗角：画面四角径向变暗。

## 验收清单
| 特性 | 命令 | 预期 |
|------|------|------|
| A1 调色 | 上表 `--screenshot` | 电影感色调 |
| A2 法线 | DamagedHelmet 特写 | 表面凹凸 |
| A3 半球光 | 阴影面 | 暗而非黑 |
| A4 天空盒 | 旋转相机 | 太阳/云随视角 |
| A5 自动曝光/暗角 | 亮暗切换 | 自适应 + 四角暗化 |

> 提示：跑一次 `neon_game --scene <视觉验收场景> --screenshot out.png 60` 即可得到单帧，
> 反复用不同 `--screenshot` 帧号可做时间轴采样。A1/A5 的 `renderstack` 是场景数据驱动，
> 在编辑器"属性面板"可直接改（反射生成），改完保存再截图即 A/B。

## Demo 玩法升级（B1/B2/B3 应用到 NeonRealm）
- **B1 NavGrid 绕过障碍**：狼群用 `NavFindPath` 路点绕障追击/回巢（`level.navgrid` 声明）。
- **B2 数据驱动技能**：技能表从 `assets/data/skills.json` 经反射 `LoadDataTable` 加载。
- **B2 物品掉落**：狼击杀掉落物品（`assets/data/items.json`，经 `LoadDataTable("item")`），
  银币加金币、浆果回血，HUD 物品栏计数。
- **B3b BlendSpace1D locomotion**：英雄 `AnimBlend("Idle","Running_A",t)` 与狼
  `AnimBlend("02_walk","01_Run",t)` 连续混合走/跑（替代硬切），移动时角色平滑过渡。
- 验证：跑 `neon_game --scene <neon_realm 场景>`，拖动输入看角色走跑连续过渡，
  狼群追击时从走平滑到跑；击杀狼看物品飘字 + HUD 计数。

## Kenney 村庄资产就地重摆（带贴图真实模型，替代程序化纯色）

从 GitHub CC0 镜像 `shorepine/kenney` 拉取 **fantasy-town 村庄 kit（167 个带贴图 GLB +
共享 `colormap.png`）**，落地 `neon_realm/assets/kenney/fantasy-town/`。引擎加载器原生支持
这种"GLB 外部相对 image URI"（源码注释即提到 Kenney），已用 `KenneyColormapExternalImage`
测试证明材质 albedo 有效。

**预览验收**（你跑一张截图，决定每个类别用哪个模型 / 多大 / 摆哪）：
```bat
.\build\release\neon_game.exe --scene projects\neon_realm\assets\scenes\kenney_preview.json --scripts projects\neon_realm --screenshot preview.png 10
```
预览场景横向摆 9 个候选（tree / tree-high / rock-large / rock-small / lantern / cart / stall /
windmill / wall / road / fountain），相机正对。截图后告诉我：树用哪个、石用哪个、路要多宽、
墙/马车/摊/风车/喷泉各留哪个——我据此批量重摆 `realm.json` 村庄。

