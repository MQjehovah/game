#pragma once
#include <memory>

#include "neon/script/script.hpp"

struct lua_State; // forward declaration only; Lua headers stay out of the public interface

namespace neon::script {

// Lua 5.4 implementation of IScriptHost. Lua internals live behind a PIMPL so
// no Lua headers leak into the engine's public interface.
//
// The host opens a deterministic sandbox by default:
//   * restricted standard library (base, table, string, math, utf8, coroutine);
//     io, os, and package are NOT opened
//   * math.random/randomseed + NMath.Random/RandomRange/Seed backed by a
//     host-seeded core::Rng (xorshift64*), default seed a fixed constant
//   * NMath.Time() reports the engine-injected simulated clock
//   * dofile/loadfile/collectgarbage are nilled; print routes to core::Log
//
// Single-threaded: not safe to share across threads.
class LuaHost : public IScriptHost {
public:
    LuaHost();
    ~LuaHost() override;

    LuaHost(const LuaHost&) = delete;
    LuaHost& operator=(const LuaHost&) = delete;

    bool Init() override;
    void Shutdown() override;
    bool Load(const std::string& source) override;
    core::Result<Value> Run() override;
    core::Result<Value> Call(const std::string& fn, const std::vector<Value>& args) override;
    void SetGlobal(const std::string& name, const Value& v) override;
    core::Result<Value> GetGlobal(const std::string& name) override;
    void Register(const std::string& name, NativeFunction fn, void* user) override;
    void RegisterField(const std::string& tableName, const std::string& fieldName,
                       NativeFunction fn, void* user) override;
    int ArgCount() const override;
    Value GetArg(int index) const override;
    void SetError(const std::string& message) override;
    const ScriptError& LastError() const override;
    bool HasFunction(const std::string& fn) const override;
    void SetRngSeed(uint64_t seed) override;
    void SetSimClock(double seconds) override;

private:
    // Raw lua_CFunction closures. Each reads its LuaHost from upvalue 1
    // (lightuserdata), exactly like NativeCallClosure; they implement the
    // sandbox's deterministic RNG / clock / print replacement and are
    // installed during Init(). None of them may keep a nontrivial C++ local
    // alive across a lua_error (longjmp), and none may throw.
    static int MathRandom(lua_State* L);
    static int MathRandomSeed(lua_State* L);
    static int NMathRandom(lua_State* L);
    static int NMathRandomRange(lua_State* L);
    static int NMathSeed(lua_State* L);
    static int NMathTime(lua_State* L);
    static int Print(lua_State* L);

    // Opens the restricted libraries and installs the deterministic sandbox
    // (RNG-backed math.random/NMath, sim-clock NMath.Time, log-routed print,
    // nilled dofile/loadfile/collectgarbage). Called by Init().
    static void OpenSandboxedLibraries(lua_State* L, LuaHost* self);

    static int NativeCallClosure(lua_State* L);
    void CaptureError();
    core::Result<Value> Fail(const std::string& message, int line = 0);

    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace neon::script
