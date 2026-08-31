#include "neon/neon.hpp"
#include "neon/kernel/module.hpp"
#include "neon/kernel/registry.hpp"
#include "neon/modules/subsystem_modules.hpp"
#include "helpers.hpp"

#include <memory>

using namespace neon;
using namespace neon::kernel;
using namespace neon::modules;

// The real engine subsystems (physics + script, both headless) wired through
// the microkernel: each is a module owning its subsystem and registering its
// interface, and consumers resolve the interface from the ServiceRegistry.
TEST(SubsystemModulesWireThroughKernel) {
    ModuleRegistry mgr;
    mgr.Add(std::make_unique<PhysicsModule>(std::make_unique<physics::World>()));
    mgr.Add(std::make_unique<ScriptModule>(script::CreateLuaHost()));

    ServiceRegistry reg;
    CHECK(mgr.InitAll(reg));
    CHECK_EQ(mgr.Count(), 2u);
    CHECK(reg.Get<physics::World>() != nullptr);
    CHECK(reg.Get<script::IScriptHost>() != nullptr);
    mgr.ShutdownAll();
}

// Replacing a module = constructing it with a different implementation and
// re-initializing; the consumer keeps resolving the same interface type.
TEST(SubsystemModuleSwap) {
    ModuleRegistry a;
    a.Add(std::make_unique<PhysicsModule>(std::make_unique<physics::World>()));
    ServiceRegistry regA;
    CHECK(a.InitAll(regA));
    physics::World* first = regA.Get<physics::World>();
    CHECK(first != nullptr);

    ModuleRegistry b;
    b.Add(std::make_unique<PhysicsModule>(std::make_unique<physics::World>()));
    ServiceRegistry regB;
    CHECK(b.InitAll(regB));
    physics::World* second = regB.Get<physics::World>();
    CHECK(second != nullptr);
    CHECK(second != first);  // a fresh, independent implementation

    b.ShutdownAll();
    a.ShutdownAll();
}
