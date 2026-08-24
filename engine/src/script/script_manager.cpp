#include "neon/script/script.hpp"

#include "neon/script/lua_host.hpp"
#include "neon/script/js_host.hpp"

namespace neon::script {

std::unique_ptr<IScriptHost> CreateLuaHost() {
    return std::make_unique<LuaHost>();
}

std::unique_ptr<IScriptHost> CreateJsHost() {
    return std::make_unique<JsHost>();
}

} // namespace neon::script
