#pragma once
#include <functional>
#include <map>
#include <string>

#include "neon/script/script.hpp"

namespace neon::script {

// Standalone global key-value store shared by scripts and the behavior tree
// engine (T2.5). Deliberately dependency-light: it does not know about
// IScriptHost or any runtime, so it can be unit-tested directly.
//
// Not thread-safe: one instance belongs to one thread (the script host is
// single-threaded). Table-typed Values share their payload via shared_ptr, so
// copying a Value aliases the same table rather than deep-copying it.
class GameVars {
public:
    // Value stored under `name`, or Nil when the key is missing.
    Value Get(const std::string& name) const;
    void Set(const std::string& name, const Value& v);
    bool Has(const std::string& name) const;
    void Clear();
    size_t Size() const { return vars_.size(); }

    // Visits every stored key/value in ascending key order (used by the player
    // to dump a session's GameVars for smoke-test verification). Single-
    // threaded store; the visitor must not call Set/Get on the same store.
    void ForEach(const std::function<void(const std::string&, const Value&)>& visit) const;

private:
    std::map<std::string, Value> vars_;
};

} // namespace neon::script
