#pragma once

namespace neon::gfx {
namespace gl {

#if defined(_WIN32)
#define NEON_GL_API __stdcall
#else
#define NEON_GL_API
#endif

using GLenum = unsigned int;
using GLuint = unsigned int;
using GLint = int;
using GLsizei = int;
using GLfloat = float;
using GLbitfield = unsigned int;
using GLsizeiptr = long long;
using GLintptr = long long;
using GLboolean = unsigned char;
using GLchar = char;

struct GLProcs {
#define NEON_GL_FUNC(ret, name, params) ret(NEON_GL_API* name) params;
#include "gl_funcs.inc"
#undef NEON_GL_FUNC
};

GLProcs& GetGL();
bool LoadGLFunctions();
void* LoadProc(const char* name);

} // namespace gl
} // namespace neon::gfx
