# NeonEngine 缺陷与差距清单（TODO，合并版）

> ## 2026-08-28 实施状态（本次会话）
>
> - **已修复（`[x]`）**：A1–A13 全部（Vulkan descriptor 泄漏/真 HDR+采样自检+动态纹理更新+彩色实例化、Json 三处 UB+精度、ParallelFor 异常、Lua 指令/内存预算、脚本 vars 隔离、Jolt 碰撞快照语义、SetPosition 物理回写+Raycast 结果、资源变体释放/热重载 retire、客户端 Ping、world.hash 字符串化、OBJ 索引上限、glTF 多 buffer）；B1（场景 uniform 按帧+程序门控）、B2、B6、B8、B10、B11、B12、B13（快照分片）、B14、B15；C11；D4、D6（部分）、D7；D1（版本门）。
> - **本次未做（`[~]` 保留 / `[ ]`）**：B3（绘制队列）、B4（深度可采样）、B5（VK 提交合并）、B7（骨骼姿势缓存）、B9（绑定 fast path）、C2（动画状态组件化）、C3（编辑器状态解耦）、C4（render graph）、C5（字符串句柄化）、C8（线程池统一）、C9（UI 四轨收敛）、C10（shader 资产化）、D2（插件权限强制）、D3（网络认证加密）、D5（CI 工程化）；C1 已拆 content+combat 两簇（其余簇同模式续拆）。
> - **PvZ 素材/动画/音频收尾（本轮）**：
>   - 序列帧动画改为 **spritesheet 图集**（12 张横排 `*.sheet.png`），185 个帧 PNG+meta 全删，目录 meta 从 185→12；`SceneSprite` 增 `sheet/sheetFrames`，运行时按帧重建 quad UV，绑定 `SetSpriteSheet`；编辑器模型/序列化/playtest/save 全链路贯通。
>   - **修场景 bug**：`projects/pvz/scenes/pvz.json` 所有植物/僵尸实体的 `sheet` 误填 `wallnut.sheet.png`（图集改造批量替换残留）→ 按实体类型改为正确图集（sunflower 18/peashooter 13/snowpea 15/wallnut 16/zombie 22 帧）。
>   - **修 WAV 音频崩溃（根因）**：miniaudio 后端 `PlayImpl` 的 `owned` 拷贝语义错误——`push_back(voice)` 是拷贝，`MixVoice::samples` 仍指向局部缓冲，局部销毁后音频线程读悬垂内存；短程序音效缓冲小不触发，真实 WAV（22050Hz 长缓冲）必然访问违规。修复：`push_back(std::move(voice))`（samples 指向随 move 转移的 owned buffer）。**WAV 真实音效恢复**：`MakePvzSfx(name, projectDir)` 优先加载 `<project>/assets/audio/<name>.wav`（带缓存+合成回退），并线性重采样 22050→44100 修正音高。崩溃经验证：demo 1000 帧连跑 4 次无崩、打包 900 帧 exit 0。
>   - **删孤儿资产**：`house.png`（0 引用）+`house.png.meta`+`test_flip.png.meta`（孤儿）删除；其余单帧 PNG 保留（HUD 卡片图标 + prefab `texture` 编辑器视口预览）。
>   - crash.cpp 增模块基址/RVA/线程 ID 转储（便于后续崩溃定位）；`--2d-play`/`--smoke-test` 全绿；**684/684 测试全绿**。
> - 附带修复：MSYS2 工具链被破坏（cc1plus 缺失）→ `pacman -S mingw-w64-x86_64-gcc` 恢复；本地主力构建切到 **MSVC**（build-msvc）；VK 补齐 `DrawMeshInstancedColored` + NOMINMAX；新增 12 项回归测试；**682/682 全绿**（GL/VK 冒烟通过）。


> 本文件是唯一待办事实源，由两份清单合并而成（2026-08-28）：
> 1. 原 [`gap-todos.md`](./gap-todos.md) 的 **G 系列**（2026-08-24 ~ 08-27，保留原编号以便引用）；
> 2. **2026-08-28 五路架构审计**新增的 **A/B/C/D 系列**（渲染 / 核心运行时 / 脚本·资源·动画 / 编辑器·UI·插件 / 网络·构建，全部落到 file:line）。
>
> 状态：`[ ]` 待办 · `[~]` 部分完成 · `[x]` 已完成。
> 引用格式：直接按编号（如 A3、B1、G3-4）。
>
> - **A 系列 = 正确性缺陷**（崩溃/泄漏/UB/精度，改动小收益高，最优先）
> - **B 系列 = 每帧性能热点**（规模化帧率天花板）
> - **C 系列 = 结构性重构**（可维护性税）
> - **D 系列 = 安全与工程化**

> ## 2026-09-02 反射落地批（A/B/D/E，MSVC 775 测试全绿）
>
> - **反射全体系** ✅：框架（全类型码+Serialize/EditorOnly/Transient+枚举 NEO_ENUM+嵌套/数组/Json+键名别名）、
>   `TypeRegistry`（schema 单一来源+JSON codec+clone，幂等）、inspector Vec4/Array/Struct 渲染、
>   `SceneFile` 序列化收敛为共享 `SerializeEntityComponents`、脚本字段访问 `EntityComponentField` 等。
>   已迁移 12 个引擎组件 + `RenderStack`。详见 [docs/reflection.md](reflection.md)。
> - **A·RenderStack 数据驱动** ✅（数据层）：`render_stack.hpp` 反射描述后处理（SSAO/体积光/SSR/bloom/tonemap/雾），
>   bloom 阈值/强度参数化进 `PostGraph`（默认不变，零回归）。**A3 全量几何 FrameGraph** ⏳ 需 GPU 逐帧验证（跨 renderer/
>   game_runtime/draw_system，从立即模式逐实体绘制流重构为声明式 pass）。
> - **B·资产 GUID** ✅：中心式 `.asset_db.json` 库（无 .meta）已存在；补 `ResolveAssetRef`（GUID 优先+路径回退）。
> - **D·MCP** ✅：`neon/mcp/mcp_server.hpp`（JSON-RPC 2.0，list/get/set_component 经反射校验）+ `neon_mcp` stdio server。
> - **E·多语言宿主** ✅（骨架）：`CreateScriptHost(kind)` 统一工厂 + `PythonHost`（`NEON_ENABLE_PYTHON` 门控，否则回落 Lua）。
>   **E-Python 运行时绑定** ⏳（CPython C-API + 确定性沙箱）需 CPython 运行时实现+验证。
> - 构建：MSVC 加 **`/Zc:preprocessor`**（修复 legacy 预处理器对 `NEO_ENUM` 变参宏计数缺陷）。
> - ⚠️ MinGW GCC 8.1 因既有工具链问题（`_stat64`、quickjs 汇编器）无法完整链接，反射/MCP 在 **MSVC** 验证。
>
> ## 2026-09-04 高品质视觉批（A1/A2/A3/A4，MSVC 784/786 全绿，2 个失败为基线既有）
>
> - **A1 颜色分级** ✅：`ColorGrade`（白平衡→Lift/Gamma/Gain→饱和度→对比度→钳制），作为程序化
>   LUT-free 调色接入 `post.composite`（后 ACES 显示空间，默认禁用零回归）；`RenderStack` 反射新增
>   grade/saturation/contrast/gain/gamma/lift/tint。**并修复 RenderStack 运行时死代码**：补 `renderstack`
>   工厂+序列化+DrawSystem 逐帧应用（此前 SetBloomParams 等从未被调用）。
> - **A2 法线贴图** ✅：**屏幕空间导数 TBN**（`dFdx/dFdy` 重建切线基，零顶点布局改动）；`Material.normalMap`
>   +`normalScale`；glTF `normalTexture` + `normalTexture.scale` 导入；SceneMesh/editor/mat.json/inspector
>   面板/历史命令贯通；绑定单元 23（20-22 为 IBL）。DamagedHelmet 真实资产法线导入验证通过。
> - **A3 间接光** ✅：**半球光**（`uAmbientGroundColor` 按世界法线 Y 把平面 ambient 分裂成天/地渐变，
>   `--ibl 0` 仍可复现旧平面）；**光探针场 GPU GI**（`BakeProbeAtlas` 把 res³ 探针烘焙成 2D RGBA8
>   atlas，lit shader 按世界位置三线性采样做间接漫反射；`Renderer::BakeLightProbes` 端点并接编辑器
>   调试探针面板）。VK 侧因 descriptor 布局未扩展而标注 GL-only（A1/A2/B5 待清后补）。
> - **A4 程序化天空盒** ✅：`skybox.hpp` 提供 view-ray 背景 pass（`InverseViewProjRay` 逐像素重建世界射线）
>   ——太阳圆盘+光环、锁定对侧的月亮、FBM 程序云，取代旧的屏幕空间渐变（此前不随相机转动）。
>   `SceneLight.skybox`（反射布尔）+ draw/editor 宿主动 `EnableSkyBox(sunDir)` 按平行光方向瞄准；
>   HDRI 天空贴图仍可作 equirect 源。GLSL 经 glslang 验证通过。
> - **A5 自动曝光 + 暗角** ✅：**GPU 自动曝光**（post_graph 新增 2 个 pass：HDR→log 平均亮度小目标→1x1
>   平均；composite 由 `keyValue/avgLum` 推导曝光乘子并夹到 [min,max]，默认禁用零回归）+ **暗角**（composite
>   径向暗化）。`RenderStack` 反射新增 autoExposure/autoExposureKey/vignette/vignetteRadius/vignetteIntensity。
>   因该后端 float 回读在 Intel 上不可靠，自动曝光采用**纯 shader 每帧适应**（无 CPU 回读/时间平滑）。
> - ⚠️ 遗留 2 个**基线失败**：`MeshoptLodSimplify` / `GltfAsyncPredecodedTextures`（stash 复测证不与本批相关）。


---

## 第一部分 A：正确性缺陷（P0）

### A1 Vulkan descriptor set 只增不泄
- [x] `vk_backend.cpp:2331-2388` `texSetCache_` 以纹理 id 组合为 key 缓存 descriptor set，`texPool_` 永不 reset；IBL 天空每 ~20 次 SetSky 销毁重建纹理产生新 id → 持续分配新 set，池耗尽后 `EnsureTextureSet` 返回 NULL → 所有绘制静默丢失。
- 修复方向：帧末统一 reset `texPool_` + 清空 `texSetCache_`（或缓存以纹理句柄存活期管理，纹理销毁时 evict）。
- 验收：长时运行（天空动态变化）下 descriptor 分配数有界；`--backend vulkan --smoke-test` 通过。

### A2 Vulkan 伪 HDR + 动态纹理更新空操作
- [x] `vk_backend.cpp:489` `CreateRenderTarget` 忽略 `floatColor`，恒 RGBA8 → bloom 阈值 1.0 采不到内容，HDR 管线在 VK 名存实亡。
- [x] `vk_backend.cpp:820` `UpdateTextureRegion` 空操作 → 动态 CJK 字形在 VK 缺字。
- 修复方向：按 floatColor 选 `VK_FORMAT_R16G16B16A16_SFLOAT`；实现 UpdateTextureRegion（staging/copyBufferToImage）。
- 验收：VK 下 bloom 生效；CJK 字形动态烘焙可见。

### A3 core::Json 三处 UB/健壮性缺陷
- [x] `json.hpp:30-32` `GetString(const std::string& def = "")` 返回 `const std::string&`，非 string 时返回绑定已销毁临时默认参数的引用（悬垂 UB）。
- [x] `json.cpp:31-46` 解析器无递归深度限制（深嵌套输入栈溢出崩溃）；`json.cpp:216-224` 解析成功后不检查尾部残留（`{} garbage` 被接受）；`ParseNumber` 接受 `1.2.3`/`e+` 等非法数字。
- 修复方向：GetString 返回值语义（返回 `std::string` 或要求调用方传 def 引用）；解析器加 `kMaxDepth`（建议 192）+ 尾部 whitespace-only 校验 + 严格数字文法。
- 验收：新增单测（深嵌套拒绝、尾残留拒绝、`1.2.3` 拒绝、GetString 无 UB）。

### A4 ParallelFor 异常安全
- [x] `parallel.cpp:210-226` 闭包捕获栈引用推入队列，靠末尾忙等 join；`fn` 抛异常则 join 被跳过、栈帧销毁、worker 执行悬垂闭包（UB）。
- 修复方向：try/catch 包裹 fn，异常记录后 rethrow 于 join 之后；或 chunk 任务持有按值捕获。
- 验收：单测：ParallelFor 内抛异常不崩溃，异常在调用线程可见。

### A5 Lua 宿主无失控保护
- [x] `lua_host.cpp:317-332` 无 interrupt handler、无内存上限、无指令预算；`while true do end` 冻死主线程（JS 侧有 20M 指令预算 + 128MB 上限，双后端不对等）。
- 修复方向：`lua_sethook(LUA_MASKCOUNT)` 指令预算 + `lua_setallocf` 内存上限，超限报 ScriptError。
- 验收：单测：死循环脚本在预算内被中断并报错，引擎不死。

### A6 per-entity 脚本 vars 写共享全局命名空间
- [x] `game_runtime.cpp:875-882` 每实体 `script.vars` 用 SetGlobal 注入共享宿主，多实体同键"最后 attach 者胜出"，逻辑串数据。
- 修复方向：vars 按实体实例隔离（per-entity 表前缀 / 实例环境），保留单实体场景行为兼容。
- 验收：单测：两实体同名 vars 互不覆盖。

### A7 Jolt Collisions() 无限增长
- [x] `jolt_world.cpp:566-570` 跨 Step 无限 append（自定义后端每 Step clear，语义相反），且无任何调用方清理 → 内存+CPU 双泄漏。
- 修复方向：JoltWorld::Step 开头 clear（与自定义世界对齐），保留显式查询窗口。
- 验收：单测：多 Step 后 Collisions() 长度有界。

### A8 脚本 SetPosition 不写回物理体 / Raycast 丢弃结果
- [x] `bindings.cpp:152-165` NativeSetPosition 只写渲染 transform，不调 physics_->SetPosition → 穿墙。
- [x] `bindings.cpp:942-951` NativeRaycast 只返回 bool，命中距离/目标实体被丢弃。
- 修复方向：SetPosition 同步物理体（存在时）；Raycast 返回 table{hit,dist,entity}（保持旧 bool 兼容或新增 RaycastEx）。
- 验收：单测：脚本移动刚体后物理位置一致；脚本可得命中实体。

### A9 资源引用计数 key 不匹配 + 热重载 use-after-free
- [x] `asset_manager.cpp:1642` glTF 纹理以 Repeat wrap（+后缀）key 缓存，`ReleaseChunkAssets` 用默认 Clamp key 释放且跳过 `gltf:` → glTF 纹理引用永不归零，显存只增不减，同图双份驻留。
- [x] `asset_manager.cpp:1697-1717` ReloadTexture/ReloadMeshOBJ 直接 erase，绕过 2 帧延迟回收 → 本帧仍引用的 GPU 句柄被删（UAF 窗口）。
- [x] `asset_manager.cpp:602-613` `inFlight_` 按裸 path 合并去重，同 path 不同 opts（flip/wrap/compress）被静默合并，选项随机失效。
- 修复方向：Acquire/Release 统一走完整 cache key；Reload 走延迟回收队列；inFlight key = path+opts。
- 验收：单测：glTF 纹理 release 后计数归零；并发不同 opts 请求各自生效。

### A10 客户端不发 Ping → lag comp 生产失效
- [x] `game/src/player.cpp` 全文件无 MsgPing；服务器 `game_server.cpp:435-444` HandlePing 完整但等不到心跳 → RTT 恒 0 → 自动回滚恒 0 tick（lag comp 只在测试里活着）。
- 修复方向：客户端按 1s 间隔发 MsgPing。
- 验收：联机 smoke 下服务器 RTT 非零、AutoLagCompTicks > 0。

### A11 world.hash 经 JSON double 传输丢精度
- [x] `game_server.cpp:940-945` FNV-1a 64 位哈希 `static_cast<double>`（53 位尾数）→ 高位丢失，客户端比对必然失真。
- 修复方向：哈希以十六进制字符串传输（同 packer 规避 %g 的先例）。
- 验收：单测：64 位哈希字符串化往返无损。

### A12 JsonWriter `%g` 精度截断
- [x] `json.cpp:263` 数字序列化 6 位有效数字 → 场景保存/加载静默漂移（0.123456789→0.123457），地形高度数组重灾区。
- 修复方向：`%.17g`（double 最短往返表示）或 `std::to_chars`；同时给 JsonWriter 增加缩进输出选项（见 D4）。
- 验收：单测：任意 double 往返逐位一致。

### A13 OBJ/glTF 导入静默数据损坏
- [x] `asset_manager.cpp:270,282-286` 顶点数 >65535 时 uint16 索引静默回绕，几何破碎无告警。
- [x] `asset_manager.cpp:407-420` glTF 仅读 buffers[0]，多 buffer 模型数据静默丢失。
- 修复方向：>65535 顶点告警 + 32 位索引支持（或拒绝并报错）；glTF 遍历全部 buffer 声明。
- 验收：单测：大 OBJ 告警/正确索引；双 buffer glTF 数据完整。

---

## 第二部分 B：每帧性能热点（P1）

### B1 uniform 无分层（每 draw 重设 per-frame 全家桶）
- [x] `renderer.cpp:1951-2064` ApplyMaterial 每 draw 重设太阳/8 点光（每灯 3 次 string 拼接）/雾/3 个 lightVP/12 采样器/IBL——per-frame 与 per-draw 混同；`vk_backend.cpp:2317` 每 draw memcpy 5568B EngineUBO（`uniformsDirty_` 从未被消费，死代码）。
- 修复方向：frame/scene 级 uniform 每帧提交一次（脏标志消费），draw 级只留材质参数；VK 侧按 dirty 增量快照。
- 验收：`--bench` 下每帧 SetUniform 调用次数数量级下降；画面逐像素不变。

### B2 GL SetUniform 附带 glGetError + string 临时分配
- [x] `gl_backend.cpp:825-875` 每次 SetUniform 构造临时 `std::string`（无异构查找）+ 尾部 `CheckError`（多数驱动同步点）。
- 修复方向：`unordered_map<std::string,GLint,hash,string_view::equal_to>` 异构查找（或预解析 uniform 偏移表）；CheckError 改编译开关/采样模式。
- 验收：每帧 uniform 提交路径零堆分配；渲染冒烟通过。

### B3 无绘制队列/排序/合批
- [~] `renderer.cpp:1701-1719` DrawMesh 按调用顺序即时提交；无 opaque 前到后排序（early-z 失效）、无透明距离排序保证、无材质/着色器合批 key。
- 修复方向：先做透明排序 + opaque 距离排序的轻量提交列表；合批 key（shader/材质/纹理）为后续 render graph（C4）的一部分。
- 验收：透明物体渲染顺序与相机距离一致；RenderStats 如实统计。

### B4 主场景深度不可采样 → SSAO 几何画两遍
- [~] `gl_backend.cpp:368-377` HDR 目标深度为 RBO；`renderer.cpp:1273-1284` SSAO 用颜色编码深度把全部 caster 重画一遍（`ssaoCasters_`/`shadowCasters_` 每次双份拷贝）。
- 修复方向：HDR 目标深度改 DEPTH_COMPONENT 纹理（保留 RBO + 颜色编码回退路径），SSAO 直接采样深度。
- 验收：SSAO 开启时几何 pass 次数减半；驱动自检失败时自动回退旧路径。

### B5 VK 每次目标切换 submit + 等 fence（全串行）
- [~] `vk_backend.cpp:2081-2106, 2111-2127` BindTarget 变化即 SubmitQueue + vkWaitForFences；一帧 10+ 次目标切换，GPU/CPU 零并行。
- 修复方向：帧内多 render pass 用单 cmd buffer + vkCmdNextSubpass/barrier（或 cmd buffer 池延迟提交，帧末一次 submit + 一次 wait）。
- 验收：`--bench` VK 帧时间下降；截图/冒烟逐像素一致。

### B6 BuildDrawList O(N×M) 存活扫描
- [x] `game_runtime.cpp:1066-1074, 1158-1161` 每帧每待检实体线性扫 `draws_` 判存活；千实体场景每帧百万次比较。
- 修复方向：维护 alive 实体→DrawItem 索引（hash map 或 dense 数组下标），DrawList 增量维护。
- 验收：万实体基准中 BuildDrawList 耗时线性且常数显著下降。

### B7 蒙皮矩阵每帧 3~4 遍重算 + HUD 逐顶点 CPU 蒙皮
- [~] `game_runtime.cpp:2348-2402` HUD 锚点逐顶点 CPU 蒙皮；主 Draw/阴影/SSAO 各自 `BindPose→Sample→ComputeBoneMatrices`（每实体每帧 3~4 次完整采样、15+ 次堆分配）。
- 修复方向：骨骼姿势每帧每实体计算一次缓存（Tick 内），各 pass 复用；HUD 锚点用 AABB 近似或缓存姿势的 bounds。
- 验收：蒙皮实体数 ×3（主+阴影+SSAO）下骨骼采样次数不翻倍；HUD 不再逐顶点蒙皮。

### B8 Lua DebugHook 常驻性能税
- [x] `lua_host.cpp:777-806, 327` Init 无条件安装 LUA_MASKLINE 钩子从不卸载——每行 Lua 一次 C 回调 + registry 查找，全引擎 10~30% 脚本税。
- 修复方向：仅调试器激活（有断点/单步请求）时 sethook，断点清空且不在单步时 sethook(NULL)。
- 验收：无断点运行基准脚本耗时下降 ≥10%；断点功能回归通过。

### B9 Lua-C++ 边界每参数全量转换
- [~] `lua_host.cpp:169-213, 226-230` GetArg 每次 PopValue 完整转换（新构造 unordered_set 堆分配 + 递归表拷贝）；`lua_host.cpp:379` native 按名字符串查 map（JS 侧是 O(1) 索引）。
- 修复方向：标量 fast path（不构造 set/不拷贝 table）；native 注册时缓存 upvalue 索引。
- 验收：脚本密集调用基准提升；行为回归（既有脚本测试全绿）。

### B10 每帧堆分配热点（pose 快照/sprite 排序/profiler）
- [x] `game_runtime.cpp:2957-2974` 每帧 new `unordered_map` 入 poseHistory_，超限 `erase(begin())` 移动 63 个 map。
- [x] `game_runtime.cpp:2436-2440` sprite 排序比较器内每对比较 2 次 ECS 池查询。
- [x] `profiler.cpp:36-53` AddTiming 线性 strcmp 扫 32 槽。
- 修复方向：pose 快照改对象池/定长数组复用；排序键预取到 vector 再 sort；profiler 换 hash 索引。
- 验证：`--bench` 分配计数（MemStats）下降。

### B11 编辑器撤销命令触发全场景 JSON 往返
- [x] `editor_play.cpp:422-451` 每个撤销命令（gizmo 拖拽每帧 Push）→ SyncWorldFromEntities 全量"构建 JSON→序列化→解析→Instantiate"。
- 修复方向：gizmo 拖拽期间 Merge/合并命令 + 拖拽结束才同步；或 World 同步改增量。
- 验收：编辑器拖 gizmo 帧时间不随实体数平方增长。

### B12 UI 布局每帧全树重算 ×2
- [x] `ui/system.cpp:101-103` Update 每帧整树 Layout；`ui/document.cpp:362-365, 464-466` Draw 先 Layout 后画、HitTest 又强制 Layout——同帧最多两遍全树求解。
- 修复方向：dirty 标记（结构/属性变化才重排）；HitTest 复用本帧已算布局。
- 验收：UI 密集场景帧耗时下降；布局测试全绿。

### B13 网络快照全量编码 + 单帧 ~48 实体硬上限 + 全走可靠通道
- [x] `protocol.cpp:104-114` 快照全量 24B/实体无 delta/量化；`reliable.hpp` 无分片，maxFrameBytes=1200 超限快照直接丢弃（snapshotTooBig_）；快照等时效消息走可靠有序通道被重传放大（`game_server.cpp:897-908`）。
- 修复方向：分片（多 datagram 承载一帧快照）；快照走不可靠通道 + 序号 newest-wins；delta/量化列后续（见 G3-4）。
- 验收：单测：>48 实体快照完整到达；丢包注入下快照不阻塞事件通道。

### B14 物理同步无脏标记
- [x] `game_runtime.cpp:687-697` SyncSceneBodies 每帧全量 ViewAll + 逐实体 GetPosition（Jolt 内部再 map 查找），静止刚体也同步。
- 修复方向：脏标记（脚本写 transform 置脏）+ Jolt body active 状态过滤。
- 验收：静止刚体大场景 tick 耗时下降。

### B15 日志热路径开销
- [x] `log.cpp:252-280` 每条日志 vsnprintf+localtime+文件写+flush 全在自旋锁内，锁外再 fprintf+fflush；环形缓冲 `erase(begin())` O(2048)。
- 修复方向：格式化在锁外/后台线程落盘；环形缓冲改头索引；localtime 缓存。
- 验收：高频 Warn 日志不拖帧（--bench 对比）。

---

## 第三部分 C：结构性重构（P1/P2）

### C1 GameRuntime 上帝类拆分（3067 行）
- [~] 13 个职责簇（2026-08-28 已拆 content+combat 两簇；2026-08-31 再拆 draw 簇 →
  `game_runtime_draw.cpp`（BuildDrawList/Resolve*/DrawVegetation，573 行），game_runtime.cpp
  2897→2457 行。ScriptHostCoordinator / AnimationSystem / CameraService / HudOverlay /
  PhysicsBridge / SceneLifecycle / BehaviorTreeRunner 待同模式续拆）。
- 分阶段：先抽纯逻辑簇（LagComp/Tween/ContentLoader），再抽渲染构建（DrawListBuilder），纯定义搬移无 API 变化。

### C2 动画状态存于 DrawItem → headless 服务器动画空转
- [ ] `game_runtime.hpp:421-476` DrawItem 25+ 字段承载动画/ASM 状态；服务器（draws_ 空）TickAnimations 空转，服务器/客户端动画行为分叉。
- 修复方向：动画状态迁到 ECS 组件（SceneAnimator 扩展），DrawItem 只留渲染快照。

### C3 EditorApp 巨类 / panels.cpp 4100 行
- [~] 2026-08-31 已做：panels.cpp 4143 行拆为 9 个 `.inc`（按面板，主文件 428 行）+ 面板注册
  中心化（`EditorApp::PanelDef` 注册表统一驱动 视图菜单 + ini 持久化，消除 hpp 字段/表项/菜单
  3 处同步，并修复"动画状态机"未持久化的漂移）+ **面板状态按面板拆分**（terrain/model-preview/
  anim/asm/script/nav/package/profiler/loc/inputmap 十簇状态抽成嵌套 struct，~55 个扁平成员收敛为
  10 个 struct 实例）。editor.hpp 仍 ~970 行（余下 gizmo/BT/UI-editor/视口相机等状态未拆）。
- [ ] 未做：gizmo/BT/UI-editor 状态继续按面板拆分；undo 覆盖洞（灯光/相机参数/地形笔刷/UI 文档
  不可撤销，`editor_scene.cpp:426-461`、`panels.cpp:1407-1452`）另列 D5。
- 方向：gizmo/BT/UI-editor 状态按面板拆分（PanelDef 已铺路），undo 覆盖洞归 D5。

### C4 Renderer 上帝类 + 无 render graph
- [ ] `renderer.cpp` 2804 行 ~18 个职责；17 个 RT 手工生命周期，新增后处理要改 4 处；三份近似重复的 caster 提交代码（CSM/SSAO 深度/点光 1161-1271 vs 1490-1560）。
- 方向：pass 描述结构 + RT 自动生命周期 + caster 提交统一 helper。

### C5 字符串 key 贯穿全栈
- [ ] GameVars/组件名/meshKey/技能冷却/groups/资源路径全 `std::string`（`gamevars.hpp:32`、`asset_manager.hpp:286` 12+ 平行 map）——无 intern/句柄/GUID；GUID 未贯通场景引用（场景内部仍是路径）。
- 方向：热路径先行（GameVars/冷却表句柄化），资源 GUID 贯通列大项。

### C6 组件序列化三份手写镜像
- [x] `scene_file.cpp:544-1175` vs `1334-1550`：解析/序列化/字段白名单三份；反射系统 G2-1 仅 SceneAudioSource 采用。
- [x] 序列化收敛（2026-09-02 反射落地）：`FromWorld`/`EntityToJson` 两个手写块**合并为共享
  `SerializeEntityComponents`**（唯一来源），并修复 `EntityToJson` 漏写 mesh-lod/uvRepeat/dirt/rock、
  sprite-frames/sheet、rigidbody/terrain/tilemap/character/SceneData 的漂移；`schema` 单一来源见 A2。
- [~] 字段级 codec：部分语义性**条件省略**（`if(rb->dynamic)` 等）与**校验/回退**（rigidbody 非法 shape→sphere）
  尚未完全交给 `kFields`，需字段级 `ReflectTraits<T>` 特化或读写钩子（见 `docs/reflection.md` §6）。

### C7 脚本绑定手写 95 个 ×3 处
- [x] `bindings.cpp` 1389 行：null hook 静默默认值（无 SetError）、双份键名表不一致（bindings.cpp:1277 缺 tab/F1-F12 vs input_map.cpp:22）、表驱动缺失。
- [x] 反射字段访问（2026-09-02）：新增 `EntityComponentField`/`SetEntityComponentField` 单字段读写绑定，
  字段名与反射 schema 一致；脚本无需整表重写。（其余 95 个业务绑定仍为独立 native，非反射生成。）
- 方向：注册表驱动（名字/参数 schema/错误上报统一），键名表单一来源。

### C8 线程基建复制 3 份
- [ ] SpinLock+SleepMs(1) 轮询在 parallel.cpp / async_loader.cpp / log.cpp 各一份；无统一 job 系统（G5-2 任务图已落地，收口到它）。

### C9 UI 四轨并存
- [ ] 控件树（system.cpp）/文档 UI（document.cpp+layout_solver）/立即模式（ui.cpp）/ImGui；文本测量/裁剪/主题两份调色板（`editor.cpp:335-441` vs ui::Theme）。TextField 无光标/选择/IME（`system.cpp:211-229`）。
- 方向：主题与文本测量单一来源；TextField 补光标/选择（IME 列远期）。

### C10 着色器系统原始
- [ ] 全内嵌 C++ 字符串（`renderer.cpp:24-613`）、无文件加载/磁盘缓存/变体；VK 端 `CreateShader` 忽略传入源码按 debugName 查预编译表（`vk_backend.cpp:2649-2654`）——同一接口两种语义，自定义 shader 在 VK 静默无效（G2-5 关联）。
- 方向：shader 源资产化 + 产物缓存（G7-2 短期建议）；VK 运行时 glslang 编译或显式能力声明。

### C11 ECS Pool 防护缺失
- [x] `world.hpp:40-47` Pool::Add 重复 id 无防护（dense 孤儿条目）、Get 无边界检查；`world.hpp:107-118` 串行迭代中增删实体无断言（靠人工纪律）；`parallel.cpp:229` rejected static 对象可被写坏。
- 方向：debug 断言 + 重复 Add 防护 + rejected 返回 const。

### C12 CMake 单文件 655 行
- [~] 已拆（2026-08-31）：neon_engine 拆为 neon_core/neon_gfx/neon_scene 三层库 +
  neon_engine INTERFACE 门面；依赖方向 core←gfx←scene 由构建强制。未做：每模块独立
  add_subdirectory（当前按层组织，非按目录）。
- 方向：按模块 add_subdirectory + PCH（engine.hpp 聚合头）。

> 以下 C13–C15 来自 2026-08-31 架构评审（分层/依赖方向，详见
> [`plans/2026-08-31-architecture-review.md`](./plans/2026-08-31-architecture-review.md)）。

### C13 scene ↔ script 循环依赖
- [x] `game_runtime.hpp` → `script/bindings.hpp`（scene 依赖 script）；`bindings.cpp` →
  `scene/scene_file.hpp` + `scene/status.hpp`（script 依赖 scene）；`bindings.hpp` 同时依赖
  `gfx/physics/platform/ecs`。靠头文件顺序"刚好能编过"，无规则约束会持续腐化。
- 已修复（2026-08-31）：`ScriptContext` 新增 `statusIdByName` / `entityComponent` /
  `setEntityComponent` 三个依赖注入 hook，`bindings.cpp` 改走 hook 并移除全部 `neon/scene/`
  include；`game_runtime.cpp` 负责接线。script 模块现在零 scene 依赖，依赖方向单向化
  （scene → script）。

### C14 玩法逻辑混进引擎核心（内聚）
- [~] 重评（2026-08-31）：`skills.hpp`/`status.hpp`/`game_runtime_combat.cpp` 实为
  **数据驱动运行时能力**（skills.json 定义技能、状态表驱动 buff/debuff），非某具体游戏
  的硬编码玩法——与物理/动画同为运行时子系统。真正的问题是 C1（GameRuntime 上帝类
  承载所有子系统），combat 已作为职责簇拆到独立 TU。将其"移出引擎"需把 GameRuntime
  的战斗状态（skillCooldowns_/projectiles_/poseSlots_）抽成独立子系统（CombatSystem
  组合），属 C1 后续，需谨慎避免破坏确定性/战斗测试。
- 方向：并入 C1——GameRuntime 按子系统抽 facade，combat 子系统作为组合成员，而非迁出引擎。

### C15 editor/server/game 未库化（测试跨层抓源码）
- [~] editor 已库化（2026-08-31）：`neon_editor_common`（history+packager）+ `neon_editor_lib`
  + `neon_editor` 薄壳；`neon_tests` 链接 `neon_editor_common` 而非抓源码。
- [ ] 未做：`server/src/game_server.cpp`/`aoi.cpp` 与 `game/src/client_sync.cpp` 仍被
  `neon_tests` 直接编译——server/game 尚未库化。
- 方向：server/game 抽公共 lib，测试链接库而非抓源码。

---

## 第四部分 D：安全与工程化

### D1 编辑器插件无版本门 + 任意路径访问
- [~] `editor_plugin.cpp:553-595` 不检查 `minEngineVersion`（runtime 侧有）；`importAsset/listDir` 接受任意绝对路径可拷贝机器任意文件进项目（editor_plugins.cpp:97-113）。

### D2 插件 permissions 字段不强制
- [ ] `plugin.cpp:156-164` manifest 声明式权限解析后完全不强制，插件拿到全量引擎绑定；同后端插件共享脚本全局命名空间互相覆盖（`runtime_plugin.cpp:186-199`）。
- 方向：permissions 白名单执行 + 每插件独立 env/前缀。

### D3 网络无认证/加密/防重放
- [ ] `game_server.cpp:310-319` 匿名账号、明文 UDP、源地址即身份、`admin.kick/ban` 任何客户端可调；ban 名单仅内存。UDP 收包无预算可被泛洪拖垮（`game_server.cpp:179-227`）。
- 方向：session token + 管理命令鉴权 + HMAC 防重放（列 G3-4 生产化）；PumpNetwork 每帧收包上限。

### D4 场景 JSON 不可 diff
- [x] `json.cpp:251-295` 紧凑单行无缩进，git diff 退化为整文件一行；叠加 A12 精度截断后版本控制基本失效。
- 方向：JsonWriter 缩进选项（场景保存用 pretty），键序稳定（已按 map 排序）。

### D5 CI 与构建工程化
- [~] Vulkan 后端不编译进 CI（NEON_ENABLE_VULKAN 默认 OFF，`ci.yml:62-67`）；
- [x] 无 MinGW CI（本地主力工具链，回归不可见）；
- [x] sanitizer 仅 Linux，Windows/macOS 无；
- [x] `third_party/glslang_tool/bin/glslang.exe` 二进制工具直接进仓库；
- [x] 冒烟仅 neon_editor，server/game 联机与打包 e2e 不在 CI。

### D6 编辑器功能洞（撤销/静默失败）
- [~] 灯光/相机参数编辑不入撤销栈（`editor_scene.cpp:426-461`、`panels.cpp:1407-1452`）；地形笔刷绕过 history（PaintTerrain）；UI 文档每次拖动即时写盘且无撤销（`editor_viewport.cpp:577-588`）。
- [x] 场景 JSON parse 错误静默吞掉（`editor_scene.cpp:756-802`）；SaveSceneAsChild 从磁盘读而非当前编辑状态（666-697）。
- [x] 缩略图主线程同步生成无预算（`editor_assets.cpp:23-107`）。

### D7 服务器时基漂移
- [x] `server/main.cpp:128-136` 虚拟时钟 +17ms/真实 sleep 16ms → 实际 ~63.75Hz 而非 60Hz，与客户端时基 ~6% 偏差。

---

## 第五部分 G 系列遗留（原 gap-todos 未完成项，编号保留）

### G1-1 [~] 渲染后端覆盖：D3D12/Metal 空白（有意搁置，保留后端注册机制即可）
### G2-1 [x] 反射系统：标量/Vec2-4/Quat/Color/枚举/数组/嵌套/Json + Transient 分类已落地（`component_reflect.hpp`+`enum_reflect.hpp`+`type_registry.hpp`，见 `docs/reflection.md`；`NEO_ENUM` 取代不兼容 GCC 8.1 的 magic_enum）；脚本绑定生成未做（并入 C7 推进）
### G2-2 [ ] ECS archetype 存储未做（调度器已落地；接口已预留）
### G2-2 [~] 组件类型按名注册：`TypeRegistry`（schema+类型擦除序列化+clone）已落地（`RegisterBuiltinReflectedTypes` 注册 SceneHealth/SceneAudioSource，接入 `RegisterBuiltinComponentSchemas`）；运行时 `ComponentRegistry`（工厂）与 TypeRegistry 未合并（见 reflection.md §6）
### G2-4 [~] 动态 GI：probe 场已落地；shader 集成/DDGI 未做
### G2-5 [ ] Vulkan 自定义 shader 热重载（并入 C10）
### G3-1 [ ] LLM 集成（远期；需先设计确定性沙箱边界）
### G3-2 [ ] PCG 节点图（未做）
### G3-3 [ ] 视频编解码（未做）
### G3-4 [~] 网络栈：lag comp/房间/防作弊已交付——**但 A10（Ping 断链）/A11（hash 精度）两处质量洞在 A 系列修复**；真未做：① 分区分服（world/instance server）② 多玩家输入收尾（on_player_join 接正式玩法、per-attacker lag comp）③ delta 编码/量化（并入 B13 后续）④ 认证加密（并入 D3）⑤ 断线重连/会话恢复 ⑥ 服务器 --bench 负载压测（64 并发 tick/带宽基线）
### G3-5 [~] UI 控件：9-slice/富文本颜色已做；图标/链接/内嵌图片未做；TextField 光标/选择（并入 C9）
### G4-1 [~] 原生插件：ABI+加载器+物理/音频示范已落地；渲染/物理后端运行时替换未做
### G5-1 [~] 运行时切换渲染后端（需 D3D12 + 渲染器重建路径）
### G5-2 [~] 任务图调度：图+动态分发已落地；读写区域自动分析/lock-free 队列未做
### G5-3 [~] 确定性：核心已交付；**跨平台 bit 一致 CI 未做**（禁止自动向量化未强制、Jolt 跨平台未验证）
### G6-1 [~] 资产变体表已落地；显存预算 API 未做
### G6-2 [~] 异步 obj/gltf 已落地；LOD 等级异步、场景/编辑器默认开启未做
### G6-3 [~] 堆监控已落地；relocating/compact 分配器未做
### G7-2 [ ] 着色器 IL/字节码翻译层（短期 shader 资产化并入 C10）
### G7-3 [~] 输入时序（和弦/双击/长按）已落地；触屏未做
### G8-2 [ ] C++ 实时代码热替换（远期；短期以原生插件 DLL 重载过渡）
### G8-4 [~] 增量打包已落地（内容哈希清单）；分布式农场未做
### G-收尾 [~] GameRuntime 分解第一里程碑已落地（tween/anim/status/skill/projectile 接调度器）；脚本/BT/物理仍串行（单线程宿主收益有限，优先级中低）；打包版烘焙端到端验证未做

---

## 第六部分 与主流引擎差距速查（决策参考，不逐项排期）

| 领域 | 差距评级 | 对标要点 | 本清单对应项 |
|------|---------|---------|-------------|
| 渲染架构 | 大→中 | RenderStack 数据驱动雏形已做（后处理参数反射+bloom参数化，A3 全量几何 FrameGraph 未做）；深度可采样/SSAO 已修 | A3 + B1/B3/B4/C4/C10, G2-4 |
| Vulkan 成熟度 | 中→大 | descriptor 泄漏/无内存子分配/串行提交/伪 HDR | A1/A2/B5/P1 系列 |
| ECS/并行 | 中 | 无 archetype/job 依赖分析/burst 级优化 | C11/G2-2/G5-2 |
| 动画 | 中 | 无动画事件/重定向；>64 骨骼上限；BlendSpace 相位漂移 | B7/C2 + 新增：AnimEvent、骨骼上限 |
| 物理 | 中→大 | Jolt 同库但封装最小子集（无旋转/关节/触发器/shape cast、隐式地面、2048 上限、单线程） | A7/A8 + Jolt 封装扩展 |
| 脚本工具链 | 中→小 | 反射字段访问（EntityComponentField）已做；多语言宿主（JS/Python 门控）已做；JS 无调试/绑定静默失败仍存 | B8/B9/C7 + E |
| 网络 | 强项+硬上限 | 确定性+AOI 强；48 实体上限/delta/认证/重连/分区分服缺 | B13/D3/G3-4 |
| 资产管线 | 中→大 | 有中心式 GUID 库（.asset_db.json）+ ResolveAssetRef 路径回退；仍无 FBX/USD、GUID 未贯通场景引用 | A13/C5 + 导入格式扩展 |
| UI | 中 | 无锚点/容器布局；TextField 不可用 | C9/G3-5 |
| 编辑器 | 中 | undo 洞/无 profiler 时间线/GPU 捕获 | C3/D6 |
| 音频 | 中 | 3D 一次性计算/无流式/无效果器/锁纪律 | 新增：音频流式与 3D 追踪 |
| 平台 | 大 | X11/Cocoa 未实机、无 Web/移动 | G1-1 关联 |

---

## 已完成存档（原 G 系列 [x] 项，2026-08-24 ~ 08-27）

> 详情与测试名见 git 历史与原 gap-todos.md（已并入本节，仅保留一行摘要）。

- G1-2 动态 BVH 空间索引（万级视锥 12.6×；test_bvh）
- G1-3 场景树接口/变换缓存/编辑器父级稳定 id/场景桥无损化/FromWorld 序列化器/编辑器持有 live World（G5-4 阶段 1-5 大部分）
- G1-4 资源依赖图（依赖边/缺失定位/异步加载；test_asset_deps）
- G1-5 SSAO + 体积光 + GPU 粒子实例化
- G2-3 地形 Layer Blend + chunked LOD + 植被 Impostor
- G3-4 服务器 lag compensation（历史姿态回溯 + RTT 自动回滚；test_lagcomp）
- G3-5 UI 九宫格切片 + 富文本颜色分段
- G4-1 原生 DLL/SO 插件 ABI+加载器+物理/音频示范插件（运行时 DLL 物理后端 `plugin:<name>`）
- G5-2 任务图调度器 + 动态工作分发（原子计数 grab）
- G7-1 VFS 全链路（pack 直读免解包 + Mod 挂载 + assets:/ scheme + 脚本直通 VFS）
- G7-3 输入时序绑定（和弦/双击/长按）
- G8-1 Profiler 环形缓冲 + 崩溃报告落盘（crash_report.txt）
- G8-3 调试覆盖层（NavMesh/音频源衰减球/光照探针/F3 面板）
- G8-4 增量打包（内容哈希清单，未变更零重打）
- G6-1 平台/LOD 资产变体表（variants.json + --variant）
- G6-2 异步 OBJ/glTF 网格加载 + asyncMeshLoad 绘制管线接入
- G6-3 全局堆监控（MemStats + operator new 钩子）
- 架构收尾：BC1 离线烘焙管线 / 检查器统一 schema / GameRuntime 组件子系统接调度器（parallelSystems 确定性）

---

## 优先级建议（2026-08-28）

1. **A1–A13 正确性批量修复**（不修则其余投入建在流沙上）
2. **B1/B2/B3/B4 渲染 CPU 管线治理**（draw call 规模化前提 + SSAO 基建）
3. **C1/C2 GameRuntime 拆分**（所有后续功能开发的税）
4. **B8/B9/C7 脚本边界提速**（B8 白捡 10%+）
5. **B13 + D3 网络规模化与安全**（突破 48 实体上限）
6. B5-B15 其余热点 → C3-C12 结构项 → D 系列 → G 系列远期