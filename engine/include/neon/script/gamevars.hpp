#pragma once
#include <map>
#include <string>

#include "neon/script/script.hpp"

namespace neon::script {

// Standalone global key-value store shared by scripts and the behavior tree
// engine (T2.5). Deliberately dependency-light: it does not know about
// IScriptHost or any runtime, so it can be unit-tested directly.
class GameVars {
public:
    // Value stored under `name`, or Nil when the key is missing.
    Value Get(const std::string& name) const;
    void Set(const std::string& name, const Value& v);
    bool Has(const std::string& name) const;
    void Clear();
    size_t Size() const { return vars_.size(); }

private:
    std::map<std::string, Value> vars_;
};

} // namespace neon::script
