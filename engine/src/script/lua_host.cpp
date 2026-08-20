#include "neon/script/lua_host.hpp"

#include <exception>
#include <unordered_map>
#include <unordered_set>
#include <utility>

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

// A registered native function plus its opaque user pointer.
struct NativeFn {
    NativeFunction fn = nullptr;
    void* user = nullptr;
};

// Opens only the deterministic subset of the standard libraries. io, os,
// package, and debug are intentionally omitted, and dofile/loadfile (arbitrary
// file read + execute) are stripped from the base library.
void OpenRestrictedLibraries(lua_State* L) {
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
    // luaopen_base exposes dofile/loadfile, which read and execute arbitrary
    // files. Close them for determinism; T2.4's sandbox task decides on `load`.
    lua_pushnil(L);
    lua_setglobal(L, "dofile");
    lua_pushnil(L);
    lua_setglobal(L, "loadfile");
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
};

LuaHost::LuaHost() : impl_(std::make_unique<Impl>()) {}

LuaHost::~LuaHost() { Shutdown(); }

bool LuaHost::Init() {
    if (impl_->L) return true; // already initialized
    lua_State* L = luaL_newstate();
    if (!L) return false; // never leaks: nothing has been allocated yet
    impl_->L = L;
    OpenRestrictedLibraries(L);
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
    if (luaL_loadbuffer(impl_->L, source.data(), source.size(), "[neon]") != LUA_OK) {
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

} // namespace neon::script
