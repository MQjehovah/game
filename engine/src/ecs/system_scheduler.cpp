#include "neon/ecs/system_scheduler.hpp"

#include <algorithm>

namespace neon::ecs {

void SystemScheduler::Add(std::string name, std::shared_ptr<System> sys,
                          std::vector<std::type_index> reads,
                          std::vector<std::type_index> writes) {
    if (!sys) return;
    Entry e;
    e.name = std::move(name);
    e.sys = std::move(sys);
    e.reads = std::move(reads);
    e.writes = std::move(writes);
    systems_.push_back(std::move(e));
}

bool SystemScheduler::Conflicts(const Entry& a, const Entry& b) {
    // A shared type matters only when at least one side writes it:
    //   a writes X and b reads/writes X  -> a must finish before b
    //   b writes X and a reads X         -> a must finish before b
    auto shares = [&](const std::vector<std::type_index>& lhs,
                      const std::vector<std::type_index>& rhs) {
        for (const std::type_index& t : lhs)
            if (std::find(rhs.begin(), rhs.end(), t) != rhs.end()) return true;
        return false;
    };
    if (shares(a.writes, b.reads) || shares(a.writes, b.writes)) return true;
    if (shares(b.writes, a.reads)) return true;
    return false;
}

bool SystemScheduler::Run(float dt, World& world, bool parallel) {
    error_.clear();
    if (systems_.empty()) return true;

    parallel::TaskGraph graph;
    for (size_t i = 0; i < systems_.size(); ++i) {
        Entry& cur = systems_[i];
        std::vector<uint32_t> deps;
        for (size_t j = 0; j < i; ++j) {
            if (Conflicts(systems_[j], cur)) deps.push_back(static_cast<uint32_t>(j));
        }
        // Capture the entry pointer before the loop moves on: entries are
        // never reallocated once pushed (the vector is only appended).
        System* sys = cur.sys.get();
        const std::string name = cur.name;
        graph.Add(name, [dt, &world, sys]() { sys->Update(dt, world); }, std::move(deps));
    }

    if (!graph.Run(parallel)) {
        error_ = graph.LastError();
        return false;
    }
    return true;
}

void SystemScheduler::Clear() { systems_.clear(); }

} // namespace neon::ecs
