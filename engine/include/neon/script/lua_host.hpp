#pragma once
#include <memory>

#include "neon/script/script.hpp"

struct lua_State; // forward declaration only; Lua headers stay out of the public interface

namespace neon::script {

// Lua 5.4 implementation of IScriptHost. Lua internals live behind a PIMPL so
// no Lua headers leak into the engine's public interface.
//
// The host opens a restricted standard library (base, table, string, math,
// utf8, coroutine); io, os, and package are intentionally NOT opened to keep
// script behavior deterministic (a full sandbox is a later task).
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
    int ArgCount() const override;
    Value GetArg(int index) const override;
    void SetError(const std::string& message) override;
    const ScriptError& LastError() const override;
    bool HasFunction(const std::string& fn) const override;

private:
    static int NativeCallClosure(lua_State* L);
    void CaptureError();
    core::Result<Value> Fail(const std::string& message, int line = 0);

    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace neon::script
