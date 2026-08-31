#pragma once

#include <memory>

#include "neon/kernel/registry.hpp"

namespace neon::kernel {

// The microkernel itself: a thin bundling of the module registry + service
// registry that the application (P-E) drives. The app adds its modules, calls
// Init() to wire them in dependency order, then fetches services from
// Services() and hands them to the runtime. Swap a module = add a different
// implementation before Init().
//
// This is the eventual replacement for core::Application's ad-hoc subsystem
// construction: the kernel only loads modules and runs the loop's lifecycle;
// every subsystem is a replaceable IModule behind it.
class Kernel {
public:
    void Add(std::unique_ptr<IModule> module) { modules_.Add(std::move(module)); }

    // Wires all added modules in dependency order; false on a cycle / missing
    // dependency / module init failure.
    bool Init() { return modules_.InitAll(services_); }

    void Shutdown() { modules_.ShutdownAll(); }

    ServiceRegistry& Services() { return services_; }
    const ServiceRegistry& Services() const { return services_; }
    ModuleRegistry& Modules() { return modules_; }
    const ModuleRegistry& Modules() const { return modules_; }

private:
    ModuleRegistry modules_;
    ServiceRegistry services_;
};

} // namespace neon::kernel
