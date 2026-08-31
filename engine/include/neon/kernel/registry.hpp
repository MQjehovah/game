#pragma once

#include <memory>
#include <typeindex>
#include <unordered_map>
#include <vector>

#include "neon/kernel/module.hpp"

namespace neon::kernel {

// Type-erased service registry: one registered implementation per service
// interface type. Modules publish their services here and look up their
// dependencies, so they stay decoupled from concrete types.
class ServiceRegistry {
public:
    // Registers `service` under its interface type T. A later registration of
    // the same type replaces the previous one (module swap).
    template <typename T>
    void Register(T* service) {
        services_[std::type_index(typeid(T))] =
            const_cast<void*>(static_cast<const void*>(service));
    }

    // Returns the registered T, or nullptr when absent.
    template <typename T>
    T* Get() const {
        const auto it = services_.find(std::type_index(typeid(T)));
        return it == services_.end() ? nullptr : static_cast<T*>(it->second);
    }

    bool Empty() const { return services_.empty(); }

private:
    std::unordered_map<std::type_index, void*> services_;
};

// Owns, orders (by `requires`) and drives a set of modules. InitAll runs the
// modules in dependency order; ShutdownAll tears them down in reverse order.
class ModuleRegistry {
public:
    void Add(std::unique_ptr<IModule> module);

    // Initializes every module in topological order of their `requires` edges.
    // Returns false when a dependency id is unknown or the graph has a cycle;
    // on failure no module is initialized.
    bool InitAll(ServiceRegistry& reg);

    void ShutdownAll();

    size_t Count() const { return modules_.size(); }

private:
    std::vector<std::unique_ptr<IModule>> modules_;
};

} // namespace neon::kernel
