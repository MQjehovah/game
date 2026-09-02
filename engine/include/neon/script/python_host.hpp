#pragma once

// E (multi-language host): Python (CPython) host. Same IScriptHost semantics as
// Lua/JS, so it can serve BOTH gameplay and editor tooling. It is gated by
// NEON_ENABLE_PYTHON (which requires a CPython runtime + headers, e.g. via
// CMake find_package(Python3)): enabled -> CreatePythonHost() returns a host,
// disabled -> nullptr and callers fall back to Lua, keeping engine/editor code
// linkable without a Python runtime.
//
// The Python host mirrors the Lua/JS determinism sandbox (instruction/memory
// budget, injected clock, deterministic RNG) so Python gameplay can participate
// in the server-authoritative deterministic replay acceptance; that sandbox and
// the CPython C-API binding live in python_host.cpp under NEON_ENABLE_PYTHON.

#include <memory>

#include "neon/script/script.hpp"

namespace neon::script {

#ifdef NEON_ENABLE_PYTHON
// CPython-backed IScriptHost. Full method set mirrors LuaHost/JsHost; the
// CPython C-API wiring and the determinism sandbox are implemented in
// python_host.cpp (built only when NEON_ENABLE_PYTHON is on).
class PythonHost final : public IScriptHost {
public:
    ~PythonHost() override;
    bool Init() override;
    void Shutdown() override;
    bool Load(const std::string& source) override;
    bool CheckSyntax(const std::string& source) override;
    core::Result<Value> Run() override;
    core::Result<Value> Call(const std::string& fn, const std::vector<Value>& args) override;
    core::Result<uint64_t> CaptureFunction(const std::string& name) override;
    core::Result<Value> CallCaptured(uint64_t handle, const std::vector<Value>& args) override;
    core::Result<uint64_t> CaptureStackFunction(int index) override;
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
};
#endif

std::unique_ptr<IScriptHost> CreatePythonHost();

} // namespace neon::script
