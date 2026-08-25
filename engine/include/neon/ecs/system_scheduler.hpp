#pragma once

#include <cstddef>
#include <memory>
#include <string>
#include <typeindex>
#include <typeinfo>
#include <vector>

#include "neon/ecs/task_graph.hpp"
#include "neon/ecs/world.hpp"

namespace neon::ecs {

// G2-2: dependency-graph system scheduler over the existing SparseSet ECS.
//
// Systems are registered in order together with the component types they
// READ and WRITE. The scheduler derives a dependency edge between any two
// systems whose accesses conflict (write-write or write-read on the same
// component type), in registration order: an earlier-registered system always
// finishes before a later one that touches the same component. Systems with
// NO conflicting access are independent and run in parallel on the shared
// worker pool (via TaskGraph, G5-2). Run(false) executes everything serially
// in the same topological order - the deterministic reference path, and the
// path tests use to validate the parallel result.
//
// Existing API unchanged: systems receive the same (dt, World&) as before and
// may use View::ForEach / View::ParallelForEach inside their own Update.
// NOTE: in parallel mode a system's Update must NOT call the global
// parallel::ParallelFor itself (the worker pool is single-submitter; two
// systems dispatching chunks at once would race). Use serial View::ForEach
// inside systems, or ParallelForEach when the system runs in a serial graph.
class SystemScheduler {
public:
    SystemScheduler() = default;

    // Registers `sys` under `name`. `reads` / `writes` are the component
    // typeids the system's Update accesses (empty = no component access, so
    // the system is independent of every other system's component traffic).
    // Systems with conflicting access run in registration order; independent
    // systems may run in parallel. The scheduler keeps a shared_ptr so the
    // caller does not own the lifetime.
    void Add(std::string name, std::shared_ptr<System> sys,
             std::vector<std::type_index> reads = {},
             std::vector<std::type_index> writes = {});

    // Runs every registered system once. `parallel=true` executes independent
    // systems on the worker pool; false executes serially in topological
    // (registration) order. Returns false (and runs NOTHING) when the graph
    // cannot be executed (cycle / bad dependency - see LastError).
    bool Run(float dt, World& world, bool parallel = true);

    void Clear();
    size_t Count() const { return systems_.size(); }
    const std::string& LastError() const { return error_; }

private:
    struct Entry {
        std::string name;
        std::shared_ptr<System> sys;
        std::vector<std::type_index> reads;
        std::vector<std::type_index> writes;
    };

    // True when the two systems share a component and at least one of them
    // writes it (write-write or write-read => they must not run in parallel).
    static bool Conflicts(const Entry& a, const Entry& b);

    std::vector<Entry> systems_;
    std::string error_;
};

} // namespace neon::ecs
