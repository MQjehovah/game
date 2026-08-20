#pragma once
#include <cstdint>
#include <functional>
#include <memory>
#include <string>

#include "neon/core/json.hpp"
#include "neon/script/blackboard.hpp"
#include "neon/script/gamevars.hpp"
#include "neon/script/script.hpp"

namespace neon::bt {

// Runtime status returned by every node's Tick.
enum class Status { Running, Success, Failure };

class Context;
class Args;
class BehaviorTree;

// Base of every behavior-tree node. Nodes are data: they are built once from
// JSON and evaluated per tick against a runtime Context. The engine stays
// host-agnostic; gameplay effects are injected as optional std::function hooks
// on the Context (a null hook makes the affected node fail gracefully).
class Node {
public:
    virtual ~Node() = default;

    // Evaluate the node once against the runtime context.
    virtual Status Tick(Context& ctx) = 0;

    // Human-readable name for editor/debug. Defaults to the JSON `name`,
    // falling back to the node's `type` string.
    const std::string& Name() const { return name_; }
    void SetName(const std::string& name) { name_ = name; }

protected:
    std::string name_;
};

using NodePtr = std::unique_ptr<Node>;

// Runtime context handed to every Tick. `gameVars` is the global key-value
// store; `blackboard` is the entity-level store (may be null, in which case
// blackboard reads return defaults and writes fail). `entity` identifies the
// entity the tree is running for; blackboard access is scoped to it.
struct Context {
    explicit Context(script::GameVars& gv, script::Blackboard* bb = nullptr)
        : gameVars(gv), blackboard(bb) {}

    script::GameVars& gameVars;
    script::Blackboard* blackboard = nullptr;
    uint64_t entity = 0;
    float dt = 0.f;

    // Gameplay hooks. Optional: when a node needs a hook that is not set it
    // fails gracefully instead of crashing.
    std::function<bool(uint64_t ent, float distance)> inRange;
    std::function<bool(uint64_t ent, float speed)> moveTo;
    std::function<bool(uint64_t ent)> attack;
    std::function<void(const std::string& text)> dialogue;
    std::function<uint64_t(const std::string& kind, uint64_t ent)> spawn;
    std::function<void(const std::string& name)> playSfx;
    // Executes a script snippet for the entity and returns its value. The BT
    // engine does not own a script host; the game injects one here.
    std::function<script::Value(const std::string& script, uint64_t ent)> callScript;

    // Blackboard accessors that tolerate a null blackboard.
    bool BBHas(const std::string& key) const;
    script::Value BBGet(const std::string& key) const;
    bool BBSet(const std::string& key, const script::Value& v);
};

// Data-driven accessor over a node's JSON `args` object. Each node reads the
// parameterized keys it understands; a missing key falls back to the supplied
// default, and a wrong-typed value is ignored in favor of the default.
class Args {
public:
    explicit Args(const core::Json* json) : json_(json) {}
    bool Has(const std::string& key) const;
    double Number(const std::string& key, double def) const;
    int Int(const std::string& key, int def) const;
    bool Bool(const std::string& key, bool def) const;
    std::string Str(const std::string& key, const std::string& def) const;
    // Any JSON scalar/array/object converted to a script::Value (Nil when the
    // key is missing). Used by blackboard_set and the *_cmp conditions.
    script::Value Value(const std::string& key) const;

private:
    const core::Json* json_;
};

// Value helpers shared by conditions and actions.
bool ValueTruthy(const script::Value& v);
// `op` is one of "==", "!=", "<", "<=", ">", ">=" (aliases eq/ne/lt/le/gt/ge
// and "truthy"). Relational ops require both sides to be numbers.
bool CompareValues(const script::Value& a, const script::Value& b, const std::string& op);
script::Value JsonToValue(const core::Json& j);

// A loadable, tickable behavior tree. Trees are data: load once from JSON,
// then Tick per entity/frame. BehaviorTree is movable but not copyable.
class BehaviorTree {
public:
    // Build the tree from a JSON DOM containing a "root" node. Returns false
    // and fills `error` on a structural error (missing root, unknown node
    // type, missing required child).
    bool Load(const core::Json& json, std::string* error = nullptr);
    bool LoadText(const std::string& text, std::string* error = nullptr);

    Status Tick(Context& ctx);
    bool Valid() const { return root_ != nullptr; }

private:
    NodePtr root_;
};

} // namespace neon::bt
