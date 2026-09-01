#pragma once
#include <cstdint>
#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

#include "neon/gfx/backend.hpp"

namespace neon::gfx {

// Frostbite-style render frame graph: passes are declared over transient render
// targets; Execute() topologically sorts them, allocates/recycles the targets
// from a per-descriptor pool and runs each pass in dependency order. The graph
// is a pure CPU orchestration layer - the actual GL/Vulkan calls happen inside
// each pass through FrameGraphContext.

using ResourceId = uint32_t;

// Description of a transient render target resource. `format` is an opaque tag
// (0 = default RGBA8 target, non-zero = half-float RGBA16F color attachment via
// the backend's floatColor flag - the low-level CreateRenderTarget signature
// only distinguishes these two). `samples > 1` requests a multisampled target.
struct FrameGraphResourceDesc {
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t format = 0;
    uint32_t samples = 1;
};

// Per-pass execution context: the bindings for the resources the pass reads
// (produced by earlier passes this frame) and writes (freshly allocated or
// recycled transient targets). The backend is reachable via Backend() so a pass
// can issue the real draw/clear calls.
class FrameGraphContext {
public:
    explicit FrameGraphContext(IRenderBackend& backend) : backend_(backend) {}

    IRenderBackend& Backend() { return backend_; }

    void SetInput(ResourceId id, RenderTargetHandle rt) { inputs_[id] = rt; }
    RenderTargetHandle GetInput(ResourceId id) const {
        const auto it = inputs_.find(id);
        return it == inputs_.end() ? RenderTargetHandle{} : it->second;
    }

    void SetOutput(ResourceId id, RenderTargetHandle rt) { outputs_[id] = rt; }
    RenderTargetHandle GetOutput(ResourceId id) const {
        const auto it = outputs_.find(id);
        return it == outputs_.end() ? RenderTargetHandle{} : it->second;
    }

private:
    IRenderBackend& backend_;
    std::unordered_map<ResourceId, RenderTargetHandle> inputs_, outputs_;
};

struct FramePass {
    std::string name;
    std::vector<ResourceId> reads;
    std::vector<ResourceId> writes;
    bool enabled = true;
    std::function<void(FrameGraphContext&)> execute;
};

// Declare resources/passes once, then Execute() per frame. A target's lifetime
// spans its last reader/writer in declaration order; earlier uses return it to
// the pool so the next same-descriptor target reuses the GPU allocation. Call
// ResetFrame() once per frame after Execute() to return any still-live targets
// to the pool (and do so before destroying the graph, which owns no backend).
class FrameGraph {
public:
    FrameGraph() = default;
    FrameGraph(const FrameGraph&) = delete;
    FrameGraph& operator=(const FrameGraph&) = delete;

    // Allocates the next resource id; records the target descriptor.
    ResourceId AddResource(const FrameGraphResourceDesc& desc);

    // Validates every read/write id against the declared resources; on any
    // out-of-range id nothing is added and false is returned. Passes that read
    // a resource are ordered after its last writer.
    bool AddPass(FramePass pass);

    // Topologically sorts the enabled passes (Kahn, cycle -> false), runs each
    // one with the transient targets wired into FrameGraphContext and returns
    // targets whose last use is over back into the pool.
    bool Execute(IRenderBackend& backend);

    // Returns every target still live this frame to the pool.
    void ResetFrame();

    size_t PassCount() const { return passes_.size(); }
    size_t ResourceCount() const { return resources_.size(); }

private:
    struct PoolKey {
        uint32_t width, height, format, samples;
        bool operator==(const PoolKey& o) const {
            return width == o.width && height == o.height && format == o.format &&
                   samples == o.samples;
        }
    };
    struct PoolKeyHash {
        size_t operator()(const PoolKey& k) const {
            size_t h = k.width;
            h = h * 31 + k.height;
            h = h * 31 + k.format;
            h = h * 31 + k.samples;
            return h;
        }
    };

    RenderTargetHandle AcquireTarget(IRenderBackend& backend,
                                     const FrameGraphResourceDesc& desc);
    void ReleaseTarget(IRenderBackend& backend, ResourceId resource,
                       RenderTargetHandle target);

    std::vector<FrameGraphResourceDesc> resources_;
    std::vector<FramePass> passes_;
    std::vector<int> lastUse_;
    std::unordered_map<ResourceId, RenderTargetHandle> liveRTs_;
    std::unordered_map<PoolKey, std::vector<RenderTargetHandle>, PoolKeyHash> pool_;
};

} // namespace neon::gfx
