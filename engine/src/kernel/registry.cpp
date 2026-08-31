#include "neon/kernel/registry.hpp"

#include <string>
#include <unordered_map>

namespace neon::kernel {

void ModuleRegistry::Add(std::unique_ptr<IModule> module) {
    if (module) modules_.push_back(std::move(module));
}

bool ModuleRegistry::InitAll(ServiceRegistry& reg) {
    const size_t n = modules_.size();

    // Map module id -> index.
    std::unordered_map<std::string, size_t> index;
    index.reserve(n);
    for (size_t i = 0; i < n; ++i) index[modules_[i]->Info().id] = i;

    // Kahn's algorithm: `incoming` = number of unsatisfied dependencies.
    std::vector<size_t> incoming(n, 0);
    std::vector<std::vector<size_t>> dependents(n);
    bool missingDep = false;
    for (size_t i = 0; i < n; ++i) {
        for (const char* req : modules_[i]->Info().requires) {
            const auto it = index.find(req);
            if (it == index.end()) {
                missingDep = true;  // unknown dependency id
                continue;
            }
            incoming[i] += 1;
            dependents[it->second].push_back(i);
        }
    }
    if (missingDep) return false;

    // Topological order via a simple repeated scan (module counts are tiny).
    std::vector<size_t> order;
    order.reserve(n);
    std::vector<bool> done(n, false);
    while (order.size() < n) {
        bool progressed = false;
        for (size_t i = 0; i < n; ++i) {
            if (done[i] || incoming[i] != 0) continue;
            done[i] = true;
            order.push_back(i);
            progressed = true;
            for (size_t d : dependents[i]) incoming[d] -= 1;
        }
        if (!progressed) return false;  // cycle
    }

    std::vector<size_t> initialized;
    initialized.reserve(n);
    for (size_t i : order) {
        if (!modules_[i]->Init(reg)) {
            // Roll back the modules already initialized (reverse order).
            for (size_t k = initialized.size(); k-- > 0;) modules_[initialized[k]]->Shutdown();
            return false;
        }
        initialized.push_back(i);
    }
    return true;
}

void ModuleRegistry::ShutdownAll() {
    for (size_t i = modules_.size(); i-- > 0;) modules_[i]->Shutdown();
}

} // namespace neon::kernel
