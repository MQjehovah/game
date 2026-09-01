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
inline constexpr ResourceId kInvalidResource = ~ResourceId(0);

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

// Per-pass execution trace: the input/output RenderTargetHandles a pass was
// wired with, recorded by the last Execute() (same order as it ran). Lets tests
// and tooling verify the dependency wiring (which producer's output feeds which
// consumer's input) instead of only the pass order.
struct FrameGraphBinding {
    ResourceId resource = kInvalidResource;
    RenderTargetHandle target;
};
struct FrameGraphPassTrace {
    std::string name;
    std::vector<FrameGraphBinding> inputs;
    std::vector<FrameGraphBinding> outputs;
};

// Declare resources/passes once, then Execute() per frame. A target's lifetime
// spans its last reader/writer in declaration order; earlier uses return it to
// the pool so the next same-descriptor target reuses the GPU allocation. Call
// ResetFrame() once per frame after Execute() to return any still-live targets
// to the pool (and do so before destroying the graph, which owns no backend).
//
// Two integration hooks keep the graph usable for real pipelines:
//   - SetExternalInput(): wires a caller-owned target into a resource that no
//     pass writes (e.g. the scene's HDR texture). Reads bind to it; it is never
//     pooled or destroyed by the graph.
//   - ExportResource(): keeps a resource's target live (NOT returned to the
//     pool) after its last reader/writer, so the caller can keep sampling it
//     (e.g. the bloom result read by the composite pass) until ResetFrame().
//
// Resources may be written by MULTIPLE passes (ping-pong). A read binds to the
// version produced by the most recent writer declared BEFORE the reader; a
// writer declared after the reader is a later overwrite and creates no edge,
// so the declaration order is always a valid topological order. State the
// passes in dataflow order.
class FrameGraph {
public:
    FrameGraph() = default;
    FrameGraph(const FrameGraph&) = delete;
    FrameGraph& operator=(const FrameGraph&) = delete;
    FrameGraph(FrameGraph&&) noexcept = default;
    FrameGraph& operator=(FrameGraph&&) noexcept = default;

    // Allocates the next resource id; records the target descriptor.
    ResourceId AddResource(const FrameGraphResourceDesc& desc);

    // Validates every read/write id against the declared resources; on any
    // out-of-range id nothing is added and false is returned. Passes that read
    // a resource are ordered after its most recent prior writer (see above).
    bool AddPass(FramePass pass);

    // Enables/disables the pass registered at `index` (0-based declaration
    // order; use PassCount() - 1 for the last one). Default is enabled. A
    // disabled pass neither produces nor consumes resources in the next
    // Execute(), letting a pipeline keep one graph structure and skip whole
    // chains per frame (e.g. SSAO off while SSR runs). Out-of-range indexes are
    // ignored.
    void SetPassEnabled(size_t index, bool enabled);

    // Wires a caller-owned render target into resource `id` (declared but never
    // written by any pass). The target's colour texture is what reads of the
    // resource sample. Pass an invalid handle to clear. The graph never pools
    // or destroys it. Valid only when Execute() runs.
    void SetExternalInput(ResourceId id, RenderTargetHandle rt);

    // Marks a resource as exported: its target stays live after its last use so
    // the caller can sample it (GetResourceTarget) until ResetFrame().
    void ExportResource(ResourceId id);

    // Live target of an exported (or still-live) resource. Valid between the
    // Execute() that produced it and ResetFrame().
    RenderTargetHandle GetResourceTarget(ResourceId id) const;

    // Topologically sorts the enabled passes (Kahn, cycle -> false), runs each
    // one with the transient targets wired into FrameGraphContext, records the
    // per-pass bindings in LastTrace() and returns targets whose last use is
    // over back into the pool (except exported ones).
    bool Execute(IRenderBackend& backend);

    // Trace of the last Execute(): the executed pass names in order plus each
    // pass's input/output target handles. Cleared at the start of Execute.
    const std::vector<FrameGraphPassTrace>& LastTrace() const { return trace_; }

    // Returns every target still live this frame to the pool.
    void ResetFrame();

    // Destroys every pooled + live target (GPU objects). Call when the graph is
    // discarded or rebuilt so stale allocations are not leaked.
    void DestroyResources(IRenderBackend& backend);

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
    std::vector<bool> exported_;
    std::unordered_map<ResourceId, RenderTargetHandle> externalInputs_;
    std::unordered_map<ResourceId, RenderTargetHandle> liveRTs_;
    std::vector<FrameGraphPassTrace> trace_;
    std::unordered_map<PoolKey, std::vector<RenderTargetHandle>, PoolKeyHash> pool_;
};

} // namespace neon::gfx
