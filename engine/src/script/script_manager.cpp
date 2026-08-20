#include "neon/script/script.hpp"

#include "neon/script/lua_host.hpp"

namespace neon::script {

std::unique_ptr<IScriptHost> CreateLuaHost() {
    return std::make_unique<LuaHost>();
}

} // namespace neon::script
