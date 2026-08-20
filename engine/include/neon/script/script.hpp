#pragma once
#include <memory>
#include <string>
#include <vector>

#include "neon/core/result.hpp"

namespace neon::script {

// Details of the most recent script failure. `line` is the source line where
// the error occurred, or 0 when the host could not determine one.
struct ScriptError {
    std::string message;
    int line = 0;
};

// A scalar value exchanged between the engine and a script host.
struct Value {
    enum class Type { Nil, Number, String, Bool };

    Type type = Type::Nil;
    double number = 0.0;
    std::string str;
    bool boolean = false;

    static Value Nil() { return Value{}; }
    static Value Num(double n) {
        Value v;
        v.type = Type::Number;
        v.number = n;
        return v;
    }
    static Value Str(std::string s) {
        Value v;
        v.type = Type::String;
        v.str = std::move(s);
        return v;
    }
    static Value Bool(bool b) {
        Value v;
        v.type = Type::Bool;
        v.boolean = b;
        return v;
    }
};

// Backend-agnostic interface for executing embedded scripts. A host is
// single-threaded: all calls must originate from the same thread.
class IScriptHost {
public:
    virtual ~IScriptHost() = default;

    // Create the backing runtime. Idempotent: calling it again on an already
    // initialized host is a no-op.
    virtual bool Init() = 0;

    // Tear down the backing runtime. Safe to call repeatedly and from the
    // destructor; after this, other methods report errors / are no-ops until
    // Init() brings the host back.
    virtual void Shutdown() = 0;

    // Compile `source` without executing it. Returns false (and records the
    // error in LastError) on a syntax error. A successful Load replaces the
    // previously loaded chunk, which is then executed by Run().
    virtual bool Load(const std::string& source) = 0;

    // Execute the chunk loaded by the last successful Load(). Errors when no
    // valid chunk is loaded or the chunk raises. Returns the chunk's value
    // (nil for most chunks).
    virtual core::Result<Value> Run() = 0;

    // Call the global function `fn` with the given arguments. Errors when the
    // global is not a function or the call raises.
    virtual core::Result<Value> Call(const std::string& fn, const std::vector<Value>& args) = 0;

    // Read/write a global variable. Missing globals read back as Nil.
    virtual void SetGlobal(const std::string& name, const Value& v) = 0;
    virtual core::Result<Value> GetGlobal(const std::string& name) = 0;

    // Details of the most recent Load/Run/Call error. Valid until the next
    // script operation clears it.
    virtual const ScriptError& LastError() const = 0;

    // True if the global `fn` currently names a callable function. Useful for
    // event dispatch ("does an onUpdate handler exist?").
    virtual bool HasFunction(const std::string& fn) const = 0;
};

// Factory: creates the engine's Lua-backed host (implemented in
// script_manager.cpp).
std::unique_ptr<IScriptHost> CreateLuaHost();

} // namespace neon::script
