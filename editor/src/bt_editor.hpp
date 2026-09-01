#pragma once

// Behavior tree visual editor: node graph model + canvas + palette + param
// editing + save/load + play debug highlight.
//
// The node GRAPH MODEL (namespace btgraph) is deliberately ImGui-free (pure
// inline code in this header) so the test binary can exercise it headlessly:
// graph -> {"root": ...} JSON -> graph round-trips, link validation, path ids.
// The canvas UI lives in panels/bt_panel.cpp (Task 18b: BtPanel : IPanel) and
// is smoke-tested from the editor itself.

#include <algorithm>
#include <cstdint>
#include <functional>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "neon/bt/behavior_tree.hpp"
#include "neon/core/json.hpp"
#include "neon/core/log.hpp"
#include "neon/math/vec2.hpp"

namespace neon::editor::btgraph {

// A node on the editor canvas. `id` is editor-local ("n0", "n1", ...); the
// serialized tree ids are index paths (see TreeIdOf). `args` is the raw "args"
// object written into the tree JSON (null when the node has no args yet).
struct BtGraphNode {
    std::string id;
    std::string type;
    std::string name;   // optional display name (empty -> default to type)
    core::Json args;    // object, or Null
    math::Vec2 pos{0.f, 0.f};
};

// parent -> child edge. Every node has at most one parent link.
struct BtGraphLink {
    std::string parent;
    std::string child;
};

// Pure node-graph model for the behavior tree editor. A valid loaded tree is a
// single-root hierarchy where each node has one parent; the model tolerates
// temporarily unparented nodes (multiple roots) while editing, and
// serialization wraps multiple roots under a synthetic "sequence" so the output
// stays loadable by bt::BehaviorTree. Link validity is enforced at SetParent:
// a parent must accept children (composite unlimited / decorator one) and the
// link must not create a cycle.
class BtGraph {
public:
    BtGraph() = default;

    // --- model ops ---
    // Add a node of `type` (any bt::AllNodeTypes() type) at `pos`. Returns the
    // new editor-local id, or "" when the type is unknown.
    std::string AddNode(const std::string& type, const math::Vec2& pos) {
        if (bt::ChildCapacity(type) == -2) {
            NEON_LOG_WARN("BtGraph: unknown node type '%s'", type.c_str());
            return "";
        }
        BtGraphNode n;
        n.id = MakeId();
        n.type = type;
        n.pos = pos;
        nodes_.push_back(std::move(n));
        return nodes_.back().id;
    }
    // Remove a node and every link touching it. Its children become
    // unparented roots. Returns false when `id` is unknown.
    bool RemoveNode(const std::string& id) {
        size_t i = IndexOf(id);
        if (i == static_cast<size_t>(-1)) return false;
        nodes_.erase(nodes_.begin() + static_cast<ptrdiff_t>(i));
        links_.erase(std::remove_if(links_.begin(), links_.end(),
                                    [&](const BtGraphLink& l) {
                                        return l.parent == id || l.child == id;
                                    }),
                     links_.end());
        return true;
    }
    // Parent `child` under `parent` ("" detaches the child -> becomes a root).
    // Validates that both exist, `parent` accepts children (a decorator is not
    // already full) and the link stays acyclic. Returns false (and logs) on an
    // invalid link.
    bool SetParent(const std::string& child, const std::string& parent) {
        size_t ci = IndexOf(child);
        if (ci == static_cast<size_t>(-1)) {
            NEON_LOG_WARN("BtGraph: SetParent: unknown child '%s'", child.c_str());
            return false;
        }
        if (parent.empty()) { // detach
            for (auto it = links_.begin(); it != links_.end(); ++it)
                if (it->child == child) {
                    links_.erase(it);
                    return true;
                }
            return true; // already a root
        }
        size_t pi = IndexOf(parent);
        if (pi == static_cast<size_t>(-1)) {
            NEON_LOG_WARN("BtGraph: SetParent: unknown parent '%s'", parent.c_str());
            return false;
        }
        if (parent == child) {
            NEON_LOG_WARN("BtGraph: SetParent: a node cannot be its own parent");
            return false;
        }
        const int cap = bt::ChildCapacity(nodes_[pi].type);
        if (cap == 0) {
            NEON_LOG_WARN("BtGraph: SetParent: '%s' accepts no children",
                          nodes_[pi].type.c_str());
            return false;
        }
        if (cap == 1) {
            for (const auto& l : links_)
                if (l.parent == parent && l.child != child) {
                    NEON_LOG_WARN("BtGraph: SetParent: decorator '%s' already has a child",
                                  nodes_[pi].type.c_str());
                    return false;
                }
        }
        if (IsDescendant(parent, child)) {
            NEON_LOG_WARN("BtGraph: SetParent: linking '%s' under its own descendant would "
                          "create a cycle",
                          child.c_str());
            return false;
        }
        for (auto it = links_.begin(); it != links_.end();)
            if (it->child == child) it = links_.erase(it);
            else ++it;
        links_.push_back({parent, child});
        return true;
    }
    // Set one arg key on `id` (a null `value` removes the key). Returns false
    // when `id` is unknown.
    bool SetArg(const std::string& id, const std::string& key, const core::Json& value) {
        size_t i = IndexOf(id);
        if (i == static_cast<size_t>(-1)) return false;
        BtGraphNode& n = nodes_[i];
        if (!n.args.IsObject()) {
            n.args.type_ = core::Json::Type::Object;
            n.args.object_.clear();
        }
        if (value.IsNull()) n.args.object_.erase(key);
        else n.args.object_[key] = value;
        return true;
    }
    // Set the optional display name of `id` (empty -> default to type).
    bool SetName(const std::string& id, const std::string& name) {
        size_t i = IndexOf(id);
        if (i == static_cast<size_t>(-1)) return false;
        nodes_[i].name = name;
        return true;
    }
    // Move `id` to an absolute canvas position.
    bool SetPos(const std::string& id, const math::Vec2& pos) {
        size_t i = IndexOf(id);
        if (i == static_cast<size_t>(-1)) return false;
        nodes_[i].pos = pos;
        return true;
    }

    // --- queries ---
    const BtGraphNode* Find(const std::string& id) const {
        size_t i = IndexOf(id);
        return i == static_cast<size_t>(-1) ? nullptr : &nodes_[i];
    }
    bool Empty() const { return nodes_.empty(); }
    size_t NodeCount() const { return nodes_.size(); }
    size_t LinkCount() const { return links_.size(); }
    const std::vector<BtGraphNode>& Nodes() const { return nodes_; }
    const std::vector<BtGraphLink>& Links() const { return links_; }

    // The serialized tree-path id of `id` mirroring bt::BehaviorTree's loader
    // ids: the (single) root is "0"; a composite's children are "0/0", "0/1",
    // ... while a decorator's child is the literal "0/child" (the loader ids a
    // decorator's child as "<parent>/child", see AttachChild). ("" when `id` is
    // missing.) Used to map the play runtime's activePath back to the
    // canvas node.
    std::string TreeIdOf(const std::string& id) const {
        size_t idx = IndexOf(id);
        if (idx == static_cast<size_t>(-1)) return "";
        std::vector<const BtGraphNode*> chain;
        const BtGraphNode* cur = &nodes_[idx];
        while (cur) {
            chain.push_back(cur);
            const std::string* parentId = nullptr;
            for (const auto& l : links_)
                if (l.child == cur->id) {
                    parentId = &l.parent;
                    break;
                }
            if (!parentId) break;
            size_t p = IndexOf(*parentId);
            if (p == static_cast<size_t>(-1)) break;
            cur = &nodes_[p];
        }
        if (chain.empty()) return "";
        if (chain.size() == 1) return "0"; // the node itself is a root
        std::string path = "0";
        for (size_t i = chain.size() - 1; i > 0; --i) {
            const BtGraphNode* parent = chain[i];
            const BtGraphNode* child = chain[i - 1];
            if (bt::ChildCapacity(parent->type) == 1) {
                // Decorator: the loader ids its single child as "<parent>/child".
                path += "/child";
                continue;
            }
            int among = 0;
            for (const auto& l : links_)
                if (l.parent == parent->id) {
                    if (l.child == child->id) break;
                    ++among;
                }
            path += "/" + std::to_string(among);
        }
        return path;
    }

    // --- serialization ---
    // Build the loader-shaped {"root": {...}} DOM with children nested under
    // their parent links. Exactly one root is emitted directly; multiple roots
    // are wrapped under a synthetic "sequence"; an empty graph yields {} (no
    // "root" key). Args are preserved verbatim.
    core::Json ToTreeJson() const {
        core::Json root;
        root.type_ = core::Json::Type::Object;
        std::vector<const BtGraphNode*> roots;
        for (const auto& n : nodes_) {
            bool hasParent = false;
            for (const auto& l : links_)
                if (l.child == n.id) {
                    hasParent = true;
                    break;
                }
            if (!hasParent) roots.push_back(&n);
        }
        if (roots.empty()) return root;
        if (roots.size() == 1) {
            root.object_["root"] = NodeToJson(*roots[0]);
            return root;
        }
        core::Json seq;
        seq.type_ = core::Json::Type::Object;
        core::Json typeJ;
        typeJ.type_ = core::Json::Type::String;
        typeJ.string_ = "sequence";
        seq.object_["type"] = std::move(typeJ);
        core::Json arr;
        arr.type_ = core::Json::Type::Array;
        for (const BtGraphNode* r : roots) arr.array_.push_back(NodeToJson(*r));
        seq.object_["children"] = std::move(arr);
        root.object_["root"] = std::move(seq);
        return root;
    }
    // Parse a tree JSON ({"root": {...}}) into the graph. Node positions are
    // assigned by a deterministic top-down layout (positions are not part of
    // the tree format). Returns false on structural errors (unknown type,
    // wrong-typed nesting); on failure the graph is left unchanged.
    bool FromTreeJson(const core::Json& json) {
        BtGraph copy;
        const core::Json* rootJ = json.Get("root");
        if (rootJ) {
            std::string err;
            if (!copy.ParseNode(*rootJ, "", 0, &err)) {
                NEON_LOG_WARN("BtGraph: FromTreeJson failed: %s", err.c_str());
                return false;
            }
        }
        if (!copy.nodes_.empty()) copy.LayoutTopDown();
        *this = std::move(copy);
        return true;
    }
    // ToTreeJson serialized to text.
    std::string Serialize() const { return core::JsonWriter::Write(ToTreeJson()); }

    // Assign a deterministic top-down layout: siblings spread left-to-right,
    // parents centered over their children.
    void LayoutTopDown() {
        std::map<std::string, std::vector<std::string>> childrenOf;
        std::set<std::string> haveParent;
        for (const auto& l : links_) {
            childrenOf[l.parent].push_back(l.child);
            haveParent.insert(l.child);
        }
        std::vector<std::string> roots;
        for (const auto& n : nodes_)
            if (!haveParent.count(n.id)) roots.push_back(n.id);
        const float kCol = 220.f;
        const float kRow = 150.f;
        std::map<std::string, math::Vec2> pos;
        float nextX = 0.f;
        std::function<float(const std::string&, int)> layout =
            [&](const std::string& id, int depth) -> float {
            const auto it = childrenOf.find(id);
            const bool leaf = it == childrenOf.end() || it->second.empty();
            if (leaf) {
                pos[id] = {nextX * kCol, static_cast<float>(depth) * kRow};
                ++nextX;
                return pos[id].x;
            }
            float sum = 0.f;
            for (const auto& k : it->second) sum += layout(k, depth + 1);
            const float cx = sum / static_cast<float>(it->second.size());
            pos[id] = {cx, static_cast<float>(depth) * kRow};
            return cx;
        };
        for (const auto& r : roots) layout(r, 0);
        for (auto& n : nodes_) {
            auto it = pos.find(n.id);
            if (it != pos.end()) n.pos = it->second;
        }
    }

private:
    size_t IndexOf(const std::string& id) const {
        for (size_t i = 0; i < nodes_.size(); ++i)
            if (nodes_[i].id == id) return i;
        return static_cast<size_t>(-1);
    }
    bool IsDescendant(const std::string& node, const std::string& ancestor) const {
        std::string cur = node;
        for (;;) {
            const std::string* p = nullptr;
            for (const auto& l : links_)
                if (l.child == cur) {
                    p = &l.parent;
                    break;
                }
            if (!p) return false;
            if (*p == ancestor) return true;
            cur = *p;
        }
    }
    std::string MakeId() { return "n" + std::to_string(nextId_++); }

    core::Json NodeToJson(const BtGraphNode& n) const {
        core::Json obj;
        obj.type_ = core::Json::Type::Object;
        core::Json typeJ;
        typeJ.type_ = core::Json::Type::String;
        typeJ.string_ = n.type;
        obj.object_["type"] = std::move(typeJ);
        if (!n.name.empty() && n.name != n.type) {
            core::Json nameJ;
            nameJ.type_ = core::Json::Type::String;
            nameJ.string_ = n.name;
            obj.object_["name"] = std::move(nameJ);
        }
        if (!n.args.IsNull()) obj.object_["args"] = n.args;

        std::vector<const BtGraphNode*> children;
        for (const auto& l : links_)
            if (l.parent == n.id) {
                size_t ci = IndexOf(l.child);
                if (ci != static_cast<size_t>(-1)) children.push_back(&nodes_[ci]);
            }
        const int cap = bt::ChildCapacity(n.type);
        if (cap == -1) { // composite: "children" array
            if (!children.empty()) {
                core::Json arr;
                arr.type_ = core::Json::Type::Array;
                for (const BtGraphNode* c : children) arr.array_.push_back(NodeToJson(*c));
                obj.object_["children"] = std::move(arr);
            }
        } else if (cap == 1 && !children.empty()) { // decorator: single "child"
            obj.object_["child"] = NodeToJson(*children[0]);
        }
        return obj;
    }

    bool ParseNode(const core::Json& j, const std::string& parent, int depth,
                   std::string* error) {
        if (depth > 256) {
            *error = "tree exceeds maximum nesting depth";
            return false;
        }
        const core::Json* typeJ = j.Get("type");
        if (!typeJ || !typeJ->IsString()) {
            *error = "node is missing a string 'type'";
            return false;
        }
        const std::string type = typeJ->GetString();
        const int cap = bt::ChildCapacity(type);
        if (cap == -2) {
            *error = "unknown node type '" + type + "'";
            return false;
        }
        const core::Json* childrenJ = j.Get("children");
        const core::Json* childJ = j.Get("child");
        const core::Json* argsJ = j.Get("args");
        if (childrenJ && !childrenJ->IsArray()) {
            *error = "'children' must be an array";
            return false;
        }
        if (childJ && !childJ->IsObject()) {
            *error = "'child' must be an object";
            return false;
        }
        if (argsJ && !argsJ->IsObject()) {
            *error = "'args' must be an object";
            return false;
        }
        if (cap == 0 && (childrenJ || childJ)) {
            *error = "node type '" + type + "' accepts no children";
            return false;
        }
        if (cap == 1 && childrenJ) {
            *error = "node type '" + type + "' is a decorator: use 'child', not 'children'";
            return false;
        }
        if (cap == -1 && childJ) {
            *error = "node type '" + type + "' is a composite: use 'children', not 'child'";
            return false;
        }

        const std::string id = AddNode(type, math::Vec2{0.f, 0.f});
        if (id.empty()) {
            *error = "cannot add node of type '" + type + "'";
            return false;
        }
        BtGraphNode& n = nodes_.back();
        if (const core::Json* nameJ = j.Get("name")) n.name = nameJ->GetString();
        if (argsJ) n.args = *argsJ;

        if (childrenJ) {
            for (size_t i = 0; i < childrenJ->Size(); ++i) {
                const core::Json* c = childrenJ->At(i);
                if (!c || !ParseNode(*c, id, depth + 1, error)) return false;
            }
        } else if (childJ) {
            if (!ParseNode(*childJ, id, depth + 1, error)) return false;
        }
        if (!parent.empty()) SetParent(id, parent);
        return true;
    }

    std::vector<BtGraphNode> nodes_;
    std::vector<BtGraphLink> links_;
    uint64_t nextId_ = 0;
};

} // namespace neon::editor::btgraph
