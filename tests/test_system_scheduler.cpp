#include <typeinfo>
#include <vector>

#include "neon/neon.hpp"
#include "helpers.hpp"

using namespace neon;

// G2-2: dependency-graph system scheduler. Systems declare the component
// types they read/write; the scheduler orders conflicting systems by
// registration and runs independent systems in parallel (TaskGraph). These
// tests assert the ordering guarantees and that the parallel path is
// bit-identical to the serial reference path.

namespace {

struct Acc {
    int value = 0;
};
struct Boost {
    int value = 0;
};
struct Readout {
    int value = 0;
};
struct Tag {
    int id = 0;
};

// System that adds `delta` to every Acc component.
class AccAdder : public ecs::System {
public:
    explicit AccAdder(int delta) : delta_(delta) {}
    void Update(float, ecs::World& w) override {
        w.ViewAll<Acc>().ForEach([this](ecs::Entity, Acc& a) { a.value += delta_; });
    }

private:
    int delta_;
};

// System that adds `delta` to every Boost component.
class BoostAdder : public ecs::System {
public:
    explicit BoostAdder(int delta) : delta_(delta) {}
    void Update(float, ecs::World& w) override {
        w.ViewAll<Boost>().ForEach([this](ecs::Entity, Boost& b) { b.value += delta_; });
    }

private:
    int delta_;
};

// System that sets Boost = 2 * Acc (reads Acc, writes Boost).
class AccReader : public ecs::System {
public:
    void Update(float, ecs::World& w) override {
        w.ViewAll<Acc>().ForEach([&w](ecs::Entity e, Acc& a) {
            if (Boost* b = w.Get<Boost>(e)) b->value = a.value * 2;
        });
    }
};

// System that writes the number of Tag entities into Readout.
class TagCounter : public ecs::System {
public:
    explicit TagCounter(Readout* out) : out_(out) {}
    void Update(float, ecs::World& w) override {
        int n = 0;
        w.ViewAll<Tag>().ForEach([&n](ecs::Entity, Tag&) { ++n; });
        out_->value = n;
    }

private:
    Readout* out_;
};

} // namespace

// Two systems writing the same component must run in registration order:
// A adds 1, B adds 10 -> the final value is the sum in that order, no matter
// how the OS schedules the parallel pass.
TEST(SystemSchedulerWriteWriteOrdered) {
    ecs::World world;
    ecs::Entity e = world.Create();
    world.Add<Acc>(e, Acc{0});

    auto a = std::make_shared<AccAdder>(1);
    auto b = std::make_shared<AccAdder>(10);
    const std::type_index t = std::type_index(typeid(Acc));

    ecs::SystemScheduler sched;
    sched.Add("add_one", a, {}, {t});
    sched.Add("add_ten", b, {}, {t});

    for (bool parallel : {false, true}) {
        world.Get<Acc>(e)->value = 0;
        CHECK(sched.Run(0.016f, world, parallel));
        CHECK_EQ(world.Get<Acc>(e)->value, 11);
    }
    CHECK_EQ(sched.Count(), 2u);
}

// A write-then-read chain: the reader must observe the writer's result.
TEST(SystemSchedulerWriteReadDependency) {
    ecs::World world;
    ecs::Entity e = world.Create();
    world.Add<Acc>(e, Acc{5});
    world.Add<Boost>(e, Boost{0});

    auto writer = std::make_shared<AccAdder>(7);
    auto reader = std::make_shared<AccReader>();
    const std::type_index accT = std::type_index(typeid(Acc));
    const std::type_index boostT = std::type_index(typeid(Boost));

    ecs::SystemScheduler sched;
    sched.Add("writer", writer, {}, {accT});
    sched.Add("reader", reader, {accT}, {boostT});

    for (bool parallel : {false, true}) {
        world.Get<Acc>(e)->value = 5;
        CHECK(sched.Run(0.016f, world, parallel));
        CHECK_EQ(world.Get<Boost>(e)->value, 24); // (5 + 7) * 2
    }
}

// Read-read systems share a component but neither writes it: they must not be
// ordered, and the parallel run must equal the serial run (determinism).
TEST(SystemSchedulerReadReadParallelDeterministic) {
    ecs::World world;
    for (int i = 0; i < 100; ++i) {
        ecs::Entity e = world.Create();
        world.Add<Tag>(e, Tag{i});
    }
    const std::type_index tagT = std::type_index(typeid(Tag));

    Readout outA{0}, outB{0};
    auto a = std::make_shared<TagCounter>(&outA);
    auto b = std::make_shared<TagCounter>(&outB);

    ecs::SystemScheduler sched;
    sched.Add("count_a", a, {tagT}, {});
    sched.Add("count_b", b, {tagT}, {});

    outA.value = outB.value = 0;
    CHECK(sched.Run(0.016f, world, false));
    const int serialA = outA.value;
    const int serialB = outB.value;
    outA.value = outB.value = 0;
    CHECK(sched.Run(0.016f, world, true));
    CHECK_EQ(outA.value, serialA);
    CHECK_EQ(outB.value, serialB);
    CHECK_EQ(outA.value, 100);
    CHECK_EQ(outB.value, 100);
}

// Systems writing DIFFERENT components are independent and both run.
TEST(SystemSchedulerIndependentWrites) {
    ecs::World world;
    ecs::Entity e = world.Create();
    world.Add<Acc>(e, Acc{1});
    world.Add<Boost>(e, Boost{2});

    auto addAcc = std::make_shared<AccAdder>(3);
    auto addBoost = std::make_shared<BoostAdder>(5);
    const std::type_index accT = std::type_index(typeid(Acc));
    const std::type_index boostT = std::type_index(typeid(Boost));

    ecs::SystemScheduler sched;
    sched.Add("acc", addAcc, {}, {accT});
    sched.Add("boost", addBoost, {}, {boostT});

    for (bool parallel : {false, true}) {
        world.Get<Acc>(e)->value = 1;
        world.Get<Boost>(e)->value = 2;
        CHECK(sched.Run(0.016f, world, parallel));
        CHECK_EQ(world.Get<Acc>(e)->value, 4);
        CHECK_EQ(world.Get<Boost>(e)->value, 7);
    }
}

// Systems with no declared component access are independent of everything.
TEST(SystemSchedulerNoAccessRunsAll) {
    ecs::World world;
    Readout a{0}, b{0};
    ecs::SystemScheduler sched;
    sched.Add("a", std::make_shared<TagCounter>(&a), {}, {});
    sched.Add("b", std::make_shared<TagCounter>(&b), {}, {});
    for (bool parallel : {false, true}) {
        a.value = b.value = 0;
        CHECK(sched.Run(0.016f, world, parallel));
        CHECK_EQ(a.value, 0);
        CHECK_EQ(b.value, 0);
    }
}

// Empty scheduler runs cleanly; Clear() drops registrations.
TEST(SystemSchedulerClearAndEmpty) {
    ecs::World world;
    ecs::SystemScheduler sched;
    CHECK(sched.Run(0.016f, world, true));
    CHECK_EQ(sched.Count(), 0u);

    Readout out{0};
    sched.Add("x", std::make_shared<TagCounter>(&out), {}, {});
    CHECK_EQ(sched.Count(), 1u);
    sched.Clear();
    CHECK_EQ(sched.Count(), 0u);
    CHECK(sched.Run(0.016f, world, true));
}
