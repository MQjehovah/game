#include "neon/ecs/task_graph.hpp"

#include <algorithm>

namespace neon::ecs {
namespace parallel {

uint32_t TaskGraph::Add(std::string name, TaskFn fn, std::vector<uint32_t> deps) {
    tasks_.push_back(Task{std::move(name), std::move(fn), std::move(deps)});
    return static_cast<uint32_t>(tasks_.size() - 1);
}

void TaskGraph::Clear() {
    tasks_.clear();
    order_.clear();
    error_.clear();
}

bool TaskGraph::Run(bool parallel) {
    error_.clear();
    order_.clear();
    const size_t n = tasks_.size();
    if (n == 0) return true;

    // Validate dependencies and build the dependents adjacency + indegree.
    std::vector<std::vector<uint32_t>> dependents(n);
    std::vector<int> indegree(n, 0);
    for (size_t t = 0; t < n; ++t) {
        for (uint32_t d : tasks_[t].deps) {
            if (d >= n) {
                error_ = "task '" + tasks_[t].name + "' depends on out-of-range task " +
                         std::to_string(d);
                return false;
            }
            dependents[d].push_back(static_cast<uint32_t>(t));
            ++indegree[t];
        }
    }

    // Kahn's algorithm: process tasks whose dependencies are all satisfied,
    // assigning each its topological level (1 + max dependency level).
    std::vector<int> level(n, 0);
    std::vector<uint32_t> ready;
    ready.reserve(n);
    for (size_t t = 0; t < n; ++t)
        if (indegree[t] == 0) ready.push_back(static_cast<uint32_t>(t));

    size_t processed = 0;
    size_t cursor = 0;
    int maxLevel = 0;
    while (cursor < ready.size()) {
        const uint32_t t = ready[cursor++];
        order_.push_back(t);
        for (uint32_t d : tasks_[t].deps)
            level[t] = std::max(level[t], level[d] + 1);
        maxLevel = std::max(maxLevel, level[t]);
        ++processed;
        for (uint32_t v : dependents[t]) {
            if (--indegree[v] == 0) ready.push_back(v);
        }
    }
    if (processed != n) {
        // The leftover tasks form at least one cycle; name them for debugging.
        error_ = "task dependency cycle involving:";
        for (size_t t = 0; t < n; ++t)
            if (indegree[t] != 0) error_ += " '" + tasks_[t].name + "'";
        order_.clear();
        return false;
    }

    // Bucket by level so each level's tasks can run in parallel (no edges
    // between same-level tasks by construction). Buckets keep task-index
    // order, making the dispatch order deterministic in both modes.
    std::vector<std::vector<uint32_t>> byLevel(static_cast<size_t>(maxLevel + 1));
    for (size_t t = 0; t < n; ++t)
        byLevel[static_cast<size_t>(level[t])].push_back(static_cast<uint32_t>(t));

    for (const std::vector<uint32_t>& bucket : byLevel) {
        if (bucket.empty()) continue;
        const bool canParallel = parallel && bucket.size() > 1 && AvailableWorkers() > 0;
        if (canParallel) {
            // ParallelFor's fixed chunk partition visits each task exactly
            // once; tasks in a bucket are independent, so the result is
            // identical to the serial loop.
            ParallelFor(static_cast<uint32_t>(bucket.size()), [&](uint32_t s, uint32_t e) {
                for (uint32_t i = s; i < e; ++i) tasks_[bucket[i]].fn();
            });
        } else {
            for (uint32_t idx : bucket) tasks_[idx].fn();
        }
    }
    return true;
}

} // namespace parallel
} // namespace neon::ecs
