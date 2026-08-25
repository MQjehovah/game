#include <atomic>
#include <string>
#include <vector>

#include "neon/neon.hpp"
#include "helpers.hpp"

using namespace neon;

namespace {

// Builds a diamond DAG: A -> {B, C} -> D. Tasks write to disjoint cells;
// D also verifies B and C completed (parallel ordering guarantee).
void BuildDiamond(ecs::parallel::TaskGraph& g, std::vector<std::atomic<int>>& cells,
                  std::atomic<int>& orderGuard) {
    const uint32_t a = g.Add("A", [&]() { cells[0].store(1); });
    const uint32_t b = g.Add("B", [&]() {
        if (cells[0].load() != 1) orderGuard.fetch_add(1000);
        cells[1].store(1);
    }, {a});
    const uint32_t c = g.Add("C", [&]() {
        if (cells[0].load() != 1) orderGuard.fetch_add(1000);
        cells[2].store(1);
    }, {a});
    g.Add("D", [&]() {
        if (cells[1].load() != 1 || cells[2].load() != 1) orderGuard.fetch_add(1000);
        cells[3].store(1);
    }, {b, c});
}

} // namespace

// A chain must execute strictly in dependency order.
TEST(TaskGraphDependencyOrder) {
    ecs::parallel::TaskGraph g;
    std::vector<int> order;
    uint32_t prev = 0;
    for (int i = 0; i < 5; ++i) {
        prev = g.Add("task" + std::to_string(i),
                     [&order, i]() { order.push_back(i); },
                     i == 0 ? std::vector<uint32_t>{} : std::vector<uint32_t>{prev});
    }
    CHECK(g.Run(false));
    CHECK_EQ(order.size(), 5u);
    for (int i = 0; i < 5; ++i) CHECK_EQ(order[static_cast<size_t>(i)], i);
    CHECK_EQ(g.ExecutionOrder().size(), 5u);
    for (size_t i = 0; i < g.ExecutionOrder().size(); ++i)
        CHECK_EQ(g.ExecutionOrder()[i], static_cast<uint32_t>(i));
}

// The diamond DAG: parallel execution must satisfy dependencies and produce
// the same state as the serial execution.
TEST(TaskGraphParallelMatchesSerial) {
    ecs::parallel::TaskGraph gp, gs;
    std::vector<std::atomic<int>> pcells(4), scells(4);
    for (auto& c : pcells) c.store(0);
    for (auto& c : scells) c.store(0);
    std::atomic<int> pGuard{0}, sGuard{0};
    BuildDiamond(gp, pcells, pGuard);
    BuildDiamond(gs, scells, sGuard);

    CHECK(gp.Run(true));
    CHECK(gs.Run(false));

    for (int i = 0; i < 4; ++i) {
        CHECK_EQ(pcells[static_cast<size_t>(i)].load(), 1);
        CHECK_EQ(scells[static_cast<size_t>(i)].load(), 1);
    }
    CHECK_EQ(pGuard.load(), 0); // every dependency was satisfied before use
    CHECK_EQ(sGuard.load(), 0);
    // Dispatch order is identical in both modes (level ascending, id order).
    CHECK_EQ(gp.ExecutionOrder().size(), gs.ExecutionOrder().size());
    for (size_t i = 0; i < gp.ExecutionOrder().size(); ++i)
        CHECK_EQ(gp.ExecutionOrder()[i], gs.ExecutionOrder()[i]);
}

// Running the same graph twice in parallel is deterministic.
TEST(TaskGraphParallelDeterministicAcrossRuns) {
    for (int run = 0; run < 2; ++run) {
        ecs::parallel::TaskGraph g;
        std::vector<std::atomic<int>> cells(4);
        for (auto& c : cells) c.store(0);
        std::atomic<int> guard{0};
        BuildDiamond(g, cells, guard);
        CHECK(g.Run(true));
        for (int i = 0; i < 4; ++i) CHECK_EQ(cells[static_cast<size_t>(i)].load(), 1);
        CHECK_EQ(guard.load(), 0);
    }
}

// A cycle (possible via forward references) is detected, nothing runs, and
// the error names the tasks.
TEST(TaskGraphCycleDetected) {
    ecs::parallel::TaskGraph g;
    std::atomic<int> ran{0};
    // Edges: C -> A (A depends on C), A -> B, B -> C  =>  C -> A -> B -> C.
    g.Add("A", [&]() { ran.fetch_add(1); }, {2u});
    g.Add("B", [&]() { ran.fetch_add(1); }, {0u});
    g.Add("C", [&]() { ran.fetch_add(1); }, {1u});
    CHECK(!g.Run(false));
    CHECK(g.LastError().find("cycle") != std::string::npos);
    CHECK(g.LastError().find("A") != std::string::npos);
    CHECK_EQ(ran.load(), 0); // nothing executed
    CHECK(g.ExecutionOrder().empty());
}

// Out-of-range dependencies are rejected without running anything.
TEST(TaskGraphOutOfRangeDependency) {
    ecs::parallel::TaskGraph g;
    std::atomic<int> ran{0};
    g.Add("A", [&]() { ran.fetch_add(1); });
    g.Add("B", [&]() { ran.fetch_add(1); }, {5u});
    CHECK(!g.Run(false));
    CHECK(g.LastError().find("out-of-range") != std::string::npos);
    CHECK_EQ(ran.load(), 0);
    CHECK(g.ExecutionOrder().empty());
}

// Clear + reuse works.
TEST(TaskGraphClearAndReuse) {
    ecs::parallel::TaskGraph g;
    std::atomic<int> ran{0};
    g.Add("one", [&]() { ran.fetch_add(1); });
    CHECK(g.Run(false));
    CHECK_EQ(ran.load(), 1);
    g.Clear();
    CHECK(g.Empty());
    g.Add("two", [&]() { ran.fetch_add(10); });
    CHECK(g.Run(true));
    CHECK_EQ(ran.load(), 11);
}

// The doc's acceptance example: input -> physics/logic -> render dependency
// graph executes correctly in parallel and matches the serial result.
TEST(TaskGraphRenderPhysicsLogicExample) {
    struct State {
        int inputs = 0;
        int physics = 0;
        int logic = 0;
        int render = 0;
    };
    for (int mode = 0; mode < 2; ++mode) {
        ecs::parallel::TaskGraph g;
        State st;
        const uint32_t inputs = g.Add("inputs", [&]() { st.inputs = 42; });
        const uint32_t physics = g.Add("physics", [&]() { st.physics = st.inputs + 1; }, {inputs});
        const uint32_t logic = g.Add("logic", [&]() { st.logic = st.inputs * 2; }, {inputs});
        g.Add("render", [&]() { st.render = st.physics + st.logic; }, {physics, logic});
        CHECK(g.Run(mode == 1));
        CHECK_EQ(st.inputs, 42);
        CHECK_EQ(st.physics, 43);
        CHECK_EQ(st.logic, 84);
        CHECK_EQ(st.render, 127);
    }
}
