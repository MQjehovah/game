#include "neon/script/script.hpp"

#include "neon/core/log.hpp"
#include "neon/script/lua_host.hpp"
#include "neon/script/python_host.hpp"
#ifdef NEON_ENABLE_JS
#include "neon/script/js_host.hpp"
#endif

namespace neon::script {

std::unique_ptr<IScriptHost> CreateLuaHost() {
    return std::make_unique<LuaHost>();
}

#ifdef NEON_ENABLE_JS
std::unique_ptr<IScriptHost> CreateJsHost() {
    return std::make_unique<JsHost>();
}
#else
// QuickJS is optional (NEON_ENABLE_JS=OFF): the JS backend is not compiled, so
// callers asking for a JS host get nullptr and scenes fall back to Lua. This
// keeps engine code (runtime/plugins) compiling without the vendored C code,
// which does not build on every toolchain (MSVC / GCC 8).
std::unique_ptr<IScriptHost> CreateJsHost() {
    return nullptr;
}
#endif

#ifdef NEON_ENABLE_PYTHON
std::unique_ptr<IScriptHost> CreatePythonHost() {
    return std::make_unique<PythonHost>();
}
#else
// Python (CPython) host is not compiled by default (needs a Python runtime +
// headers). Callers fall back to Lua; enable NEON_ENABLE_PYTHON to build it.
std::unique_ptr<IScriptHost> CreatePythonHost() {
    return nullptr;
}
#endif

std::unique_ptr<IScriptHost> CreateScriptHost(const std::string& kind) {
    if (kind == "lua") return CreateLuaHost();
    if (kind == "js") return CreateJsHost();
    if (kind == "python") return CreatePythonHost();
    return nullptr; // unknown language
}

} // namespace neon::script
