#pragma once
#include <memory>

#include "neon/script/script.hpp"

struct JSRuntime; // forward declarations only; QuickJS headers stay out of the
struct JSContext; // public interface (same pattern as LuaHost)

namespace neon::script {

// QuickJS (ES2020) implementation of IScriptHost. Semantics mirror LuaHost so
// the two backends are drop-in replacements for the engine's script layer:
//   * sandboxed ES core (Object/Array/String/Math/JSON/Date/RegExp/...);
//     NO host stdlib is installed, so there is no require/import/console/fs
//   * Math.random + NMath.Random/RandomRange/Seed are backed by the host-seeded
//     core::Rng (xorshift64*), same default seed and fixed one-draw-per-call
//     modulo sampling as LuaHost -- JS and Lua streams are reproducible across
//     engine peers (deliberately NOT bit-identical to V8's Math.random)
//   * NMath.Time() reports the engine-injected simulated clock
//   * print() routes to core::Log (contained; never throws into JS)
//   * a memory limit + interrupt budget guard runaway loops / blowups
//   * a UTF-8 BOM is stripped from every source (Windows editors)
//
// The debugger (P1-2) is intentionally NOT implemented: IScriptHost's default
// no-op debugger methods keep hosts working, and the Lua debugger stays the
// canonical editor debugging path.
//
// Single-threaded: not safe to share across threads.
class JsHost : public IScriptHost {
public:
    JsHost();
    ~JsHost() override;

    JsHost(const JsHost&) = delete;
    JsHost& operator=(const JsHost&) = delete;

    bool Init() override;
    void Shutdown() override;
    bool Load(const std::string& source) override;
    bool CheckSyntax(const std::string& source) override;
    core::Result<Value> Run() override;
    core::Result<Value> Call(const std::string& fn, const std::vector<Value>& args) override;
    core::Result<uint64_t> CaptureFunction(const std::string& name) override;
    core::Result<Value> CallCaptured(uint64_t handle,
                                     const std::vector<Value>& args) override;
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
    void SetRngSeed(uint64_t seed) override; // seed 0 is treated as 1 (see LuaHost)
    void SetSimClock(double seconds) override;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace neon::script
