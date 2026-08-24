# 插件系统（NeonEngine）

插件是引擎的平台化扩展机制：**编辑器插件**扩展编辑器本身（面板/工具/资产源/
组件检查器），**运行时插件**是跨游戏复用的玩法系统模块（任务/背包/经济/副本）。
两者共用同一套清单（`plugin.json`）与双语言加载器（Lua / QuickJS），并共享
引擎的确定性沙箱（无 io/os、引擎注入的 RNG 与模拟时钟）。

## 插件目录与清单

```
plugins/
  tree_gen/
    plugin.json
    init.lua
  inventory/
    plugin.json
    init.js
```

```json
{
  "id": "tree_gen",
  "name": "树木生成器",
  "version": "1.0.0",
  "type": "editor",
  "backend": "lua",
  "entry": "init.lua",
  "minEngineVersion": "0.1.0",
  "requires": [],
  "permissions": []
}
```

- `type`: `editor`（编辑器扩展）/ `runtime`（运行时系统模块）/ `native`（预留，未实现）
- `backend`: `lua` | `js`（复用 `IScriptHost` 双后端）
- `minEngineVersion`: 引擎版本门槛，不满足则跳过加载
- `requires`: 依赖的插件 id（加载时按依赖排序）
- `permissions`: 权限声明（目前为审计字段，未强制）

## 运行时插件 API（Lua / JS 同构）

入口脚本定义 `on_load()`，通过全局 `Plugin` 使用：

- `Plugin.Info()` -> {id,name,version,type,backend}
- `Plugin.Log(level, msg)`
- `Plugin.On("tick" | "stop" | "player_join" | 任意事件, fn)`
- `Plugin.OnCommand(name, fn, help)` — 命令注册（服务器可用 `RunPluginCommand` 调用）
- `Plugin.GetVar(key)` / `Plugin.SetVar(key, value)` — 作用域为 `plugin:<id>:<key>`
- `Plugin.Export(name, fn)` / `Plugin.Call(pluginId, name, ...)` — 模块 API（跨插件调用）
- `Plugin.RegisterComponent(name, schema)` — 注册编辑器组件检查器 schema
- 引擎绑定全量可用：`Spawn` / `SetVar` / `Raycast` / `CastSkill` / `EntityComponent` ...

加载顺序：`GameRuntime::Start` 扫描 `<scriptBaseDir>/plugins`，按依赖排序加载，
调用 `on_load()` 与 `on_start()`；`Tick` 派发 `tick` 事件；`Stop` 派发 `stop`。
服务器把 `player_join` 等事件转发给插件（`GameServer::HandleJoin`）。

## 编辑器插件 API（`NeonEditor`）

入口脚本定义 `on_load()`，通过全局 `NeonEditor` 使用：

- `NeonEditor.panel(id, title, drawFn)` — 注册可停靠面板（自动进"视图"菜单）
- `NeonEditor.tool(id, label, fn)` — 工具条按钮
- `NeonEditor.assetSource(id, name, listFn, importFn)` — 资产源（素材市场），
  出现在资产面板；`listFn` 返回 `{name,type,path}` 数组，`importFn(path)` 导入
- `NeonEditor.registerComponent(name, schema)` — 注册组件 schema，检查器自动生成 UI
- `NeonEditor.buildMesh(name, verts, indices)` — 程序化网格（OBJ 资产），返回 `obj:...`
- `NeonEditor.spawn(meshKey, x, y, z)` — 生成实体（可撤销）
- `NeonEditor.selected()` / `NeonEditor.entities()` — 读取编辑器场景
- `NeonEditor.importAsset(path)` / `NeonEditor.listDir(dir)` — 受控文件访问
- `NeonEditor.log(msg)`
- `NeonEditor.ui.*` — 精选 ImGui 控件：Button / Checkbox / SliderFloat /
  DragFloat / DragFloat3 / InputText / Combo / Table / TreeNode / ColorEdit4 /
  ProgressBar / Tooltip / BeginChild 等

示例插件位于 `plugins/`：

| 插件 | 类型 | 说明 |
| --- | --- | --- |
| `tree_gen` | editor/lua | 程序化树木生成器（面板 + 工具条 + buildMesh + spawn） |
| `asset_vault` | editor/lua | 本地素材库资产源（listDir + importAsset） |
| `inventory` | runtime/js | 背包系统（组件 schema + GM 命令 + 事件 + 模块 API） |

## 关键实现

- `engine/src/plugin/plugin.cpp` — 清单解析 + 版本校验 + 目录发现
- `engine/src/plugin/runtime_plugin.cpp` — 运行时插件管理器（双语言宿主、事件/命令/模块 API）
- `editor/src/editor_plugin.cpp` — 编辑器插件管理器（NeonEditor API + ImGui 绑定）
- 自定义组件：无工厂的组件以 `SceneData` 存入实体，脚本经 `EntityComponent(entity, name)`
  读取（对 `plant` 这类纯数据组件同样生效）
- 嵌套命名空间：`RegisterField` 支持点号路径（`NeonEditor.ui.Button`），Lua/JS 一致

## 路线图

- [x] 清单 + 发现 + 版本/类型门禁
- [x] 运行时插件（Lua + JS，事件/命令/模块 API/作用域状态）
- [x] 编辑器插件（面板/工具/资产源/组件 schema/程序化网格）
- [x] 服务器事件桥接（player_join）
- [ ] 权限系统深化（网络/RPC 按权限开放）
- [ ] 插件管理面板（启用/禁用/重载/日志）
- [ ] 打包器插件模式（zip 制品 + 安装）
- [ ] 原生 C++ 插件（`DynamicLibrary` + 稳定 C ABI）
