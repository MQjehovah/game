#include "neon/neon.hpp"
#include "neon/kernel/module.hpp"
#include "neon/kernel/registry.hpp"
#include "neon/modules/subsystem_modules.hpp"
#include "neon/scene/game_runtime.hpp"
#include "neon/scene/scene_file.hpp"
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

// P-B: GameRuntime fetches its physics world from the ServiceRegistry instead
// of creating its own — the module wiring reaches the runtime.
TEST(GameRuntimeInjectsPhysicsService) {
    ModuleRegistry mgr;
    auto physicsModule = std::make_unique<PhysicsModule>(std::make_unique<physics::World>());
    physics::World* world = physicsModule->World();
    mgr.Add(std::move(physicsModule));
    ServiceRegistry reg;
    CHECK(mgr.InitAll(reg));

    const char* scene = R"({
      "entities": [
        {"name": "Ball", "components": {
          "transform": {"pos": [0, 5, 0]},
          "rigidbody": {"shape": "sphere", "radius": 0.5, "dynamic": true}}}
      ]
    })";
    scene::GameRuntime runtime;
    scene::GameRuntimeConfig cfg;
    cfg.headless = true;
    cfg.services = &reg;  // physics comes from the registry, not the runtime
    CHECK(runtime.Start(scene, cfg).Ok());

    // The ball was registered in the INJECTED world (owned by the module).
    CHECK_EQ(runtime.PhysicsBodyCount(), 1u);
    CHECK_EQ(world->BodyCount(), 1u);

    for (int i = 0; i < 180; ++i) runtime.Tick(1.0f / 60.0f);
    ecs::Entity ball = runtime.FindNamedEntity("Ball");
    CHECK(ball.IsValid());
    if (const scene::SceneTransform* t = runtime.World().Get<scene::SceneTransform>(ball))
        CHECK_NEAR(t->pos.y, 0.5f, 0.02f);

    runtime.Stop();
    mgr.ShutdownAll();
}
