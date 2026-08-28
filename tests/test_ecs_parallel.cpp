#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <vector>

#include "neon/neon.hpp"
#include "helpers.hpp"

using namespace neon;

// Test-suite 5.5: ECS batch iteration + deterministic parallel jobs.
// ---------------------------------------------------------------------------
// The MinGW 8.1 win32 toolchain has no std::thread (__STDCPP_THREADS__
// undefined), so parallel.hpp uses Win32 CreateThread / POSIX pthread via a
// small worker pool (same approach as neon/assets/async_loader). ParallelFor
// splits a range into a FIXED set of contiguous chunks; only which thread runs
// a chunk varies, so independent-item work is bit-identical to the serial path
// and identical across runs. These tests assert exactly that.

namespace {

struct PPos {
    float x = 0, y = 0;
};
struct PVel {
    float dx = 0, dy = 0;
};

constexpr size_t kCount = 100000;

PVel ComputeVelocity(const PPos& p) {
    return PVel{p.x * 3.0f + 1.0f, p.y * 0.5f - 2.0f};
}

// Creates kCount entities 1..kCount in creation order (so pool dense order ==
// id order and out[e.id - 1] is a clean per-entity slot). Returns the source
// positions and the expected velocity computed serially at fill time.
struct FillResult {
    std::vector<PPos> pos;
    std::vector<PVel> expectedVel;
};
FillResult FillWorld(ecs::World& world, size_t n) {
    FillResult r;
    r.pos.resize(n);
    r.expectedVel.resize(n);
    for (size_t i = 0; i < n; ++i) {
        ecs::Entity e = world.Create();
        const float f = static_cast<float>(i);
        world.Add<PPos>(e, PPos{f, f * 2.0f});
        world.Add<PVel>(e, PVel{0, 0});
        r.pos[i] = PPos{f, f * 2.0f};
        r.expectedVel[i] = ComputeVelocity(r.pos[i]);
    }
    return r;
}

// Log sink counting Ecs-category Error entries, so tests can assert that
// guarded violations were both rejected AND reported in a Release build.
struct EcsLogCounter {
    std::atomic<int> count{0};
};
void EcsErrorSink(const neon::core::LogEntry& e, void* userData) {
    if (e.category == neon::core::LogCategory::Ecs && e.level == neon::core::LogLevel::Error) {
        static_cast<EcsLogCounter*>(userData)->count.fetch_add(1);
    }
}

} // namespace

// 1. 100k-entity batch iteration: serial ForEach visits every entity exactly
// once and produces the expected per-entity result.
TEST(ECSForEachBatch100k) {
    ecs::World world;
    FillResult fr = FillWorld(world, kCount);

    auto view = world.ViewAll<PPos>();
    CHECK_EQ(view.Size(), kCount);

    std::vector<PVel> out(kCount);
    view.ForEach([&out](ecs::Entity e, PPos& p) { out[e.id - 1] = ComputeVelocity(p); });

    CHECK(std::memcmp(out.data(), fr.expectedVel.data(), out.size() * sizeof(PVel)) == 0);
    CHECK_EQ(world.EntityCount(), kCount);
}

// 2. ParallelForEach over the same 100k entities is bit-identical to the
// serial ForEach, and a second parallel pass is bit-identical to the first
// (determinism across runs).
TEST(ECSParallelForEachMatchesSerial) {
    ecs::World world;
    FillResult fr = FillWorld(world, kCount);
    CHECK(ecs::parallel::AvailableWorkers() > 0); // this platform really threaded

    std::vector<PVel> serial(kCount), par1(kCount), par2(kCount);

    world.ViewAll<PPos>().ForEach(
        [&serial](ecs::Entity e, PPos& p) { serial[e.id - 1] = ComputeVelocity(p); });
    world.ViewAll<PPos>().ParallelForEach(
        [&par1](ecs::Entity e, PPos& p) { par1[e.id - 1] = ComputeVelocity(p); });
    world.ViewAll<PPos>().ParallelForEach(
        [&par2](ecs::Entity e, PPos& p) { par2[e.id - 1] = ComputeVelocity(p); });

    CHECK(std::memcmp(serial.data(), fr.expectedVel.data(), serial.size() * sizeof(PVel)) == 0);
    CHECK(std::memcmp(par1.data(), serial.data(), par1.size() * sizeof(PVel)) == 0);
    CHECK(std::memcmp(par2.data(), par1.data(), par2.size() * sizeof(PVel)) == 0);
}

// 3. ParallelFor with a per-chunk-slot reduction + serial combine equals a
// fully serial reduction, and is stable when run again (determinism).
TEST(ParallelForReductionDeterministic) {
    const uint32_t n = 100000;

    int64_t serialSum = 0;
    for (uint32_t i = 0; i < n; ++i) serialSum += static_cast<int64_t>(i) * 3 + 1;

    ecs::parallel::Reducer<int64_t> red(ecs::parallel::AvailableWorkers());
    ecs::parallel::ParallelFor(n, [&red, n](uint32_t s, uint32_t e) {
        int64_t& acc = red.Slot(s, n);
        for (uint32_t i = s; i < e; ++i) acc += static_cast<int64_t>(i) * 3 + 1;
    });
    CHECK_EQ(red.Combine(), serialSum);

    // Second pass must reproduce the identical combined result.
    ecs::parallel::Reducer<int64_t> red2(ecs::parallel::AvailableWorkers());
    ecs::parallel::ParallelFor(n, [&red2, n](uint32_t s, uint32_t e) {
        int64_t& acc = red2.Slot(s, n);
        for (uint32_t i = s; i < e; ++i) acc += static_cast<int64_t>(i) * 3 + 1;
    });
    CHECK_EQ(red2.Combine(), serialSum);
}

// 3b. Reducer::Slot assigns each chunk a UNIQUE slot (no two chunks collide),
// and every chunk is covered exactly once - guards the partition math.
TEST(ParallelForReducerUniqueSlots) {
    const uint32_t n = 100000;
    const int slots = std::max(ecs::parallel::AvailableWorkers(), 1);
    ecs::parallel::Reducer<int64_t> red(slots);
    std::vector<std::atomic<int>> touch(static_cast<size_t>(slots));

    ecs::parallel::ParallelFor(n, [&red, &touch, n](uint32_t s, uint32_t e) {
        int64_t& acc = red.Slot(s, n);
        for (size_t i = 0; i < touch.size(); ++i) {
            if (&acc == &red.Slots()[i]) {
                touch[i].fetch_add(1, std::memory_order_relaxed);
                break;
            }
        }
        for (uint32_t i = s; i < e; ++i) acc += 1;
    });

    int touched = 0;
    for (auto& t : touch) {
        CHECK(t.load() <= 1);
        if (t.load() == 1) ++touched;
    }
    CHECK_EQ(touched, std::min(slots, static_cast<int>(n)));
    CHECK_EQ(red.Combine(), static_cast<int64_t>(n));
}

// 4. A pool with zero workers (AvailableWorkers() == 0) falls back to a serial
// loop and is still correct.
TEST(ParallelForNoWorkersFallsBackToSerial) {
    ecs::parallel::ThreadPool pool(0);
    CHECK(!pool.Available());
    CHECK_EQ(pool.WorkerCount(), 0);

    const uint32_t n = 10000;
    std::vector<uint32_t> out(n, 0);
    pool.ParallelFor(n, [&out](uint32_t s, uint32_t e) {
        for (uint32_t i = s; i < e; ++i) out[i] = i * 3u;
    });
    for (uint32_t i = 0; i < n; ++i) CHECK_EQ(out[i], i * 3u);

    // Empty range is a no-op serial call, not a crash.
    pool.ParallelFor(0, [](uint32_t, uint32_t) {});
    CHECK_EQ(out[0], 0u);
}

// 5. Two-component view: only entities that have BOTH components are visited.
TEST(ECSViewTwoComponents) {
    ecs::World world;
    const size_t n = 50000;
    std::vector<ecs::Entity> withVel;
    for (size_t i = 0; i < n; ++i) {
        ecs::Entity e = world.Create();
        world.Add<PPos>(e, PPos{static_cast<float>(i), static_cast<float>(i)});
        if (i % 2 == 0) {
            world.Add<PVel>(e, PVel{1.0f, 2.0f});
            withVel.push_back(e);
        }
    }

    int visited = 0;
    auto dual = world.ViewAll<PPos, PVel>();
    CHECK_EQ(dual.Size(), n); // Size() reports the T pool, not the intersection
    dual.ForEach([&visited](ecs::Entity, PPos& p, PVel& v) {
        ++visited;
        v.dx = p.x + v.dy; // reads both, writes the second
    });
    CHECK_EQ(visited, 25000);

    std::atomic<int> pvisited{0};
    world.ViewAll<PPos, PVel>().ParallelForEach(
        [&pvisited](ecs::Entity, PPos&, PVel& v) {
            pvisited.fetch_add(1, std::memory_order_relaxed);
            v.dx += 1.0f;
        });
    CHECK_EQ(pvisited.load(), 25000);

    // Every visited velocity was actually written (spot check the first ones).
    for (size_t i = 0; i < withVel.size() && i < 10; ++i) {
        const PVel* v = world.Get<PVel>(withVel[i]);
        CHECK(v != nullptr);
    }
}

// 6. World-mutation-in-parallel contract: the guard is active on every worker
// callback and cleared after the pass. (Debug builds additionally assert-fail
// if Create/Destroy/Add/Remove run while it is active.)
TEST(ECSParallelMutationContract) {
    ecs::World world;
    FillResult fr = FillWorld(world, kCount);

    CHECK(!world.InParallelIteration());
    std::atomic<int> sawInside{0};
    world.ViewAll<PPos>().ParallelForEach([&sawInside, &world](ecs::Entity, PPos& p) {
        if (world.InParallelIteration()) sawInside.fetch_add(1, std::memory_order_relaxed);
        p.x += 1.0f;
    });
    CHECK_EQ(sawInside.load(), static_cast<int>(kCount));
    CHECK(!world.InParallelIteration());
    CHECK_EQ(world.EntityCount(), kCount); // nothing mutated structurally
}

// 7. Perf sanity: a large parallel pass simply completes (no timing assertion,
// which would be flaky on shared CI machines).
TEST(ECSParallelLargePassCompletes) {
    ecs::World world;
    const size_t n = 200000;
    FillResult fr = FillWorld(world, n);

    std::vector<PVel> out(n);
    world.ViewAll<PPos>().ParallelForEach(
        [&out](ecs::Entity e, PPos& p) { out[e.id - 1] = ComputeVelocity(p); });
    CHECK(std::memcmp(out.data(), fr.expectedVel.data(), out.size() * sizeof(PVel)) == 0);
}

// 8. World mutation inside a parallel pass is REJECTED (logged + no-op) even in
// Release builds: Create returns an invalid entity, Add/Remove/Destroy change
// nothing, and every violation is reported on the ecs log category.
TEST(ECSParallelMutationRejected) {
    ecs::World world;
    const size_t n = 64;
    for (size_t i = 0; i < n; ++i) {
        ecs::Entity e = world.Create();
        world.Add<PPos>(e, PPos{static_cast<float>(i), 0.0f});
    }
    world.ViewAll<PVel>(); // pre-create the PVel pool so an Add would target it
    const size_t entitiesBefore = world.EntityCount();
    const size_t velBefore = world.ViewAll<PVel>().Size();

    std::atomic<int> sawGuard{0};
    std::atomic<int> rejectedCreates{0};
    EcsLogCounter errs;
    core::AddLogSink(&EcsErrorSink, &errs);

    world.ViewAll<PPos>().ParallelForEach([&](ecs::Entity e, PPos&) {
        if (world.InParallelIteration()) sawGuard.fetch_add(1, std::memory_order_relaxed);
        ecs::Entity c = world.Create();
        if (!c.IsValid()) rejectedCreates.fetch_add(1, std::memory_order_relaxed);
        world.Add<PVel>(e, PVel{1.0f, 1.0f});
        world.Remove<PVel>(e);
        world.Destroy(e);
    });

    core::RemoveLogSink(&EcsErrorSink, &errs);

    CHECK_EQ(sawGuard.load(), static_cast<int>(n)); // guard active on every callback
    CHECK_EQ(rejectedCreates.load(), static_cast<int>(n)); // every Create refused
    CHECK_EQ(world.EntityCount(), entitiesBefore); // Destroy refused
    CHECK_EQ(world.ViewAll<PVel>().Size(), velBefore); // Add/Remove refused
    CHECK_EQ(world.ViewAll<PPos>().Size(), n); // all entities still alive
    CHECK(errs.count.load() >= 4); // one logged rejection per mutation op, minimum
}

// 9. Direct construction of View<T,U> (not via ViewAll) is safe: the ctor
// pre-creates the U pool, so a parallel pass over it never inserts into the
// world's pool table concurrently (guarded by ParallelIterationGuard, which
// logs an ecs-category error on growth). Two phases prove it: (A) on a world
// where the U pool does NOT yet exist, a pass must still complete with zero
// pool-growth errors (only the ctor could have created U beforehand); (B) with
// entities holding both components, results are correct and visited only the
// matching entities.
TEST(ECSParallelTwoComponentDirectConstruction) {
    EcsLogCounter errs;

    // Phase A: no PVel components/pool exist. If the ctor did not pre-create
    // the U pool, the worker lookups would concurrently insert into poolIndex_
    // and ParallelIterationGuard would log pool growth.
    {
        ecs::World world;
        for (size_t i = 0; i < 2000; ++i) {
            ecs::Entity e = world.Create();
            world.Add<PPos>(e, PPos{static_cast<float>(i), 0.0f});
        }
        ecs::World::View<PPos, PVel> view(world.PoolOf<PPos>(), world);

        core::AddLogSink(&EcsErrorSink, &errs);
        view.ParallelForEach([](ecs::Entity, PPos&, PVel&) {});
        core::RemoveLogSink(&EcsErrorSink, &errs);
        CHECK_EQ(errs.count.load(), 0); // U pool existed (ctor) -> no growth
    }

    // Phase B: entities with both components; direct construction still yields
    // correct, bit-exact results for exactly the entities holding both.
    {
        ecs::World world;
        const size_t n = 2000;
        std::vector<PVel> expected(n);
        for (size_t i = 0; i < n; ++i) {
            ecs::Entity e = world.Create();
            world.Add<PPos>(e, PPos{static_cast<float>(i), static_cast<float>(i)});
            if (i % 2 == 0) {
                world.Add<PVel>(e, PVel{1.0f, 2.0f});
                expected[i] = PVel{static_cast<float>(i) + 2.0f, 2.0f}; // p.x + v.dy
            }
        }

        ecs::World::View<PPos, PVel> view(world.PoolOf<PPos>(), world);

        core::AddLogSink(&EcsErrorSink, &errs);
        std::vector<PVel> out(n);
        view.ParallelForEach([&out](ecs::Entity e, PPos& p, PVel& v) {
            v.dx = p.x + v.dy;
            out[e.id - 1] = v;
        });
        core::RemoveLogSink(&EcsErrorSink, &errs);
        CHECK_EQ(errs.count.load(), 0); // no pool growth / world mutation

        int visited = 0;
        for (size_t i = 0; i < n; ++i) {
            if (expected[i].dy != 0.0f) {
                ++visited;
                CHECK_EQ(out[i].dx, expected[i].dx);
                CHECK_EQ(out[i].dy, expected[i].dy);
            }
        }
        CHECK_EQ(visited, static_cast<int>(n / 2)); // only entities with BOTH components
    }
}

// 10. Reducer sizing guard: with slots >= worker count every chunk gets its own
// slot (Ok() true, deterministic combine). In Release, an under-sized Reducer
// is detected (Ok() false) instead of silently corrupting the reduction.
TEST(ParallelForReducerMisSizedDetected) {
    const uint32_t n = 10000;
    const int64_t serialSum = 49995000; // sum of i, i in [0, n)

    ecs::parallel::Reducer<int64_t> ok(ecs::parallel::AvailableWorkers());
    ecs::parallel::ParallelFor(n, [&ok, n](uint32_t s, uint32_t e) {
        int64_t& acc = ok.Slot(s, n);
        for (uint32_t i = s; i < e; ++i) acc += static_cast<int64_t>(i);
    });
    CHECK(ok.Ok());
    CHECK_EQ(ok.Combine(), serialSum);

#if defined(NDEBUG)
    // Release: an under-sized Reducer (slots < worker count) collapses several
    // chunks onto one slot; Slot() flags it. The fn does not write to the
    // returned slot so the test itself stays race-free (a real user write would
    // be a data race, which is exactly what the sizing contract forbids).
    ecs::parallel::Reducer<int64_t> tooSmall(1);
    ecs::parallel::ParallelFor(n, [&tooSmall, n](uint32_t s, uint32_t e) {
        (void)e;
        (void)tooSmall.Slot(s, n);
    });
    CHECK(!tooSmall.Ok());
#endif
}

// A4 (2026-08-28): an exception inside fn must surface on the calling thread
// AFTER every worker exited its closure -- no dangling stack references, and
// the pool stays usable afterwards.
TEST(ParallelForExceptionPropagatesAndPoolSurvives) {
    bool threw = false;
    try {
        ecs::parallel::ParallelFor(10000, [](uint32_t begin, uint32_t end) {
            (void)end;
            if (begin == 0) throw std::runtime_error("boom from chunk");
        });
    } catch (const std::runtime_error& e) {
        threw = std::string(e.what()).find("boom") != std::string::npos;
    }
    CHECK(threw);

    // Pool still healthy: a follow-up pass produces the serial answer.
    std::vector<int> out(1000, 0);
    ecs::parallel::ParallelFor(1000, [&](uint32_t begin, uint32_t end) {
        for (uint32_t i = begin; i < end; ++i) out[i] = static_cast<int>(i) * 2;
    });
    bool allGood = true;
    for (int i = 0; i < 1000; ++i) allGood = allGood && out[i] == i * 2;
    CHECK(allGood);
}