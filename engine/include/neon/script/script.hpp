#pragma once
#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "neon/core/result.hpp"

namespace neon::script {

struct Value;
struct TableValue;

// A scalar value exchanged between the engine and a script host.
struct Value {
    enum class Type { Nil, Number, String, Bool, Table };

    Type type = Type::Nil;
    double number = 0.0;
    std::string str;
    bool boolean = false;

    // Lua-style table (Type::Table). `array` is the 1-based sequence (index i
    // lives at array[i-1]); `fields` holds string-keyed entries. Nil array
    // entries are dropped when pushed to Lua (rawseti with nil removes the
    // key), which matches how JSON null behaves in Lua.
    std::shared_ptr<TableValue> table;

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
    static Value Tbl() {
        Value v;
        v.type = Type::Table;
        v.table = std::make_shared<TableValue>();
        return v;
    }
};

// Shared payload of a table-typed Value. Defined after Value so its
// std::vector<Value> members see a complete type.
struct TableValue {
    std::vector<Value> array;
    std::vector<std::pair<std::string, Value>> fields;
};

// Details of the most recent script failure. `line` is the source line where
// the error occurred, or 0 when the host could not determine one.
struct ScriptError {
    std::string message;
    int line = 0;
};

// A native (C++) function callable from scripts. `host` is the calling host:
// the implementation can read its arguments with ArgCount/GetArg and return a
// Value. `user` is the opaque pointer supplied at registration time.
//
// Implementations must not let C++ exceptions escape. The host catches them
// (a caught exception surfaces as a script error), but unwinding through
// Lua's setjmp-based frames is undefined behavior, so treat the call as
// exception-free C++.
class IScriptHost;
using NativeFunction = Value (*)(IScriptHost& host, void* user);

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

    // Validate `source` without executing or installing it. Returns true when
    // the source is syntactically valid; on a syntax error records the message
    // + line in LastError and returns false. Unlike Load, a successful check
    // does NOT replace the previously loaded chunk, so a host can lint files
    // (the editor's script panel) without disturbing a running script.
    virtual bool CheckSyntax(const std::string& source) = 0;

    // Execute the chunk loaded by the last successful Load(). Errors when no
    // valid chunk is loaded or the chunk raises. Returns the chunk's value
    // (nil for most chunks).
    virtual core::Result<Value> Run() = 0;

    // Call the global function `fn` with the given arguments. Errors when the
    // global is not a function or the call raises.
    virtual core::Result<Value> Call(const std::string& fn, const std::vector<Value>& args) = 0;

    // Captures the CURRENT global function `name` into an opaque handle so a
    // later chunk overwriting the same global cannot change which function
    // this caller invokes (per-entity script isolation: every ScriptInst
    // captures its own chunk's on_start/on_update). Returns Err (handle 0)
    // when the global is not a function. The handle stays valid until
    // Shutdown.
    virtual core::Result<uint64_t> CaptureFunction(const std::string& name) = 0;

    // Calls a function previously captured by CaptureFunction. Errors when the
    // handle is invalid or the call raises. Same semantics as Call otherwise.
    virtual core::Result<Value> CallCaptured(uint64_t handle,
                                             const std::vector<Value>& args) = 0;

    // Read/write a global variable. Missing globals read back as Nil.
    virtual void SetGlobal(const std::string& name, const Value& v) = 0;
    virtual core::Result<Value> GetGlobal(const std::string& name) = 0;

    // Register a native function under a global name so scripts can call it.
    // Overwrites any previous registration with the same name. `fn` runs on
    // the host's thread; inside it, ArgCount/GetArg read its arguments and
    // SetError raises a script error at the call boundary.
    virtual void Register(const std::string& name, NativeFunction fn, void* user = nullptr) = 0;

    // Register a native function as `tableName.fieldName`, creating the table
    // when it does not exist yet (used for namespaced APIs like Json.Parse).
    // Identical semantics to Register otherwise.
    virtual void RegisterField(const std::string& tableName, const std::string& fieldName,
                               NativeFunction fn, void* user = nullptr) = 0;

    // Number of arguments passed to the currently executing native function.
    // Valid only inside a native call; returns 0 outside one.
    virtual int ArgCount() const = 0;

    // The index-th argument (0-based) of the currently executing native
    // function. Returns Nil when called outside a native call or with an
    // out-of-range index.
    virtual Value GetArg(int index) const = 0;

    // Signal an error from inside a native function. The host raises it at the
    // script call boundary and records it in LastError.
    virtual void SetError(const std::string& message) = 0;

    // Details of the most recent Load/Run/Call error. Valid until the next
    // script operation clears it.
    virtual const ScriptError& LastError() const = 0;

    // True if the global `fn` currently names a callable function. Useful for
    // event dispatch ("does an onUpdate handler exist?").
    virtual bool HasFunction(const std::string& fn) const = 0;

    // Seed the host's deterministic random number generator (math.random /
    // NMath.Random). Two hosts seeded identically produce identical streams;
    // the default seed is a fixed constant, never wall time. Init() resets the
    // RNG to that default.
    //
    // Seed 0 is treated as 1: core::Rng cannot store a zero state (an
    // all-zeros xorshift state would lock the generator), so 0 aliases the
    // seed-1 stream.
    virtual void SetRngSeed(uint64_t seed) = 0;

    // Pin the simulated clock reported by NMath.Time() (and future os.clock).
    // The default is 0; script time is always engine-injected, never wall time.
    virtual void SetSimClock(double seconds) = 0;
};

// Factory: creates the engine's Lua-backed host (implemented in
// script_manager.cpp).
std::unique_ptr<IScriptHost> CreateLuaHost();

} // namespace neon::script
