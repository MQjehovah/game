#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "neon/ecs/parallel.hpp"

namespace neon::ecs {
namespace parallel {

// Dependency-graph task scheduler (G5-2).
//
// Callers add tasks with explicit dependencies (edges to earlier tasks).
// Run() topologically orders the graph (Kahn's algorithm), detects cycles,
// and executes each level's INDEPENDENT tasks on the shared worker pool
// (levels are bucketed so tasks in the same level have no dependency path
// between them). Run(false) executes serially in topological order.
//
// The determinism contract is the same as ParallelFor: a task must only touch
// its own state (or state explicitly shared with its dependencies, which are
// guaranteed to have finished). Independent tasks never share mutable state,
// so the parallel result equals the serial result. No lock-free queue is
// needed because ParallelFor already joins per level; a work-stealing queue is
// the documented future upgrade path for "hundreds of cores".
class TaskGraph {
public:
    using TaskFn = std::function<void()>;

    // Adds a task. `deps` are indices of tasks that must finish first; they
    // may reference tasks added LATER (forward references), so cycles are
    // possible and are detected by Run(). Returns the new task's index.
    uint32_t Add(std::string name, TaskFn fn, std::vector<uint32_t> deps = {});

    // Executes every task respecting dependencies. Returns false and runs
    // NOTHING when the graph has a cycle or an out-of-range dependency
    // (LastError() describes the problem). `parallel` uses the shared worker
    // pool; false runs serially in topological order (tests / determinism
    // validation).
    bool Run(bool parallel = true);

    void Clear();
    size_t TaskCount() const { return tasks_.size(); }
    bool Empty() const { return tasks_.empty(); }
    const std::string& LastError() const { return error_; }

    // Task indices in execution (topological) order from the last Run().
    // Stable in both modes: parallel mode records the per-level dispatch
    // order (level ascending, task index ascending), which is also the serial
    // order. Cleared by Clear().
    const std::vector<uint32_t>& ExecutionOrder() const { return order_; }

private:
    struct Task {
        std::string name;
        TaskFn fn;
        std::vector<uint32_t> deps;
    };

    std::vector<Task> tasks_;
    std::vector<uint32_t> order_;
    std::string error_;
};

} // namespace parallel
} // namespace neon::ecs
