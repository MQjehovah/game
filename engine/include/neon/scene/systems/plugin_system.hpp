#pragma once

#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "neon/plugin/runtime_plugin.hpp"
#include "neon/script/bindings.hpp"
#include "neon/script/script.hpp"

namespace neon::scene {

// 运行时插件系统：加载/驱动跨游戏玩法模块（plugin.json + Lua/JS）。纯机械拆分自
// GameRuntime 的 plugins_/DispatchPluginEvent/RunPluginCommand（Task 8）：Load
// 创建 RuntimePluginManager 并扫 <scriptBaseDir>/plugins 按依赖排序加载 +
// on_start，Tick 注入模拟时钟后派发 tick，Shutdown 派发 stop 并回收 manager。
// 事件/命令与 Manager 访问仍由 GameRuntime（及 server）继续转发。
class PluginSystem {
public:
    using ReadScriptFn = std::function<std::string(const std::string&)>;

    // 创建 manager、扫 <scriptBaseDir>/plugins 按依赖排序加载并 on_start。
    // ctx 提供引擎绑定上下文（gameVars 取 ctx->gameVars，与 GameRuntime 一致）；
    // rngSeed 与场景脚本共享同一随机流（0 别名 seed 1）。可重复调用：每次重建
    // 一个全新 manager（旧 manager 随析构派发 stop）。
    void Load(const std::string& scriptBaseDir, const ReadScriptFn& readScript,
              script::ScriptContext* ctx, uint64_t rngSeed);
    // 注入模拟时钟后派发 tick 处理器。
    void Tick(float dt, double simTime);
    // 派发 stop 处理器并回收 manager（Load 之后可安全调用多次）。
    void Shutdown();
    bool Active() const { return static_cast<bool>(manager_); }

    plugin::RuntimePluginManager* Manager() { return manager_.get(); }
    const plugin::RuntimePluginManager* Manager() const { return manager_.get(); }
    // 派发命名事件到所有订阅的插件；未加载或未订阅时 no-op，返回是否已加载。
    bool DispatchEvent(const std::string& name, const std::vector<script::Value>& args);
    // 运行插件注册的命令；未加载、命令未知或处理器抛错时返回 false。
    bool RunCommand(const std::string& name, const std::vector<script::Value>& args,
                    std::string* error);

private:
    std::unique_ptr<plugin::RuntimePluginManager> manager_;
};

} // namespace neon::scene
