#include <string>
#include <vector>

#include "neon/gfx/frame_graph.hpp"
#include "helpers.hpp"
#include "test_backend.hpp"

using namespace neon;

namespace {

// NullBackend's CreateRenderTarget always returns the same handle, so it cannot
// tell pooled reuse from fresh allocation. This subclass hands out distinct ids
// and counts the create/destroy calls so the pool tests can assert reuse.
class CountingNullBackend : public test::NullBackend {
public:
    int createCount = 0;
    int destroyCount = 0;

    gfx::RenderTargetHandle CreateRenderTarget(int, int, bool, int) override {
        ++createCount;
        return {static_cast<uint32_t>(createCount)};
    }
    void DestroyRenderTarget(gfx::RenderTargetHandle) override { ++destroyCount; }
};

} // namespace

// A writes r1 -> B reads r1 writes r2 -> C reads r2. Execute() must run them in
// dependency order and chain the transient targets: B's input is A's output.
TEST(FrameGraphTopologicalOrder) {
    CountingNullBackend backend;
    gfx::FrameGraph graph;

    const gfx::ResourceId r1 = graph.AddResource({320, 240, 0, 1});
    const gfx::ResourceId r2 = graph.AddResource({320, 240, 0, 1});

    std::vector<std::string> order;
    gfx::RenderTargetHandle aOut, bIn, bOut, cIn;

    gfx::FramePass a;
    a.name = "A";
    a.writes = {r1};
    a.execute = [&](gfx::FrameGraphContext& ctx) {
        order.push_back("A");
        aOut = ctx.GetOutput(r1);
    };
    CHECK(graph.AddPass(std::move(a)));

    gfx::FramePass b;
    b.name = "B";
    b.reads = {r1};
    b.writes = {r2};
    b.execute = [&](gfx::FrameGraphContext& ctx) {
        order.push_back("B");
        bIn = ctx.GetInput(r1);
        bOut = ctx.GetOutput(r2);
    };
    CHECK(graph.AddPass(std::move(b)));

    gfx::FramePass c;
    c.name = "C";
    c.reads = {r2};
    c.execute = [&](gfx::FrameGraphContext& ctx) {
        order.push_back("C");
        cIn = ctx.GetInput(r2);
    };
    CHECK(graph.AddPass(std::move(c)));

    CHECK_EQ(graph.PassCount(), 3u);
    CHECK_EQ(graph.ResourceCount(), 2u);

    CHECK(graph.Execute(backend));
    graph.ResetFrame();

    CHECK_EQ(order.size(), 3u);
    CHECK_EQ(order[0], std::string("A"));
    CHECK_EQ(order[1], std::string("B"));
    CHECK_EQ(order[2], std::string("C"));

    // The resource chain flows through the context: B sees A's r1 target and C
    // sees B's r2 target. Each target is a distinct allocation.
    CHECK(aOut.Valid());
    CHECK_EQ(aOut.id, bIn.id);
    CHECK_EQ(bOut.id, cIn.id);
    CHECK_EQ(backend.createCount, 2);
}

// A reads r2 writes r1, B reads r1 writes r2: a mutual dependency, so the sort
// must fail instead of deadlocking.
TEST(FrameGraphCycleDetected) {
    test::NullBackend backend;
    gfx::FrameGraph graph;

    const gfx::ResourceId r1 = graph.AddResource({16, 16, 0, 1});
    const gfx::ResourceId r2 = graph.AddResource({16, 16, 0, 1});

    gfx::FramePass a;
    a.name = "A";
    a.reads = {r2};
    a.writes = {r1};
    a.execute = [](gfx::FrameGraphContext&) {};
    CHECK(graph.AddPass(std::move(a)));

    gfx::FramePass b;
    b.name = "B";
    b.reads = {r1};
    b.writes = {r2};
    b.execute = [](gfx::FrameGraphContext&) {};
    CHECK(graph.AddPass(std::move(b)));

    // Kahn cannot make progress -> Execute reports the cycle.
    CHECK(!graph.Execute(backend));
    graph.ResetFrame();
}

// Two passes with same-sized targets across two frames: the transient pool must
// reuse the single allocation, never creating a second GPU target.
TEST(FrameGraphTransientPoolReuse) {
    CountingNullBackend backend;
    gfx::FrameGraph graph;

    const gfx::ResourceId r1 = graph.AddResource({640, 480, 0, 1});
    const gfx::ResourceId r2 = graph.AddResource({640, 480, 0, 1});

    std::vector<gfx::RenderTargetHandle> aHandles, bHandles;

    gfx::FramePass a;
    a.name = "A";
    a.writes = {r1};
    a.execute = [&](gfx::FrameGraphContext& ctx) { aHandles.push_back(ctx.GetOutput(r1)); };
    CHECK(graph.AddPass(std::move(a)));

    gfx::FramePass b;
    b.name = "B";
    b.writes = {r2};
    b.execute = [&](gfx::FrameGraphContext& ctx) { bHandles.push_back(ctx.GetOutput(r2)); };
    CHECK(graph.AddPass(std::move(b)));

    for (int frame = 0; frame < 2; ++frame) {
        CHECK(graph.Execute(backend));
        graph.ResetFrame();
    }

    CHECK_EQ(aHandles.size(), 2u);
    CHECK_EQ(bHandles.size(), 2u);
    // Exactly one target was ever created; every pass in every frame received
    // that same recycled allocation.
    CHECK_EQ(backend.createCount, 1);
    CHECK_EQ(backend.destroyCount, 0);
    CHECK(aHandles[0].Valid());
    CHECK_EQ(aHandles[0].id, aHandles[1].id);
    CHECK_EQ(aHandles[0].id, bHandles[0].id);
    CHECK_EQ(bHandles[0].id, bHandles[1].id);
}

// A pass referencing an undeclared resource id must be rejected without
// corrupting the graph (later passes still build and execute).
TEST(FrameGraphUnknownResourceRejected) {
    test::NullBackend backend;
    gfx::FrameGraph graph;

    const gfx::ResourceId ok = graph.AddResource({4, 4, 0, 1});

    gfx::FramePass badRead;
    badRead.name = "bad-read";
    badRead.reads = {ok + 1};
    CHECK(!graph.AddPass(std::move(badRead)));

    gfx::FramePass badWrite;
    badWrite.name = "bad-write";
    badWrite.writes = {999};
    CHECK(!graph.AddPass(std::move(badWrite)));

    gfx::FramePass good;
    good.name = "good";
    good.writes = {ok};
    good.execute = [&](gfx::FrameGraphContext& ctx) { CHECK(ctx.GetOutput(ok).Valid()); };
    CHECK(graph.AddPass(std::move(good)));

    CHECK_EQ(graph.PassCount(), 1u); // the two rejected passes left no trace
    CHECK(graph.Execute(backend));
    graph.ResetFrame();
}
