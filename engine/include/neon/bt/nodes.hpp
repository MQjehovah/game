#pragma once
#include <string>
#include <vector>

#include "neon/bt/behavior_tree.hpp"

namespace neon::bt {

// ---------------------------------------------------------------------------
// Base classes
// ---------------------------------------------------------------------------

// A node with an ordered list of children.
class Composite : public Node {
public:
    void AddChild(NodePtr child) { children_.push_back(std::move(child)); }
    size_t ChildCount() const { return children_.size(); }

protected:
    std::vector<NodePtr> children_;
};

// A node wrapping exactly one child.
class Decorator : public Node {
public:
    void SetChild(NodePtr child) { child_ = std::move(child); }

protected:
    NodePtr child_;
};

// Marker base for side-effecting nodes.
class Behavior : public Node {};

// Marker base for read-only predicates. Nodes that only read blackboard /
// gameVars / hooks belong here; composites treat them exactly like any other
// child.
class Condition : public Node {};

// ---------------------------------------------------------------------------
// Composites
// ---------------------------------------------------------------------------

// All children must succeed; stops at the first Failure or Running.
class SequenceNode : public Composite {
public:
    Status Tick(Context& ctx) const override;
};

// First success wins; falls through on Failure.
class SelectorNode : public Composite {
public:
    Status Tick(Context& ctx) const override;
};

// Selector that evaluates children in a shuffled order. The shuffle is seeded
// deterministically from the entity so two identical trees (same entity)
// evaluate in the same order.
class RandomSelectorNode : public Composite {
public:
    Status Tick(Context& ctx) const override;
};

// Ticks every child each evaluation. Succeeds once `threshold` children have
// succeeded; fails once the remaining children can no longer reach the
// threshold; otherwise Running.
class ParallelNode : public Composite {
public:
    void SetThreshold(int t) { threshold_ = t; }
    Status Tick(Context& ctx) const override;

private:
    int threshold_ = -1; // <= 0 -> require all children
};

// ---------------------------------------------------------------------------
// Decorators
// ---------------------------------------------------------------------------

// Flips Success <-> Failure; Running passes through.
class InvertNode : public Decorator {
public:
    Status Tick(Context& ctx) const override;
};

// After the child succeeds, blocks it for `seconds` (counted in ctx.dt via the
// entity's Context timer). While blocked the node returns Failure without
// ticking the child. The cooldown is tracked per entity in Context, so one
// CooldownNode instance serves many entities independently.
class CooldownNode : public Decorator {
public:
    void SetSeconds(float s) { seconds_ = s; }
    Status Tick(Context& ctx) const override;

private:
    float seconds_ = 0.f;  // <= 0 disables the cooldown
};

// Re-ticks the child `count` times within a single Tick (must be >= 1;
// validated at load). A child Failure or Running short-circuits. Stateless:
// the loop finishes within one Tick, so no per-entity state is needed.
class RepeatNode : public Decorator {
public:
    void SetCount(int c) { count_ = c; }
    Status Tick(Context& ctx) const override;

private:
    int count_ = 0;
};

// Success once the child fails; Running while the child keeps succeeding.
class UntilFailNode : public Decorator {
public:
    Status Tick(Context& ctx) const override;
};

// Writes key = value to the entity's blackboard, then succeeds.
class BlackboardSetNode : public Node {
public:
    void SetKey(const std::string& k) { key_ = k; }
    void SetValue(const script::Value& v) { value_ = v; }
    Status Tick(Context& ctx) const override;

private:
    std::string key_;
    script::Value value_;
};

// ---------------------------------------------------------------------------
// Behaviors
// ---------------------------------------------------------------------------

// Asks ctx.moveTo to move the entity. Succeeds when the hook reports ok.
class MoveToNode : public Behavior {
public:
    void SetSpeed(float s) { speed_ = s; }
    Status Tick(Context& ctx) const override;

private:
    float speed_ = 0.f;
};

class AttackNode : public Behavior {
public:
    Status Tick(Context& ctx) const override;
};

class DialogueNode : public Behavior {
public:
    void SetText(const std::string& t) { text_ = t; }
    Status Tick(Context& ctx) const override;

private:
    std::string text_;
};

class SpawnNode : public Behavior {
public:
    void SetKind(const std::string& k) { kind_ = k; }
    Status Tick(Context& ctx) const override;

private:
    std::string kind_;
};

// Accumulates ctx.dt in the entity's Context timer; Running until the
// requested time elapses. Per-entity state lives in Context, so one WaitNode
// instance serves many entities independently.
class WaitNode : public Behavior {
public:
    void SetSeconds(float s) { seconds_ = s; }
    Status Tick(Context& ctx) const override;

private:
    float seconds_ = 0.f;
};

class PlaySfxNode : public Behavior {
public:
    void SetName(const std::string& n) { name_ = n; }
    Status Tick(Context& ctx) const override;

private:
    std::string name_;
};

// Runs a script snippet through ctx.callScript (null hook -> Failure).
class RunScriptNode : public Behavior {
public:
    void SetScript(const std::string& s) { script_ = s; }
    Status Tick(Context& ctx) const override;

private:
    std::string script_;
};

// ---------------------------------------------------------------------------
// Conditions
// ---------------------------------------------------------------------------

// True when ctx.inRange reports the entity within `distance`; without the
// hook, falls back to a blackboard "dist" value.
class InRangeNode : public Condition {
public:
    void SetDistance(float d) { distance_ = d; }
    Status Tick(Context& ctx) const override;

private:
    float distance_ = 0.f;
};

class HasTargetNode : public Condition {
public:
    Status Tick(Context& ctx) const override;
};

// True when gameVars (or the blackboard) key "quest_<quest>" equals `state`.
class QuestStateNode : public Condition {
public:
    void SetQuest(const std::string& q) { quest_ = q; }
    void SetState(const std::string& s) { state_ = s; }
    Status Tick(Context& ctx) const override;

private:
    std::string quest_;
    std::string state_;
};

// True when blackboard hp/maxHp ratio is below pct/100.
class HealthBelowNode : public Condition {
public:
    void SetPct(double p) { pct_ = p; }
    Status Tick(Context& ctx) const override;

private:
    double pct_ = 100.0;
};

class BlackboardCmpNode : public Condition {
public:
    void SetKey(const std::string& k) { key_ = k; }
    void SetOp(const std::string& o) { op_ = o; }
    void SetValue(const script::Value& v) { value_ = v; }
    Status Tick(Context& ctx) const override;

private:
    std::string key_;
    std::string op_ = "==";
    script::Value value_;
};

class GameVarCmpNode : public Condition {
public:
    void SetKey(const std::string& k) { key_ = k; }
    void SetOp(const std::string& o) { op_ = o; }
    void SetValue(const script::Value& v) { value_ = v; }
    Status Tick(Context& ctx) const override;

private:
    std::string key_;
    std::string op_ = "==";
    script::Value value_;
};

// True when ctx.callScript's result is truthy (null hook -> Failure).
class ScriptBoolNode : public Condition {
public:
    void SetScript(const std::string& s) { script_ = s; }
    Status Tick(Context& ctx) const override;

private:
    std::string script_;
};

} // namespace neon::bt
