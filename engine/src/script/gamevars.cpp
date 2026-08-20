#include "neon/script/gamevars.hpp"

namespace neon::script {

Value GameVars::Get(const std::string& name) const {
    auto it = vars_.find(name);
    return it != vars_.end() ? it->second : Value::Nil();
}

void GameVars::Set(const std::string& name, const Value& v) {
    vars_[name] = v;
}

bool GameVars::Has(const std::string& name) const {
    return vars_.find(name) != vars_.end();
}

void GameVars::Clear() {
    vars_.clear();
}

} // namespace neon::script
