#include "neon/script/lua_host.hpp"

#include <exception>
#include <unordered_map>
#include <unordered_set>
#include <utility>

#include "neon/core/log.hpp"
#include "neon/core/rng.hpp"

extern "C" {
#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"
}

namespace neon::script {
namespace {

// Registry key under which the currently loaded chunk is stored.
const void* kChunkKey() {
    static char key = 0;
    return &key;
}

// Registry key under which captured chunk functions are stored:
// registry[capturedKey][handle] = function. A single lightuserdata key cannot
// collide with Lua's reserved integer registry indices (LUA_RIDX_MAINTHREAD =
// 1, LUA_RIDX_GLOBALS = 2), which a raw integer key would clobber and corrupt
// the whole state (the globals table reference lives at index 2).
const void* kCapturedKey() {
    static char key = 0;
    return &key;
}

// A registered native function plus its opaque user pointer.
struct NativeFn {
    NativeFunction fn = nullptr;
    void* user = nullptr;
};

// Fixed seed for every fresh host (never wall time). Matches core::Rng's own
// default so an unseeded host and an explicit SetRngSeed(kDefaultRngSeed)
// produce identical streams.
constexpr uint64_t kDefaultRngSeed = 0x9E3779B97F4A7C15ull;

// 53-bit double in [0, 1) from the top bits of a single xorshift64* draw.
// Bit-exact across hosts with the same seed.
double RngFloat01(uint64_t rv) {
    return static_cast<double>(rv >> 11) * (1.0 / 9007199254740992.0);
}

// Pushes a uniformly chosen integer in [low, up] (inclusive). Deliberate engine
// design decision: the one already-drawn value is projected onto the span with
// modulo, so every call consumes exactly one RNG draw. This is deterministic
// and identical across engine peers with the same seed, but it is NOT
// bit-reproducible with stock Lua's math.random(m,n), which uses rejection
// sampling and consumes a variable number of draws. The full 64-bit span
// (low=INT64_MIN, up=INT64_MAX) cannot be expressed as span+1, so it returns
// the raw draw.
int PushRandomInt(lua_State* L, uint64_t rv, lua_Integer low, lua_Integer up) {
    if (low > up) return luaL_error(L, "bad argument #1 to 'random' (interval is empty)");
    lua_Unsigned span = static_cast<lua_Unsigned>(up) - static_cast<lua_Unsigned>(low);
    lua_Unsigned r = (span == static_cast<lua_Unsigned>(~0ull)) ? rv : rv % (span + 1);
    lua_pushinteger(L, static_cast<lua_Integer>(static_cast<lua_Unsigned>(low) + r));
    return 1;
}

// Pushes a closure whose single upvalue is the LuaHost (lightuserdata), so the
// sandbox functions can reach the host's RNG / sim clock.
void PushHostClosure(lua_State* L, LuaHost* self, lua_CFunction fn) {
    lua_pushlightuserdata(L, self);
    lua_pushcclosure(L, fn, 1);
}

// core::Log can throw (log-buffer allocation in GetRecentLogs, user-supplied
// sinks). It is called from inside Lua closures, where an uncaught C++
// exception would unwind through Lua's setjmp frames and corrupt the state;
// swallow it so print degrades to a silent no-op. Mirrors the NativeCallClosure
// exception discipline.
void SafeLog(const char* msg) {
    try {
        core::Log(core::LogLevel::Info, core::LogCategory::Script, nullptr, 0, "%s", msg);
    } catch (...) {
    }
}

// Extracts the line number from a Lua error message of the form
// "[<source>]:<line>: <message>". Returns 0 when no line is present.
int ParseLineNumber(const std::string& message) {
    for (size_t i = 0; i + 2 < message.size(); ++i) {
        if (message[i] != ':') continue;
        size_t j = i + 1;
        if (j >= message.size() || message[j] < '0' || message[j] > '9') continue;
        int line = 0;
        while (j < message.size() && message[j] >= '0' && message[j] <= '9') {
            line = line * 10 + (message[j] - '0');
            ++j;
        }
        if (j < message.size() && message[j] == ':') return line;
    }
    return 0;
}

// Recursion budget for Lua table conversion. Deeper structures are truncated
// (their offending entry converts to nil) instead of overflowing the C++
// stack. Mirrors the depth used by test_json.cpp's JsonDeepNesting.
constexpr int kMaxConversionDepth = 1000;

// Strips a leading UTF-8 BOM (EF BB BF), which Windows editors and PowerShell
// often prepend to saved files. luaL_loadbuffer does NOT skip a BOM and would
// report "unexpected symbol near '<\239>'", so every load path (GameRuntime,
// the editor's script panel, pack readers) tolerates BOM'd sources here.
void StripUtf8Bom(std::string& s) {
    if (s.size() >= 3 && static_cast<unsigned char>(s[0]) == 0xEF &&
        static_cast<unsigned char>(s[1]) == 0xBB &&
        static_cast<unsigned char>(s[2]) == 0xBF) {
        s.erase(0, 3);
    }
}

void PushValueImpl(lua_State* L, const Value& v, int depth, std::unordered_set<const void*>& seen);
Value PopValueImpl(lua_State* L, int index, int depth, std::unordered_set<const void*>& seen);

void PushValueImpl(lua_State* L, const Value& v, int depth, std::unordered_set<const void*>& seen) {
    switch (v.type) {
        case Value::Type::Nil: lua_pushnil(L); break;
        case Value::Type::Number: lua_pushnumber(L, v.number); break;
        case Value::Type::String: lua_pushlstring(L, v.str.data(), v.str.size()); break;
        case Value::Type::Bool: lua_pushboolean(L, v.boolean ? 1 : 0); break;
        case Value::Type::Table: {
            if (!v.table) {
                lua_newtable(L);
                break;
            }
            // Cycle guard: reaching the same TableValue twice in one conversion
            // (a self-reference, or a shared subtree) truncates to nil. Depth
            // budget caps long chains. Both early-outs push nil so the caller's
            // rawseti/setfield still consumes exactly one value.
            const void* ptr = v.table.get();
            if (depth >= kMaxConversionDepth || !seen.insert(ptr).second) {
                lua_pushnil(L);
                break;
            }
            // lua_newtable/setfield/rawseti push without growing the Lua stack
            // (the release build compiles the overflow check out), so ensure
            // room for this level's table plus a transient key before pushing.
            if (!lua_checkstack(L, 2)) {
                lua_pushnil(L);
                break;
            }
            lua_newtable(L);
            for (size_t i = 0; i < v.table->array.size(); ++i) {
                PushValueImpl(L, v.table->array[i], depth + 1, seen);
                lua_rawseti(L, -2, static_cast<int>(i) + 1);
            }
            for (const auto& kv : v.table->fields) {
                PushValueImpl(L, kv.second, depth + 1, seen);
                lua_setfield(L, -2, kv.first.c_str());
            }
            break;
        }
    }
}

Value PopValueImpl(lua_State* L, int index, int depth, std::unordered_set<const void*>& seen) {
    if (lua_isnil(L, index)) return Value::Nil();
    if (lua_isboolean(L, index)) return Value::Bool(lua_toboolean(L, index) != 0);
    if (lua_isnumber(L, index)) return Value::Num(lua_tonumber(L, index));
    if (lua_isstring(L, index)) {
        size_t len = 0;
        const char* s = lua_tolstring(L, index, &len);
        return Value::Str(std::string(s ? s : "", len));
    }
    if (lua_istable(L, index)) {
        // Cycle guard via the table's stable Lua object pointer (lua_topointer)
        // plus the shared depth budget. Either condition yields nil and stops
        // recursing; no values are pushed on these paths, so the caller's
        // lua_pop keeps the stack balanced.
        const void* ptr = lua_topointer(L, index);
        if (depth >= kMaxConversionDepth || !seen.insert(ptr).second) return Value::Nil();
        // lua_next/pushnil push without growing the Lua stack (the release
        // build compiles the overflow check out); each recursion level holds a
        // key+value pair, so reserve two slots before iterating.
        if (!lua_checkstack(L, 2)) return Value::Nil();
        Value v = Value::Tbl();
        int absIndex = lua_absindex(L, index);
        // Sequence part: contiguous indices 1..rawlen (the # operator).
        lua_Unsigned seq = lua_rawlen(L, absIndex);
        for (lua_Unsigned i = 1; i <= seq; ++i) {
            lua_rawgeti(L, absIndex, static_cast<lua_Integer>(i));
            v.table->array.push_back(PopValueImpl(L, -1, depth + 1, seen));
            lua_pop(L, 1);
        }
        // Remaining string-keyed entries (hash part). Keys that Lua reports as
        // numbers are skipped: they are integer keys, not field names.
        lua_pushnil(L);
        while (lua_next(L, absIndex) != 0) {
            if (lua_isstring(L, -2) && !lua_isnumber(L, -2)) {
                size_t len = 0;
                const char* s = lua_tolstring(L, -2, &len);
                v.table->fields.emplace_back(std::string(s ? s : "", len),
                                             PopValueImpl(L, -1, depth + 1, seen));
            }
            lua_pop(L, 1); // value; the key stays for the next lua_next
        }
        return v;
    }
    return Value::Nil();
}

void PushValue(lua_State* L, const Value& v) {
    // Reserve enough Lua stack for the whole recursion up front so the
    // per-level checks above are cheap no-ops instead of repeated reallocations.
    if (!lua_checkstack(L, kMaxConversionDepth * 2 + 32)) {
        lua_pushnil(L);
        return;
    }
    std::unordered_set<const void*> seen;
    PushValueImpl(L, v, 0, seen);
}

Value PopValue(lua_State* L, int index) {
    if (!lua_checkstack(L, kMaxConversionDepth * 2 + 32)) return Value::Nil();
    std::unordered_set<const void*> seen;
    return PopValueImpl(L, index, 0, seen);
}

} // namespace

struct LuaHost::Impl {
    lua_State* L = nullptr;
    bool hasChunk = false;
    ScriptError lastError;
    std::unordered_map<std::string, NativeFn> nativeFns;
    std::vector<int> frameArgCounts;
    bool nativeErrorPending = false;
    std::string nativeErrorMessage;
    uint64_t nextFunctionKey = 0; // registry keys for captured chunk functions
    core::Rng rng;          // sandbox RNG; reseeded by SetRngSeed / NMath.Seed
    double simClock = 0.0;  // engine-injected simulated time (NMath.Time)
};

// Opens the restricted standard library, then applies the deterministic
// sandbox. io, os, and package are never opened (require stays nil). math.random
// /randomseed and the NMath.* API are backed by the host's core::Rng, NMath.Time
// returns the engine-injected sim clock, print routes to core::Log, and
// dofile/loadfile/collectgarbage are nilled.
void LuaHost::OpenSandboxedLibraries(lua_State* L, LuaHost* self) {
    static const struct {
        const char* name;
        lua_CFunction open;
    } kLibraries[] = {
        {LUA_GNAME, luaopen_base},
        {LUA_COLIBNAME, luaopen_coroutine},
        {LUA_TABLIBNAME, luaopen_table},
        {LUA_STRLIBNAME, luaopen_string},
        {LUA_MATHLIBNAME, luaopen_math},
        {LUA_UTF8LIBNAME, luaopen_utf8},
    };
    for (const auto& lib : kLibraries) {
        luaL_requiref(L, lib.name, lib.open, 1);
        lua_pop(L, 1);
    }

    // File load/execute and GC control are outside the deterministic core.
    lua_pushnil(L);
    lua_setglobal(L, "dofile");
    lua_pushnil(L);
    lua_setglobal(L, "loadfile");
    lua_pushnil(L);
    lua_setglobal(L, "collectgarbage");

    // print -> engine log (a contained side effect instead of raw stdout).
    PushHostClosure(L, self, &LuaHost::Print);
    lua_setglobal(L, "print");

    // math.random/randomseed -> host RNG (deterministic, host-seeded).
    lua_getglobal(L, LUA_MATHLIBNAME);
    PushHostClosure(L, self, &LuaHost::MathRandom);
    lua_setfield(L, -2, "random");
    PushHostClosure(L, self, &LuaHost::MathRandomSeed);
    lua_setfield(L, -2, "randomseed");
    lua_pop(L, 1);

    // NMath: the canonical deterministic API. Unlike the stdlib math.random
    // legacy surface, its contract is stable across Lua versions. Both math.*
    // and NMath.* share one host RNG and use the engine's fixed one-draw-per-call
    // modulo sampling (see PushRandomInt); streams are reproducible across engine
    // peers, not bit-identical to stock Lua.
    lua_newtable(L);
    PushHostClosure(L, self, &LuaHost::NMathRandom);
    lua_setfield(L, -2, "Random");
    PushHostClosure(L, self, &LuaHost::NMathRandomRange);
    lua_setfield(L, -2, "RandomRange");
    PushHostClosure(L, self, &LuaHost::NMathSeed);
    lua_setfield(L, -2, "Seed");
    PushHostClosure(L, self, &LuaHost::NMathTime);
    lua_setfield(L, -2, "Time");
    lua_setglobal(L, "NMath");
}

LuaHost::LuaHost() : impl_(std::make_unique<Impl>()) {}

LuaHost::~LuaHost() { Shutdown(); }

bool LuaHost::Init() {
    if (impl_->L) return true; // already initialized
    lua_State* L = luaL_newstate();
    if (!L) return false; // never leaks: nothing has been allocated yet
    impl_->L = L;
    // A fresh state starts with a fresh, fixed-seed RNG and a zero clock, so a
    // re-initialized host behaves like a newly created one.
    impl_->rng = core::Rng(kDefaultRngSeed);
    impl_->simClock = 0.0;
    OpenSandboxedLibraries(L, this);
    return true;
}

void LuaHost::Shutdown() {
    if (impl_->L) {
        lua_close(impl_->L);
        impl_->L = nullptr;
    }
    impl_->hasChunk = false;
    impl_->lastError = {};
}

void LuaHost::CaptureError() {
    size_t len = 0;
    const char* msg = lua_tolstring(impl_->L, -1, &len);
    impl_->lastError.message.assign(msg ? msg : "unknown script error", len);
    impl_->lastError.line = ParseLineNumber(impl_->lastError.message);
    lua_pop(impl_->L, 1);
}

core::Result<Value> LuaHost::Fail(const std::string& message, int line) {
    impl_->lastError = {message, line};
    return core::Result<Value>::Err(message);
}

// Lua-side entry point for every registered native function. Upvalues: [1] is
// a lightuserdata pointing to the LuaHost, [2] is the registered name (string).
// While `fn` runs, the script arguments sit at stack indices 1..n, which is
// what ArgCount/GetArg read via the recorded frame count.
//
// Error discipline: this closure is a C++ function called from Lua's C code,
// and lua_error() longjmps back through Lua's setjmp frames, skipping C++
// destructors. lua_error() must therefore only be called when no nontrivial
// C++ local (a Value holding a string, a std::string) is alive. The fn call
// and its result therefore live in the inner scope below (the result is
// destroyed before any raise), and the error message is pushed straight from
// the Impl field, never copied into a local. C++ exceptions from `fn` are
// caught and funneled through the same path so they never cross Lua's setjmp
// frames (which would leave the state permanently corrupt).
int LuaHost::NativeCallClosure(lua_State* L) {
    LuaHost* self = static_cast<LuaHost*>(lua_touserdata(L, lua_upvalueindex(1)));
    const char* name = lua_tostring(L, lua_upvalueindex(2));
    if (!self || !name) return luaL_error(L, "invalid native function registration");

    // Copy the registration into a trivially destructible local so nothing
    // non-trivial stays alive across the lua_error below.
    NativeFn nf;
    {
        auto it = self->impl_->nativeFns.find(name);
        if (it == self->impl_->nativeFns.end())
            return luaL_error(L, "native function '%s' is not registered", name);
        nf = it->second;
    }
    if (nf.fn == nullptr) return luaL_error(L, "native function '%s' has no implementation", name);

    self->impl_->frameArgCounts.push_back(lua_gettop(L));
    {
        Value result;
        try {
            result = nf.fn(*self, nf.user);
        } catch (const std::exception& e) {
            self->impl_->lastError = {e.what(), 0};
            self->impl_->nativeErrorMessage = e.what();
            self->impl_->nativeErrorPending = true;
        } catch (...) {
            const char* kMessage = "native function threw a non-std exception";
            self->impl_->lastError = {kMessage, 0};
            self->impl_->nativeErrorMessage = kMessage;
            self->impl_->nativeErrorPending = true;
        }
        if (!self->impl_->nativeErrorPending) {
            self->impl_->frameArgCounts.pop_back();
            PushValue(L, result); // no raise on this path; `result` is destroyed after return
            return 1;
        }
    } // `result` (and any string it held) is destroyed here, before the raise
    self->impl_->frameArgCounts.pop_back();
    self->impl_->nativeErrorPending = false;
    lua_pushlstring(L, self->impl_->nativeErrorMessage.data(),
                    self->impl_->nativeErrorMessage.size());
    return lua_error(L);
}

bool LuaHost::Load(const std::string& source) {
    if (!impl_->L) return false;
    std::string src = source;
    StripUtf8Bom(src);
    if (luaL_loadbuffer(impl_->L, src.data(), src.size(), "[neon]") != LUA_OK) {
        CaptureError();
        impl_->hasChunk = false;
        return false;
    }
    // Move the compiled chunk into the registry and pop it from the stack.
    lua_pushlightuserdata(impl_->L, const_cast<void*>(kChunkKey()));
    lua_pushvalue(impl_->L, -2);
    lua_rawset(impl_->L, LUA_REGISTRYINDEX);
    lua_pop(impl_->L, 1);
    impl_->hasChunk = true;
    impl_->lastError = {};
    return true;
}

bool LuaHost::CheckSyntax(const std::string& source) {
    if (!impl_->L) return false;
    std::string src = source;
    StripUtf8Bom(src);
    if (luaL_loadbuffer(impl_->L, src.data(), src.size(), "[neon]") != LUA_OK) {
        CaptureError();
        return false;
    }
    impl_->lastError = {};
    lua_pop(impl_->L, 1); // discard the compiled chunk: the loaded chunk is untouched
    return true;
}

core::Result<Value> LuaHost::Run() {
    if (!impl_->L) return Fail("script host is not initialized");
    if (!impl_->hasChunk) return Fail("no script chunk loaded");
    lua_pushlightuserdata(impl_->L, const_cast<void*>(kChunkKey()));
    lua_rawget(impl_->L, LUA_REGISTRYINDEX);
    if (lua_pcall(impl_->L, 0, 1, 0) != LUA_OK) {
        CaptureError();
        return Fail(impl_->lastError.message, impl_->lastError.line);
    }
    Value result = PopValue(impl_->L, -1);
    lua_pop(impl_->L, 1);
    impl_->lastError = {};
    return core::Result<Value>::Ok(result);
}

core::Result<Value> LuaHost::Call(const std::string& fn, const std::vector<Value>& args) {
    if (!impl_->L) return Fail("script host is not initialized");
    lua_getglobal(impl_->L, fn.c_str());
    if (!lua_isfunction(impl_->L, -1)) {
        lua_pop(impl_->L, 1);
        return Fail(fn + " is not a function");
    }
    for (const Value& v : args) PushValue(impl_->L, v);
    if (lua_pcall(impl_->L, static_cast<int>(args.size()), 1, 0) != LUA_OK) {
        CaptureError();
        return Fail(impl_->lastError.message, impl_->lastError.line);
    }
    Value result = PopValue(impl_->L, -1);
    lua_pop(impl_->L, 1);
    impl_->lastError = {};
    return core::Result<Value>::Ok(result);
}

core::Result<uint64_t> LuaHost::CaptureFunction(const std::string& name) {
    if (!impl_->L) return core::Result<uint64_t>::Err("script host is not initialized");
    lua_getglobal(impl_->L, name.c_str());
    if (!lua_isfunction(impl_->L, -1)) {
        lua_pop(impl_->L, 1);
        return core::Result<uint64_t>::Err(name + " is not a function");
    }
    // Park the function in the registry under registry[capturedKey][handle];
    // the handle stays valid even when a later chunk overwrites the global
    // name. Nested under one pointer key keeps integer handles far away from
    // Lua's reserved registry indices (1 = main thread, 2 = globals).
    const uint64_t key = ++impl_->nextFunctionKey;
    lua_pushlightuserdata(impl_->L, const_cast<void*>(kCapturedKey()));
    lua_rawget(impl_->L, LUA_REGISTRYINDEX);
    if (!lua_istable(impl_->L, -1)) {
        lua_pop(impl_->L, 1);
        lua_newtable(impl_->L);
        lua_pushlightuserdata(impl_->L, const_cast<void*>(kCapturedKey()));
        lua_pushvalue(impl_->L, -2);
        lua_rawset(impl_->L, LUA_REGISTRYINDEX);
    }
    lua_pushinteger(impl_->L, static_cast<lua_Integer>(key));
    lua_pushvalue(impl_->L, -3); // the function (below the table)
    // Stack here is [fn, table, key, fn2]: the captured table sits at -3.
    // lua_rawset pops key + value and stores into the table at the index.
    lua_rawset(impl_->L, -3);    // captured[key] = fn
    lua_pop(impl_->L, 1);        // the table
    lua_pop(impl_->L, 1);        // the global copy
    impl_->lastError = {};
    return core::Result<uint64_t>::Ok(key);
}

core::Result<Value> LuaHost::CallCaptured(uint64_t handle, const std::vector<Value>& args) {
    if (!impl_->L) return Fail("script host is not initialized");
    lua_pushlightuserdata(impl_->L, const_cast<void*>(kCapturedKey()));
    lua_rawget(impl_->L, LUA_REGISTRYINDEX);
    if (!lua_istable(impl_->L, -1)) {
        lua_pop(impl_->L, 1);
        return Fail("captured function registry is missing");
    }
    lua_pushinteger(impl_->L, static_cast<lua_Integer>(handle));
    lua_rawget(impl_->L, -2);
    lua_remove(impl_->L, -2); // drop the table, keep the function
    if (!lua_isfunction(impl_->L, -1)) {
        lua_pop(impl_->L, 1);
        return Fail("captured function handle is invalid");
    }
    for (const Value& v : args) PushValue(impl_->L, v);
    if (lua_pcall(impl_->L, static_cast<int>(args.size()), 1, 0) != LUA_OK) {
        CaptureError();
        return Fail(impl_->lastError.message, impl_->lastError.line);
    }
    Value result = PopValue(impl_->L, -1);
    lua_pop(impl_->L, 1);
    impl_->lastError = {};
    return core::Result<Value>::Ok(result);
}

void LuaHost::SetGlobal(const std::string& name, const Value& v) {
    if (!impl_->L) return;
    PushValue(impl_->L, v);
    lua_setglobal(impl_->L, name.c_str());
}

core::Result<Value> LuaHost::GetGlobal(const std::string& name) {
    if (!impl_->L) return Fail("script host is not initialized");
    lua_getglobal(impl_->L, name.c_str());
    Value v = PopValue(impl_->L, -1);
    lua_pop(impl_->L, 1);
    return core::Result<Value>::Ok(v);
}

void LuaHost::Register(const std::string& name, NativeFunction fn, void* user) {
    if (!impl_->L) return;
    impl_->nativeFns[name] = NativeFn{fn, user};
    lua_pushlightuserdata(impl_->L, this);
    lua_pushlstring(impl_->L, name.data(), name.size());
    lua_pushcclosure(impl_->L, &LuaHost::NativeCallClosure, 2);
    lua_setglobal(impl_->L, name.c_str());
}

void LuaHost::RegisterField(const std::string& tableName, const std::string& fieldName,
                            NativeFunction fn, void* user) {
    if (!impl_->L) return;
    // Look up the closure by its fully qualified name so fields of different
    // tables cannot collide (the closure's name upvalue is the dotted key).
    const std::string fullName = tableName + "." + fieldName;
    impl_->nativeFns[fullName] = NativeFn{fn, user};
    lua_getglobal(impl_->L, tableName.c_str());
    if (!lua_istable(impl_->L, -1)) {
        lua_pop(impl_->L, 1);
        lua_newtable(impl_->L);
        lua_pushvalue(impl_->L, -1);
        lua_setglobal(impl_->L, tableName.c_str());
    }
    lua_pushlightuserdata(impl_->L, this);
    lua_pushlstring(impl_->L, fullName.data(), fullName.size());
    lua_pushcclosure(impl_->L, &LuaHost::NativeCallClosure, 2);
    lua_setfield(impl_->L, -2, fieldName.c_str());
    lua_pop(impl_->L, 1);
}

int LuaHost::ArgCount() const {
    if (impl_->frameArgCounts.empty()) return 0;
    return impl_->frameArgCounts.back();
}

Value LuaHost::GetArg(int index) const {
    if (impl_->frameArgCounts.empty()) return Value::Nil();
    int argCount = impl_->frameArgCounts.back();
    if (index < 0 || index >= argCount) return Value::Nil();
    return PopValue(impl_->L, index + 1); // Lua arguments start at stack index 1
}

void LuaHost::SetError(const std::string& message) {
    impl_->nativeErrorPending = true;
    impl_->nativeErrorMessage = message;
    impl_->lastError = {message, 0};
}

const ScriptError& LuaHost::LastError() const { return impl_->lastError; }

bool LuaHost::HasFunction(const std::string& fn) const {
    if (!impl_->L) return false;
    lua_getglobal(impl_->L, fn.c_str());
    bool isFunction = lua_isfunction(impl_->L, -1) != 0;
    lua_pop(impl_->L, 1);
    return isFunction;
}

void LuaHost::SetRngSeed(uint64_t seed) { impl_->rng = core::Rng(seed); }

void LuaHost::SetSimClock(double seconds) { impl_->simClock = seconds; }

// math.random(): [0,1); math.random(n): integer [1,n]; math.random(m,n):
// integer [m,n]; math.random(0): full random integer. Matches Lua 5.4's calling
// conventions (arity, ranges, "interval is empty" error) but the integer path
// uses the engine's fixed one-draw-per-call modulo sampling, so streams are
// deterministic across engine peers yet not bit-identical to stock Lua's
// math.random(m,n).
int LuaHost::MathRandom(lua_State* L) {
    LuaHost* self = static_cast<LuaHost*>(lua_touserdata(L, lua_upvalueindex(1)));
    if (!self) return luaL_error(L, "invalid sandbox closure");
    uint64_t rv = self->impl_->rng.Next(); // always one draw per call, like stock Lua
    switch (lua_gettop(L)) {
        case 0:
            lua_pushnumber(L, RngFloat01(rv));
            return 1;
        case 1: {
            lua_Integer up = luaL_checkinteger(L, 1);
            if (up == 0) {
                lua_pushinteger(L, static_cast<lua_Integer>(rv));
                return 1;
            }
            return PushRandomInt(L, rv, 1, up);
        }
        case 2: {
            lua_Integer low = luaL_checkinteger(L, 1);
            lua_Integer up = luaL_checkinteger(L, 2);
            return PushRandomInt(L, rv, low, up);
        }
        default:
            return luaL_error(L, "wrong number of arguments");
    }
}

// math.randomseed(x) reseeds the host RNG deterministically. With no argument,
// stock Lua 5.4 reseeds from wall time; the sandbox reseeds from the fixed
// default constant instead, keeping every stream reproducible. Documented
// deviations from stock Lua 5.4: only the first argument is used (a second
// seed argument is accepted and ignored) and one value is returned rather than
// two, because the host RNG has a single 64-bit state.
int LuaHost::MathRandomSeed(lua_State* L) {
    LuaHost* self = static_cast<LuaHost*>(lua_touserdata(L, lua_upvalueindex(1)));
    if (!self) return luaL_error(L, "invalid sandbox closure");
    lua_Integer seed = lua_gettop(L) >= 1
        ? luaL_checkinteger(L, 1)
        : static_cast<lua_Integer>(kDefaultRngSeed);
    self->impl_->rng = core::Rng(static_cast<uint64_t>(seed));
    lua_pushinteger(L, seed);
    return 1;
}

// NMath.Random shares the host RNG with math.random, so mixing both in one
// stream stays deterministic.
int LuaHost::NMathRandom(lua_State* L) { return MathRandom(L); }

int LuaHost::NMathRandomRange(lua_State* L) {
    LuaHost* self = static_cast<LuaHost*>(lua_touserdata(L, lua_upvalueindex(1)));
    if (!self) return luaL_error(L, "invalid sandbox closure");
    uint64_t rv = self->impl_->rng.Next();
    switch (lua_gettop(L)) {
        case 1: {
            lua_Integer up = luaL_checkinteger(L, 1);
            return PushRandomInt(L, rv, 1, up);
        }
        case 2: {
            lua_Integer low = luaL_checkinteger(L, 1);
            lua_Integer up = luaL_checkinteger(L, 2);
            return PushRandomInt(L, rv, low, up);
        }
        default:
            return luaL_error(L, "RandomRange expects 1 or 2 arguments");
    }
}

int LuaHost::NMathSeed(lua_State* L) {
    LuaHost* self = static_cast<LuaHost*>(lua_touserdata(L, lua_upvalueindex(1)));
    if (!self) return luaL_error(L, "invalid sandbox closure");
    lua_Integer seed = lua_gettop(L) >= 1
        ? luaL_checkinteger(L, 1)
        : static_cast<lua_Integer>(kDefaultRngSeed);
    self->impl_->rng = core::Rng(static_cast<uint64_t>(seed));
    // Returns the applied seed, mirroring math.randomseed, so scripts can
    // observe/replay the reseed value.
    lua_pushinteger(L, seed);
    return 1;
}

int LuaHost::NMathTime(lua_State* L) {
    LuaHost* self = static_cast<LuaHost*>(lua_touserdata(L, lua_upvalueindex(1)));
    if (!self) return luaL_error(L, "invalid sandbox closure");
    lua_pushnumber(L, self->impl_->simClock);
    return 1;
}

// print(...) -> core::Log(Info). Arguments are tostring'd and tab-joined on the
// Lua stack (lua_concat never raises for strings), so no nontrivial C++ local
// is alive across the raising luaL_tolstring conversions. The log write itself
// goes through SafeLog so an exception from core::Log (allocation, user sinks)
// is contained instead of unwinding through Lua's setjmp frames.
int LuaHost::Print(lua_State* L) {
    LuaHost* self = static_cast<LuaHost*>(lua_touserdata(L, lua_upvalueindex(1)));
    if (!self) return luaL_error(L, "invalid sandbox closure");
    int n = lua_gettop(L);
    if (n > 0) {
        int count = 0;
        for (int i = 1; i <= n; ++i) {
            luaL_tolstring(L, i, nullptr); // pushes tostring(value at i)
            if (i > 1) lua_pushliteral(L, "\t");
            count += (i > 1 ? 2 : 1);
        }
        lua_concat(L, count);
        const char* msg = lua_tostring(L, -1);
        SafeLog(msg ? msg : "");
        lua_pop(L, 1);
    } else {
        SafeLog("");
    }
    return 0;
}

} // namespace neon::script
