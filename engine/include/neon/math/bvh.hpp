#pragma once

#include <cstdint>
#include <vector>

#include "neon/math/math.hpp"

namespace neon::math {

// Dynamic bounding-volume hierarchy for 3D spatial queries (G1-2). A
// Box2D-style dynamic tree: leaves hold caller-owned ids, internal nodes are
// AABB unions chosen by a surface-area heuristic so queries visit only the
// branches that can intersect the query volume. Supports the unified spatial
// index contract - Insert / Update (move) / Remove / queries - and is
// conservative: a query never misses a leaf whose AABB overlaps the query
// volume, so it can replace brute-force enumeration without changing results.
//
// Header-only, no allocations beyond the internal node/id arrays (which grow
// on demand and are reused across frames). Not thread-safe; the engine drives
// it from one thread per frame.
class Bvh {
public:
    using Id = uint32_t;

    Bvh() { nodes_.reserve(64); }

    // Inserts a leaf with the caller-owned id. Re-inserting a live id is a
    // no-op (use Update to move an existing leaf).
    void Insert(Id id, const AABB& box) {
        if (Contains(id)) return;
        const uint32_t leaf = AllocNode();
        Node& n = nodes_[leaf];
        n.box = box;
        n.id = id;
        n.leaf = true;
        if (id >= idToNode_.size()) idToNode_.resize(static_cast<size_t>(id) + 1, 0);
        idToNode_[id] = leaf;
        ++leafCount_;
        InsertLeaf(leaf);
    }

    void Remove(Id id) {
        if (id >= idToNode_.size() || idToNode_[id] == 0) return;
        const uint32_t leaf = idToNode_[id];
        idToNode_[id] = 0;
        RemoveLeaf(leaf);
        FreeNode(leaf);
        --leafCount_;
    }

    // Moves/resizes a live leaf (remove + reinsert; correct for dynamic
    // entities, ~O(log n) average).
    void Update(Id id, const AABB& box) {
        if (id >= idToNode_.size() || idToNode_[id] == 0) {
            Insert(id, box);
            return;
        }
        const uint32_t leaf = idToNode_[id];
        nodes_[leaf].box = box;
        // Detach + reinsert so the tree re-branches around the new bounds.
        RemoveLeaf(leaf);
        InsertLeaf(leaf);
    }

    bool Contains(Id id) const {
        return id < idToNode_.size() && idToNode_[id] != 0;
    }

    void Clear() {
        nodes_.clear();
        nodes_.resize(1); // index 0 = null sentinel
        freeList_.clear();
        idToNode_.clear();
        root_ = 0;
        leafCount_ = 0;
    }

    size_t Size() const { return leafCount_; }
    bool Empty() const { return leafCount_ == 0; }
    size_t NodeCount() const { return nodes_.size() - 1; } // excluding the sentinel

    // Visits every leaf id whose AABB overlaps `box`.
    template <class Fn>
    void QueryAABB(const AABB& box, Fn&& fn) const {
        if (root_ != 0) QueryNode(root_, box, fn);
    }

    // Visits every leaf id whose AABB intersects the frustum (same
    // conservative Frustum::Intersects test the renderer uses).
    template <class Fn>
    void QueryFrustum(const Frustum& frustum, Fn&& fn) const {
        if (root_ != 0) QueryNodeFrustum(root_, frustum, fn);
    }

    // Visits every leaf id whose AABB is hit by the ray within maxDist.
    template <class Fn>
    void QueryRay(const Ray& ray, float maxDist, Fn&& fn) const {
        if (root_ != 0) QueryNodeRay(root_, ray, maxDist, fn);
    }

    // Visits ids whose leaf AABB overlaps the sphere's bounding box; callers
    // needing exact distance filtering apply it in fn.
    template <class Fn>
    void QuerySphere(const Vec3& center, float radius, Fn&& fn) const {
        AABB box;
        box.min = center - Vec3{radius, radius, radius};
        box.max = center + Vec3{radius, radius, radius};
        QueryAABB(box, fn);
    }

    // Union AABB of everything in the tree (zero-sized box when empty).
    AABB Bounds() const { return root_ != 0 ? nodes_[root_].box : AABB{}; }

private:
    struct Node {
        AABB box;
        uint32_t parent = 0;
        uint32_t left = 0;
        uint32_t right = 0;
        Id id = 0;
        bool leaf = false;
    };

    static AABB Union(const AABB& a, const AABB& b) {
        AABB out;
        out.min.x = std::fmin(a.min.x, b.min.x);
        out.min.y = std::fmin(a.min.y, b.min.y);
        out.min.z = std::fmin(a.min.z, b.min.z);
        out.max.x = std::fmax(a.max.x, b.max.x);
        out.max.y = std::fmax(a.max.y, b.max.y);
        out.max.z = std::fmax(a.max.z, b.max.z);
        return out;
    }

    static float Area(const AABB& a) {
        const Vec3 e = a.Extents();
        return 2.0f * (e.x * e.y + e.y * e.z + e.z * e.x);
    }

    static float UnionArea(const AABB& a, const AABB& b) { return Area(Union(a, b)); }

    uint32_t AllocNode() {
        if (!freeList_.empty()) {
            const uint32_t idx = freeList_.back();
            freeList_.pop_back();
            nodes_[idx] = Node{};
            return idx;
        }
        nodes_.emplace_back();
        return static_cast<uint32_t>(nodes_.size() - 1);
    }

    void FreeNode(uint32_t index) {
        nodes_[index] = Node{};
        freeList_.push_back(index);
    }

    void Refit(uint32_t node) {
        while (node != 0) {
            Node& n = nodes_[node];
            n.box = Union(nodes_[n.left].box, nodes_[n.right].box);
            node = n.parent;
        }
    }

    void InsertLeaf(uint32_t leaf) {
        if (root_ == 0) {
            root_ = leaf;
            nodes_[leaf].parent = 0;
            return;
        }

        // Walk from the root, descending into the child whose growth is
        // cheapest, until the leaf is cheaper to attach where we are.
        // Copy by value: AllocNode() below can reallocate nodes_ and would
        // leave a reference into the vector dangling.
        const AABB leafBox = nodes_[leaf].box;
        uint32_t index = root_;
        while (!nodes_[index].leaf) {
            const uint32_t child1 = nodes_[index].left;
            const uint32_t child2 = nodes_[index].right;
            const float area = Area(nodes_[index].box);
            const float combinedArea = UnionArea(nodes_[index].box, leafBox);
            const float cost = 2.0f * combinedArea;
            const float inheritanceCost = 2.0f * (combinedArea - area);

            auto childCost = [&](uint32_t child) {
                const AABB& cb = nodes_[child].box;
                if (nodes_[child].leaf) return UnionArea(leafBox, cb) + inheritanceCost;
                const float oldArea = Area(cb);
                const float newArea = UnionArea(leafBox, cb);
                return (newArea - oldArea) + inheritanceCost;
            };
            const float cost1 = childCost(child1);
            const float cost2 = childCost(child2);

            if (cost < cost1 && cost < cost2) break;
            index = cost1 < cost2 ? child1 : child2;
        }

        const uint32_t sibling = index;
        const uint32_t oldParent = nodes_[sibling].parent;
        const uint32_t newParent = AllocNode();
        Node& np = nodes_[newParent];
        np.parent = oldParent;
        np.box = Union(nodes_[sibling].box, leafBox);
        np.left = sibling;
        np.right = leaf;
        np.leaf = false;
        nodes_[sibling].parent = newParent;
        nodes_[leaf].parent = newParent;

        if (oldParent == 0) {
            root_ = newParent;
        } else {
            Node& op = nodes_[oldParent];
            if (op.left == sibling)
                op.left = newParent;
            else
                op.right = newParent;
            Refit(oldParent);
        }
    }

    void RemoveLeaf(uint32_t leaf) {
        if (leaf == root_) {
            root_ = 0;
            return;
        }
        const uint32_t parent = nodes_[leaf].parent;
        const uint32_t grandParent = nodes_[parent].parent;
        const uint32_t sibling = nodes_[parent].left == leaf ? nodes_[parent].right
                                                             : nodes_[parent].left;

        if (grandParent != 0) {
            Node& gp = nodes_[grandParent];
            if (gp.left == parent)
                gp.left = sibling;
            else
                gp.right = sibling;
            nodes_[sibling].parent = grandParent;
            FreeNode(parent);
            Refit(grandParent);
        } else {
            root_ = sibling;
            nodes_[sibling].parent = 0;
            FreeNode(parent);
        }
    }

    template <class Fn>
    void QueryNode(uint32_t node, const AABB& box, Fn& fn) const {
        if (!box.Intersects(nodes_[node].box)) return;
        if (nodes_[node].leaf) {
            fn(nodes_[node].id);
            return;
        }
        QueryNode(nodes_[node].left, box, fn);
        QueryNode(nodes_[node].right, box, fn);
    }

    template <class Fn>
    void QueryNodeFrustum(uint32_t node, const Frustum& frustum, Fn& fn) const {
        if (!frustum.Intersects(nodes_[node].box)) return;
        if (nodes_[node].leaf) {
            fn(nodes_[node].id);
            return;
        }
        QueryNodeFrustum(nodes_[node].left, frustum, fn);
        QueryNodeFrustum(nodes_[node].right, frustum, fn);
    }

    template <class Fn>
    void QueryNodeRay(uint32_t node, const Ray& ray, float maxDist, Fn& fn) const {
        float t = 0.0f;
        if (!IntersectRayAABB(ray, nodes_[node].box, t) || t > maxDist) return;
        if (nodes_[node].leaf) {
            fn(nodes_[node].id);
            return;
        }
        QueryNodeRay(nodes_[node].left, ray, maxDist, fn);
        QueryNodeRay(nodes_[node].right, ray, maxDist, fn);
    }

    std::vector<Node> nodes_{1}; // index 0 = null sentinel
    std::vector<uint32_t> freeList_;
    std::vector<uint32_t> idToNode_;
    uint32_t root_ = 0;
    size_t leafCount_ = 0;
};

} // namespace neon::math
