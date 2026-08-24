#include "neon/script/script.hpp"

#include "neon/script/lua_host.hpp"
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

} // namespace neon::script
