#include "neon/gfx/backend.hpp"

#include <algorithm>
#include <cstring>
#include <unordered_map>
#include <vector>

#include "neon/core/log.hpp"
#include "neon/gfx/color.hpp"
#include "neon/gfx/gl/gl_loader.hpp"
#include "neon/gfx/mesh.hpp"

namespace neon::gfx {
namespace {

namespace glc {
constexpr gl::GLenum Triangles = 0x0004;
constexpr gl::GLenum Lines = 0x0001;
constexpr gl::GLenum UnsignedShort = 0x1403;
constexpr gl::GLenum Float = 0x1406;
constexpr gl::GLenum Texture2D = 0x0DE1;
constexpr gl::GLenum Rgba = 0x1908;
constexpr gl::GLenum Rgba8 = 0x8058;
constexpr gl::GLenum Rgba16f = 0x881A;
constexpr gl::GLenum UnsignedByte = 0x1401;
constexpr gl::GLenum HalfFloat = 0x140B;
constexpr gl::GLenum ArrayBuffer = 0x8892;
constexpr gl::GLenum ElementArrayBuffer = 0x8893;
constexpr gl::GLenum StaticDraw = 0x88E4;
constexpr gl::GLenum DynamicDraw = 0x88E8;
constexpr gl::GLenum FragmentShader = 0x8B30;
constexpr gl::GLenum VertexShader = 0x8B31;
constexpr gl::GLenum CompileStatus = 0x8B81;
constexpr gl::GLenum LinkStatus = 0x8B82;
constexpr gl::GLenum InfoLogLength = 0x8B84;
constexpr gl::GLenum Texture0 = 0x84C0;
constexpr gl::GLenum TextureMinFilter = 0x2801;
constexpr gl::GLenum TextureMagFilter = 0x2800;
constexpr gl::GLenum TextureWrapS = 0x2802;
constexpr gl::GLenum TextureWrapT = 0x2803;
constexpr gl::GLenum ClampToEdge = 0x812F;
constexpr gl::GLenum Repeat = 0x2901;
constexpr gl::GLenum Linear = 0x2601;
constexpr gl::GLenum Nearest = 0x2600;
constexpr gl::GLenum LinearMipmapLinear = 0x2703;
constexpr gl::GLenum Blend = 0x0BE2;
constexpr gl::GLenum SrcAlpha = 0x0302;
constexpr gl::GLenum One = 0x0001;
constexpr gl::GLenum OneMinusSrcAlpha = 0x0303;
constexpr gl::GLenum DepthTest = 0x0B71;
constexpr gl::GLenum CullFace = 0x0B44;
constexpr gl::GLenum Back = 0x0405;
constexpr gl::GLenum Front = 0x0404;
constexpr gl::GLenum CCW = 0x0901;
constexpr gl::GLenum ColorBufferBit = 0x00004000;
constexpr gl::GLenum DepthBufferBit = 0x00000100;
constexpr gl::GLenum Version = 0x1F02;
constexpr gl::GLenum RendererStr = 0x1F01;
constexpr gl::GLenum NoError = 0;
constexpr gl::GLenum Framebuffer = 0x8D40;
constexpr gl::GLenum ReadFramebuffer = 0x8CA8;
constexpr gl::GLenum DrawFramebuffer = 0x8CA9;
constexpr gl::GLenum DepthAttachment = 0x8D00;
constexpr gl::GLenum DepthComponent24 = 0x81A6;
constexpr gl::GLenum ColorAttachment0 = 0x8CE0;
constexpr gl::GLenum None = 0;
constexpr gl::GLenum Renderbuffer = 0x8D41;
constexpr gl::GLenum TextureCompareMode = 0x884C;
constexpr gl::GLenum CompareRefToTexture = 0x884E;
constexpr gl::GLenum TextureCompareFunc = 0x884D;
constexpr gl::GLenum Lequal = 0x0203;
constexpr gl::GLenum ScissorTest = 0x0C11;
constexpr gl::GLenum DepthComponent = 0x1902;
// GL_COMPRESSED_RGBA_S3TC_DXT1_EXT - the only compressed format the asset
// pipeline produces today (BC1 via stb_dxt, 4x4 blocks, 8 bytes/block).
constexpr gl::GLenum CompressedRgbaS3tcDxt1 = 0x83F1;
} // namespace glc

void CheckError(const char* where) {
    gl::GLenum err = gl::GetGL().GetError();
    if (err != 0)
        NEON_LOG_CAT(neon::core::LogCategory::Gfx, neon::core::LogLevel::Error,
                     "GL error 0x%X at %s", err, where);
}

struct Program {
    gl::GLuint id = 0;
    std::unordered_map<std::string, gl::GLint> uniforms;
};

struct GLMesh {
    gl::GLuint vao = 0;
    gl::GLuint vbo = 0;
    gl::GLuint ibo = 0;
    uint32_t indexCount = 0;
};

struct GLTexture {
    gl::GLuint id = 0;
    int width = 0;
    int height = 0;
};

struct GLRenderTarget {
    gl::GLuint fbo = 0;
    gl::GLuint colorTex = 0;
    gl::GLuint colorRbo = 0;
    gl::GLuint depthTex = 0;
    // Depth renderbuffer attached to float (HDR) color targets and to every
    // multisample target so depth testing works inside the HDR FBO on drivers
    // with a functional depth buffer. Zero for the color-encoded CSM/point-
    // shadow targets (they rely on painter's order) and for the depth-texture
    // targets.
    gl::GLuint depthRbo = 0;
    uint32_t colorTextureHandle = 0;
    uint32_t textureHandle = 0;
    int width = 0;
    int height = 0;
    int samples = 0;
};
class OpenGLBackend : public IRenderBackend {
public:
    ~OpenGLBackend() override { Shutdown(); }

    bool Init(platform::IWindow* window) override {
        window_ = window;
        if (!window_ || !window_->MakeGLContextCurrent()) return false;
        if (!gl::LoadGLFunctions()) return false;

        auto& g = gl::GetGL();
        const char* version = reinterpret_cast<const char*>(g.GetString(glc::Version));
        const char* renderer = reinterpret_cast<const char*>(g.GetString(glc::RendererStr));
        NEON_LOG_CAT(neon::core::LogCategory::Gfx, neon::core::LogLevel::Info,
                     "GL backend: %s (%s)", version ? version : "?", renderer ? renderer : "?");

        unsigned char whitePx[4] = {255, 255, 255, 255};
        TextureDesc whiteDesc;
        whiteDesc.width = 1;
        whiteDesc.height = 1;
        whiteDesc.rgba = whitePx;
        whiteTex_ = CreateTexture(whiteDesc);

        g.GenVertexArrays(1, &linesVao_);
        g.GenBuffers(1, &linesVbo_);
        g.GenBuffers(1, &linesEbo_);
        g.BindVertexArray(linesVao_);
        g.BindBuffer(glc::ArrayBuffer, linesVbo_);
        g.EnableVertexAttribArray(0);
        g.VertexAttribPointer(0, 3, glc::Float, 0, 28, nullptr);
        g.EnableVertexAttribArray(1);
        g.VertexAttribPointer(1, 4, glc::Float, 0, 28, reinterpret_cast<const void*>(12));

        g.GenVertexArrays(1, &uiVao_);
        g.GenBuffers(1, &uiVbo_);
        g.GenBuffers(1, &uiEbo_);
        g.GenBuffers(1, &instanceVbo_);
        g.GenBuffers(1, &instanceColorVbo_);
        g.BindVertexArray(uiVao_);
        g.BindBuffer(glc::ArrayBuffer, uiVbo_);
        g.EnableVertexAttribArray(0);
        g.VertexAttribPointer(0, 2, glc::Float, 0, 32, nullptr);
        g.EnableVertexAttribArray(1);
        g.VertexAttribPointer(1, 2, glc::Float, 0, 32, reinterpret_cast<const void*>(8));
        g.EnableVertexAttribArray(2);
        g.VertexAttribPointer(2, 4, glc::Float, 0, 32, reinterpret_cast<const void*>(16));
        g.BindVertexArray(0);

        g.Enable(glc::DepthTest);
        g.DepthFunc(0x0201); // GL_LEQUAL
        g.FrontFace(glc::CCW);

        // Self-test: clear to red and read back one pixel.
        g.ClearColor(1.0f, 0.0f, 0.0f, 1.0f);
        g.Clear(glc::ColorBufferBit);
        uint8_t probe[4] = {0, 0, 0, 0};
        g.ReadPixels(0, 0, 1, 1, glc::Rgba, glc::UnsignedByte, probe);
        gl::GLenum err = g.GetError();
        NEON_LOG_CAT(neon::core::LogCategory::Gfx, neon::core::LogLevel::Info,
                     "GL self-test: pixel=%u,%u,%u,%u err=%u", probe[0], probe[1], probe[2], probe[3],
                     err);
        gl::GLint depthBits = 0;
        g.GetIntegerv(0x0D56, &depthBits); // GL_DEPTH_BITS
        // Verify the depth buffer actually clears to the requested value.
        // Some drivers report depth bits but clear to 0, breaking GL_LESS.
        // IMPORTANT: this must run on an OFFSCREEN FBO - reading the window
        // framebuffer's depth is implementation-defined (NVIDIA returns 0),
        // which used to false-positive "depth broken" and force painter's
        // order with wrong occlusion.
        gl::GLuint depthFbo = 0, depthRbo = 0, depthTex = 0;
        g.GenFramebuffers(1, &depthFbo);
        g.BindFramebuffer(glc::Framebuffer, depthFbo);
        g.GenTextures(1, &depthTex);
        g.BindTexture(glc::Texture2D, depthTex);
        g.TexImage2D(glc::Texture2D, 0, static_cast<gl::GLint>(glc::Rgba8), 4, 4, 0, glc::Rgba,
                     glc::UnsignedByte, nullptr);
        g.FramebufferTexture2D(glc::Framebuffer, glc::ColorAttachment0, glc::Texture2D, depthTex,
                               0);
        g.GenRenderbuffers(1, &depthRbo);
        g.BindRenderbuffer(glc::Renderbuffer, depthRbo);
        g.RenderbufferStorage(glc::Renderbuffer, glc::DepthComponent24, 4, 4);
        g.FramebufferRenderbuffer(glc::Framebuffer, glc::DepthAttachment, glc::Renderbuffer,
                                  depthRbo);
        if (g.CheckFramebufferStatus(glc::Framebuffer) == 0x8CD5) {
            g.ClearDepth(1.0f);
            g.Clear(glc::DepthBufferBit);
            float depthValue = -1.0f;
            g.ReadPixels(0, 0, 1, 1, 0x1902, 0x1406, &depthValue); // GL_DEPTH_COMPONENT, GL_FLOAT
            depthUsable_ = depthBits >= 16 && depthValue > 0.9f;
            NEON_LOG_CAT(neon::core::LogCategory::Gfx, neon::core::LogLevel::Info,
                         "GL depth bits=%d value-after-clear=%.3f err=%u (FBO probe)", depthBits,
                         depthValue, g.GetError());
        } else {
            depthUsable_ = false;
            NEON_LOG_CAT(neon::core::LogCategory::Gfx, neon::core::LogLevel::Warn,
                         "GL depth probe FBO incomplete; falling back to painter's order");
        }
        g.BindFramebuffer(glc::Framebuffer, 0);
        g.DeleteFramebuffers(1, &depthFbo);
        g.DeleteRenderbuffers(1, &depthRbo);
        g.DeleteTextures(1, &depthTex);
        NEON_LOG_CAT(neon::core::LogCategory::Gfx, neon::core::LogLevel::Info, "GL depth %s",
                     depthUsable_ ? "usable" : "BROKEN - using painter's order");

        // Compressed-texture (BC1/DXT1) capability probe. The asset layer
        // compresses opaque textures to BC1 when this works and falls back to
        // RGBA8 when it does not (some drivers reject S3TC uploads with
        // GL_INVALID_OPERATION). The block is a solid-white 4x4 BC1 block
        // (color0 == color1 == white, all 2-bit indices 0); a successful upload
        // with no GL error is sufficient evidence the driver accepts S3TC.
        {
            const uint8_t bc1Block[8] = {0xFF, 0xFF, 0xFF, 0xFF, 0x00, 0x00, 0x00, 0x00};
            gl::GLuint probeId = 0;
            g.GenTextures(1, &probeId);
            g.BindTexture(glc::Texture2D, probeId);
            g.CompressedTexImage2D(glc::Texture2D, 0, glc::CompressedRgbaS3tcDxt1, 4, 4, 0, 8,
                                   bc1Block);
            compressedTexSupported_ = g.GetError() == glc::NoError;
            g.DeleteTextures(1, &probeId);
            g.BindTexture(glc::Texture2D, 0);
            NEON_LOG_CAT(neon::core::LogCategory::Gfx, neon::core::LogLevel::Info,
                         "GL compressed textures (BC1/DXT1): %s",
                         compressedTexSupported_ ? "supported" : "NOT supported - textures fall back to RGBA8");
        }

        glReady_ = true;
        return true;
    }

    void Shutdown() override {
        if (!window_) return;
        auto& g = gl::GetGL();
        if (!glReady_) {
            window_ = nullptr;
            return;
        }
        for (auto& [id, mesh] : meshes_) {
            g.DeleteBuffers(1, &mesh.vbo);
            g.DeleteBuffers(1, &mesh.ibo);
            g.DeleteVertexArrays(1, &mesh.vao);
        }
        meshes_.clear();
        for (auto& [id, tex] : textures_) g.DeleteTextures(1, &tex.id);
        textures_.clear();
        for (auto& [id, rt] : renderTargets_) {
            g.DeleteFramebuffers(1, &rt.fbo);
            g.DeleteTextures(1, &rt.depthTex);
            if (rt.depthRbo) g.DeleteRenderbuffers(1, &rt.depthRbo);
            if (rt.colorRbo) g.DeleteRenderbuffers(1, &rt.colorRbo);
            g.DeleteTextures(1, &rt.colorTex);
        }
        renderTargets_.clear();
        for (auto& [id, prog] : shaders_) g.DeleteProgram(prog.id);
        shaders_.clear();
        if (whiteTex_.Valid()) {
            DestroyTexture(whiteTex_);
            whiteTex_ = {};
        }
        g.DeleteVertexArrays(1, &linesVao_);
        g.DeleteBuffers(1, &linesVbo_);
        g.DeleteBuffers(1, &linesEbo_);
        g.DeleteVertexArrays(1, &uiVao_);
        g.DeleteBuffers(1, &uiVbo_);
        g.DeleteBuffers(1, &uiEbo_);
        g.DeleteBuffers(1, &instanceVbo_);
        g.DeleteBuffers(1, &instanceColorVbo_);
        window_ = nullptr;
    }

    const char* Name() const override { return "OpenGL 3.3"; }

    RenderTargetHandle CreateRenderTarget(int width, int height, bool floatColor,
                                          int samples) override {
        auto& g = gl::GetGL();
        GLRenderTarget rt;
        rt.width = width;
        rt.height = height;
        rt.samples = samples;
        g.GenFramebuffers(1, &rt.fbo);
        g.BindFramebuffer(glc::Framebuffer, rt.fbo);
        if (samples > 0) {
            // MSAA target: the color + depth attachments are multisample
            // renderbuffers (no sampleable texture). Content is read back only
            // after ResolveRenderTarget blits into a single-sample target.
            g.GenRenderbuffers(1, &rt.colorRbo);
            g.BindRenderbuffer(glc::Renderbuffer, rt.colorRbo);
            g.RenderbufferStorageMultisample(glc::Renderbuffer, samples,
                                             floatColor ? glc::Rgba16f : glc::Rgba8, width, height);
            g.FramebufferRenderbuffer(glc::Framebuffer, glc::ColorAttachment0, glc::Renderbuffer,
                                      rt.colorRbo);
            g.GenRenderbuffers(1, &rt.depthRbo);
            g.BindRenderbuffer(glc::Renderbuffer, rt.depthRbo);
            g.RenderbufferStorageMultisample(glc::Renderbuffer, samples, glc::DepthComponent24,
                                             width, height);
            g.FramebufferRenderbuffer(glc::Framebuffer, glc::DepthAttachment, glc::Renderbuffer,
                                      rt.depthRbo);
        } else {
            // Color texture: RGBA8 encodes light-space depth for shadow
            // sampling; the HDR + bloom pipeline requests a half-float
            // (RGBA16F) attachment so HDR scene values above 1.0 survive
            // between passes.
            g.GenTextures(1, &rt.colorTex);
            g.BindTexture(glc::Texture2D, rt.colorTex);
            if (floatColor) {
                g.TexImage2D(glc::Texture2D, 0, static_cast<gl::GLint>(glc::Rgba16f), width, height, 0,
                             glc::Rgba, glc::HalfFloat, nullptr);
                g.TexParameteri(glc::Texture2D, glc::TextureMinFilter, glc::Linear);
                g.TexParameteri(glc::Texture2D, glc::TextureMagFilter, glc::Linear);
            } else {
                g.TexImage2D(glc::Texture2D, 0, static_cast<gl::GLint>(glc::Rgba8), width, height, 0,
                             glc::Rgba, glc::UnsignedByte, nullptr);
                g.TexParameteri(glc::Texture2D, glc::TextureMinFilter, glc::Nearest);
                g.TexParameteri(glc::Texture2D, glc::TextureMagFilter, glc::Nearest);
            }
            g.TexParameteri(glc::Texture2D, glc::TextureWrapS, glc::ClampToEdge);
            g.TexParameteri(glc::Texture2D, glc::TextureWrapT, glc::ClampToEdge);
            g.FramebufferTexture2D(glc::Framebuffer, glc::ColorAttachment0, glc::Texture2D,
                                   rt.colorTex, 0);
            if (floatColor) {
                // Half-float HDR target: attach a depth renderbuffer so the
                // main pass keeps correct occlusion on drivers where the window
                // depth buffer works (on broken-depth drivers the renderer
                // disables depth testing anyway, so this attachment is inert).
                g.GenRenderbuffers(1, &rt.depthRbo);
                g.BindRenderbuffer(glc::Renderbuffer, rt.depthRbo);
                g.RenderbufferStorage(glc::Renderbuffer, glc::DepthComponent24, width, height);
                g.FramebufferRenderbuffer(glc::Framebuffer, glc::DepthAttachment, glc::Renderbuffer,
                                          rt.depthRbo);
                NEON_LOG_CAT(neon::core::LogCategory::Gfx, neon::core::LogLevel::Info,
                             "GL: float render target %dx%d (RGBA16F + depth RBO)", width, height);
            }
        }
        g.DrawBuffer(glc::ColorAttachment0);
        g.ReadBuffer(glc::ColorAttachment0);
        gl::GLenum status = g.CheckFramebufferStatus(glc::Framebuffer);
        if (status != 0x8CD5) { // GL_FRAMEBUFFER_COMPLETE
            NEON_LOG_CAT(neon::core::LogCategory::Gfx, neon::core::LogLevel::Error,
                         "GL: render target %dx%d (samples=%d) incomplete, status=0x%X", width,
                         height, samples, status);
            g.DeleteFramebuffers(1, &rt.fbo);
            g.DeleteTextures(1, &rt.colorTex);
            if (rt.depthRbo) g.DeleteRenderbuffers(1, &rt.depthRbo);
            if (rt.colorRbo) g.DeleteRenderbuffers(1, &rt.colorRbo);
            g.BindFramebuffer(glc::Framebuffer, 0);
            return {};
        }
        g.BindFramebuffer(glc::Framebuffer, 0);
        if (rt.colorTex) {
            rt.colorTextureHandle = ++nextTextureId_;
            textures_[rt.colorTextureHandle] = GLTexture{rt.colorTex, width, height};
            rt.textureHandle = ++nextTextureId_;
            textures_[rt.textureHandle] = GLTexture{rt.depthTex, width, height};
        }
        renderTargets_[++nextRenderTargetId_] = rt;
        return {nextRenderTargetId_};
    }

    void ResolveRenderTarget(RenderTargetHandle src, RenderTargetHandle dst) override {
        auto srcIt = renderTargets_.find(src.id);
        auto dstIt = renderTargets_.find(dst.id);
        if (srcIt == renderTargets_.end() || dstIt == renderTargets_.end()) return;
        auto& g = gl::GetGL();
        // Blit the multisample color into the single-sample target (GL 3.3
        // core glBlitFramebuffer); the depth attachment is not carried over
        // (the resolved HDR target's depth is only used for occlusion checks
        // during the main pass, never after resolve).
        g.BindFramebuffer(glc::ReadFramebuffer, srcIt->second.fbo);
        g.BindFramebuffer(glc::DrawFramebuffer, dstIt->second.fbo);
        g.BlitFramebuffer(0, 0, srcIt->second.width, srcIt->second.height, 0, 0,
                          dstIt->second.width, dstIt->second.height, glc::ColorBufferBit,
                          glc::Nearest);
        g.BindFramebuffer(glc::ReadFramebuffer, currentFBO_);
        g.BindFramebuffer(glc::DrawFramebuffer, currentFBO_);
        CheckError("ResolveRenderTarget");
    }

    void DestroyRenderTarget(RenderTargetHandle target) override {
        auto it = renderTargets_.find(target.id);
        if (it == renderTargets_.end()) return;
        auto& g = gl::GetGL();
        g.DeleteFramebuffers(1, &it->second.fbo);
        g.DeleteTextures(1, &it->second.colorTex);
        g.DeleteTextures(1, &it->second.depthTex);
        if (it->second.depthRbo) g.DeleteRenderbuffers(1, &it->second.depthRbo);
        if (it->second.colorRbo) g.DeleteRenderbuffers(1, &it->second.colorRbo);
        renderTargets_.erase(it);
    }

    RenderTargetHandle CreateDepthTarget(int width, int height) override {
        auto& g = gl::GetGL();
        GLRenderTarget rt;
        rt.width = width;
        rt.height = height;
        g.GenFramebuffers(1, &rt.fbo);
        g.BindFramebuffer(glc::Framebuffer, rt.fbo);
        g.GenTextures(1, &rt.depthTex);
        g.BindTexture(glc::Texture2D, rt.depthTex);
        g.TexStorage2D(glc::Texture2D, 1, glc::DepthComponent24, width, height);
        g.TexParameteri(glc::Texture2D, glc::TextureMinFilter, glc::Nearest);
        g.TexParameteri(glc::Texture2D, glc::TextureMagFilter, glc::Nearest);
        g.TexParameteri(glc::Texture2D, glc::TextureWrapS, glc::ClampToEdge);
        g.TexParameteri(glc::Texture2D, glc::TextureWrapT, glc::ClampToEdge);
        // Manual depth comparison in the shader; never enable hardware
        // shadow comparison (raw depth is read back in the .r channel).
        g.TexParameteri(glc::Texture2D, glc::TextureCompareMode, glc::None);
        g.FramebufferTexture2D(glc::Framebuffer, glc::DepthAttachment, glc::Texture2D,
                               rt.depthTex, 0);
        g.DrawBuffer(glc::None);
        g.ReadBuffer(glc::None);
        gl::GLenum status = g.CheckFramebufferStatus(glc::Framebuffer);
        if (status != 0x8CD5) { // GL_FRAMEBUFFER_COMPLETE
            NEON_LOG_CAT(neon::core::LogCategory::Gfx, neon::core::LogLevel::Error,
                         "GL: depth target incomplete, status=0x%X", status);
            g.DeleteFramebuffers(1, &rt.fbo);
            g.DeleteTextures(1, &rt.depthTex);
            g.BindFramebuffer(glc::Framebuffer, 0);
            return {};
        }
        g.BindFramebuffer(glc::Framebuffer, 0);
        // Both the color/depth texture handles resolve to the same depth
        // texture so BindShadowMap / RenderTargetDepthTexture work uniformly.
        rt.colorTextureHandle = ++nextTextureId_;
        textures_[rt.colorTextureHandle] = GLTexture{rt.depthTex, width, height};
        rt.textureHandle = ++nextTextureId_;
        textures_[rt.textureHandle] = GLTexture{rt.depthTex, width, height};
        renderTargets_[++nextRenderTargetId_] = rt;
        return {nextRenderTargetId_};
    }

    void BeginDepthPass(RenderTargetHandle target) override {
        auto it = renderTargets_.find(target.id);
        if (it == renderTargets_.end()) return;
        currentFBO_ = it->second.fbo;
        auto& g = gl::GetGL();
        g.BindFramebuffer(glc::Framebuffer, it->second.fbo);
        g.Viewport(0, 0, it->second.width, it->second.height);
    }

    void EndDepthPass() override { BindDefaultTarget(); }

    void BindShadowMap(int slot, RenderTargetHandle target) override {
        auto it = renderTargets_.find(target.id);
        if (it == renderTargets_.end()) return;
        auto& g = gl::GetGL();
        g.ActiveTexture(glc::Texture0 + static_cast<gl::GLenum>(slot));
        g.BindTexture(glc::Texture2D, it->second.depthTex);
    }

    bool ReadCurrentTargetDepth(int width, int height, float* out) override {
        if (!out) return false;
        auto& g = gl::GetGL();
        gl::GLenum err = g.GetError();
        g.ReadPixels(0, 0, width, height, glc::DepthComponent, glc::Float, out);
        return g.GetError() == 0 && err == 0;
    }

    void BindRenderTarget(RenderTargetHandle target) override {
        auto it = renderTargets_.find(target.id);
        if (it == renderTargets_.end()) return;
        currentFBO_ = it->second.fbo;
        auto& g = gl::GetGL();
        g.BindFramebuffer(glc::Framebuffer, it->second.fbo);
        g.Viewport(0, 0, it->second.width, it->second.height);
    }

    void BindDefaultTarget() override {
        currentFBO_ = 0;
        auto& g = gl::GetGL();
        g.BindFramebuffer(glc::Framebuffer, 0);
        // FBO setup switches the draw/read buffers to COLOR_ATTACHMENT0; restore
        // them for the window's default framebuffer (double-buffered => GL_BACK).
        g.DrawBuffer(glc::Back);
        g.ReadBuffer(glc::Back);
        if (window_) g.Viewport(0, 0, window_->Width(), window_->Height());
    }

    TextureHandle RenderTargetDepthTexture(RenderTargetHandle target) const override {
        auto it = renderTargets_.find(target.id);
        return it != renderTargets_.end() ? TextureHandle{it->second.textureHandle}
                                          : TextureHandle{};
    }

    TextureHandle RenderTargetColorTexture(RenderTargetHandle target) const override {
        auto it = renderTargets_.find(target.id);
        return it != renderTargets_.end() ? TextureHandle{it->second.colorTextureHandle}
                                          : TextureHandle{};
    }

    ShaderHandle CreateShader(const char* vertexSource, const char* fragmentSource,
                              const char* debugName) override {
        auto& g = gl::GetGL();
        gl::GLuint vs = CompileShader(glc::VertexShader, vertexSource, debugName);
        if (!vs) return {};
        gl::GLuint fs = CompileShader(glc::FragmentShader, fragmentSource, debugName);
        if (!fs) {
            g.DeleteShader(vs);
            return {};
        }
        gl::GLuint program = g.CreateProgram();
        g.AttachShader(program, vs);
        g.AttachShader(program, fs);
        g.LinkProgram(program);
        g.DeleteShader(vs);
        g.DeleteShader(fs);

        gl::GLint status = 0;
        g.GetProgramiv(program, glc::LinkStatus, &status);
        if (status == 0) {
            gl::GLint len = 0;
            g.GetProgramiv(program, glc::InfoLogLength, &len);
            std::vector<char> log(std::max(len, 1));
            g.GetProgramInfoLog(program, static_cast<gl::GLsizei>(log.size()), nullptr, log.data());
            NEON_LOG_CAT(neon::core::LogCategory::Gfx, neon::core::LogLevel::Error,
                         "GL: failed to link shader '%s': %s", debugName, log.data());
            g.DeleteProgram(program);
            return {};
        }

        Program prog;
        prog.id = program;
        shaders_[++nextShaderId_] = prog;
        return {nextShaderId_};
    }

    void DestroyShader(ShaderHandle shader) override {
        auto it = shaders_.find(shader.id);
        if (it == shaders_.end()) return;
        gl::GetGL().DeleteProgram(it->second.id);
        shaders_.erase(it);
    }

    TextureHandle CreateTexture(const TextureDesc& desc) override {
        auto& g = gl::GetGL();
        gl::GLuint id = 0;
        g.GenTextures(1, &id);
        g.BindTexture(glc::Texture2D, id);
        const gl::GLenum wrap = desc.wrap == Wrap::Repeat ? glc::Repeat : glc::ClampToEdge;
        g.TexParameteri(glc::Texture2D, glc::TextureWrapS, wrap);
        g.TexParameteri(glc::Texture2D, glc::TextureWrapT, wrap);
        g.TexParameteri(glc::Texture2D, glc::TextureMinFilter,
                        desc.filter == Filter::Nearest ? glc::Nearest : glc::Linear);
        g.TexParameteri(glc::Texture2D, glc::TextureMagFilter,
                        desc.filter == Filter::Nearest ? glc::Nearest : glc::Linear);

        // Allocate immutable storage (loaded from the ICD via wglGetProcAddress,
        // avoiding the legacy opengl32 glTexImage2D stub) and upload the base level.
        int levels = 1;
        if (desc.mipmaps) {
            int maxDim = std::max(desc.width, desc.height);
            while (maxDim > 1) {
                maxDim >>= 1;
                ++levels;
            }
        }
        g.TexStorage2D(glc::Texture2D, levels, glc::Rgba8, desc.width, desc.height);
        CheckError("CreateTexture.TexStorage2D");
        g.TexSubImage2D(glc::Texture2D, 0, 0, 0, desc.width, desc.height, glc::Rgba,
                        glc::UnsignedByte, desc.rgba);
        CheckError("CreateTexture.TexSubImage2D");
        if (desc.mipmaps) {
            g.GenerateMipmap(glc::Texture2D);
        }
        g.BindTexture(glc::Texture2D, 0);
        textures_[++nextTextureId_] = GLTexture{id, desc.width, desc.height};
        return {nextTextureId_};
    }

    void DestroyTexture(TextureHandle texture) override {
        auto it = textures_.find(texture.id);
        if (it == textures_.end()) return;
        gl::GetGL().DeleteTextures(1, &it->second.id);
        textures_.erase(it);
    }

    void UpdateTextureRegion(TextureHandle texture, int x, int y, int w, int h,
                             const void* rgba) override {
        auto it = textures_.find(texture.id);
        if (it == textures_.end() || !rgba || w <= 0 || h <= 0) return;
        if (x < 0 || y < 0 || x + w > it->second.width || y + h > it->second.height) return;
        auto& g = gl::GetGL();
        g.BindTexture(glc::Texture2D, it->second.id);
        g.TexSubImage2D(glc::Texture2D, 0, x, y, w, h, glc::Rgba, glc::UnsignedByte, rgba);
        g.BindTexture(glc::Texture2D, 0);
        CheckError("UpdateTextureRegion");
    }

    TextureHandle CreateTextureCompressed(int width, int height, uint32_t format,
                                          const void* data, size_t size) override {
        if (!data || width <= 0 || height <= 0 || size == 0) return {};
        if (format != glc::CompressedRgbaS3tcDxt1) {
            NEON_LOG_CAT(neon::core::LogCategory::Gfx, neon::core::LogLevel::Error,
                         "GL: unsupported compressed format 0x%X (only BC1/DXT1 today)", format);
            return {};
        }
        if (!compressedTexSupported_) {
            // The init-time probe already told us this driver rejects S3TC
            // uploads; the asset layer falls back to RGBA8 and logs once.
            return {};
        }
        auto& g = gl::GetGL();
        gl::GLuint id = 0;
        g.GenTextures(1, &id);
        g.BindTexture(glc::Texture2D, id);
        g.TexParameteri(glc::Texture2D, glc::TextureWrapS, glc::ClampToEdge);
        g.TexParameteri(glc::Texture2D, glc::TextureWrapT, glc::ClampToEdge);
        // Only the base level is uploaded (no mip chain for compressed data);
        // use plain linear filtering so minification bilinearly samples the
        // base level instead of shimmering.
        g.TexParameteri(glc::Texture2D, glc::TextureMinFilter, glc::Linear);
        g.TexParameteri(glc::Texture2D, glc::TextureMagFilter, glc::Linear);
        // glCompressedTexImage2D requires BOTH dimensions to be multiples of
        // the 4x4 block size; the encoder pads non-multiple sizes, so allocate
        // the padded size here.
        const int bw = (width + 3) & ~3;
        const int bh = (height + 3) & ~3;
        g.CompressedTexImage2D(glc::Texture2D, 0, static_cast<gl::GLenum>(format), bw, bh, 0,
                               static_cast<gl::GLsizei>(size), data);
        gl::GLenum err = g.GetError();
        if (err != glc::NoError) {
            NEON_LOG_CAT(neon::core::LogCategory::Gfx, neon::core::LogLevel::Error,
                         "GL: compressed texture upload %dx%d rejected by the driver "
                         "(err=0x%X); caller must fall back to RGBA8",
                         width, height, err);
            g.DeleteTextures(1, &id);
            return {};
        }
        g.BindTexture(glc::Texture2D, 0);
        textures_[++nextTextureId_] = GLTexture{id, width, height};
        return {nextTextureId_};
    }

    MeshHandle CreateMesh(const void* vertices, uint32_t vertexCount,
                          const uint16_t* indices, uint32_t indexCount) override {
        auto& g = gl::GetGL();
        constexpr gl::GLsizei kStride = static_cast<gl::GLsizei>(sizeof(Vertex3D));
        GLMesh mesh;
        g.GenVertexArrays(1, &mesh.vao);
        g.GenBuffers(1, &mesh.vbo);
        g.GenBuffers(1, &mesh.ibo);
        g.BindVertexArray(mesh.vao);
        g.BindBuffer(glc::ArrayBuffer, mesh.vbo);
        g.BufferData(glc::ArrayBuffer,
                     static_cast<gl::GLsizeiptr>(vertexCount * static_cast<size_t>(kStride)),
                     vertices, glc::StaticDraw);
        g.EnableVertexAttribArray(0);
        g.VertexAttribPointer(0, 3, glc::Float, 0, kStride, nullptr);
        g.EnableVertexAttribArray(1);
        g.VertexAttribPointer(1, 3, glc::Float, 0, kStride,
                              reinterpret_cast<const void*>(offsetof(Vertex3D, normal)));
        g.EnableVertexAttribArray(2);
        g.VertexAttribPointer(2, 2, glc::Float, 0, kStride,
                              reinterpret_cast<const void*>(offsetof(Vertex3D, uv)));
        g.EnableVertexAttribArray(3);
        g.VertexAttribPointer(3, 4, glc::Float, 0, kStride,
                              reinterpret_cast<const void*>(offsetof(Vertex3D, color)));
        // Skinning attributes are always bound: non-skinned meshes carry zeros,
        // and only the skinned shader variant reads locations 4/5.
        g.EnableVertexAttribArray(4);
        g.VertexAttribPointer(4, 4, glc::Float, 0, kStride,
                              reinterpret_cast<const void*>(offsetof(Vertex3D, j)));
        g.EnableVertexAttribArray(5);
        g.VertexAttribPointer(5, 4, glc::Float, 0, kStride,
                              reinterpret_cast<const void*>(offsetof(Vertex3D, w)));
        g.BindBuffer(glc::ElementArrayBuffer, mesh.ibo);
        g.BufferData(glc::ElementArrayBuffer, static_cast<gl::GLsizeiptr>(indexCount * 2),
                     indices, glc::StaticDraw);
        g.BindVertexArray(0);
        mesh.indexCount = indexCount;
        meshes_[mesh.vao] = mesh;
        return {mesh.vao, mesh.vbo, mesh.ibo, mesh.indexCount};
    }

    void DestroyMesh(const MeshHandle& mesh) override {
        auto it = meshes_.find(mesh.vao);
        if (it == meshes_.end()) return;
        auto& g = gl::GetGL();
        g.DeleteBuffers(1, &it->second.vbo);
        g.DeleteBuffers(1, &it->second.ibo);
        g.DeleteVertexArrays(1, &it->second.vao);
        meshes_.erase(it);
    }

    void UpdateMeshVertices(const MeshHandle& mesh, const void* vertices,
                            uint32_t vertexCount) override {
        if (!vertices) return;
        auto it = meshes_.find(mesh.vao);
        if (it == meshes_.end()) return;
        auto& g = gl::GetGL();
        g.BindVertexArray(it->second.vao);
        g.BindBuffer(glc::ArrayBuffer, it->second.vbo);
        g.BufferData(glc::ArrayBuffer,
                     static_cast<gl::GLsizeiptr>(static_cast<size_t>(vertexCount) *
                                                 sizeof(Vertex3D)),
                     vertices, glc::StaticDraw);
        g.BindVertexArray(0);
        CheckError("UpdateMeshVertices");
    }

    void SetBlendMode(BlendMode mode) override {
        auto& g = gl::GetGL();
        if (mode == BlendMode::Opaque) {
            g.Disable(glc::Blend);
        } else {
            g.Enable(glc::Blend);
            if (mode == BlendMode::Additive) {
                g.BlendFunc(glc::SrcAlpha, glc::One);
            } else if (mode == BlendMode::Premultiplied) {
                g.BlendFunc(glc::One, glc::OneMinusSrcAlpha);
            } else {
                g.BlendFunc(glc::SrcAlpha, glc::OneMinusSrcAlpha);
            }
        }
    }

    void SetDepthTest(bool enabled, bool write) override {
        auto& g = gl::GetGL();
        if (enabled) g.Enable(glc::DepthTest);
        else g.Disable(glc::DepthTest);
        g.DepthMask(write ? 1 : 0);
    }

    void SetCullMode(CullMode mode) override {
        auto& g = gl::GetGL();
        if (mode == CullMode::None) {
            g.Disable(glc::CullFace);
        } else {
            g.Enable(glc::CullFace);
            g.CullFace(mode == CullMode::Back ? glc::Back : glc::Front);
        }
    }

    void SetViewport(int x, int y, int width, int height) override {
        const int winH = window_ ? window_->Height() : height;
        gl::GetGL().Viewport(x, winH - (y + height), width, height);
    }

    void SetScissor(int x, int y, int width, int height, bool enabled) override {
        auto& g = gl::GetGL();
        if (!enabled) {
            g.Disable(glc::ScissorTest);
            return;
        }
        g.Enable(glc::ScissorTest);
        int winH = window_ ? window_->Height() : height;
        g.Scissor(x, winH - (y + height), width, height);
    }

    void Clear(const Color& color, float depth) override {
        auto& g = gl::GetGL();
        // glClear respects the depth WRITE MASK: a previous SetDepthTest(false,
        // false) leaves DepthMask(0), so the depth clear would silently no-op
        // and the LEQUAL test would reject every pixel. Force the mask on.
        g.DepthMask(1);
        g.ClearColor(color.r, color.g, color.b, color.a);
        g.ClearDepth(depth);
        g.Clear(glc::ColorBufferBit | glc::DepthBufferBit);
        CheckError("Clear");
    }

    void UseShader(ShaderHandle shader) override {
        currentShader_ = shader;
        const Program& prog = GetProgram(shader);
        if (prog.id == 0)
            NEON_LOG_CAT(neon::core::LogCategory::Gfx, neon::core::LogLevel::Error,
                         "GL: UseShader with invalid program (handle %u)", shader.id);
        gl::GetGL().UseProgram(prog.id);
        gl::GLenum err = gl::GetGL().GetError();
        if (err) {
            gl::GLint link = 0;
            gl::GetGL().GetProgramiv(prog.id, glc::LinkStatus, &link);
            NEON_LOG_CAT(neon::core::LogCategory::Gfx, neon::core::LogLevel::Error,
                         "GL: UseShader err=0x%X program=%u link=%d", err, prog.id, link);
        }
    }

    void SetUniformMat4(const char* name, const math::Mat4& value) override {
        gl::GLint loc = GetUniformLocation(currentShader_, name);
        // Mat4 is row-major; GL expects column-major with transpose=GL_FALSE.
        if (loc >= 0) gl::GetGL().UniformMatrix4fv(loc, 1, 1, value.Data());
        CheckError(name);
    }

    void SetUniformMat4Array(const char* name, const float* values, int count) override {
        if (!values || count <= 0) return;
        gl::GLint loc = GetUniformLocation(currentShader_, name);
        if (loc >= 0) gl::GetGL().UniformMatrix4fv(loc, count, 1, values);
        else if (std::string(name) == "uBoneMatrices")
            NEON_LOG_CAT(neon::core::LogCategory::Gfx, neon::core::LogLevel::Error,
                         "GL: uBoneMatrices uniform not found in program %u",
                         GetProgram(currentShader_).id);
        else
            NEON_LOG_CAT(neon::core::LogCategory::Gfx, neon::core::LogLevel::Error,
                         "GL: mat4-array uniform '%s' not found in program %u",
                         name, GetProgram(currentShader_).id);
        CheckError(name);
    }

    void SetUniformVec4(const char* name, const math::Vec4& value) override {
        gl::GLint loc = GetUniformLocation(currentShader_, name);
        if (loc >= 0) gl::GetGL().Uniform4f(loc, value.x, value.y, value.z, value.w);
        CheckError(name);
    }

    void SetUniformVec3(const char* name, const math::Vec3& value) override {
        gl::GLint loc = GetUniformLocation(currentShader_, name);
        if (loc >= 0) gl::GetGL().Uniform3f(loc, value.x, value.y, value.z);
        CheckError(name);
    }

    void SetUniformFloat(const char* name, float value) override {
        gl::GLint loc = GetUniformLocation(currentShader_, name);
        if (loc >= 0) gl::GetGL().Uniform1f(loc, value);
        CheckError(name);
    }

    void SetUniformVec2(const char* name, const math::Vec2& value) override {
        gl::GLint loc = GetUniformLocation(currentShader_, name);
        if (loc >= 0) gl::GetGL().Uniform2f(loc, value.x, value.y);
        CheckError(name);
    }

    void SetUniformInt(const char* name, int value) override {
        gl::GLint loc = GetUniformLocation(currentShader_, name);
        if (loc >= 0) gl::GetGL().Uniform1i(loc, value);
        CheckError(name);
    }

    void BindTexture(int slot, TextureHandle texture) override {
        auto& g = gl::GetGL();
        g.ActiveTexture(glc::Texture0 + static_cast<gl::GLenum>(slot));
        auto it = textures_.find(texture.id);
        g.BindTexture(glc::Texture2D, it != textures_.end() ? it->second.id : 0);
    }

    void DrawMesh(const MeshHandle& mesh) override {
        auto it = meshes_.find(mesh.vao);
        if (it == meshes_.end()) return;
        auto& g = gl::GetGL();
        g.BindVertexArray(it->second.vao);
        g.DrawElements(glc::Triangles, static_cast<gl::GLsizei>(it->second.indexCount),
                       glc::UnsignedShort, nullptr);
        CheckError("DrawMesh");
        g.BindVertexArray(0);
    }

    void DrawMeshInstanced(const MeshHandle& mesh, const math::Mat4* models,
                           uint32_t count) override {
        auto it = meshes_.find(mesh.vao);
        if (it == meshes_.end() || !models || count == 0) return;
        auto& g = gl::GetGL();
        g.BindVertexArray(it->second.vao);
        g.BindBuffer(glc::ArrayBuffer, instanceVbo_);
        // The engine matrices are row-major (m[row*4+col]); GLSL `mat4`
        // attributes are filled column-major, so each instance matrix must be
        // TRANSPOSED before upload or the translation is lost and every
        // instance renders at the origin. Mirrors the transpose=GL_TRUE used
        // for mat4 uniforms.
        std::vector<float> flat(static_cast<size_t>(count) * 16);
        for (uint32_t m = 0; m < count; ++m) {
            const float* src = models[m].Data();
            float* dst = flat.data() + static_cast<size_t>(m) * 16;
            for (int r = 0; r < 4; ++r)
                for (int c = 0; c < 4; ++c) dst[r * 4 + c] = src[c * 4 + r];
        }
        g.BufferData(glc::ArrayBuffer, static_cast<gl::GLsizeiptr>(count * 64), flat.data(),
                     glc::DynamicDraw);
        for (int i = 0; i < 4; ++i) {
            g.EnableVertexAttribArray(4 + i);
            g.VertexAttribPointer(4 + i, 4, glc::Float, 0, 64,
                                  reinterpret_cast<const void*>(i * 16));
            g.VertexAttribDivisor(4 + i, 1);
        }
        g.DrawElementsInstanced(glc::Triangles, static_cast<gl::GLsizei>(it->second.indexCount),
                                glc::UnsignedShort, nullptr, static_cast<gl::GLsizei>(count));
        for (int i = 0; i < 4; ++i) g.DisableVertexAttribArray(4 + i);
        g.BindVertexArray(0);
    }

    void DrawMeshInstancedColored(const MeshHandle& mesh, const math::Mat4* models,
                                  const math::Vec4* colors, uint32_t count) override {
        auto it = meshes_.find(mesh.vao);
        if (it == meshes_.end() || !models || !colors || count == 0) return;
        auto& g = gl::GetGL();
        g.BindVertexArray(it->second.vao);

        // Instance matrices (attributes 4..7), transposed row->column-major.
        g.BindBuffer(glc::ArrayBuffer, instanceVbo_);
        std::vector<float> flat(static_cast<size_t>(count) * 16);
        for (uint32_t m = 0; m < count; ++m) {
            const float* src = models[m].Data();
            float* dst = flat.data() + static_cast<size_t>(m) * 16;
            for (int r = 0; r < 4; ++r)
                for (int c = 0; c < 4; ++c) dst[r * 4 + c] = src[c * 4 + r];
        }
        g.BufferData(glc::ArrayBuffer, static_cast<gl::GLsizeiptr>(count * 64), flat.data(),
                     glc::DynamicDraw);
        for (int i = 0; i < 4; ++i) {
            g.EnableVertexAttribArray(4 + i);
            g.VertexAttribPointer(4 + i, 4, glc::Float, 0, 64,
                                  reinterpret_cast<const void*>(i * 16));
            g.VertexAttribDivisor(4 + i, 1);
        }

        // Per-instance color (attribute 8).
        g.BindBuffer(glc::ArrayBuffer, instanceColorVbo_);
        g.BufferData(glc::ArrayBuffer, static_cast<gl::GLsizeiptr>(count * 16), colors,
                     glc::DynamicDraw);
        g.EnableVertexAttribArray(8);
        g.VertexAttribPointer(8, 4, glc::Float, 0, 16, nullptr);
        g.VertexAttribDivisor(8, 1);

        g.DrawElementsInstanced(glc::Triangles, static_cast<gl::GLsizei>(it->second.indexCount),
                                glc::UnsignedShort, nullptr, static_cast<gl::GLsizei>(count));
        for (int i = 0; i < 4; ++i) g.DisableVertexAttribArray(4 + i);
        g.DisableVertexAttribArray(8);
        g.BindVertexArray(0);
    }

    void DrawPrimitives(const void* vertices, uint32_t vertexCount, uint32_t stride,
                        const uint16_t* indices, uint32_t indexCount,
                        PrimitiveTopology topology) override {
        auto& g = gl::GetGL();
        const bool isLines = stride == 28;
        g.BindVertexArray(isLines ? linesVao_ : uiVao_);
        g.BindBuffer(glc::ArrayBuffer, isLines ? linesVbo_ : uiVbo_);
        g.BufferData(glc::ArrayBuffer, static_cast<gl::GLsizeiptr>(vertexCount * stride),
                     vertices, glc::DynamicDraw);
        if (indices && indexCount > 0) {
            g.BindBuffer(glc::ElementArrayBuffer, isLines ? linesEbo_ : uiEbo_);
            g.BufferData(glc::ElementArrayBuffer, static_cast<gl::GLsizeiptr>(indexCount * 2),
                         indices, glc::DynamicDraw);
            g.DrawElements(topology == PrimitiveTopology::Lines ? glc::Lines : glc::Triangles,
                           static_cast<gl::GLsizei>(indexCount), glc::UnsignedShort, nullptr);
        } else {
            g.DrawArrays(topology == PrimitiveTopology::Lines ? glc::Lines : glc::Triangles,
                         0, static_cast<gl::GLsizei>(vertexCount));
        }
        CheckError("DrawPrimitives");
        g.BindVertexArray(0);
    }

    void BeginFrame() override {}
    void EndFrame() override { if (window_) window_->SwapBuffers(); }
    bool DepthAvailable() const override { return depthUsable_; }
    void CaptureFrame(int width, int height, void* rgba) override {
        auto& g = gl::GetGL();
        if (!rgba) return;
        g.ReadPixels(0, 0, width, height, glc::Rgba, glc::UnsignedByte, rgba);
        // OpenGL rows are bottom-up; flip to top-down.
        auto* bytes = static_cast<uint8_t*>(rgba);
        size_t rowBytes = static_cast<size_t>(width) * 4;
        std::vector<uint8_t> temp(rowBytes);
        for (int y = 0; y < height / 2; ++y) {
            uint8_t* top = bytes + static_cast<size_t>(y) * rowBytes;
            uint8_t* bottom = bytes + static_cast<size_t>(height - 1 - y) * rowBytes;
            std::memcpy(temp.data(), top, rowBytes);
            std::memcpy(top, bottom, rowBytes);
            std::memcpy(bottom, temp.data(), rowBytes);
        }
    }
    void ReadCurrentTargetPixel(int x, int y, unsigned char* rgba) override {
        if (!rgba) return;
        gl::GetGL().ReadPixels(x, y, 1, 1, glc::Rgba, glc::UnsignedByte, rgba);
    }

private:
    gl::GLuint CompileShader(gl::GLenum type, const char* source, const char* debugName) {
        auto& g = gl::GetGL();
        gl::GLuint shader = g.CreateShader(type);
        const char* sources[] = {source};
        g.ShaderSource(shader, 1, sources, nullptr);
        g.CompileShader(shader);
        gl::GLint status = 0;
        g.GetShaderiv(shader, glc::CompileStatus, &status);
        if (status == 0) {
            gl::GLint len = 0;
            g.GetShaderiv(shader, glc::InfoLogLength, &len);
            std::vector<char> log(std::max(len, 1));
            g.GetShaderInfoLog(shader, static_cast<gl::GLsizei>(log.size()), nullptr, log.data());
            NEON_LOG_CAT(neon::core::LogCategory::Gfx, neon::core::LogLevel::Error,
                         "GL: failed to compile %s shader '%s': %s",
                           type == glc::VertexShader ? "vertex" : "fragment", debugName, log.data());
            g.DeleteShader(shader);
            return 0;
        }
        return shader;
    }

    const Program& GetProgram(ShaderHandle shader) {
        auto it = shaders_.find(shader.id);
        return it != shaders_.end() ? it->second : dummyProgram_;
    }

    gl::GLint GetUniformLocation(ShaderHandle shader, const char* name) {
        Program& prog = const_cast<Program&>(GetProgram(shader));
        auto it = prog.uniforms.find(name);
        if (it != prog.uniforms.end()) return it->second;
        gl::GLint loc = gl::GetGL().GetUniformLocation(prog.id, name);
        prog.uniforms.emplace(name, loc);
        return loc;
    }

    platform::IWindow* window_ = nullptr;
    TextureHandle whiteTex_;
    ShaderHandle currentShader_;
    Program dummyProgram_;
    gl::GLuint linesVao_ = 0, linesVbo_ = 0, linesEbo_ = 0;
    gl::GLuint uiVao_ = 0, uiVbo_ = 0, uiEbo_ = 0;
    gl::GLuint instanceVbo_ = 0;
    gl::GLuint instanceColorVbo_ = 0;
    std::unordered_map<uint32_t, Program> shaders_;
    std::unordered_map<uint32_t, GLMesh> meshes_;
    std::unordered_map<uint32_t, GLTexture> textures_;
    std::unordered_map<uint32_t, GLRenderTarget> renderTargets_;
    uint32_t nextShaderId_ = 0;
    uint32_t nextRenderTargetId_ = 0;
    uint32_t nextTextureId_ = 0;
    bool glReady_ = false;
    bool depthUsable_ = false;
    bool compressedTexSupported_ = false;
    gl::GLuint currentFBO_ = 0;
};

} // namespace

std::unique_ptr<IRenderBackend> CreateOpenGLBackend() {
    return std::make_unique<OpenGLBackend>();
}

} // namespace neon::gfx
