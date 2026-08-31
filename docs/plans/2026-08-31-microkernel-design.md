# NeonEngine 微内核模块化设计

日期：2026-08-31
状态：已确认（接口形态 = 混合：内核/注册表用 C++ 接口，二进制可替换模块用 C ABI 包一层）

## 1. 目标

把引擎重构为**微内核 + 模块**架构：内核只做模块加载、服务注册、主循环；每个子系统
（渲染/物理/音频/脚本/资产/场景/UI/BT/动画/导航）都是实现统一 `IModule` 接口的模块，
通过 `ServiceRegistry` 拿依赖、通过 `EventBus` 通信。**每个模块都可重写/替换**——替换一个
模块 = 换掉注册表里的一项，其余模块与内核不动。

## 2. 核心抽象（P-A，第一阶段）

位置：`engine/include/neon/kernel/`，属于 `neon_core`（L0/L1 基础层，gfx/scene 依赖它）。

### 2.1 模块接口 `IModule`

```cpp
struct ModuleInfo {
    const char* id;                 // "gfx" / "physics" / ...
    const char* version;
    std::vector<const char*> requires;  // 依赖的模块 id（先加载）
};

class IModule {
    virtual ModuleInfo Info() const = 0;
    virtual bool Init(ServiceRegistry& reg) = 0;  // 注册服务 + 拿依赖；false = 失败
    virtual void Shutdown() = 0;
};
```

模块之间**不互相 include/链接**：在 `Init` 里把服务注册到 `ServiceRegistry`，用
`reg.Get<T>()` 拿依赖。

### 2.2 服务注册表 `ServiceRegistry`

```cpp
template <typename T> void Register(T* service);  // 按接口类型注册
template <typename T> T* Get() const;             // 按接口类型取；缺失返回 nullptr
```

类型擦除（`type_index` → `void*`）。一个接口类型一个实现，替换 = 覆盖注册。

### 2.3 模块注册表 `ModuleRegistry`

```cpp
void Add(std::unique_ptr<IModule> m);
bool InitAll(ServiceRegistry& reg);  // 按 `requires` 拓扑序初始化；环/缺依赖失败
void ShutdownAll();                  // 逆序关闭
```

## 3. 接口形态决策（已确认）

- **内核/注册表 + 模块间通信**：C++ 纯虚接口（源码级替换，零开销）。
- **二进制可替换模块（DLL）**：C ABI 函数表（`NeonPhysicsApi` 模式），由内核包一层
  `IModule` 适配器加载。原生插件可独立发版、不重编译替换。
- 已存在先例：`IRenderBackend`/`IAudioBackend`（C++ 接口）、`physics::World`（抽象）、
  `NeonPhysicsApi`/`NeonPhysicsWorldApi`（C ABI）。

## 4. 现状 → 目标映射

| 子系统 | 现状 | 模块化动作 |
|---|---|---|
| 渲染 gfx | `IRenderBackend`（GL/Vulkan） | P-C 接成 `IModule`，注册 `IRenderBackend`+`Renderer` |
| 物理 physics | `physics::World`（自研/Jolt/插件） | P-C 接成 `IModule` |
| 音频 audio | `IAudioBackend` | P-C 接成 `IModule` |
| 脚本 script | `IScriptHost` | P-C 接成 `IModule` |
| 平台 platform | `IWindow`/`IInput` | P-C 接成 `IModule` |
| 资产 assets | `AssetManager`（无接口） | P-D 补 `IAssetStore` 接口 |
| 场景 scene | `GameRuntime`（上帝对象） | P-B 拆成组合服务（最大工程） |
| UI / BT / 动画 / 导航 | 各自独立类 | P-D 补模块边界 |
| 内核 Application | 与 game 层耦合 | P-E 瘦身为加载器 |

## 5. 增量路径

- **P-A（本阶段）**：`neon/kernel/module.hpp` + `registry.hpp` + `registry.cpp` + 测试。
  纯新增，不动现有子系统，零回归。
- **P-B**：`GameRuntime` 拆成组合服务（渲染/物理/脚本/动画各从注册表拿），打通模块化的钥匙。
- **P-C**：已接口化的 5 个子系统接成正式 `IModule`。
- **P-D**：assets/ui/bt/anim 补接口。
- **P-E**：`Application` 瘦身为"模块加载 + 主循环"。

## 6. 验收（P-A）

- 单测：依赖拓扑序初始化、服务注册/查找、模块替换（换实现后依赖方拿到新实现）、环检测失败、缺依赖失败。
- `neon_tests` 全绿；现有 688 项不回归。
