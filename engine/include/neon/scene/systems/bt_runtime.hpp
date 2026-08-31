#pragma once
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "neon/bt/behavior_tree.hpp"
#include "neon/ecs/world.hpp"
#include "neon/script/bindings.hpp"
#include "neon/script/script.hpp"

namespace neon::scene {

// 行为树运行时（Task 14）：驱动场景实体行为树的加载/逐帧 Tick 与
// run_script/script_bool 节点的脚本 hook。纯机械拆分——GameRuntime 的
// trees_/AttachTrees/CallScriptOnTree/EntityBlackboardValue/ActiveTreePath 及
// Tick 里的 BT 驱动段迁入此类，无行为变化。
//
// 共享状态边界：scriptCtx_（ScriptContext，GameVars 拥有者）与 hosts_（Lua/JS
// host）被脚本（ScriptRuntime, Task 13）+ 行为树 + 绑定共享，保留在 GameRuntime；
// 方法以参数/引用接收（与 ScriptRuntime::Hosts 同一模式）。树文本读取依赖
// cfg_（scriptBaseDir/readScript/fileSystem），同样保留在 GameRuntime，经
// Configure() 注入：AttachAll 解析 "bt:<name>" 引用时走
// readScript(scriptBaseDir + "/assets/behaviors/<name>.bt.json")。
class BtRuntime {
public:
    // 一个已附加的行为树实例（原 GameRuntime::BtInst）。
    struct BtInst {
        ecs::Entity ent;
        std::unique_ptr<bt::BehaviorTree> tree;
        script::Blackboard board;
        // Persistent per-entity timer state. bt::Context owns its timers map
        // but is rebuilt per tick, so the map is parked here between ticks and
        // swapped in/out of the fresh Context (a Context cannot be stored: it
        // binds a GameVars reference and the board address may move).
        std::map<uint64_t, std::map<std::string, float>> timers;
        // bt::Context::activePath captured after the last Tick (debug highlight).
        std::string activePath;
        // The host run_script/script_bool nodes call through (the tree
        // entity's script backend, defaulting to Lua).
        script::IScriptHost* host = nullptr;
    };
    // 脚本 + 行为树共享的 backend host 集合（GameRuntime::ScriptHosts 的非拥有
    // 投影）。lua 是规范 backend（编辑器调试器面向它）；js 可选。
    struct Hosts {
        script::IScriptHost* lua = nullptr;
        script::IScriptHost* js = nullptr;
        script::IScriptHost* Get(const std::string& backend) const {
            return backend == "js" ? js : lua;
        }
    };
    // 树文本读取（full path -> 文本）；GameRuntime 用 ReadScript 注入。
    using Reader = std::function<std::string(const std::string&)>;
    struct Content {
        std::string scriptBaseDir;
        Reader readScript;
    };

    void Configure(Content content);
    // 遍历 world_ 中带 SceneBehaviorTree 组件的实体，解析树文本（内联 JSON 或
    // "bt:<name>" 引用 assets/behaviors/<name>.bt.json），加载并构造 trees_。
    // run_script/script_bool 节点的脚本后端取实体首个脚本的 backend（默认 Lua）。
    void AttachAll(ecs::World& world, script::ScriptContext& ctx, Hosts hosts);
    // 每帧驱动每棵存活实体的树：构造 bt::Context（绑 GameVars + blackboard）、
    // 交换持久 timers、调 tree->Tick、记录 activePath（debug highlight）。死实体
    // 计数，累计到五分之一时压缩 trees_。
    void Tick(float dt, ecs::World& world, script::ScriptContext& ctx);
    // 行为树脚本 hook：调用命名全局函数（run_script/script_bool 节点）；函数缺失
    // 或调用失败返回 Nil。
    script::Value CallOnTree(script::ScriptContext& ctx, const BtInst& inst,
                             const std::string& fn, uint64_t ent);
    // 实体的 blackboard 值（无树/键未设时 Nil）。观测用（test/debug）。
    script::Value BlackboardValue(const ecs::Entity& ent, const std::string& key) const;
    // 实体最近 tick 的节点路径 id（无树/未 tick 时 ""）。观测用（编辑器高亮）。
    std::string ActivePath(const ecs::Entity& ent) const;
    void Clear(); // 清空实例（Start/Stop；host 由 GameRuntime 管理）

    const std::vector<BtInst>& Trees() const { return trees_; }
    size_t Count() const { return trees_.size(); }

private:
    // 相对脚本路径 -> full path（scriptBaseDir 为空时原样返回）。
    std::string FullScriptPath(const std::string& path) const;

    std::vector<BtInst> trees_;
    std::string scriptBaseDir_;
    Reader readScript_;
};

} // namespace neon::scene
