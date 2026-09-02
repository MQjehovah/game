# 反射系统设计（NeonEngine Reflection）

> 状态：✅ 已落地（框架 + TypeRegistry + 12 组件 + RenderStack，MSVC 775 测试全绿）。
> 治理 C6 / G2-1 / G2-2，完成 C7 字段访问；并延伸出 A（RenderStack 数据驱动）、B（资产 GUID 路径回退）、
> D（MCP）、E（多语言脚本宿主 + Python 门控）。
> A3（全量几何 FrameGraph）与 E-Python（CPython 运行时绑定）为需 GPU / CPython 运行时的专项（见 §6）。
>
> 相关代码：`engine/include/neon/scene/component_reflect.hpp`、`enum_reflect.hpp`、
> `type_registry.hpp`（+`engine/src/scene/type_registry.cpp`）。
> 测试：`tests/test_reflect.cpp`。类型定义：扩展了 `component_schema.hpp` 的
> `FieldType`（新增 `Array/Struct/Vec4`）与 `FieldSchema`（新增 `header/tooltip/widget`）。

## 1. 为什么需要

引擎以往把"组件字段元数据"写三遍，口径必然漂移：

1. **编辑器 schema**：`component_schema.cpp::BuildSchemas()` 里 `push_back` 手写
   （transform/mesh/rigidbody/plant/zombie… 全都在），只对 `SceneHealth`/`SceneAudioSource`
   用了反射（`FieldList::Schemas()`）。
2. **运行时序列化**：`scene_file.cpp` 里 `FromWorld` 与 `EntityToJson` **各手写了一份几乎相同**
   的组件序列化（1490–1678 与 1711–1819），仅 health/audio 走 `kFields.ToJson`。
3. **脚本绑定**：`bindings.cpp` 手写 95 个绑定 ×3 处（C7）。

这三份各自维护 → 加字段要改三处，改一处其余漂移（正是 C6 说的问题）。组件结构与 schema
已经实际分叉（如 `SceneCamera` 有 `orthoSize/aspect`，schema 却只写 `fov/ortho`）。

**目标**：一处声明（`kFields`），模板同时派生出 schema、Json 编解码、clone/equal/默认值，
并进入统一 TypeRegistry。今后加字段只改结构体 + `kFields` 一行。

## 2. 设计取向（借鉴谁）

| 来源 | 借了什么 |
|---|---|
| **Godot `_bind_methods`** | "单处声明 → 驱动 inspector + 序列化"。本文的 `kFields` 即该思路 |
| **Unity `[SerializeField]`/`[Range]`/`[Header]`** | 字段元数据（min/max/step/header/tooltip）与**字段分类**（Serialize/EditorOnly/Transient） |
| **EnTT `meta` / RTTR** | 按名字注册的类型注册表（`TypeRegistry`），统一 schema + 序列化 + clone |
| **magic_enum** | 曾计划采用；但其 v0.9.3 在项目 **MinGW GCC 8.1** 上 `static_assert("unsupported compiler")`（见 `enum_reflect.hpp` 顶部注释）。为确定性 + 零依赖，改用自研 `NEO_ENUM` 宏（`enum_reflect.hpp`），语义等值（有序 value↔name） |
| **UE UHT / Boost.PFR** | 不用：前者要独立代码生成工具链（破坏"只依赖 CMake/编译器"）；后者拿不到成员名 |

**落地约束**：纯 header-only、C++17、MSVC + MinGW 通吃、只依赖引擎自带 `core::Json`，**不新增第三方构建依赖**。

## 3. 架构（三层）

```
Layer 1  ReflectTraits<T>     值 ⇄ Json 的类型码（标量/字符串/Vec2-4/Quat/Color/枚举/数组/嵌套/Json）
Layer 2  FieldRef + FieldList 字段元数据 + 分类（Serialize/EditorOnly/Transient）+ header/tooltip/资源/widget
Layer 3  TypeRegistry         按名注册：schema + 类型擦除的序列化 + clone（Godot ClassDB / EnTT meta）
```

### 3.1 `ReflectTraits<T, Enable>`
主模板**不定义**——对未登记的字段类型会**编译期报错**，而不是运行时静默丢字段：
```cpp
template <typename T, typename Enable = void> struct ReflectTraits;   // 未登记 => 编译失败

template<> struct ReflectTraits<math::Vec3, void> { ... ToJson/FromJson/Default/kKind ... };
template<typename T> struct ReflectTraits<T, enable_if_t<is_enum_v<T> && EnumSpecs<T>::Enabled>> {...};
template<typename T> struct ReflectTraits<std::vector<T>, void> {...};         // 数组
template<typename T> struct ReflectTraits<T, enable_if_t<HasReflectedFields<T>::value>> {...}; // 嵌套聚合
```
每种 trait 提供：`kKind`（ValueKind）、`ToJson`、`FromJson`、`Default`。

### 3.2 `FieldRef` + `Field` + `FieldList`
```cpp
enum class FieldCategory : uint8_t { Serialize, EditorOnly, Transient };

template <typename Owner, typename T>
FieldRef<Owner, T> Field(key, label, type, &Owner::member,
                         def, min, max, step,          // 历史签名，可省略
                         const FieldMeta& meta);       // 可选：category + header/tooltip/资源/widget/options
```
`FieldList<Owner, Fs...>`（由 `ReflectFields(...)` CTAD 生成）派生：
- `Schemas()` → `std::vector<FieldSchema>`（跳过 Transient；枚举字段自动注入 `NEO_ENUM` 的 options）
- `ToJson(obj)` / `FromJson(json, obj, err)` → 只处理 **Serialize** 字段
- `Clone(dst, src)`（整结构拷贝，保留 EditorOnly 元数据）
- `Equal(a, b)`（对**序列化字段**做 `JsonEquals`，忽略 transient/editor 状态）
- `ApplyDefaults(obj)`（按类型默认值复位）

### 3.3 字段分类语义（Unity SerializeField 对齐）
| 分类 | 编辑器可见 | 进场景 JSON | 说明 |
|---|---|---|---|
| `Serialize` | ✅ | ✅ | 运行时数据（默认） |
| `EditorOnly` | ✅ | ❌ | 编辑器元数据，不改运行时 |
| `Transient` | ❌ | ❌ | 运行时状态（如 `bodyId`、`active`） |

## 4. 枚举反射 `NEO_ENUM`

枚举必须**从 0 连续**取值（声明顺序即值序），否则不要反射（文档注明）。用法：
```cpp
enum class ArmorKind { None, Cone, Bucket };
NEO_ENUM(ArmorKind, None, Cone, Bucket);   // 值序 == 声明序
```
`EnumSpecs<T>::Enabled` 作为"是否已反射"的编译期开关；未反射的枚举在 `ReflectTraits<T>`
处 SFINAE 出局 → 编译失败（强制显式登记）。`Names()` 返回的 `const char* const*` 稳定，
供 `FieldSchema::options` 给 inspector 的下拉框用。

> 为什么不用 magic_enum：见第 2 节。`NEO_ENUM` 是自研 30 行宏（`FOR_EACH` + 双级 CAT token-paste），
> 零依赖、GCC 8.1/MSVC 都能编、行为确定。

## 5. `TypeRegistry`（单一来源入口）

```cpp
TypeRegistry::Register<MyComp>("mycomp", "我的组件");   // T 必须声明 kFields
const TypeInfo* info = TypeRegistry::Find("mycomp");     // schema + toJson/fromJson/clone
```
`TypeInfo` 携带：`name/label`、`fields`（编辑器 schema）、类型擦除的 `toJson/fromJson/clone`。
`RegisterBuiltinReflectedTypes()` 注册 `SceneHealth`/`SceneAudioSource`，并从
`RegisterBuiltinComponentSchemas()` 调用（幂等）。数据组件（plant/zombie 等走 `SceneData`）
暂不注册，继续由手写表服务——这正是反射正在逐组件消解的漂移。

## 6. 现状与后续

### 已落地
- 反射框架：全类型码 + 三类字段 + 嵌套/数组/枚举/Json + transient。
- `TypeRegistry`：类型擦除的 schema + 序列化 + clone，**幂等注册**，接入 `RegisterBuiltinComponentSchemas`。
- **编辑器 schema 单一来源（A2）**：`AllComponentSchemas()/FindComponentSchema` 现在优先取
  `TypeRegistry`（反射即权威），再并入无损的手写表（去重），所以反射组件不会再被手写孪生覆盖。
- **inspector 渲染补齐（A1）**：`FieldType::Vec4`（DragFloat4）、`Array`（列表编辑：数字/字符串
  内联 + 增减）、`Struct`（折叠 + 只读 JSON）可在编辑器渲染；"添加组件"默认构造也给出合法初值。
- **C6 序列化收敛**：`scene_file.cpp` 把 `FromWorld` 与 `EntityToJson` 的两个手写序列化块
  **收敛为共享的 `SerializeEntityComponents(world, entity)`**（唯一来源，二者用同一函数），
  并顺势修复了实际漂移 bug：原先 `EntityToJson` 漏写 `mesh` 的 `lod/uvRepeat/dirtColorHex/rockColorHex`、
  `sprite` 的 `frames/sheet`、以及 `rigidbody/terrain/tilemap/character/SceneData`，现在与 `FromWorld` 完全一致。
- **C7 脚本字段访问**：新增 `EntityComponentField(entity, comp, field)` / `SetEntityComponentField(...)`
  反射驱动的**单字段**读写绑定（复用 `EntityComponent`/`SetEntityComponent` 的 JSON hook 通道），
  脚本可改一个字段而不整表重写，字段名与反射 schema 一致。
- `FieldSchema` 增 `header/tooltip/widget`；`FieldType` 增 `Array/Struct/Vec4`（向后兼容）。
- **已迁移真实组件（编辑器 schema + JSON codec 走反射，共 12 个引擎组件 + RenderStack）**：
  `SceneHealth`、`SceneAudioSource`、`SceneSortOrder`、`SceneDecal`、`SceneCharacter`、`SceneRigidBody`、
  `SceneTransform`、`SceneGroups`、`SceneNodeType`、`SceneCamera`、`SceneLight`、`SceneScript`，
  以及数据驱动的 **`RenderStack`**（后处理栈，见下）注册为 `renderstack`。`SceneAnimOverride` 已声明
  `kFields`（含 Transient `active`）但暂不注册——它没有场景工厂（注册会在"添加组件"里出现不生效条目）。
  - 演示的技巧：
    - **键名↔成员名别名**——`SceneRigidBody` 的 `damping` 键 → `linearDamping` 成员。
    - **字符串枚举下拉**——`shape`/`type`/`light.type` 用 `FieldMeta{options}`。
    - **运行时字段 `Transient`**——`bodyId`/`active` 不存盘、不渲染。
    - **以结构体为准**——`SceneTransform::rot` 是真 `Quat`，编辑器用四元数输入（不再造假"欧拉角"）。
  - 序列化器顺带修复：补写 `camera.aspect`、`sprite.billboard`（此前被序列化丢弃）。
- **RenderStack（渲染数据驱动，A）**：`render_stack.hpp` 用反射描述后处理效果（SSAO/体积光/SSR/
  bloom/tonemap/雾 的开关+参数），注册进 `TypeRegistry`；编辑器属性面板自动生成、JSON 往返、脚本单字段访问。
  bloom 的 `threshold/strength` 已参数化进 `PostGraph::CompositeParams`/`Renderer::SetBloomParams`
  （默认=原常量，零回归）——"渲染开放给前端"的数据层。
- **D·MCP**：`neon/mcp/mcp_server.hpp` 提供 JSON-RPC 2.0（`initialize/tools/list/tools/call`），
  工具 `list_entities/get_component/set_component`，**set 用反射 schema 校验字段类型**；
  `tools/neon_mcp.cpp` 是 stdio server（`--scene` 加载、改动后漂亮化写盘）。AI 助手经标准 MCP 读写引擎组件。
- **E·多语言宿主**：`script_manager.cpp` 的 `CreateScriptHost(kind)` 统一按语言名建宿主；
  `python_host.hpp` 提供 **CPython 宿主**（`NEON_ENABLE_PYTHON` 门控，未编译则回落 Lua）。
- **B·资产 GUID**：中心式 `.asset_db.json`（`asset_db.hpp`，无 .meta）已存在；补 `ResolveAssetRef`
  （GUID 优先、路径回退）+ `IsGuidToken`，供未来 GUID 化引用解析。
- 构建：MSVC 加 **`/Zc:preprocessor`**（修复 legacy 预处理器对 `NEO_ENUM` 变参宏的计数缺陷）。
- 单测：`tests/test_reflect.cpp`（13 项反射）+ `tests/test_mcp.cpp`（2 项）+ `tests/test_script_host.cpp`
  （2 项）；MSVC 下 **775 项测试全部通过**、`neon_mcp` stdio 实机工作、`neon_editor` 编译通过。
- **剩余组件（mesh/sprite）**：因编辑器**专项实体区**渲染（网格/精灵区块，非通用 schema 字段区，
  schema 仅供"添加组件"下拉），迁移字段反射无编辑收益——维持专项，属通用引擎的 Custom 面板常态。

### 后续（按 TODO 项）
- **字段级 codec**：共享序列化器内部仍保留部分**条件省略**（如 `if (rb->dynamic)` 省略 true 值、
  `if (!s->sheet.empty())`）与**校验/回退**（rigidbody 非法 shape→sphere）。这些是**语义化**的，
  通用反射无法自动复现；如要彻底交给 `kFields`，需给这些字段配 `ReflectTraits<T>` 特化或字段级读写钩子。
- **Struct 递归字段编辑**：`FieldSchema::Struct` 尚未携带子字段 schema（inspector 现以只读 JSON
  展示）；递归可编辑需在 `FieldSchema` 里挂子 `ComponentSchema`/`FieldList` 指针。
- **G2-2**：把 `ComponentRegistry`（运行时工厂）与 `TypeRegistry`（元数据）进一步合并，
  让 `Instantiate` 对反射组件走统一创建。
- **A3·全量几何 FrameGraph**：把主场景/阴影/SSAO 等从**立即模式逐实体绘制流**（GameRuntime→renderer）
  重构为声明式 pass 图。跨 renderer/game_runtime/draw_system，且**必须 GPU 逐帧验证画面**——
  需在能跑 `--screenshot` 的环境专项迭代（第一步：逐实体绘制捕获→可重放列表 + `scene.geometry` pass）。
- **E·Python 运行时绑定**：`python_host.cpp`（CPython C-API + 确定性沙箱：指令/内存预算、确定性
  时钟+RNG，对齐 Lua/JS 守卫）需在**有 CPython ≥3 运行时**的环境实现+验证（`NEON_ENABLE_PYTHON` 开启）。
- **顺带发现的既有隐藏缺陷（本次未改，避免回归）**：`sprite` 组件的 `billboard` 字段在
  `FromWorld`/共享序列化器里都未写出（与 schema 不一致）——迁移 sprite 到 `kFields` 时一并修。
- **D2 插件权限**：`plugin.cpp:156` 已解析 `permissions` 但未强制执行（编辑器插件版本门 D1 同样）——
  插件隔离/权限白名单执行列为安全专项。

## 7. 如何给引擎加一个可反射组件（示例）

```cpp
struct SceneGear {
    int teeth = 16;
    float radius = 0.5f;
    gfx::Color tint{1,1,1,1};            // Color 内建支持
    std::vector<float> profile;          // 数组
    uint32_t bodyId = 0;                 // 运行时状态
    inline static const auto kFields = scene::ReflectFields(
        scene::Field("teeth",  "齿数", FieldType::Int, &SceneGear::teeth, 0, 1, 128),
        scene::Field("radius", "半径", FieldType::Number, &SceneGear::radius, 0.5, 0, 10),
        scene::Field("tint",   "颜色", FieldType::Color, &SceneGear::tint),
        scene::Field("profile","轮廓", FieldType::Array, &SceneGear::profile),
        scene::Field("bodyId", "物理体", FieldType::Int, &SceneGear::bodyId,
                     scene::FieldMeta{scene::FieldCategory::Transient}));
};
// 注册（编辑器 schema + 序列化 + clone 从此单一来源）
scene::TypeRegistry::Register<SceneGear>("gear", "齿轮");
// 或者并入内置注册表：在 RegisterBuiltinReflectedTypes() 里加一行。
```
新增字段类型只需特化一个 `ReflectTraits<T, void>`（定义 `ToJson/FromJson/Default/kKind`）。

## 8. 验收与回归

- 反射框架 + TypeRegistry 在项目头文件下的隔离编译、`test_reflect.cpp` 全部通过。
- `component_schema.cpp`/`scene_file.cpp` 用新头文件 `-fsyntax-only` 通过（未破坏现有消费方）。
- ⚠️ 全工程 `neon_tests` 链接在**本环境** MinGW GCC 8.1 上因**既有的**工具链问题无法完成
  （`vfs.cpp` 的 `_stat64` 构造、quickjs 的 `.seh_savexmm` 汇编器 bug 与本次改动无关）；
  在较新的 MSVC/MinGW 上应可完整链接。CI 应加一条以新工具链验证。
