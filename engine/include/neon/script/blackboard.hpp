#pragma once
#include <cstdint>
#include <map>
#include <string>

#include "neon/script/script.hpp"

namespace neon::script {

// Entity-scoped key-value store shared by scripts and the behavior tree engine
// (T2.5). Mirrors GameVars semantics but scopes values per entity, so several
// entities can run concurrent trees (and concurrent scripts) without
// clobbering each other. Deliberately dependency-light like GameVars: it does
// not know about IScriptHost or any runtime.
//
// Not thread-safe: one instance belongs to one thread.
class Blackboard {
public:
    // Value stored for `entity` under `name`, or Nil when the key is missing.
    Value Get(uint64_t entity, const std::string& name) const;
    void Set(uint64_t entity, const std::string& name, const Value& v);
    bool Has(uint64_t entity, const std::string& name) const;
    void Clear();
    size_t Size() const { return per_.size(); }

private:
    std::map<uint64_t, std::map<std::string, Value>> per_;
};

inline Value Blackboard::Get(uint64_t entity, const std::string& name) const {
    auto ent = per_.find(entity);
    if (ent == per_.end()) return Value::Nil();
    auto it = ent->second.find(name);
    return it != ent->second.end() ? it->second : Value::Nil();
}

inline void Blackboard::Set(uint64_t entity, const std::string& name, const Value& v) {
    per_[entity][name] = v;
}

inline bool Blackboard::Has(uint64_t entity, const std::string& name) const {
    auto ent = per_.find(entity);
    if (ent == per_.end()) return false;
    return ent->second.find(name) != ent->second.end();
}

inline void Blackboard::Clear() {
    per_.clear();
}

} // namespace neon::script
