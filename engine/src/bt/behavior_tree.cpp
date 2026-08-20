#include "neon/bt/nodes.hpp"

#include <algorithm>
#include <functional>
#include <map>
#include <vector>

#include "neon/core/rng.hpp"

namespace neon::bt {

// ---------------------------------------------------------------------------
// Context
// ---------------------------------------------------------------------------

bool Context::BBHas(const std::string& key) const {
    return blackboard && blackboard->Has(entity, key);
}

script::Value Context::BBGet(const std::string& key) const {
    return blackboard ? blackboard->Get(entity, key) : script::Value::Nil();
}

bool Context::BBSet(const std::string& key, const script::Value& v) {
    if (!blackboard) return false;
    blackboard->Set(entity, key, v);
    return true;
}

// ---------------------------------------------------------------------------
// Args
// ---------------------------------------------------------------------------

bool Args::Has(const std::string& key) const {
    return json_ && json_->Get(key) != nullptr;
}

double Args::Number(const std::string& key, double def) const {
    const core::Json* v = json_ ? json_->Get(key) : nullptr;
    return v && v->IsNumber() ? v->GetNumber(def) : def;
}

int Args::Int(const std::string& key, int def) const {
    const core::Json* v = json_ ? json_->Get(key) : nullptr;
    return v && v->IsNumber() ? v->GetInt(def) : def;
}

bool Args::Bool(const std::string& key, bool def) const {
    const core::Json* v = json_ ? json_->Get(key) : nullptr;
    return v && v->IsBool() ? v->GetBool(def) : def;
}

std::string Args::Str(const std::string& key, const std::string& def) const {
    const core::Json* v = json_ ? json_->Get(key) : nullptr;
    return v && v->IsString() ? v->GetString(def) : def;
}

script::Value Args::Value(const std::string& key) const {
    const core::Json* v = json_ ? json_->Get(key) : nullptr;
    return v ? JsonToValue(*v) : script::Value::Nil();
}

// ---------------------------------------------------------------------------
// Value helpers
// ---------------------------------------------------------------------------

bool ValueTruthy(const script::Value& v) {
    switch (v.type) {
        case script::Value::Type::Nil: return false;
        case script::Value::Type::Number: return v.number != 0.0;
        case script::Value::Type::String: return !v.str.empty();
        case script::Value::Type::Bool: return v.boolean;
        case script::Value::Type::Table: return true;
    }
    return false;
}

script::Value JsonToValue(const core::Json& j) {
    switch (j.type()) {
        case core::Json::Type::Null: return script::Value::Nil();
        case core::Json::Type::Bool: return script::Value::Bool(j.GetBool());
        case core::Json::Type::Number: return script::Value::Num(j.GetNumber());
        case core::Json::Type::String: return script::Value::Str(j.GetString());
        case core::Json::Type::Array: {
            script::Value t = script::Value::Tbl();
            for (size_t i = 0; i < j.Size(); ++i)
                t.table->array.push_back(JsonToValue(*j.At(i)));
            return t;
        }
        case core::Json::Type::Object: {
            script::Value t = script::Value::Tbl();
            for (const auto& kv : j.Members()) t.table->fields.emplace_back(kv.first, JsonToValue(kv.second));
            return t;
        }
    }
    return script::Value::Nil();
}

namespace {
bool ValuesEqual(const script::Value& a, const script::Value& b) {
    if (a.type != b.type) return false;
    switch (a.type) {
        case script::Value::Type::Nil: return true;
        case script::Value::Type::Number: return a.number == b.number;
        case script::Value::Type::String: return a.str == b.str;
        case script::Value::Type::Bool: return a.boolean == b.boolean;
        case script::Value::Type::Table: return a.table == b.table;
    }
    return false;
}
} // namespace

bool CompareValues(const script::Value& a, const script::Value& b, const std::string& op) {
    if (op == "==" || op == "eq") return ValuesEqual(a, b);
    if (op == "!=" || op == "ne") return !ValuesEqual(a, b);
    if (op == "truthy") return ValueTruthy(a);
    if (a.type != script::Value::Type::Number || b.type != script::Value::Type::Number)
        return false; // relational ops are numeric
    if (op == "<" || op == "lt") return a.number < b.number;
    if (op == "<=" || op == "le") return a.number <= b.number;
    if (op == ">" || op == "gt") return a.number > b.number;
    if (op == ">=" || op == "ge") return a.number >= b.number;
    return false; // unknown operator -> false
}

// ---------------------------------------------------------------------------
// Composite ticks
// ---------------------------------------------------------------------------

Status SequenceNode::Tick(Context& ctx) {
    for (auto& child : children_) {
        Status s = child->Tick(ctx);
        if (s != Status::Success) return s;
    }
    return Status::Success;
}

Status SelectorNode::Tick(Context& ctx) {
    for (auto& child : children_) {
        Status s = child->Tick(ctx);
        if (s != Status::Failure) return s;
    }
    return Status::Failure;
}

Status RandomSelectorNode::Tick(Context& ctx) {
    // Deterministic per-entity shuffle: identical trees under the same entity
    // pick the same order. (entity == 0 keeps the non-zero default constant.)
    const uint64_t kMix = 0xD1B54A32D192ED03ull;
    core::Rng rng(0x9E3779B97F4A7C15ull ^ (ctx.entity * kMix));

    std::vector<size_t> order(children_.size());
    for (size_t i = 0; i < order.size(); ++i) order[i] = i;
    for (size_t i = order.size(); i > 1; --i) {
        size_t j = static_cast<size_t>(rng.Int(0, static_cast<int>(i)));
        std::swap(order[i - 1], order[j]);
    }
    for (size_t idx : order) {
        Status s = children_[idx]->Tick(ctx);
        if (s != Status::Failure) return s;
    }
    return Status::Failure;
}

Status ParallelNode::Tick(Context& ctx) {
    const size_t total = children_.size();
    int need = threshold_ > 0 ? threshold_ : static_cast<int>(total);
    if (need <= 0) need = 1;

    int successes = 0, failures = 0;
    for (auto& child : children_) {
        Status s = child->Tick(ctx);
        if (s == Status::Success) ++successes;
        else if (s == Status::Failure) ++failures;
    }
    if (successes >= need) return Status::Success;
    if (failures > static_cast<int>(total) - need) return Status::Failure;
    return Status::Running;
}

// ---------------------------------------------------------------------------
// Decorator ticks
// ---------------------------------------------------------------------------

Status InvertNode::Tick(Context& ctx) {
    if (!child_) return Status::Failure;
    Status s = child_->Tick(ctx);
    if (s == Status::Success) return Status::Failure;
    if (s == Status::Failure) return Status::Success;
    return Status::Running;
}

Status CooldownNode::Tick(Context& ctx) {
    if (!child_) return Status::Failure;
    if (remaining_ > 0.f) {
        remaining_ -= ctx.dt;
        if (remaining_ > 0.f) return Status::Failure; // blocked
        remaining_ = 0.f;
    }
    Status s = child_->Tick(ctx);
    if (s == Status::Success && seconds_ > 0.f) remaining_ = seconds_;
    return s;
}

Status RepeatNode::Tick(Context& ctx) {
    if (!child_) return Status::Failure;
    if (count_ <= 0) { // until failure
        Status s = child_->Tick(ctx);
        return s == Status::Failure ? Status::Failure : Status::Running;
    }
    for (int i = 0; i < count_; ++i) {
        Status s = child_->Tick(ctx);
        if (s == Status::Running) return Status::Running;
        if (s == Status::Failure) return Status::Failure;
    }
    return Status::Success;
}

Status UntilFailNode::Tick(Context& ctx) {
    if (!child_) return Status::Failure;
    Status s = child_->Tick(ctx);
    return s == Status::Failure ? Status::Success : Status::Running;
}

Status BlackboardSetNode::Tick(Context& ctx) {
    return ctx.BBSet(key_, value_) ? Status::Success : Status::Failure;
}

// ---------------------------------------------------------------------------
// Behavior ticks
// ---------------------------------------------------------------------------

Status MoveToNode::Tick(Context& ctx) {
    if (!ctx.moveTo) return Status::Failure;
    return ctx.moveTo(ctx.entity, speed_) ? Status::Success : Status::Failure;
}

Status AttackNode::Tick(Context& ctx) {
    if (!ctx.attack) return Status::Failure;
    return ctx.attack(ctx.entity) ? Status::Success : Status::Failure;
}

Status DialogueNode::Tick(Context& ctx) {
    if (!ctx.dialogue) return Status::Failure;
    ctx.dialogue(text_);
    return Status::Success;
}

Status SpawnNode::Tick(Context& ctx) {
    if (!ctx.spawn) return Status::Failure;
    ctx.spawn(kind_, ctx.entity);
    return Status::Success;
}

Status WaitNode::Tick(Context& ctx) {
    if (seconds_ <= 0.f) return Status::Success;
    elapsed_ += ctx.dt;
    if (elapsed_ >= seconds_) {
        elapsed_ = 0.f;
        return Status::Success;
    }
    return Status::Running;
}

Status PlaySfxNode::Tick(Context& ctx) {
    if (!ctx.playSfx) return Status::Failure;
    ctx.playSfx(name_);
    return Status::Success;
}

Status RunScriptNode::Tick(Context& ctx) {
    if (!ctx.callScript) return Status::Failure;
    ctx.callScript(script_, ctx.entity);
    return Status::Success;
}

// ---------------------------------------------------------------------------
// Condition ticks
// ---------------------------------------------------------------------------

Status InRangeNode::Tick(Context& ctx) {
    if (ctx.inRange) return ctx.inRange(ctx.entity, distance_) ? Status::Success : Status::Failure;
    // Fallback: a precomputed blackboard distance.
    script::Value d = ctx.BBGet("dist");
    if (d.type == script::Value::Type::Number)
        return d.number <= distance_ ? Status::Success : Status::Failure;
    return Status::Failure;
}

Status HasTargetNode::Tick(Context& ctx) {
    return ctx.BBHas("target") ? Status::Success : Status::Failure;
}

Status QuestStateNode::Tick(Context& ctx) {
    const std::string key = "quest_" + quest_;
    script::Value v = ctx.gameVars.Get(key);
    if (v.type == script::Value::Type::Nil) v = ctx.BBGet(key);
    if (v.type != script::Value::Type::String) return Status::Failure;
    return v.str == state_ ? Status::Success : Status::Failure;
}

Status HealthBelowNode::Tick(Context& ctx) {
    script::Value hp = ctx.BBGet("hp");
    script::Value maxHp = ctx.BBGet("maxHp");
    if (hp.type != script::Value::Type::Number || maxHp.type != script::Value::Type::Number)
        return Status::Failure;
    if (maxHp.number <= 0.0) return Status::Failure;
    return hp.number / maxHp.number < pct_ / 100.0 ? Status::Success : Status::Failure;
}

Status BlackboardCmpNode::Tick(Context& ctx) {
    return CompareValues(ctx.BBGet(key_), value_, op_) ? Status::Success : Status::Failure;
}

Status GameVarCmpNode::Tick(Context& ctx) {
    return CompareValues(ctx.gameVars.Get(key_), value_, op_) ? Status::Success : Status::Failure;
}

Status ScriptBoolNode::Tick(Context& ctx) {
    if (!ctx.callScript) return Status::Failure;
    return ValueTruthy(ctx.callScript(script_, ctx.entity)) ? Status::Success : Status::Failure;
}

// ---------------------------------------------------------------------------
// JSON loading
// ---------------------------------------------------------------------------

namespace {
using Factory = NodePtr (*)(const core::Json& node, std::string* error);

void ReadName(Node* n, const core::Json& node) {
    const core::Json* nameJ = node.Get("name");
    if (nameJ && nameJ->IsString()) {
        n->SetName(nameJ->GetString());
        return;
    }
    const core::Json* typeJ = node.Get("type");
    if (typeJ && typeJ->IsString()) n->SetName(typeJ->GetString());
}

NodePtr BuildNode(const core::Json& node, std::string* error);

bool BuildChildren(const core::Json* childrenJ, std::string* error, std::vector<NodePtr>& out) {
    if (!childrenJ) return true;
    for (size_t i = 0; i < childrenJ->Size(); ++i) {
        const core::Json* child = childrenJ->At(i);
        if (!child) continue;
        NodePtr n = BuildNode(*child, error);
        if (!n) return false;
        out.push_back(std::move(n));
    }
    return true;
}

NodePtr MakeComposite(NodePtr node, const core::Json& json, std::string* error) {
    auto* composite = static_cast<Composite*>(node.get());
    ReadName(composite, json);
    std::vector<NodePtr> children;
    if (!BuildChildren(json.Get("children"), error, children)) return nullptr;
    for (auto& c : children) composite->AddChild(std::move(c));
    return node;
}

bool AttachChild(Decorator& n, const core::Json& node, std::string* error) {
    const core::Json* childJ = node.Get("child");
    if (!childJ) {
        if (error) *error = "node '" + n.Name() + "' requires a 'child' object";
        return false;
    }
    NodePtr child = BuildNode(*childJ, error);
    if (!child) return false;
    n.SetChild(std::move(child));
    return true;
}

NodePtr BuildSequence(const core::Json& node, std::string* error) {
    return MakeComposite(std::make_unique<SequenceNode>(), node, error);
}
NodePtr BuildSelector(const core::Json& node, std::string* error) {
    return MakeComposite(std::make_unique<SelectorNode>(), node, error);
}
NodePtr BuildRandomSelector(const core::Json& node, std::string* error) {
    return MakeComposite(std::make_unique<RandomSelectorNode>(), node, error);
}
NodePtr BuildParallel(const core::Json& node, std::string* error) {
    auto n = std::make_unique<ParallelNode>();
    ReadName(n.get(), node);
    Args args(node.Get("args"));
    n->SetThreshold(args.Int("threshold", -1));
    std::vector<NodePtr> children;
    if (!BuildChildren(node.Get("children"), error, children)) return nullptr;
    for (auto& c : children) n->AddChild(std::move(c));
    return n;
}

NodePtr BuildInvert(const core::Json& node, std::string* error) {
    auto n = std::make_unique<InvertNode>();
    ReadName(n.get(), node);
    if (!AttachChild(*n, node, error)) return nullptr;
    return n;
}
NodePtr BuildCooldown(const core::Json& node, std::string* error) {
    auto n = std::make_unique<CooldownNode>();
    ReadName(n.get(), node);
    Args args(node.Get("args"));
    n->SetSeconds(static_cast<float>(args.Number("seconds", 0.0)));
    if (!AttachChild(*n, node, error)) return nullptr;
    return n;
}
NodePtr BuildRepeat(const core::Json& node, std::string* error) {
    auto n = std::make_unique<RepeatNode>();
    ReadName(n.get(), node);
    Args args(node.Get("args"));
    n->SetCount(args.Int("count", 0));
    if (!AttachChild(*n, node, error)) return nullptr;
    return n;
}
NodePtr BuildUntilFail(const core::Json& node, std::string* error) {
    auto n = std::make_unique<UntilFailNode>();
    ReadName(n.get(), node);
    if (!AttachChild(*n, node, error)) return nullptr;
    return n;
}

NodePtr BuildBlackboardSet(const core::Json& node, std::string* error) {
    (void)error;
    auto n = std::make_unique<BlackboardSetNode>();
    ReadName(n.get(), node);
    Args args(node.Get("args"));
    n->SetKey(args.Str("key", ""));
    n->SetValue(args.Value("value"));
    return n;
}
NodePtr BuildMoveTo(const core::Json& node, std::string* error) {
    (void)error;
    auto n = std::make_unique<MoveToNode>();
    ReadName(n.get(), node);
    Args args(node.Get("args"));
    n->SetSpeed(static_cast<float>(args.Number("speed", 0.0)));
    return n;
}
NodePtr BuildAttack(const core::Json& node, std::string* error) {
    (void)error;
    auto n = std::make_unique<AttackNode>();
    ReadName(n.get(), node);
    return n;
}
NodePtr BuildDialogue(const core::Json& node, std::string* error) {
    (void)error;
    auto n = std::make_unique<DialogueNode>();
    ReadName(n.get(), node);
    Args args(node.Get("args"));
    n->SetText(args.Str("text", ""));
    return n;
}
NodePtr BuildSpawn(const core::Json& node, std::string* error) {
    (void)error;
    auto n = std::make_unique<SpawnNode>();
    ReadName(n.get(), node);
    Args args(node.Get("args"));
    n->SetKind(args.Str("kind", ""));
    return n;
}
NodePtr BuildWait(const core::Json& node, std::string* error) {
    (void)error;
    auto n = std::make_unique<WaitNode>();
    ReadName(n.get(), node);
    Args args(node.Get("args"));
    n->SetSeconds(static_cast<float>(args.Number("seconds", 0.0)));
    return n;
}
NodePtr BuildPlaySfx(const core::Json& node, std::string* error) {
    (void)error;
    auto n = std::make_unique<PlaySfxNode>();
    ReadName(n.get(), node);
    Args args(node.Get("args"));
    n->SetName(args.Str("name", ""));
    return n;
}
NodePtr BuildRunScript(const core::Json& node, std::string* error) {
    (void)error;
    auto n = std::make_unique<RunScriptNode>();
    ReadName(n.get(), node);
    Args args(node.Get("args"));
    n->SetScript(args.Str("script", ""));
    return n;
}

NodePtr BuildInRange(const core::Json& node, std::string* error) {
    (void)error;
    auto n = std::make_unique<InRangeNode>();
    ReadName(n.get(), node);
    Args args(node.Get("args"));
    n->SetDistance(static_cast<float>(args.Number("distance", 0.0)));
    return n;
}
NodePtr BuildHasTarget(const core::Json& node, std::string* error) {
    (void)error;
    auto n = std::make_unique<HasTargetNode>();
    ReadName(n.get(), node);
    return n;
}
NodePtr BuildQuestState(const core::Json& node, std::string* error) {
    (void)error;
    auto n = std::make_unique<QuestStateNode>();
    ReadName(n.get(), node);
    Args args(node.Get("args"));
    n->SetQuest(args.Str("quest", ""));
    n->SetState(args.Str("state", ""));
    return n;
}
NodePtr BuildHealthBelow(const core::Json& node, std::string* error) {
    (void)error;
    auto n = std::make_unique<HealthBelowNode>();
    ReadName(n.get(), node);
    Args args(node.Get("args"));
    n->SetPct(args.Number("pct", 100.0));
    return n;
}
NodePtr BuildBlackboardCmp(const core::Json& node, std::string* error) {
    (void)error;
    auto n = std::make_unique<BlackboardCmpNode>();
    ReadName(n.get(), node);
    Args args(node.Get("args"));
    n->SetKey(args.Str("key", ""));
    n->SetOp(args.Str("op", "=="));
    n->SetValue(args.Value("value"));
    return n;
}
NodePtr BuildGameVarCmp(const core::Json& node, std::string* error) {
    (void)error;
    auto n = std::make_unique<GameVarCmpNode>();
    ReadName(n.get(), node);
    Args args(node.Get("args"));
    n->SetKey(args.Str("key", ""));
    n->SetOp(args.Str("op", "=="));
    n->SetValue(args.Value("value"));
    return n;
}
NodePtr BuildScriptBool(const core::Json& node, std::string* error) {
    (void)error;
    auto n = std::make_unique<ScriptBoolNode>();
    ReadName(n.get(), node);
    Args args(node.Get("args"));
    n->SetScript(args.Str("script", ""));
    return n;
}

const std::map<std::string, Factory>& Factories() {
    static const std::map<std::string, Factory> factories = {
        {"sequence", &BuildSequence},
        {"selector", &BuildSelector},
        {"random_selector", &BuildRandomSelector},
        {"parallel", &BuildParallel},
        {"invert", &BuildInvert},
        {"cooldown", &BuildCooldown},
        {"repeat", &BuildRepeat},
        {"until_fail", &BuildUntilFail},
        {"blackboard_set", &BuildBlackboardSet},
        {"move_to", &BuildMoveTo},
        {"attack", &BuildAttack},
        {"dialogue", &BuildDialogue},
        {"spawn", &BuildSpawn},
        {"wait", &BuildWait},
        {"play_sfx", &BuildPlaySfx},
        {"run_script", &BuildRunScript},
        {"in_range", &BuildInRange},
        {"has_target", &BuildHasTarget},
        {"quest_state", &BuildQuestState},
        {"health_below", &BuildHealthBelow},
        {"blackboard_cmp", &BuildBlackboardCmp},
        {"gamevar_cmp", &BuildGameVarCmp},
        {"script_bool", &BuildScriptBool},
    };
    return factories;
}

// Categories accepted in the "type" field when the concrete node name lives in
// "name" (e.g. {"type":"condition","name":"in_range"}).
bool IsCategory(const std::string& s) {
    return s == "action" || s == "condition" || s == "composite" || s == "decorator";
}

NodePtr BuildNode(const core::Json& node, std::string* error) {
    const core::Json* typeJ = node.Get("type");
    if (!typeJ || !typeJ->IsString()) {
        if (error) *error = "node is missing a string 'type'";
        return nullptr;
    }
    const std::string type = typeJ->GetString();
    std::string key = type;
    if (!Factories().count(type) && IsCategory(type)) {
        const core::Json* nameJ = node.Get("name");
        if (nameJ && nameJ->IsString() && Factories().count(nameJ->GetString()))
            key = nameJ->GetString();
    }
    auto it = Factories().find(key);
    if (it == Factories().end()) {
        if (error) *error = "unknown node type '" + type + "'";
        return nullptr;
    }
    return it->second(node, error);
}
} // namespace

// ---------------------------------------------------------------------------
// BehaviorTree
// ---------------------------------------------------------------------------

bool BehaviorTree::Load(const core::Json& json, std::string* error) {
    const core::Json* rootJ = json.Get("root");
    if (!rootJ) {
        if (error) *error = "missing 'root' node";
        return false;
    }
    NodePtr root = BuildNode(*rootJ, error);
    if (!root) return false;
    root_ = std::move(root);
    return true;
}

bool BehaviorTree::LoadText(const std::string& text, std::string* error) {
    std::string parseError;
    core::Json json = core::Json::Parse(text, &parseError);
    if (json.IsNull()) {
        if (error) *error = parseError.empty() ? "invalid JSON" : parseError;
        return false;
    }
    return Load(json, error);
}

Status BehaviorTree::Tick(Context& ctx) {
    return root_ ? root_->Tick(ctx) : Status::Failure;
}

} // namespace neon::bt
