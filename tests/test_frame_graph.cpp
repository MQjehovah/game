#include <string>
#include <vector>

#include "neon/gfx/bloom_graph.hpp"
#include "neon/gfx/frame_graph.hpp"
#include "neon/gfx/post_graph.hpp"
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

// Records the samples argument passed to CreateRenderTarget so the descriptor
// -> backend samples translation can be asserted.
class SamplesRecordingBackend : public test::NullBackend {
public:
    std::vector<int> sampleArgs;
    int createCount = 0;

    gfx::RenderTargetHandle CreateRenderTarget(int, int, bool, int samples) override {
        sampleArgs.push_back(samples);
        ++createCount;
        return {static_cast<uint32_t>(createCount)};
    }
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

// Ping-pong: A writes r1 -> B reads r1 writes r2 -> C reads r2 writes r1 -> D
// reads r1. Each read binds to the version produced by the most recent PRIOR
// writer, so a later overwrite (C writing r1, which A also wrote) creates no
// edge: the chain must sort to A,B,C,D instead of deadlocking on a "cycle"
// between B (reads r1) and C (writes r1).
TEST(FrameGraphPingPongVersionedReads) {
    CountingNullBackend backend;
    gfx::FrameGraph graph;

    const gfx::ResourceId r1 = graph.AddResource({16, 16, 0, 1});
    const gfx::ResourceId r2 = graph.AddResource({16, 16, 0, 1});

    std::vector<std::string> order;
    gfx::RenderTargetHandle aOut, bIn, bOut, cIn, cOut, dIn;

    gfx::FramePass a;
    a.name = "A";
    a.writes = {r1};
    a.execute = [&](gfx::FrameGraphContext& ctx) { order.push_back("A"); aOut = ctx.GetOutput(r1); };
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
    c.writes = {r1}; // overwrites r1 (the ping-pong write-back)
    c.execute = [&](gfx::FrameGraphContext& ctx) {
        order.push_back("C");
        cIn = ctx.GetInput(r2);
        cOut = ctx.GetOutput(r1);
    };
    CHECK(graph.AddPass(std::move(c)));

    gfx::FramePass d;
    d.name = "D";
    d.reads = {r1};
    d.execute = [&](gfx::FrameGraphContext& ctx) {
        order.push_back("D");
        dIn = ctx.GetInput(r1);
    };
    CHECK(graph.AddPass(std::move(d)));

    // The ping-pong chain must sort instead of reporting a cycle.
    CHECK(graph.Execute(backend));
    graph.ResetFrame();

    CHECK_EQ(order.size(), 4u);
    CHECK_EQ(order[0], std::string("A"));
    CHECK_EQ(order[1], std::string("B"));
    CHECK_EQ(order[2], std::string("C"));
    CHECK_EQ(order[3], std::string("D"));

    // Each reader binds to its immediate producer's target: B sees A's r1, C
    // sees B's r2, and D sees C's r1 (the second write, not A's).
    CHECK(aOut.Valid());
    CHECK_EQ(bIn.id, aOut.id);
    CHECK_EQ(cIn.id, bOut.id);
    CHECK_EQ(dIn.id, cOut.id);
    // Only two distinct allocations exist: the transient pool recycles r1's
    // target for C's overwrite (same descriptor).
    CHECK_EQ(backend.createCount, 2);
    CHECK_EQ(backend.destroyCount, 0);
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

// The full bloom chain as a FrameGraph: 7 passes (bright -> blur h/v ->
// downsample -> blur h/v -> upsample-add) must execute in EXACTLY the order of
// the hand-written chain, with each pass wired to its immediate producer's
// target (the ping-pong versions) and the scene HDR injected as the bright
// pass's external input. The accumulated bloom is exported for the composite.
TEST(BloomGraphPassOrderAndWiring) {
    CountingNullBackend backend;
    gfx::BloomGraph bloom;

    bloom.Build(gfx::ShaderHandle{1}, gfx::ShaderHandle{2}, gfx::ShaderHandle{3},
                gfx::ShaderHandle{4}, gfx::MeshHandle{1, 1, 1, 6}, 640, 360);
    CHECK_EQ(bloom.PassCount(), 7u);

    const gfx::RenderTargetHandle hdrScene{100};
    CHECK(bloom.Execute(backend, hdrScene, 640, 360, true));
    CHECK(bloom.Ran());

    const auto& trace = bloom.LastTrace();
    CHECK_EQ(trace.size(), 7u);
    const char* expected[] = {"bloom.bright",      "bloom.blurHalfH",  "bloom.blurHalfV",
                              "bloom.downsample",  "bloom.blurQuarterH", "bloom.blurQuarterV",
                              "bloom.upsampleAdd"};
    for (size_t i = 0; i < 7; ++i) CHECK_EQ(trace[i].name, std::string(expected[i]));

    // External input injection: the bright pass samples the caller's HDR RT.
    CHECK_EQ(trace[0].inputs.size(), 1u);
    CHECK_EQ(trace[0].inputs[0].target.id, hdrScene.id);

    // Ping-pong wiring: each reader binds to its immediate producer's output.
    CHECK_EQ(trace[1].inputs[0].target.id, trace[0].outputs[0].target.id); // blurH <- bright
    CHECK_EQ(trace[2].inputs[0].target.id, trace[1].outputs[0].target.id); // blurV <- blurH
    CHECK_EQ(trace[3].inputs[0].target.id, trace[2].outputs[0].target.id); // downsample <- blurV
    CHECK_EQ(trace[4].inputs[0].target.id, trace[3].outputs[0].target.id); // blurQH <- downsample
    CHECK_EQ(trace[5].inputs[0].target.id, trace[4].outputs[0].target.id); // blurQV <- blurQH
    // Upsample-add reads halfA (blurV's output) and quarterA (blurQV's output).
    CHECK_EQ(trace[6].inputs.size(), 2u);
    CHECK_EQ(trace[6].inputs[0].target.id, trace[2].outputs[0].target.id);
    CHECK_EQ(trace[6].inputs[1].target.id, trace[5].outputs[0].target.id);

    // The chain allocates exactly 4 transient targets (halfA/B + quarterA/B),
    // the same count as the renderer's hand-managed bloom RTs; all are pooled.
    CHECK_EQ(backend.createCount, 4);
    CHECK_EQ(backend.destroyCount, 0);

    // The upsample-add output is exported: the composite can sample it right
    // after Execute, and it is gone once the frame's pool is reset.
    CHECK(bloom.BloomColorTexture(backend).Valid());
    bloom.ResetFrame();
    CHECK(!bloom.BloomColorTexture(backend).Valid());
}

// enabled=false must skip the whole chain (bloom off), and a resize-style
// rebuild (Build -> Destroy -> Build) must release the old graph's GPU targets
// so the transient pool does not leak across resolutions.
TEST(BloomGraphDisabledAndRebuild) {
    CountingNullBackend backend;
    gfx::BloomGraph bloom;
    bloom.Build(gfx::ShaderHandle{1}, gfx::ShaderHandle{2}, gfx::ShaderHandle{3},
                gfx::ShaderHandle{4}, gfx::MeshHandle{1, 1, 1, 6}, 640, 360);

    const gfx::RenderTargetHandle hdrScene{100};
    CHECK(!bloom.Execute(backend, hdrScene, 640, 360, false)); // bloom off
    CHECK(!bloom.Ran());
    CHECK_EQ(backend.createCount, 0); // nothing ran, nothing allocated

    // Two pooled passes: exactly the 4 pyramid targets, released on Destroy.
    CHECK(bloom.Execute(backend, hdrScene, 640, 360, true));
    CHECK(bloom.Ran());
    const int created = backend.createCount;
    CHECK_EQ(created, 4);
    bloom.Destroy(backend);
    CHECK_EQ(backend.destroyCount, created);

    // Rebuild at a new resolution: fresh allocations, no reuse of old targets.
    bloom.Build(gfx::ShaderHandle{1}, gfx::ShaderHandle{2}, gfx::ShaderHandle{3},
                gfx::ShaderHandle{4}, gfx::MeshHandle{1, 1, 1, 6}, 1280, 720);
    CHECK(bloom.Execute(backend, hdrScene, 1280, 720, true));
    CHECK_EQ(backend.createCount, created + 4);
    CHECK_EQ(backend.destroyCount, created);
    bloom.ResetFrame();
}

// FrameGraphResourceDesc uses the convention "samples > 1 = multisampled,
// 1 = single-sample", while IRenderBackend::CreateRenderTarget uses "samples >
// 0 = multisampled, 0 = single-sample". AcquireTarget must translate: a
// single-sample (1) target must NOT reach the backend as a 1x MSAA target
// (which has no sampleable colour texture and would break every post pass).
TEST(FrameGraphSamplesConventionTranslation) {
    SamplesRecordingBackend backend;
    gfx::FrameGraph graph;

    const gfx::ResourceId single = graph.AddResource({64, 64, 1, 1}); // single-sample
    const gfx::ResourceId msaa = graph.AddResource({64, 64, 1, 4});   // 4x MSAA

    gfx::FramePass a;
    a.name = "single";
    a.writes = {single};
    a.execute = [](gfx::FrameGraphContext&) {};
    CHECK(graph.AddPass(std::move(a)));

    gfx::FramePass b;
    b.name = "msaa";
    b.writes = {msaa};
    b.execute = [](gfx::FrameGraphContext&) {};
    CHECK(graph.AddPass(std::move(b)));

    CHECK(graph.Execute(backend));
    graph.ResetFrame();

    // single (desc 1) -> backend 0 (sampleable colour); msaa (desc 4) -> 4.
    CHECK_EQ(backend.sampleArgs.size(), 2u);
    CHECK_EQ(backend.sampleArgs[0], 0);
    CHECK_EQ(backend.sampleArgs[1], 4);
}

namespace {

// Builds a fully-enabled PostGraph with all four chains (depth + ssao + vol +
// ssr) at 640x360 and a depth-caster callback that counts invocations.
gfx::PostGraph BuildFullPostGraph(CountingNullBackend& backend, int& depthCasterCalls) {
    gfx::PostGraph post;
    post.Build(gfx::ShaderHandle{1}, gfx::ShaderHandle{2}, gfx::ShaderHandle{3},
               gfx::ShaderHandle{4}, gfx::MeshHandle{1, 1, 1, 6}, 640, 360,
               [&] { ++depthCasterCalls; });
    CHECK_EQ(post.PassCount(), 10u);
    return post;
}

gfx::PostGraph::FrameParams PostFrameParams(int w, int h, bool depth, bool ssao, bool vol,
                                            bool ssr) {
    gfx::PostGraph::FrameParams p;
    p.hdrScene = {100};
    p.hdrW = w;
    p.hdrH = h;
    p.depthPass = depth;
    p.ssaoPass = ssao;
    p.volumetricPass = vol;
    p.ssrPass = ssr;
    p.camPos = {0.0f, 3.0f, 10.0f};
    p.sunDir = {-0.4f, -1.0f, -0.3f};
    p.viewProj = neon::math::Mat4::Identity();
    p.camera = {};
    return p;
}

} // namespace

// The full post chain (depth -> ssao -> blur -> volumetric -> blur -> ssr ->
// blur) as a FrameGraph: 10 passes must execute in EXACTLY the order of the
// hand-written chain, with each reader wired to its immediate producer's target
// (the ping-pong versions), the scene HDR injected as the external input of the
// volumetric/ssr passes, and the depth pre-pass drawing the casters through the
// injected callback instead of a fullscreen quad.
TEST(PostGraphPassOrderAndWiring) {
    CountingNullBackend backend;
    int depthCasterCalls = 0;
    gfx::PostGraph post = BuildFullPostGraph(backend, depthCasterCalls);

    CHECK(post.Execute(backend, PostFrameParams(640, 360, true, true, true, true)));
    CHECK(post.Ran());
    CHECK_EQ(depthCasterCalls, 1); // the depth pass drew the casters exactly once

    const auto& trace = post.LastTrace();
    CHECK_EQ(trace.size(), 10u);
    const char* expected[] = {"post.depth",     "post.ssao",     "post.ssaoBlurH",
                              "post.ssaoBlurV", "post.volumetric", "post.volBlurH",
                              "post.volBlurV",  "post.ssr",      "post.ssrBlurH",
                              "post.ssrBlurV"};
    for (size_t i = 0; i < 10; ++i) CHECK_EQ(trace[i].name, std::string(expected[i]));

    // The depth pass has NO inputs (casters are drawn directly) and writes the
    // scene depth; ssao/ssr bind that same target as their depth input.
    CHECK_EQ(trace[0].inputs.size(), 0u);
    CHECK_EQ(trace[0].outputs.size(), 1u);
    CHECK_EQ(trace[1].inputs[0].target.id, trace[0].outputs[0].target.id); // ssao <- depth
    CHECK_EQ(trace[7].inputs[1].target.id, trace[0].outputs[0].target.id); // ssr  <- depth

    // AO ping-pong: ao -> aoBlurA -> aoBlurB.
    CHECK_EQ(trace[1].outputs[0].target.id, trace[2].inputs[0].target.id); // blurH <- ao
    CHECK_EQ(trace[2].outputs[0].target.id, trace[3].inputs[0].target.id); // blurV <- aoBlurA

    // Volumetric reads the external HDR, blurs vol -> volBlurA -> volBlurB.
    CHECK_EQ(trace[4].inputs[0].target.id, 100u); // external hdrScene
    CHECK_EQ(trace[4].outputs[0].target.id, trace[5].inputs[0].target.id);
    CHECK_EQ(trace[5].outputs[0].target.id, trace[6].inputs[0].target.id);

    // SSR reads BOTH the external HDR and the scene depth, blurs ssr ->
    // ssrBlurA -> ssrBlurB.
    CHECK_EQ(trace[7].inputs.size(), 2u);
    CHECK_EQ(trace[7].inputs[0].target.id, 100u); // external hdrScene
    CHECK_EQ(trace[7].outputs[0].target.id, trace[8].inputs[0].target.id);
    CHECK_EQ(trace[8].outputs[0].target.id, trace[9].inputs[0].target.id);

    // The exported finals are sampleable right after Execute: scene depth, the
    // raw AO, and the blurred volumetric / SSR results (what composite reads).
    CHECK(post.SceneDepthTexture(backend).Valid());
    CHECK(post.AoTex(backend).Valid());
    CHECK(post.VolTex(backend).Valid());
    CHECK(post.SsrTex(backend).Valid());

    // ResetFrame returns the exports to the pool; composite sampled them already.
    post.ResetFrame();
    CHECK(!post.SceneDepthTexture(backend).Valid());
    CHECK(!post.AoTex(backend).Valid());
    CHECK(!post.VolTex(backend).Valid());
    CHECK(!post.SsrTex(backend).Valid());
}

// Disabled chains must neither run nor produce: with only the ssao chain on,
// just 4 passes execute (depth + ao + blurH + blurV), the depth-caster callback
// still runs once, and the volumetric/ssr exports are absent while the ao
// export stays live for the composite.
TEST(PostGraphChainDisabling) {
    CountingNullBackend backend;
    int depthCasterCalls = 0;
    gfx::PostGraph post = BuildFullPostGraph(backend, depthCasterCalls);

    // Only the SSAO chain requested (renderer: ssaoEnabled && casters present).
    CHECK(post.Execute(backend, PostFrameParams(640, 360, true, true, false, false)));
    CHECK(post.Ran());
    CHECK(post.DepthRan());
    CHECK(post.SsaoRan());
    CHECK(!post.VolumetricRan());
    CHECK(!post.SsrRan());
    CHECK_EQ(depthCasterCalls, 1);

    const auto& trace = post.LastTrace();
    CHECK_EQ(trace.size(), 4u);
    CHECK_EQ(trace[0].name, std::string("post.depth"));
    CHECK_EQ(trace[1].name, std::string("post.ssao"));
    CHECK_EQ(trace[2].name, std::string("post.ssaoBlurH"));
    CHECK_EQ(trace[3].name, std::string("post.ssaoBlurV"));

    CHECK(post.SceneDepthTexture(backend).Valid());
    CHECK(post.AoTex(backend).Valid());
    CHECK(!post.VolTex(backend).Valid());
    CHECK(!post.SsrTex(backend).Valid());
    post.ResetFrame();

    // Next frame: only SSR runs; depth still needed for the depth read.
    CHECK(post.Execute(backend, PostFrameParams(640, 360, true, false, false, true)));
    CHECK(post.DepthRan());
    CHECK(!post.SsaoRan());
    CHECK(!post.VolumetricRan());
    CHECK(post.SsrRan());
    CHECK_EQ(post.LastTrace().size(), 4u); // depth + ssr + ssrBlurH + ssrBlurV
    CHECK(post.SceneDepthTexture(backend).Valid());
    CHECK(!post.AoTex(backend).Valid());
    CHECK(post.SsrTex(backend).Valid());
    post.ResetFrame();
}

// depthPass=false but a chain that needs the depth is on: the graph must still
// run the depth pre-pass (the depth is only needed when ssao/ssr sample it).
TEST(PostGraphDepthDrivenByConsumers) {
    CountingNullBackend backend;
    int depthCasterCalls = 0;
    gfx::PostGraph post = BuildFullPostGraph(backend, depthCasterCalls);

    // ssr on, depthPass flag missed (defensive path): depth still executes.
    CHECK(post.Execute(backend, PostFrameParams(640, 360, false, false, false, true)));
    CHECK(post.DepthRan());
    CHECK(post.SsrRan());
    CHECK_EQ(depthCasterCalls, 1);
    CHECK_EQ(post.LastTrace().size(), 4u); // depth + ssr chain
    post.ResetFrame();
}

// Everything disabled: the graph runs nothing (no casters drawn, no targets
// allocated) and every chain reports off.
TEST(PostGraphAllDisabled) {
    CountingNullBackend backend;
    int depthCasterCalls = 0;
    gfx::PostGraph post = BuildFullPostGraph(backend, depthCasterCalls);

    CHECK(post.Execute(backend, PostFrameParams(640, 360, false, false, false, false)));
    CHECK(!post.Ran());
    CHECK(!post.DepthRan());
    CHECK(!post.SsaoRan());
    CHECK(!post.VolumetricRan());
    CHECK(!post.SsrRan());
    CHECK_EQ(depthCasterCalls, 0);
    CHECK_EQ(post.LastTrace().size(), 0u);
    CHECK_EQ(backend.createCount, 0); // nothing ran, nothing allocated
    post.ResetFrame();
}

// The full chain allocates a handful of pooled transient targets (the AO/blur/
// vol/ssr descriptors are all identical half-res float, so they share one pool
// bucket and get recycled aggressively), and a rebuild releases every target.
TEST(PostGraphTransientPoolAndRebuild) {
    CountingNullBackend backend;
    int depthCasterCalls = 0;
    gfx::PostGraph post = BuildFullPostGraph(backend, depthCasterCalls);

    CHECK(post.Execute(backend, PostFrameParams(640, 360, true, true, true, true)));
    post.ResetFrame();
    const int created = backend.createCount;
    CHECK_EQ(created, 5); // 1 full-res depth + 4 half-res float (pool-recycled)

    // A second frame reuses the same pooled targets: no new allocations.
    CHECK(post.Execute(backend, PostFrameParams(640, 360, true, true, true, true)));
    post.ResetFrame();
    CHECK_EQ(backend.createCount, created);
    CHECK_EQ(backend.destroyCount, 0);

    // Rebuild at a new resolution: Destroy releases everything first.
    post.Destroy(backend);
    CHECK_EQ(backend.destroyCount, created);
}
