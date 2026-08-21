#pragma once
#include <cstdint>
#include <memory>
#include "neon/gfx/color.hpp"
#include "neon/math/mat4.hpp"
#include "neon/platform/window.hpp"

namespace neon::gfx {

enum class BlendMode : uint8_t { Opaque, Alpha, Additive, Premultiplied };
enum class CullMode : uint8_t { None, Back, Front };
enum class PrimitiveTopology : uint8_t { Triangles, Lines };
enum class Filter : uint8_t { Nearest, Linear };

struct TextureDesc {
    int width = 0;
    int height = 0;
    const uint8_t* rgba = nullptr; // 8-bit RGBA, row-major, top-left origin
    bool mipmaps = false;
    Filter filter = Filter::Linear;
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

    virtual bool Init(platform::IWindow* window) = 0;
    virtual void Shutdown() = 0;
    virtual const char* Name() const = 0;

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
    virtual void DestroyRenderTarget(RenderTargetHandle target) = 0;
    virtual void BindRenderTarget(RenderTargetHandle target) = 0;
    virtual void BindDefaultTarget() = 0;
    // Blits the color attachment of a multisample target (src) into a
    // single-sample target (dst) of the same size (GL 3.3 glBlitFramebuffer).
    // src may also be single-sample (an identity copy). No-op on backends
    // without a resolve (NullBackend/Vulkan placeholder).
    virtual void ResolveRenderTarget(RenderTargetHandle src, RenderTargetHandle dst) = 0;
    virtual TextureHandle RenderTargetColorTexture(RenderTargetHandle target) const = 0;
    virtual TextureHandle RenderTargetDepthTexture(RenderTargetHandle target) const = 0;

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

    // Vertex layout is fixed: position(3f) normal(3f) uv(2f) color(4f)
    // joints(4f) weights(4f) = 80 bytes (see gfx::Vertex3D).
    virtual MeshHandle CreateMesh(const void* vertices, uint32_t vertexCount,
                                  const uint16_t* indices, uint32_t indexCount) = 0;
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
    virtual void SetViewport(int width, int height) = 0;
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
    // True if the depth buffer is functional (some drivers expose a broken one).
    virtual bool DepthAvailable() const = 0;
};

std::unique_ptr<IRenderBackend> CreateOpenGLBackend();
std::unique_ptr<IRenderBackend> CreateVulkanBackend(); // placeholder

} // namespace neon::gfx
