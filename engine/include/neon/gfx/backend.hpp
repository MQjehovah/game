#pragma once
#include <cstddef>
#include <cstdint>
#include <memory>
#include "neon/gfx/color.hpp"
#include "neon/math/mat4.hpp"
#include "neon/platform/window.hpp"

namespace neon::gfx {

// GPU particle simulation handle (transform-feedback pool). Optional: backends
// that cannot simulate on the GPU report SupportsGpuParticles()==false and the
// runtime keeps its CPU particle path, so nothing regresses.
struct ParticleSimHandle {
    uint32_t id = 0;
    bool Valid() const { return id != 0; }
};

enum class BlendMode : uint8_t { Opaque, Alpha, Additive, Premultiplied };
enum class CullMode : uint8_t { None, Back, Front };
enum class PrimitiveTopology : uint8_t { Triangles, Lines };
enum class Filter : uint8_t { Nearest, Linear };
enum class Wrap : uint8_t { Clamp, Repeat };

struct TextureDesc {
    int width = 0;
    int height = 0;
    const uint8_t* rgba = nullptr; // 8-bit RGBA, row-major, top-left origin
    bool mipmaps = false;
    Filter filter = Filter::Linear;
    Wrap wrap = Wrap::Clamp;
};

struct ShaderHandle {
    uint32_t id = 0;
    bool Valid() const { return id != 0; }
};

struct TextureHandle {
    uint32_t id = 0;
    bool Valid() const { return id != 0; }
};

struct MeshHandle {
    uint32_t vao = 0;
    uint32_t vbo = 0;
    uint32_t ibo = 0;
    uint32_t indexCount = 0;
    // 0 = uint16 indices (legacy, all procedural/OBJ/glTF meshes), 1 = uint32
    // indices (large FBX geometry merged into one mesh). DrawMesh selects the
    // GL element type from this so the existing 16-bit pipeline is untouched.
    uint32_t indexType = 0;
    bool Valid() const { return vao != 0; }
};

struct RenderTargetHandle {
    uint32_t id = 0;
    bool Valid() const { return id != 0; }
};

// Low-level render backend. One implementation per graphics API
// (OpenGL now; Vulkan next). Everything above (Renderer/Material/Mesh)
// only talks to this interface.
class IRenderBackend {
public:
    virtual ~IRenderBackend() = default;

    // G6-1: GPU memory budget/usage (bytes). Total 0 = the driver reports no
    // budget (backend or extension unavailable). Used 0 = not tracked.
    struct GpuMemStats {
        uint64_t totalBytes = 0;
        uint64_t usedBytes = 0;
    };

    virtual bool Init(platform::IWindow* window) = 0;
    virtual void Shutdown() = 0;
    virtual const char* Name() const = 0;
    // G6-1: driver-reported GPU memory budget/usage; zeros when unavailable.
    virtual GpuMemStats GpuMemory() const { return {}; }

    // Resources
    // Color render target. When floatColor is true the color attachment is a
    // half-float (RGBA16F) texture - used by the HDR + bloom post pipeline so
    // values above 1.0 survive between passes. Default (false) keeps the
    // established RGBA8 color-encoded-depth targets used by CSM / point
    // shadows untouched.
    //
    // samples > 0 creates a MULTISAMPLED target (samples per pixel, e.g. 4):
    // the color + depth attachments become renderbuffers (no sampleable
    // texture exists, so RenderTargetColorTexture returns an invalid handle)
    // and the content is only readable after ResolveRenderTarget blits it into
    // a matching single-sample target. Used for MSAA on the HDR main scene
    // target; the shadow-map and bloom-pyramid targets stay single-sample.
    virtual RenderTargetHandle CreateRenderTarget(int width, int height,
                                                  bool floatColor = false,
                                                  int samples = 0) = 0;
    // Single-sample colour target WITH a real depth attachment (RGBA8 + depth24).
    // Used by the CSM cascade pass so the shadow map can be built with a genuine
    // depth TEST - nearest surface wins per texel - instead of painter's-order
    // sorting. Painter's order needs one sort key per object, so geometry that
    // interpenetrates *inside a single merged mesh* (a whole level exported as
    // one node: terrain + trees + towers) cannot be ordered correctly and large
    // parts of the map silently stop casting. Backends without a usable depth
    // buffer report unsupported here (invalid handle, NOT a plain colour target -
    // the caller would otherwise enable a depth test against a target that has no
    // depth attachment) and the caller keeps painter's order.
    virtual RenderTargetHandle CreateRenderTargetWithDepth(int width, int height) {
        (void)width;
        (void)height;
        return {};
    }
    virtual void DestroyRenderTarget(RenderTargetHandle target) = 0;
    virtual void BindRenderTarget(RenderTargetHandle target) = 0;
    virtual void BindDefaultTarget() = 0;
    // Blits the color attachment of a multisample target (src) into a
    // single-sample target (dst) of the same size (GL 3.3 glBlitFramebuffer).
    // src may also be single-sample (an identity copy). No-op on backends
    // without a resolve (NullBackend/Vulkan placeholder).
    virtual void ResolveRenderTarget(RenderTargetHandle src, RenderTargetHandle dst) = 0;
    // Blits the DEPTH attachment of a multisample target (src) into the depth
    // target (dst) at the same size, so the main pass depth becomes a
    // single-sample, sampleable texture (GL 3.3 glBlitFramebuffer with
    // GL_DEPTH_BUFFER_BIT; filter must be NEAREST). Returns false when the
    // backend cannot resolve depth (no depth-resolve support, or a driver whose
    // depth attachments are unreliable) so the caller keeps its colour-encoded
    // fallback. The default implementation reports unsupported.
    virtual bool ResolveDepth(RenderTargetHandle src, RenderTargetHandle dst) {
        (void)src;
        (void)dst;
        return false;
    }
    virtual TextureHandle RenderTargetColorTexture(RenderTargetHandle target) const = 0;
    virtual TextureHandle RenderTargetDepthTexture(RenderTargetHandle target) const = 0;

    // --- GPU particle simulation (optional; transform feedback) --------------
    // A backend that can simulate particles on the GPU returns an invalid handle
    // from CreateParticleSim and false from SupportsGpuParticles; the runtime
    // then keeps its CPU particle path. Concrete emit/step/draw methods land
    // with the GL implementation.
    virtual bool SupportsGpuParticles() const { return false; }
    // True when a render target the post chain writes one frame can be reliably
    // read back / rewritten the next frame (persistent auto-exposure history).
    // The Vulkan backend drops out-of-graph writes to such targets, so it
    // returns false and the exposure chain skips the temporal blend.
    virtual bool SupportsTargetPersistence() const { return true; }
    virtual ParticleSimHandle CreateParticleSim(uint32_t capacity) {
        (void)capacity;
        return {};
    }
    virtual void DestroyParticleSim(ParticleSimHandle sim) { (void)sim; }

    // Shadow-map depth target: an FBO whose only attachment is a depth texture
    // (no color buffer). Depth is written by the rasterizer; sample it with
    // BindShadowMap and compare manually in the shader (no hardware shadow
    // comparison is configured, so the raw depth comes back in .r).
    virtual RenderTargetHandle CreateDepthTarget(int width, int height) = 0;
    // Binds the depth target for the depth pre-pass (viewport set to its size).
    virtual void BeginDepthPass(RenderTargetHandle target) = 0;
    // Unbinds the depth target and restores the window framebuffer/viewport.
    virtual void EndDepthPass() = 0;
    // Binds the target's depth texture on a sampler slot for reading.
    virtual void BindShadowMap(int slot, RenderTargetHandle target) = 0;
    // Reads float depth (GL_DEPTH_COMPONENT, GL_FLOAT) for width*height pixels
    // from the currently bound framebuffer. NOTE: the tested Intel driver
    // returns garbage (zeros) for depth readbacks even when the attachment
    // holds valid depth, so the renderer's capability self-test uses the color
    // readback path instead. Kept for backends with reliable depth readback.
    virtual bool ReadCurrentTargetDepth(int width, int height, float* out) = 0;

    virtual ShaderHandle CreateShader(const char* vertexSource, const char* fragmentSource,
                                      const char* debugName) = 0;
    virtual void DestroyShader(ShaderHandle shader) = 0;

    virtual TextureHandle CreateTexture(const TextureDesc& desc) = 0;
    virtual void DestroyTexture(TextureHandle texture) = 0;
    // Uploads an RGBA8 sub-rectangle into an existing texture (dynamic font
    // atlases). `x/y` are in pixels; the data must cover w*h*4 bytes.
    virtual void UpdateTextureRegion(TextureHandle texture, int x, int y, int w, int h,
                                     const void* rgba) = 0;
    // Compressed (block-compressed) texture upload, e.g. BC1/DXT1: 4x4 blocks,
    // 8 bytes per block (GL_COMPRESSED_RGBA_S3TC_DXT1_EXT). `format` is the
    // API-specific internal format code (see assets::kBc1Format); both
    // dimensions are padded up to the block grid. Returns an invalid handle
    // when the driver rejects compressed uploads - callers must then fall back
    // to an uncompressed upload (and should not retry compressed per texture).
    virtual TextureHandle CreateTextureCompressed(int width, int height, uint32_t format,
                                                  const void* data, size_t size) = 0;

    // Vertex layout is fixed: position(3f) normal(3f) uv(2f) color(4f)
    // joints(4f) weights(4f) = 80 bytes (see gfx::Vertex3D).
    virtual MeshHandle CreateMesh(const void* vertices, uint32_t vertexCount,
                                  const uint16_t* indices, uint32_t indexCount) = 0;

    // Same fixed vertex layout but with 32-bit indices (meshes with more than
    // 65535 vertices — e.g. a merged FBX model). The returned handle carries
    // indexType=1 so DrawMesh uses GL_UNSIGNED_INT.
    virtual MeshHandle CreateMeshU32(const void* vertices, uint32_t vertexCount,
                                     const uint32_t* indices, uint32_t indexCount) = 0;
    virtual void DestroyMesh(const MeshHandle& mesh) = 0;
    // Replaces the vertex buffer contents of an existing mesh (same layout as
    // CreateMesh). Used when skinned joint/weight data is attached after the
    // initial upload.
    virtual void UpdateMeshVertices(const MeshHandle& mesh, const void* vertices,
                                    uint32_t vertexCount) = 0;

    // State
    virtual void SetBlendMode(BlendMode mode) = 0;
    virtual void SetDepthTest(bool enabled, bool write = true) = 0;
    virtual void SetCullMode(CullMode mode) = 0;
    // Slope-scaled depth bias for coplanar layered content (decals, water
    // sheets over a riverbed): 0/0 disables. Default no-op for backends
    // without support. `factor` scales with the surface slope, `units` is a
    // constant depth-quantum nudge (GL polygonOffset units / VK depthBias).
    virtual void SetPolygonOffset(float factor, float units) { (void)factor; (void)units; }
    // Sets the rasterization viewport in screen/window coordinates (top-left
    // origin; each backend translates to its own convention). Lets a 3D scene
    // render into a sub-rect of the target (e.g. the editor viewport dock).
    virtual void SetViewport(int x, int y, int width, int height) = 0;
    // Scissor rect in window pixels, y-down (origin top-left).
    virtual void SetScissor(int x, int y, int width, int height, bool enabled) = 0;
    virtual void Clear(const Color& color, float depth = 1.0f) = 0;

    // Shader uniforms (current program)
    virtual void UseShader(ShaderHandle shader) = 0;
    virtual void SetUniformMat4(const char* name, const math::Mat4& value) = 0;
    // Uploads `count` row-major mat4s as a contiguous array (e.g. uBoneMatrices).
    virtual void SetUniformMat4Array(const char* name, const float* values, int count) = 0;
    virtual void SetUniformVec4(const char* name, const math::Vec4& value) = 0;
    virtual void SetUniformVec3(const char* name, const math::Vec3& value) = 0;
    virtual void SetUniformFloat(const char* name, float value) = 0;
    virtual void SetUniformVec2(const char* name, const math::Vec2& value) = 0;
    virtual void SetUniformInt(const char* name, int value) = 0;
    virtual void BindTexture(int slot, TextureHandle texture) = 0;

    // Drawing
    virtual void DrawMesh(const MeshHandle& mesh) = 0;
    // Draws the mesh once per model matrix (GPU instancing).
    virtual void DrawMeshInstanced(const MeshHandle& mesh, const math::Mat4* models,
                                   uint32_t count) = 0;
    // GPU instancing with a per-instance RGBA color (particles/skotches that
    // vary color per instance). `colors` has `count` entries.
    virtual void DrawMeshInstancedColored(const MeshHandle& mesh, const math::Mat4* models,
                                          const math::Vec4* colors, uint32_t count) = 0;
    // GPU instancing with a per-instance color AND UV rectangle
    // (offset.xy, scale.xy). Used by flipbook/atlas particles: the vertex shader
    // maps the quad's 0..1 UV into one atlas cell. Backends without the extra
    // attribute fall back to DrawMeshInstancedColored (the uv rectangles are
    // ignored and the quad samples the whole texture), so a caller that only
    // needs the fallback stays correct.
    virtual void DrawMeshInstancedColoredUv(const MeshHandle& mesh, const math::Mat4* models,
                                            const math::Vec4* colors, const math::Vec4* uvRects,
                                            uint32_t count) {
        (void)uvRects;
        DrawMeshInstancedColored(mesh, models, colors, count);
    }
    // Immediate vertex submission (stride = bytes per vertex).
    virtual void DrawPrimitives(const void* vertices, uint32_t vertexCount, uint32_t stride,
                                const uint16_t* indices, uint32_t indexCount,
                                PrimitiveTopology topology) = 0;

    virtual void BeginFrame() = 0;
    virtual void EndFrame() = 0; // swap buffers
    // Capture the current frame (RGBA8, bottom-up rows) before EndFrame.
    virtual void CaptureFrame(int width, int height, void* rgba) = 0;
    // Reads a single pixel from the currently bound render target.
    virtual void ReadCurrentTargetPixel(int x, int y, unsigned char* rgba) = 0;
    // Debug (NEON_DUMP_POST): bulk-read the currently bound render target as
    // RGBA8. Default no-op; backends without a bulk read simply skip dumps.
    virtual void ReadTargetPixelsRect(int x, int y, int w, int h, unsigned char* rgba) {
        (void)x; (void)y; (void)w; (void)h; (void)rgba;
    }
    // True if the depth buffer is functional (some drivers expose a broken one).
    virtual bool DepthAvailable() const = 0;
};

std::unique_ptr<IRenderBackend> CreateOpenGLBackend();
std::unique_ptr<IRenderBackend> CreateVulkanBackend(); // placeholder

} // namespace neon::gfx
