#include "neon/gfx/gl/gl_loader.hpp"

#include "neon/core/log.hpp"

#if defined(_WIN32)
#include <windows.h>
#elif defined(__APPLE__)
#include <OpenGL/gl3.h>
#else
extern "C" {
typedef void (*NEON_GLXProc)(void);
extern NEON_GLXProc glXGetProcAddressARB(const unsigned char*);
}
#endif

namespace neon::gfx {
namespace gl {

namespace {

void* ResolveProc(const char* name) {
#if defined(_WIN32)
    void* p = reinterpret_cast<void*>(wglGetProcAddress(name));
    if (!p || p == reinterpret_cast<void*>(1) || p == reinterpret_cast<void*>(2) ||
        p == reinterpret_cast<void*>(3) || p == reinterpret_cast<void*>(-1)) {
        static HMODULE module = GetModuleHandleA("opengl32.dll");
        if (!module) {
            NEON_LOG_ERROR("GL: GetModuleHandleA(opengl32.dll) failed, error=%lu", GetLastError());
        }
        p = module ? reinterpret_cast<void*>(GetProcAddress(module, name)) : nullptr;
        if (!p) {
            NEON_LOG_ERROR("GL: GetProcAddress(%s) failed, error=%lu", name, GetLastError());
        }
    }
    return p;
#elif defined(__APPLE__)
    (void)name;
    return nullptr; // macOS exports GL symbols directly
#else
    return reinterpret_cast<void*>(glXGetProcAddressARB(reinterpret_cast<const unsigned char*>(name)));
#endif
}

} // namespace

void* LoadProc(const char* name) { return ResolveProc(name); }

GLProcs& GetGL() {
    static GLProcs procs;
    return procs;
}

bool LoadGLFunctions() {
    GLProcs& g = GetGL();
#if defined(__APPLE__)
#define NEON_GL_FUNC(ret, name, params) g.name = &::gl##name;
#include "neon/gfx/gl/gl_funcs.inc"
#undef NEON_GL_FUNC
    return true;
#else
    bool ok = true;
#define NEON_GL_FUNC(ret, name, params)                                 \
    g.name = reinterpret_cast<ret(NEON_GL_API*) params>(ResolveProc("gl" #name)); \
    if (!g.name) {                                                      \
        NEON_LOG_ERROR("GL: failed to load function %s", #name);        \
        ok = false;                                                     \
    }
#include "neon/gfx/gl/gl_funcs.inc"
#undef NEON_GL_FUNC
    return ok;
#endif
}

} // namespace gl
} // namespace neon::gfx
