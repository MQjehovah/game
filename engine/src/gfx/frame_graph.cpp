#include "neon/gfx/frame_graph.hpp"

#include <utility>

namespace neon::gfx {

namespace {
// Cap on pooled targets per descriptor: reusing a handful of transient buffers
// is the goal; beyond that it is cheaper to destroy and re-create on demand.
constexpr size_t kMaxPoolPerKey = 8;
} // namespace

ResourceId FrameGraph::AddResource(const FrameGraphResourceDesc& desc) {
    const ResourceId id = static_cast<ResourceId>(resources_.size());
    resources_.push_back(desc);
    exported_.push_back(false);
    return id;
}

bool FrameGraph::AddPass(FramePass pass) {
    for (ResourceId r : pass.reads)
        if (r >= resources_.size()) return false;
    for (ResourceId r : pass.writes)
        if (r >= resources_.size()) return false;

    passes_.push_back(std::move(pass));
    return true;
}

void FrameGraph::SetPassEnabled(size_t index, bool enabled) {
    if (index < passes_.size()) passes_[index].enabled = enabled;
}

void FrameGraph::SetExternalInput(ResourceId id, RenderTargetHandle rt) {
    if (id >= resources_.size()) return;
    if (rt.Valid())
        externalInputs_[id] = rt;
    else
        externalInputs_.erase(id);
}

void FrameGraph::ExportResource(ResourceId id) {
    if (id < exported_.size()) exported_[id] = true;
}

RenderTargetHandle FrameGraph::GetResourceTarget(ResourceId id) const {
    const auto it = liveRTs_.find(id);
    return it == liveRTs_.end() ? RenderTargetHandle{} : it->second;
}

bool FrameGraph::Execute(IRenderBackend& backend) {
    const size_t n = passes_.size();
    trace_.clear();

    // Disabled passes neither produce nor consume; only enabled ones take part.
    std::vector<size_t> enabled;
    enabled.reserve(n);
    for (size_t i = 0; i < n; ++i)
        if (passes_[i].enabled) enabled.push_back(i);
    if (enabled.empty()) return true;

    // Map original pass index -> position in the enabled list (Kahn).
    std::unordered_map<size_t, size_t> pos;
    pos.reserve(enabled.size());
    for (size_t p = 0; p < enabled.size(); ++p) pos[enabled[p]] = p;

    // Per-frame accounting + dependency edges, accumulated IN DECLARATION
    // ORDER. A read of r binds to the version produced by the most recent PRIOR
    // writer of r (lastWriter as of this pass). A writer declared AFTER this
    // pass is a later overwrite (ping-pong), not this read's producer, so it
    // creates no edge: edges always run forward in declaration order, and a
    // pipeline declared in dataflow order sorts immediately. lastUse[r] is the
    // last pass (in declaration order) to read or write r and drives release.
    std::vector<int> lastWriter(resources_.size(), -1);
    std::vector<int> lastUse(resources_.size(), -1);
    std::vector<size_t> incoming(enabled.size(), 0);
    std::vector<std::vector<size_t>> dependents(enabled.size());
    for (size_t p = 0; p < enabled.size(); ++p) {
        const size_t orig = enabled[p];
        for (ResourceId r : passes_[orig].reads) {
            lastUse[r] = static_cast<int>(orig);
            const int writer = lastWriter[r];
            if (writer < 0) continue; // external / produced after this pass
            const auto it = pos.find(static_cast<size_t>(writer));
            if (it == pos.end()) continue; // writer disabled (impossible)
            const size_t dep = it->second;
            if (dep == p) continue;
            incoming[p] += 1;
            dependents[dep].push_back(p);
        }
        for (ResourceId r : passes_[orig].writes) {
            lastWriter[r] = static_cast<int>(orig);
            lastUse[r] = static_cast<int>(orig);
        }
    }

    // Kahn's algorithm, same repeated-scan shape as ModuleRegistry::InitAll
    // (pass counts are small, so no priority queue is needed).
    std::vector<size_t> order;
    order.reserve(enabled.size());
    std::vector<bool> done(enabled.size(), false);
    while (order.size() < enabled.size()) {
        bool progressed = false;
        for (size_t p = 0; p < enabled.size(); ++p) {
            if (done[p] || incoming[p] != 0) continue;
            done[p] = true;
            order.push_back(p);
            progressed = true;
            for (size_t d : dependents[p]) incoming[d] -= 1;
        }
        if (!progressed) return false; // cycle (unreachable with forward edges)
    }

    // Run the passes in dependency order.
    for (size_t p : order) {
        const size_t orig = enabled[p];
        FramePass& pass = passes_[orig];

        // Allocate outputs first so a pass can also read its own write.
        for (ResourceId r : pass.writes) {
            const auto prev = liveRTs_.find(r);
            if (prev != liveRTs_.end()) {
                // Overwriting a target still live this frame: its old contents
                // are dead, so hand the previous allocation back to the pool.
                ReleaseTarget(backend, r, prev->second);
            }
            liveRTs_[r] = AcquireTarget(backend, resources_[r]);
        }

        FrameGraphContext ctx(backend);
        for (ResourceId r : pass.reads) {
            // Reads with no in-graph producer are external: the caller wires
            // them up with SetExternalInput before Execute.
            const auto ext = externalInputs_.find(r);
            if (ext != externalInputs_.end()) {
                ctx.SetInput(r, ext->second);
                continue;
            }
            const auto it = liveRTs_.find(r);
            if (it != liveRTs_.end()) ctx.SetInput(r, it->second);
        }
        for (ResourceId r : pass.writes) {
            const auto it = liveRTs_.find(r);
            if (it != liveRTs_.end()) ctx.SetOutput(r, it->second);
        }

        if (pass.execute) pass.execute(ctx);

        trace_.push_back(FrameGraphPassTrace{pass.name, {}, {}});
        FrameGraphPassTrace& tr = trace_.back();
        for (ResourceId r : pass.reads) {
            RenderTargetHandle rt = ctx.GetInput(r);
            if (rt.Valid()) tr.inputs.push_back({r, rt});
        }
        for (ResourceId r : pass.writes) {
            RenderTargetHandle rt = ctx.GetOutput(r);
            if (rt.Valid()) tr.outputs.push_back({r, rt});
        }

        // A target whose last reader/writer is this pass can go back to the
        // pool immediately (reused by a later pass with the same descriptor) -
        // unless exported, where the caller keeps sampling it until ResetFrame.
        for (ResourceId r : pass.writes) {
            if (lastUse[r] != static_cast<int>(orig)) continue;
            if (r < exported_.size() && exported_[r]) continue;
            const auto it = liveRTs_.find(r);
            if (it == liveRTs_.end()) continue;
            ReleaseTarget(backend, r, it->second);
            liveRTs_.erase(it);
        }
        for (ResourceId r : pass.reads) {
            if (lastUse[r] != static_cast<int>(orig)) continue;
            if (r < exported_.size() && exported_[r]) continue;
            const auto it = liveRTs_.find(r);
            if (it == liveRTs_.end()) continue;
            ReleaseTarget(backend, r, it->second);
            liveRTs_.erase(it);
        }
    }
    return true;
}

void FrameGraph::ResetFrame() {
    // No backend is stored, so a target only ever lives between Execute()
    // (which allocates) and ResetFrame() (which returns it). Callers must run
    // ResetFrame() every frame and before destroying the graph.
    for (auto& kv : liveRTs_) {
        const PoolKey key{resources_[kv.first].width, resources_[kv.first].height,
                          resources_[kv.first].format, resources_[kv.first].samples};
        std::vector<RenderTargetHandle>& bucket = pool_[key];
        if (bucket.size() < kMaxPoolPerKey) bucket.push_back(kv.second);
    }
    liveRTs_.clear();
}

void FrameGraph::DestroyResources(IRenderBackend& backend) {
    for (auto& kv : liveRTs_) backend.DestroyRenderTarget(kv.second);
    liveRTs_.clear();
    for (auto& kv : pool_) {
        for (RenderTargetHandle rt : kv.second) backend.DestroyRenderTarget(rt);
        kv.second.clear();
    }
    pool_.clear();
}

RenderTargetHandle FrameGraph::AcquireTarget(IRenderBackend& backend,
                                             const FrameGraphResourceDesc& desc) {
    const PoolKey key{desc.width, desc.height, desc.format, desc.samples};
    auto it = pool_.find(key);
    if (it != pool_.end() && !it->second.empty()) {
        RenderTargetHandle rt = it->second.back();
        it->second.pop_back();
        return rt;
    }
    // Translate the descriptor's convention (samples > 1 = multisampled, 1 =
    // single-sample) into the backend's (samples > 0 = multisampled, 0 =
    // single-sample): a 1x "MSAA" target has no sampleable colour texture.
    const int samples = desc.samples > 1 ? static_cast<int>(desc.samples) : 0;
    return backend.CreateRenderTarget(static_cast<int>(desc.width),
                                      static_cast<int>(desc.height),
                                      desc.format != 0, samples);
}

void FrameGraph::ReleaseTarget(IRenderBackend& backend, ResourceId resource,
                               RenderTargetHandle target) {
    const FrameGraphResourceDesc& desc = resources_[resource];
    const PoolKey key{desc.width, desc.height, desc.format, desc.samples};
    std::vector<RenderTargetHandle>& bucket = pool_[key];
    if (bucket.size() < kMaxPoolPerKey) {
        bucket.push_back(target);
    } else {
        backend.DestroyRenderTarget(target);
    }
}

} // namespace neon::gfx
