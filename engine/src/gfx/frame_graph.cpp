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
    lastUse_.push_back(-1);
    return id;
}

bool FrameGraph::AddPass(FramePass pass) {
    for (ResourceId r : pass.reads)
        if (r >= resources_.size()) return false;
    for (ResourceId r : pass.writes)
        if (r >= resources_.size()) return false;

    const size_t index = passes_.size();
    passes_.push_back(std::move(pass));
    // The last *writer* of each resource, in declaration order. Execute()
    // derives the per-frame liveness over enabled passes from this same rule
    // (a resource is released right after its last reader/writer).
    for (ResourceId r : passes_.back().writes) lastUse_[r] = static_cast<int>(index);
    return true;
}

bool FrameGraph::Execute(IRenderBackend& backend) {
    const size_t n = passes_.size();

    // Disabled passes neither produce nor consume; only enabled ones take part.
    std::vector<size_t> enabled;
    enabled.reserve(n);
    for (size_t i = 0; i < n; ++i)
        if (passes_[i].enabled) enabled.push_back(i);
    if (enabled.empty()) return true;

    // Per-frame accounting over enabled passes, in declaration order:
    //   lastWriter[r] = the pass that produced r (drives dependency edges),
    //   lastUse[r]    = the last pass to read or write r (drives release).
    std::vector<int> lastWriter(resources_.size(), -1);
    std::vector<int> lastUse(resources_.size(), -1);
    for (size_t i : enabled) {
        for (ResourceId r : passes_[i].writes) lastWriter[r] = static_cast<int>(i);
        for (ResourceId r : passes_[i].reads) lastUse[r] = static_cast<int>(i);
        for (ResourceId r : passes_[i].writes) lastUse[r] = static_cast<int>(i);
    }

    // Map original pass index -> position in the enabled list (Kahn).
    std::unordered_map<size_t, size_t> pos;
    pos.reserve(enabled.size());
    for (size_t p = 0; p < enabled.size(); ++p) pos[enabled[p]] = p;

    // Dependency edge: a pass that reads r runs after the pass that wrote r.
    std::vector<size_t> incoming(enabled.size(), 0);
    std::vector<std::vector<size_t>> dependents(enabled.size());
    for (size_t p = 0; p < enabled.size(); ++p) {
        const size_t orig = enabled[p];
        for (ResourceId r : passes_[orig].reads) {
            const int writer = lastWriter[r];
            if (writer < 0) continue;                 // external / never written
            if (static_cast<size_t>(writer) == orig) continue; // self (in-place) write
            const auto it = pos.find(static_cast<size_t>(writer));
            if (it == pos.end()) continue;            // writer disabled (impossible)
            const size_t dep = it->second;
            if (dep == p) continue;
            incoming[p] += 1;
            dependents[dep].push_back(p);
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
        if (!progressed) return false; // cycle
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
            const auto it = liveRTs_.find(r);
            if (it != liveRTs_.end()) ctx.SetInput(r, it->second);
            // Reads with no in-graph producer are external; the caller is
            // expected to wire them up outside the graph (skipped for now).
        }
        for (ResourceId r : pass.writes) {
            const auto it = liveRTs_.find(r);
            if (it != liveRTs_.end()) ctx.SetOutput(r, it->second);
        }

        if (pass.execute) pass.execute(ctx);

        // A target whose last reader/writer is this pass can go back to the
        // pool immediately (reused by a later pass with the same descriptor).
        for (ResourceId r : pass.writes) {
            if (lastUse[r] != static_cast<int>(orig)) continue;
            const auto it = liveRTs_.find(r);
            if (it == liveRTs_.end()) continue;
            ReleaseTarget(backend, r, it->second);
            liveRTs_.erase(it);
        }
        for (ResourceId r : pass.reads) {
            if (lastUse[r] != static_cast<int>(orig)) continue;
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

RenderTargetHandle FrameGraph::AcquireTarget(IRenderBackend& backend,
                                             const FrameGraphResourceDesc& desc) {
    const PoolKey key{desc.width, desc.height, desc.format, desc.samples};
    auto it = pool_.find(key);
    if (it != pool_.end() && !it->second.empty()) {
        RenderTargetHandle rt = it->second.back();
        it->second.pop_back();
        return rt;
    }
    return backend.CreateRenderTarget(static_cast<int>(desc.width),
                                      static_cast<int>(desc.height),
                                      desc.format != 0, static_cast<int>(desc.samples));
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
