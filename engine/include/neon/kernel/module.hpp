#pragma once

#include <string>
#include <vector>

namespace neon::kernel {

class ServiceRegistry;

// Static metadata describing a module. `requires` lists the module ids this
// module depends on — the kernel initializes those first.
struct ModuleInfo {
    const char* id = "";
    const char* version = "0.0.0";
    std::vector<const char*> requires;
};

// The uniform in-process module contract. Every engine subsystem (gfx, physics,
// audio, script, assets, scene, ...) implements this so the kernel can load,
// wire and swap them uniformly.
//
// Modules do NOT #include/link each other directly: each publishes its services
// on the ServiceRegistry in Init() and fetches its dependencies from it, so a
// module is replaced by swapping its registration — the rest of the engine and
// the kernel never change.
class IModule {
public:
    virtual ~IModule() = default;

    virtual ModuleInfo Info() const = 0;

    // Register services + fetch dependencies from `reg`. Return false on a
    // fatal init error (e.g. a missing required dependency).
    virtual bool Init(ServiceRegistry& reg) = 0;

    virtual void Shutdown() = 0;
};

} // namespace neon::kernel
