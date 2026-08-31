// BtRuntime implementation. Migrated from GameRuntime's AttachTrees /
// CallScriptOnTree / EntityBlackboardValue / ActiveTreePath plus the per-frame
// behavior-tree driving block in Tick (Task 14). Pure code movement, no
// semantic change: scriptCtx_ (ScriptContext, owns GameVars) and the Lua/JS
// hosts stay on GameRuntime as shared state (scripts + behavior trees +
// bindings all share them); the tree instances and the per-tick Context
// construction moved here.
#include "neon/scene/systems/bt_runtime.hpp"

#include <algorithm>
#include <utility>

#include "neon/core/log.hpp"
#include "neon/core/pack.hpp"
#include "neon/scene/scene_file.hpp"

namespace neon::scene {
namespace {

// Stable 64-bit key for per-entity BT/blackboard scoping: id occupies the high
// half so an id reused across generations still keys uniquely.
uint64_t EntityKey(const ecs::Entity& e) {
    return (static_cast<uint64_t>(e.id) << 32) | static_cast<uint64_t>(e.generation);
}

} // namespace

void BtRuntime::Configure(Content content) {
    scriptBaseDir_ = std::move(content.scriptBaseDir);
    readScript_ = std::move(content.readScript);
}

script::Value BtRuntime::CallOnTree(script::ScriptContext& ctx, const BtInst& inst,
                                    const std::string& fn, uint64_t ent) {
    script::IScriptHost* host = inst.host;
    if (!host || !host->HasFunction(fn)) return script::Value::Nil();
    script::Value entVal = script::Value::Tbl();
    entVal.table->fields.emplace_back("id", script::Value::Num(static_cast<double>(ent)));
    ctx.currentEntity = inst.ent;
    const core::Result<script::Value> res = host->Call(fn, {entVal});
    ctx.currentEntity = {};
    if (!res.Ok()) {
        NEON_LOG_CAT(core::LogCategory::Bt, core::LogLevel::Error,
                     "runtime: tree script %s() failed: %s", fn.c_str(),
                     host->LastError().message.c_str());
        return script::Value::Nil();
    }
    return res.Value();
}

void BtRuntime::AttachAll(ecs::World& world, script::ScriptContext& ctx, Hosts hosts) {
    (void)ctx;
    auto view = world.ViewAll<SceneBehaviorTree>();
    for (size_t i = 0; i < view.Size(); ++i) {
        ecs::Entity ent = world.EntityAt<SceneBehaviorTree>(i);
        const SceneBehaviorTree* bt = world.Get<SceneBehaviorTree>(ent);
        if (!bt) continue;

        // The tree field is either inline JSON ("{...}") or a named reference
        // ("bt:<name>") resolving to assets/behaviors/<name>.bt.json under the
        // script base dir. The same ReadScript path used for Lua scripts honors
        // the readScript override (pack readers) and the disk reader, so packed
        // games load named trees exactly like loose-file projects.
        std::string treeText;
        const std::string& ref = bt->treeJson;
        if (ref.compare(0, 3, "bt:") == 0) {
            const std::string name = ref.substr(3);
            if (name.empty()) {
                NEON_LOG_CAT(neon::core::LogCategory::Bt, neon::core::LogLevel::Error,
                             "runtime: empty behavior tree reference 'bt:' (skipped)");
                continue;
            }
            // Defense-in-depth: reject names that could escape the base dir
            // (a hostile "bt:../../secret" would otherwise read an arbitrary
            // local file).
            if (neon::core::IsUnsafeRelPath(name)) {
                NEON_LOG_CAT(neon::core::LogCategory::Bt, neon::core::LogLevel::Error,
                             "runtime: behavior tree '%s' has an unsafe name (skipped)",
                             ref.c_str());
                continue;
            }
            treeText = readScript_(FullScriptPath("assets/behaviors/" + name + ".bt.json"));
            if (treeText.empty()) {
                NEON_LOG_CAT(neon::core::LogCategory::Bt, neon::core::LogLevel::Error,
                             "runtime: cannot read behavior tree '%s' (skipped)", ref.c_str());
                continue;
            }
        } else if (!ref.empty() && ref[0] == '{') {
            treeText = ref; // inline JSON tree
        } else {
            NEON_LOG_CAT(neon::core::LogCategory::Bt, neon::core::LogLevel::Error,
                         "runtime: behavior tree reference '%s' is neither inline JSON "
                         "nor 'bt:<name>' (skipped)",
                         ref.c_str());
            continue;
        }
        // A UTF-8 BOM (common on Windows editors) must not break the JSON parse.
        if (treeText.size() >= 3 && static_cast<unsigned char>(treeText[0]) == 0xEF &&
            static_cast<unsigned char>(treeText[1]) == 0xBB &&
            static_cast<unsigned char>(treeText[2]) == 0xBF) {
            treeText.erase(0, 3);
        }

        auto tree = std::make_unique<bt::BehaviorTree>();
        std::string err;
        if (!tree->LoadText(treeText, &err)) {
            NEON_LOG_CAT(neon::core::LogCategory::Bt, neon::core::LogLevel::Error,
                         "runtime: entity behavior tree failed to load: %s (skipped)",
                         err.c_str());
            continue;
        }
        BtInst inst;
        inst.ent = ent;
        inst.tree = std::move(tree);
        // run_script / script_bool nodes call global functions on the tree
        // entity's own script backend (first mounted script's `backend`;
        // default Lua), so a JS-scripted entity's tree talks to JS.
        std::string backend = "lua";
        if (const SceneScript* sc = world.Get<SceneScript>(ent))
            backend = sc->backend.empty() ? "lua" : sc->backend;
        else if (const SceneScripts* list = world.Get<SceneScripts>(ent))
            if (!list->items.empty())
                backend = list->items[0].backend.empty() ? "lua"
                                                         : list->items[0].backend;
        inst.host = hosts.Get(backend);
        if (!inst.host) inst.host = hosts.lua;
        trees_.push_back(std::move(inst));
    }
}

void BtRuntime::Tick(float dt, ecs::World& world, script::ScriptContext& ctx) {
    size_t deadTrees = 0;
    for (BtInst& inst : trees_) {
        if (!world.Alive(inst.ent)) {
            ++deadTrees; // scripts can Despawn entities mid-playtest
            continue;
        }
        bt::Context c(ctx.gameVars, &inst.board);
        c.entity = EntityKey(inst.ent);
        c.dt = dt;
        // run_script / script_bool nodes execute a named global function on
        // the tree entity's script backend (wired here for the first time; the
        // hook previously existed but nothing set it).
        c.callScript = [this, &ctx, &inst](const std::string& fn, uint64_t ent) {
            return CallOnTree(ctx, inst, fn, ent);
        };
        c.timers.swap(inst.timers); // carry over accumulated wait/cooldown state
        inst.tree->Tick(c);
        c.timers.swap(inst.timers); // persist it for the next tick
        inst.activePath = c.activePath; // debug highlight: deepest node that ran
    }
    // Compact when a fifth of the trees belong to dead entities.
    if (deadTrees && deadTrees * 5 > trees_.size()) {
        trees_.erase(std::remove_if(trees_.begin(), trees_.end(),
                                    [&world](const BtInst& i) { return !world.Alive(i.ent); }),
                     trees_.end());
    }
}

script::Value BtRuntime::BlackboardValue(const ecs::Entity& ent,
                                         const std::string& key) const {
    for (const BtInst& inst : trees_) {
        if (inst.ent == ent) return inst.board.Get(EntityKey(ent), key);
    }
    return script::Value::Nil();
}

std::string BtRuntime::ActivePath(const ecs::Entity& ent) const {
    for (const BtInst& inst : trees_) {
        if (inst.ent == ent) return inst.activePath;
    }
    return {};
}

std::string BtRuntime::FullScriptPath(const std::string& path) const {
    if (path.empty() || scriptBaseDir_.empty()) return path;
    return scriptBaseDir_ + "/" + path;
}

void BtRuntime::Clear() {
    trees_.clear();
}

} // namespace neon::scene
