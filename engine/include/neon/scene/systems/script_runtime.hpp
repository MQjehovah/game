#pragma once
#include <cstdint>
#include <functional>
#include <map>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "neon/ecs/world.hpp"
#include "neon/scene/scene_file.hpp"
#include "neon/script/bindings.hpp"
#include "neon/script/input_map.hpp"
#include "neon/script/script.hpp"

namespace neon::scene {

// 脚本运行时（内容运行时的心脏）：驱动场景脚本（Lua/JS）的加载与逐实体
// on_start/on_update 派发。纯机械拆分（Task 13）——GameRuntime 的
// scripts_/loadedScripts_/scriptFailed_/chunkHandlers_/inputMap_ 及
// AttachOneScript/AttachScripts/CallEntityFunctionHandle/HasScriptFunction/
// CallScriptFunction 迁入此类，无行为变化。
//
// 共享状态边界：scriptCtx_（ScriptContext，GameVars 拥有者）与 hosts_（Lua/JS
// host）被脚本 + 行为树（BtRuntime, Task 14）+ 绑定共享，保留在 GameRuntime；
// 方法以参数/引用接收。脚本源码读取依赖 cfg_（scriptBaseDir/readScript/
// fileSystem），同样保留在 GameRuntime，经 Configure() 注入（脚本按
// backend + full path 只加载一次）。
class ScriptRuntime {
public:
    // 一个已附加的脚本实例（原 GameRuntime::ScriptInst）。path 用于错误日志；
    // 源按路径只加载一次。host 是该实例 chunk 所在的 backend host；onStart/
    // onUpdate 是该 chunk 捕获的 handler 函数句柄（每个实例调自己的 chunk）。
    struct ScriptInst {
        ecs::Entity ent;
        std::string path;
        script::IScriptHost* host = nullptr;
        uint64_t onStart = 0;
        uint64_t onUpdate = 0;
        bool errorLogged = false; // one log per script instance per Start
        // 每实例快照的组件声明 vars（调用前注入 host 全局、调用后读回），
        // 两个实体带同一脚本时互不覆盖（A6 "last attach wins" 修复）。
        script::Value vars;
    };
    // 每个已加载 chunk 捕获的 handler 句柄缓存（keyed by backend + "|" + full
    // path）。Lua/JS host 共享一个全局环境，后加载的 chunk 会覆盖 on_start/
    // on_update 全局；缓存捕获的函数句柄让每个实例复用自己 chunk 的 handler。
    struct ChunkHandlers {
        uint64_t onStart = 0;
        uint64_t onUpdate = 0;
    };
    // 脚本 + 行为树共享的 backend host 集合（GameRuntime::ScriptHosts 的非拥有
    // 投影）。lua 是规范 backend（编辑器调试器面向它）；js 可选。
    struct Hosts {
        script::IScriptHost* lua = nullptr;
        script::IScriptHost* js = nullptr;
    };
    // 脚本源码读取（full path -> 文本）；GameRuntime 用 ReadScript 注入。
    using Reader = std::function<std::string(const std::string&)>;
    // 脚本内容来源：scriptBaseDir（full path 前缀）+ 读取回调。Configure 时注入。
    struct Content {
        std::string scriptBaseDir;
        Reader readScript;
    };

    void Configure(Content content);
    // 附加一个脚本组件（AttachScripts / 预制体 / Spawn 第 3 参 / SpawnEntity
    // 共用）。加载按 (backend, full path) 去重、捕获并缓存 handler、对声明了
    // on_start 的实例立即派发 on_start。返回 false 表示跳过（缺文件/编译错误/
    // 不安全路径/之前失败）。
    bool AttachOne(ecs::Entity ent, const SceneScript& s, script::ScriptContext& ctx,
                   Hosts hosts);
    // 批量附加：遍历收集的 (entity, script) 对，逐对 AttachOne。
    void AttachAll(const std::vector<std::pair<ecs::Entity, SceneScript>>& scripts,
                   script::ScriptContext& ctx, Hosts hosts);
    // 调用一个实例捕获的 chunk 函数（on_start/on_update），含每实体输入路由
    // 上下文与 vars 隔离；失败按实例去重记录，绝不中止运行时。
    void CallEntity(script::ScriptContext& ctx, ScriptInst& inst, uint64_t handle,
                    const char* fn, const std::vector<script::Value>& args);
    // 任一 backend host 定义了 `name`（外部主机驱动场景用：HasScriptFunction）。
    bool HasFunction(Hosts hosts, const std::string& name) const;
    // 调用命名全局函数（Lua 优先于 JS）；失败记录一次，不致命。
    bool CallFunction(script::ScriptContext& ctx, Hosts hosts, const std::string& name,
                      const std::vector<script::Value>& args) const;
    // 每帧派发所有存活实体的 on_update(ent, dt)。索引遍历：脚本里的 SpawnPrefab
    // 可能中途 push 新实例（迭代器会失效）；新实例同帧处理（spawned this frame
    // acts this frame）。
    void Tick(float dt, ecs::World& world, script::ScriptContext& ctx);
    void Clear(); // 清空实例与加载状态（Start/Stop；host 由 GameRuntime 管理）

    const std::vector<ScriptInst>& Instances() const { return scripts_; }
    std::vector<ScriptInst>& Instances() { return scripts_; }
    size_t Count() const { return scripts_.size(); }
    // Godot-style input action map（input.json；脚本 bindings 经 scriptCtx_.
    // inputMap 读取）。GameRuntime 负责加载/复位/每帧推进，这里只持有状态。
    script::InputMap* InputMap() { return &inputMap_; }
    const script::InputMap* InputMap() const { return &inputMap_; }

private:
    // 相对脚本路径 -> full path（scriptBaseDir 为空时原样返回）。
    std::string FullScriptPath(const std::string& path) const;

    std::vector<ScriptInst> scripts_;
    std::set<std::string> loadedScripts_; // resolved paths whose chunk ran (presence only)
    std::set<std::string> scriptFailed_;  // resolved paths that failed (skip later)
    std::map<std::string, ChunkHandlers> chunkHandlers_;
    script::InputMap inputMap_;
    std::string scriptBaseDir_;
    Reader readScript_;
};

} // namespace neon::scene
