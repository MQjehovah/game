#include "neon/script/js_host.hpp"

#include <cmath>
#include <cstdint>
#include <map>
#include <set>
#include <string>
#include <unordered_set>
#include <vector>

#include "quickjs.h"

#include "neon/core/log.hpp"
#include "neon/core/rng.hpp"

namespace neon::script {
namespace {

// Fixed seed for every fresh host (never wall time). Matches core::Rng's own
// default and LuaHost's kDefaultRngSeed so both backends share one seed space.
constexpr uint64_t kDefaultRngSeed = 0x9E3779B97F4A7C15ull;

// 53-bit double in [0, 1) from the top bits of a single xorshift64* draw.
// Bit-exact across hosts with the same seed (identical to LuaHost).
double RngFloat01(uint64_t rv) {
    return static_cast<double>(rv >> 11) * (1.0 / 9007199254740992.0);
}

// Uniform integer in [low, up] (inclusive), one RNG draw per call, mirroring
// LuaHost::PushRandomInt's modulo projection. The full 64-bit span cannot be
// expressed as span+1, so it returns the raw draw.
int64_t RandomInt(uint64_t rv, int64_t low, int64_t up) {
    const uint64_t span = static_cast<uint64_t>(up) - static_cast<uint64_t>(low);
    const uint64_t r = (span == ~0ull) ? rv : rv % (span + 1);
    return static_cast<int64_t>(static_cast<uint64_t>(low) + r);
}

// Strips a leading UTF-8 BOM (EF BB BF), which Windows editors and PowerShell
// often prepend to saved files. QuickJS rejects the BOM like Lua does.
void StripUtf8Bom(std::string& s) {
    if (s.size() >= 3 && static_cast<unsigned char>(s[0]) == 0xEF &&
        static_cast<unsigned char>(s[1]) == 0xBB &&
        static_cast<unsigned char>(s[2]) == 0xBF) {
        s.erase(0, 3);
    }
}

// core::Log can throw (log-buffer allocation in GetRecentLogs, user-supplied
// sinks). It is called from inside QuickJS closures, which are plain C frames;
// an uncaught C++ exception unwinding through them is undefined behavior.
// Swallow it so print degrades to a silent no-op. Mirrors LuaHost::SafeLog.
void SafeLog(const char* msg) {
    try {
        core::Log(core::LogLevel::Info, core::LogCategory::Script, nullptr, 0, "%s", msg);
    } catch (...) {
    }
}

// Recursion budget for JS value conversion (mirrors LuaHost's budget).
constexpr int kMaxConversionDepth = 1000;

// Number of interrupt-handler calls allowed before a script is aborted
// (runaway-loop guard; the handler fires roughly once per executed bytecode
// batch, so this is a very generous budget, not a real time limit).
constexpr int64_t kInterruptBudget = 20000000;

// The registered native function behind one JS function (looked up by the
// JSCFunctionData magic). Kept in a stable vector; magics are indices.
struct NativeFn {
    NativeFunction fn = nullptr;
    void* user = nullptr;
};

// One active native-call frame: the script-side arguments, copied (dup'd) so
// nested script calls inside a binding cannot invalidate the outer argv array.
struct ArgFrame {
    std::vector<JSValue> args;
};

} // namespace

// Value <-> JSValue conversion helpers (defined below, after Impl).
JSValue PushValue(JSContext* ctx, const Value& v);
Value PopValue(JSContext* ctx, JSValueConst val);

struct JsHost::Impl {
    JSRuntime* rt = nullptr;
    JSContext* ctx = nullptr;
    bool hasChunk = false;
    std::string chunkSource; // retained for Run() (global-eval semantics)
    ScriptError lastError;
    std::vector<NativeFn> nativeFns; // index == JSCFunctionData magic
    std::vector<ArgFrame> argFrames;
    bool nativeErrorPending = false;
    std::string nativeErrorMessage;
    uint64_t nextFunctionKey = 0;
    std::map<uint64_t, JSValue> captured; // dup'd function values (handle -> fn)
    core::Rng rng;                        // sandbox RNG; reseeded by SetRngSeed
    double simClock = 0.0;                // engine-injected time (NMath.Time)
    int64_t interruptCalls = 0;
    bool interrupted = false;

    // QuickJS C entry points. As members of the private nested Impl they can
    // reach JsHost's privates while keeping QuickJS types out of the public
    // header. Each reads its JsHost from the context opaque pointer set at
    // Init and implements one sandbox surface: the engine's deterministic
    // RNG / clock / print and the registered-native-call bridge. None of them
    // may let a C++ exception escape (JS_Call frames are plain C).
    static JSValue NativeCallData(JSContext* ctx, JSValueConst this_val, int argc,
                                  JSValueConst* argv, int magic, JSValue* func_data);
    static JSValue Print(JSContext* ctx, JSValueConst this_val, int argc,
                         JSValueConst* argv);
    static JSValue MathRandom(JSContext* ctx, JSValueConst this_val, int argc,
                              JSValueConst* argv);
    static JSValue NMathRandom(JSContext* ctx, JSValueConst this_val, int argc,
                               JSValueConst* argv);
    static JSValue NMathRandomRange(JSContext* ctx, JSValueConst this_val, int argc,
                                    JSValueConst* argv);
    static JSValue NMathSeed(JSContext* ctx, JSValueConst this_val, int argc,
                             JSValueConst* argv);
    static JSValue NMathTime(JSContext* ctx, JSValueConst this_val, int argc,
                             JSValueConst* argv);
    static int Interrupt(JSRuntime* rt, void* opaque);
    void CaptureError();
    core::Result<Value> Fail(const std::string& message, int line);
};

JsHost::JsHost() : impl_(std::make_unique<Impl>()) {}

JsHost::~JsHost() { Shutdown(); }

bool JsHost::Init() {
    if (impl_->ctx) return true; // already initialized
    impl_->rt = JS_NewRuntime();
    if (!impl_->rt) return false;
    // Runaway-loop / memory-blowup guards (the sandbox's only hard limits).
    JS_SetMemoryLimit(impl_->rt, 128u * 1024 * 1024);
    JS_SetInterruptHandler(impl_->rt, &JsHost::Impl::Interrupt, this);
    impl_->ctx = JS_NewContext(impl_->rt);
    if (!impl_->ctx) {
        JS_FreeRuntime(impl_->rt);
        impl_->rt = nullptr;
        return false;
    }
    JS_SetContextOpaque(impl_->ctx, this);
    // A fresh context starts with a fresh, fixed-seed RNG and a zero clock, so
    // a re-initialized host behaves like a newly created one.
    impl_->rng = core::Rng(kDefaultRngSeed);
    impl_->simClock = 0.0;

    // Sandbox surface: print -> engine log, Math.random -> host RNG, NMath.*
    // -> deterministic RNG / simulated clock. Everything else (Object/Array/
    // String/Math/JSON/Date/RegExp...) comes from QuickJS's bare ES core with
    // NO host stdlib, so there is no require/import/console/fs to abuse.
    JSValue global = JS_GetGlobalObject(impl_->ctx);
    JS_SetPropertyStr(impl_->ctx, global, "print",
                      JS_NewCFunction(impl_->ctx, &JsHost::Impl::Print, "print", 1));

    JSValue math = JS_GetPropertyStr(impl_->ctx, global, "Math");
    if (JS_IsObject(math)) {
        JS_SetPropertyStr(impl_->ctx, math, "random",
                          JS_NewCFunction(impl_->ctx, &JsHost::Impl::MathRandom,
                                          "random", 0));
    }
    JS_FreeValue(impl_->ctx, math);

    JSValue nmath = JS_NewObject(impl_->ctx);
    JS_SetPropertyStr(impl_->ctx, nmath, "Random",
                      JS_NewCFunction(impl_->ctx, &JsHost::Impl::NMathRandom,
                                      "Random", 0));
    JS_SetPropertyStr(impl_->ctx, nmath, "RandomRange",
                      JS_NewCFunction(impl_->ctx, &JsHost::Impl::NMathRandomRange,
                                      "RandomRange", 0));
    JS_SetPropertyStr(impl_->ctx, nmath, "Seed",
                      JS_NewCFunction(impl_->ctx, &JsHost::Impl::NMathSeed, "Seed", 0));
    JS_SetPropertyStr(impl_->ctx, nmath, "Time",
                      JS_NewCFunction(impl_->ctx, &JsHost::Impl::NMathTime, "Time", 0));
    JS_SetPropertyStr(impl_->ctx, global, "NMath", nmath);
    JS_FreeValue(impl_->ctx, global);
    return true;
}

void JsHost::Shutdown() {
    if (impl_->ctx) {
        // Release every held JSValue (captured functions) BEFORE the context
        // dies: JS_FreeRuntime asserts when live objects remain.
        for (auto& kv : impl_->captured) JS_FreeValue(impl_->ctx, kv.second);
        impl_->captured.clear();
        JS_FreeContext(impl_->ctx);
        impl_->ctx = nullptr;
    }
    if (impl_->rt) {
        JS_FreeRuntime(impl_->rt);
        impl_->rt = nullptr;
    }
    impl_->captured.clear(); // JSValues die with the context
    impl_->argFrames.clear();
    impl_->hasChunk = false;
    impl_->chunkSource.clear();
    impl_->lastError = {};
}

void JsHost::Impl::CaptureError() {
    JSValue exc = JS_GetException(ctx);
    // Prefer the Error object's `message`; a thrown string falls back to the
    // value itself.
    JSValue msgVal = JS_GetPropertyStr(ctx, exc, "message");
    if (!JS_IsString(msgVal)) {
        JS_FreeValue(ctx, msgVal);
        msgVal = JS_DupValue(ctx, exc);
    }
    size_t len = 0;
    const char* s = JS_ToCStringLen(ctx, &len, msgVal);
    lastError.message.assign(s ? s : "unknown script error", len);
    if (s) JS_FreeCString(ctx, s);
    JS_FreeValue(ctx, msgVal);

    // lineNumber is reliable for syntax errors but can be garbage (INT_MIN)
    // for user-thrown errors in eval'd chunks; fall back to the stack trace
    // ("at [neon]:<line>"), whose last :<digits> segment is the deepest frame.
    JSValue lineVal = JS_GetPropertyStr(ctx, exc, "lineNumber");
    double line = 0;
    if (JS_ToFloat64(ctx, &line, lineVal) != 0) line = 0;
    int lineNum = static_cast<int>(line);
    if (lineNum <= 0) {
        JSValue stackVal = JS_GetPropertyStr(ctx, exc, "stack");
        if (JS_IsString(stackVal)) {
            size_t slen = 0;
            const char* s = JS_ToCStringLen(ctx, &slen, stackVal);
            const std::string stack(s ? s : "", slen);
            if (s) JS_FreeCString(ctx, s);
            for (size_t i = 0; i + 1 < stack.size();) {
                if (stack[i] == ':' && stack[i + 1] >= '0' && stack[i + 1] <= '9') {
                    int n = 0;
                    size_t j = i + 1;
                    while (j < stack.size() && stack[j] >= '0' && stack[j] <= '9') {
                        n = n * 10 + (stack[j] - '0');
                        ++j;
                    }
                    if (n > 0) lineNum = n; // last match wins (deepest frame)
                    i = j;
                } else {
                    ++i;
                }
            }
        }
        JS_FreeValue(ctx, stackVal);
    }
    lastError.line = lineNum;
    JS_FreeValue(ctx, lineVal);
    JS_FreeValue(ctx, exc);
    interruptCalls = 0;
    interrupted = false;
}

core::Result<Value> JsHost::Impl::Fail(const std::string& message, int line) {
    lastError = {message, line};
    return core::Result<Value>::Err(message);
}

bool JsHost::Load(const std::string& source) {
    if (!impl_->ctx) return false;
    std::string src = source;
    StripUtf8Bom(src);
    // Compile-only validation. The source is retained and re-evaluated as
    // global code by Run(), which is what gives `function on_start(...)`
    // declarations real global scope (QuickJS global eval semantics).
    JSValue chunk =
        JS_Eval(impl_->ctx, src.data(), src.size(), "[neon]",
                JS_EVAL_TYPE_GLOBAL | JS_EVAL_FLAG_COMPILE_ONLY);
    if (JS_IsException(chunk)) {
        impl_->CaptureError();
        impl_->hasChunk = false;
        return false;
    }
    JS_FreeValue(impl_->ctx, chunk);
    impl_->chunkSource = std::move(src);
    impl_->hasChunk = true;
    impl_->lastError = {};
    return true;
}

bool JsHost::CheckSyntax(const std::string& source) {
    if (!impl_->ctx) return false;
    std::string src = source;
    StripUtf8Bom(src);
    JSValue chunk =
        JS_Eval(impl_->ctx, src.data(), src.size(), "[neon]",
                JS_EVAL_TYPE_GLOBAL | JS_EVAL_FLAG_COMPILE_ONLY);
    if (JS_IsException(chunk)) {
        impl_->CaptureError();
        return false;
    }
    JS_FreeValue(impl_->ctx, chunk);
    impl_->lastError = {};
    return true;
}

core::Result<Value> JsHost::Run() {
    if (!impl_->ctx) return impl_->Fail("script host is not initialized", 0);
    if (!impl_->hasChunk) return impl_->Fail("no script chunk loaded", 0);
    impl_->interruptCalls = 0;
    JSValue res = JS_Eval(impl_->ctx, impl_->chunkSource.data(),
                          impl_->chunkSource.size(), "[neon]",
                          JS_EVAL_TYPE_GLOBAL);
    if (JS_IsException(res)) {
        impl_->CaptureError();
        return impl_->Fail(impl_->lastError.message, impl_->lastError.line);
    }
    Value result = PopValue(impl_->ctx, res);
    JS_FreeValue(impl_->ctx, res);
    impl_->lastError = {};
    return core::Result<Value>::Ok(result);
}

core::Result<Value> JsHost::Call(const std::string& fn, const std::vector<Value>& args) {
    if (!impl_->ctx) return impl_->Fail("script host is not initialized", 0);
    JSValue global = JS_GetGlobalObject(impl_->ctx);
    JSValue func = JS_GetPropertyStr(impl_->ctx, global, fn.c_str());
    JS_FreeValue(impl_->ctx, global);
    if (!JS_IsFunction(impl_->ctx, func)) {
        JS_FreeValue(impl_->ctx, func);
        return impl_->Fail(fn + " is not a function", 0);
    }
    std::vector<JSValue> jargs;
    jargs.reserve(args.size());
    for (const Value& v : args) jargs.push_back(PushValue(impl_->ctx, v));
    impl_->interruptCalls = 0;
    JSValue res = JS_Call(impl_->ctx, func, JS_UNDEFINED,
                          static_cast<int>(jargs.size()),
                          jargs.empty() ? nullptr : jargs.data());
    JS_FreeValue(impl_->ctx, func);
    for (JSValue& j : jargs) JS_FreeValue(impl_->ctx, j);
    if (JS_IsException(res)) {
        impl_->CaptureError();
        return impl_->Fail(impl_->lastError.message, impl_->lastError.line);
    }
    Value result = PopValue(impl_->ctx, res);
    JS_FreeValue(impl_->ctx, res);
    impl_->lastError = {};
    return core::Result<Value>::Ok(result);
}

core::Result<uint64_t> JsHost::CaptureFunction(const std::string& name) {
    if (!impl_->ctx) return core::Result<uint64_t>::Err("script host is not initialized");
    JSValue global = JS_GetGlobalObject(impl_->ctx);
    JSValue func = JS_GetPropertyStr(impl_->ctx, global, name.c_str());
    JS_FreeValue(impl_->ctx, global);
    if (!JS_IsFunction(impl_->ctx, func)) {
        JS_FreeValue(impl_->ctx, func);
        return core::Result<uint64_t>::Err(name + " is not a function");
    }
    // The handle keeps a reference to THIS function value; a later chunk
    // overwriting the global cannot change which function is called.
    const uint64_t key = ++impl_->nextFunctionKey;
    impl_->captured[key] = func; // ownership transfer (no dup needed)
    impl_->lastError = {};
    return core::Result<uint64_t>::Ok(key);
}

core::Result<Value> JsHost::CallCaptured(uint64_t handle,
                                         const std::vector<Value>& args) {
    if (!impl_->ctx) return impl_->Fail("script host is not initialized", 0);
    const auto it = impl_->captured.find(handle);
    if (it == impl_->captured.end())
        return impl_->Fail("captured function handle is invalid", 0);
    std::vector<JSValue> jargs;
    jargs.reserve(args.size());
    for (const Value& v : args) jargs.push_back(PushValue(impl_->ctx, v));
    impl_->interruptCalls = 0;
    JSValue res = JS_Call(impl_->ctx, it->second, JS_UNDEFINED,
                          static_cast<int>(jargs.size()),
                          jargs.empty() ? nullptr : jargs.data());
    for (JSValue& j : jargs) JS_FreeValue(impl_->ctx, j);
    if (JS_IsException(res)) {
        impl_->CaptureError();
        return impl_->Fail(impl_->lastError.message, impl_->lastError.line);
    }
    Value result = PopValue(impl_->ctx, res);
    JS_FreeValue(impl_->ctx, res);
    impl_->lastError = {};
    return core::Result<Value>::Ok(result);
}

core::Result<uint64_t> JsHost::CaptureStackFunction(int index) {
    if (!impl_->ctx) return core::Result<uint64_t>::Err("script host is not initialized");
    if (impl_->argFrames.empty())
        return core::Result<uint64_t>::Err("no active native call frame");
    const ArgFrame& frame = impl_->argFrames.back();
    if (index < 0 || index >= static_cast<int>(frame.args.size()))
        return core::Result<uint64_t>::Err("bad stack index");
    const JSValue fn = frame.args[static_cast<size_t>(index)];
    if (!JS_IsFunction(impl_->ctx, fn))
        return core::Result<uint64_t>::Err("value at stack index is not a function");
    const uint64_t key = ++impl_->nextFunctionKey;
    impl_->captured[key] = JS_DupValue(impl_->ctx, fn);
    impl_->lastError = {};
    return core::Result<uint64_t>::Ok(key);
}

void JsHost::SetGlobal(const std::string& name, const Value& v) {
    if (!impl_->ctx) return;
    JSValue global = JS_GetGlobalObject(impl_->ctx);
    JS_SetPropertyStr(impl_->ctx, global, name.c_str(), PushValue(impl_->ctx, v));
    JS_FreeValue(impl_->ctx, global);
}

core::Result<Value> JsHost::GetGlobal(const std::string& name) {
    if (!impl_->ctx) return impl_->Fail("script host is not initialized", 0);
    JSValue global = JS_GetGlobalObject(impl_->ctx);
    JSValue val = JS_GetPropertyStr(impl_->ctx, global, name.c_str());
    JS_FreeValue(impl_->ctx, global);
    Value v = PopValue(impl_->ctx, val);
    JS_FreeValue(impl_->ctx, val);
    return core::Result<Value>::Ok(v);
}

void JsHost::Register(const std::string& name, NativeFunction fn, void* user) {
    if (!impl_->ctx) return;
    const int magic = static_cast<int>(impl_->nativeFns.size());
    impl_->nativeFns.push_back(NativeFn{fn, user});
    JSValue func = JS_NewCFunctionData(impl_->ctx, &JsHost::Impl::NativeCallData, 0, magic,
                                       0, nullptr);
    JSValue global = JS_GetGlobalObject(impl_->ctx);
    JS_SetPropertyStr(impl_->ctx, global, name.c_str(), func);
    JS_FreeValue(impl_->ctx, global);
}

void JsHost::RegisterField(const std::string& tableName, const std::string& fieldName,
                           NativeFunction fn, void* user) {
    if (!impl_->ctx) return;
    const int magic = static_cast<int>(impl_->nativeFns.size());
    impl_->nativeFns.push_back(NativeFn{fn, user});
    JSValue func = JS_NewCFunctionData(impl_->ctx, &JsHost::Impl::NativeCallData, 0, magic,
                                       0, nullptr);
    // Dotted paths ("A.B.C") walk/create nested objects so namespaced APIs
    // like NeonEditor.ui.Button register as real JS object properties.
    JSValue cur = JS_GetGlobalObject(impl_->ctx);
    size_t start = 0;
    for (size_t i = 0; i <= tableName.size(); ++i) {
        if (i < tableName.size() && tableName[i] != '.') continue;
        const std::string seg = tableName.substr(start, i - start);
        start = i + 1;
        if (seg.empty()) continue;
        JSValue next = JS_GetPropertyStr(impl_->ctx, cur, seg.c_str());
        if (!JS_IsObject(next)) {
            JS_FreeValue(impl_->ctx, next);
            next = JS_NewObject(impl_->ctx);
            JS_SetPropertyStr(impl_->ctx, cur, seg.c_str(),
                              JS_DupValue(impl_->ctx, next));
        }
        JS_FreeValue(impl_->ctx, cur);
        cur = next;
    }
    JS_SetPropertyStr(impl_->ctx, cur, fieldName.c_str(), func);
    JS_FreeValue(impl_->ctx, cur);
}

int JsHost::ArgCount() const {
    return impl_->argFrames.empty() ? 0
                                    : static_cast<int>(impl_->argFrames.back().args.size());
}

Value JsHost::GetArg(int index) const {
    if (impl_->argFrames.empty()) return Value::Nil();
    const ArgFrame& frame = impl_->argFrames.back();
    if (index < 0 || index >= static_cast<int>(frame.args.size())) return Value::Nil();
    return PopValue(impl_->ctx, frame.args[static_cast<size_t>(index)]);
}

void JsHost::SetError(const std::string& message) {
    impl_->nativeErrorPending = true;
    impl_->nativeErrorMessage = message;
    impl_->lastError = {message, 0};
}

const ScriptError& JsHost::LastError() const { return impl_->lastError; }

bool JsHost::HasFunction(const std::string& fn) const {
    if (!impl_->ctx) return false;
    JSValue global = JS_GetGlobalObject(impl_->ctx);
    JSValue val = JS_GetPropertyStr(impl_->ctx, global, fn.c_str());
    const bool isFunction = JS_IsFunction(impl_->ctx, val) != 0;
    JS_FreeValue(impl_->ctx, val);
    JS_FreeValue(impl_->ctx, global);
    return isFunction;
}

void JsHost::SetRngSeed(uint64_t seed) { impl_->rng = core::Rng(seed); }

void JsHost::SetSimClock(double seconds) { impl_->simClock = seconds; }

// Registered native function entry point (JSCFunctionData). `magic` is the
// index of the registration in impl_->nativeFns; the host comes from the
// context opaque pointer. Arguments are copied into an ArgFrame so ArgCount/
// GetArg work while the binding runs; nested script calls from inside a
// binding (SignalEmit, callbacks) push their own frames.
//
// Error discipline: exceptions from the binding are caught and converted into
// a JS throw (JS_Throw), so no C++ exception ever unwinds through QuickJS's
// plain-C JS_Call frames.
JSValue JsHost::Impl::NativeCallData(JSContext* ctx, JSValueConst this_val, int argc,
                                     JSValueConst* argv, int magic,
                                     JSValue* func_data) {
    (void)this_val;
    (void)func_data;
    JsHost* self = static_cast<JsHost*>(JS_GetContextOpaque(ctx));
    if (!self) return JS_ThrowInternalError(ctx, "invalid native function registration");
    Impl& impl = *self->impl_;
    if (magic < 0 || magic >= static_cast<int>(impl.nativeFns.size()))
        return JS_ThrowInternalError(ctx, "invalid native function registration");
    const NativeFn nf = impl.nativeFns[static_cast<size_t>(magic)];
    if (!nf.fn) return JS_ThrowInternalError(ctx, "native function has no implementation");

    impl.argFrames.push_back(ArgFrame{});
    ArgFrame& frame = impl.argFrames.back();
    frame.args.reserve(static_cast<size_t>(argc));
    for (int i = 0; i < argc; ++i)
        frame.args.push_back(JS_DupValue(ctx, argv[i]));
    {
        Value result;
        try {
            result = nf.fn(*self, nf.user);
        } catch (const std::exception& e) {
            impl.lastError = {e.what(), 0};
            impl.nativeErrorMessage = e.what();
            impl.nativeErrorPending = true;
        } catch (...) {
            const char* kMessage = "native function threw a non-std exception";
            impl.lastError = {kMessage, 0};
            impl.nativeErrorMessage = kMessage;
            impl.nativeErrorPending = true;
        }
        if (!impl.nativeErrorPending) {
            for (JSValue& j : frame.args) JS_FreeValue(ctx, j);
            impl.argFrames.pop_back();
            return PushValue(ctx, result); // caller frees it (JS_Call owns argv)
        }
    }
    const std::string msg = impl.nativeErrorMessage;
    impl.nativeErrorPending = false;
    for (JSValue& j : frame.args) JS_FreeValue(ctx, j);
    impl.argFrames.pop_back();
    return JS_Throw(ctx, JS_NewStringLen(ctx, msg.data(), msg.size()));
}

// print(...) -> core::Log(Info), tab-joined like Lua's print. JS_ToString can
// raise (a throwing toString), in which case the whole call logs the fallback.
JSValue JsHost::Impl::Print(JSContext* ctx, JSValueConst this_val, int argc,
                            JSValueConst* argv) {
    (void)this_val;
    JsHost* self = static_cast<JsHost*>(JS_GetContextOpaque(ctx));
    if (!self) return JS_ThrowInternalError(ctx, "invalid sandbox closure");
    std::string out;
    for (int i = 0; i < argc; ++i) {
        JSValue s = JS_ToString(ctx, argv[i]);
        if (JS_IsException(s)) {
            JS_FreeValue(ctx, s);
            SafeLog("print: argument could not be converted to string");
            return JS_UNDEFINED;
        }
        size_t len = 0;
        const char* c = JS_ToCStringLen(ctx, &len, s);
        if (!out.empty()) out += "\t";
        out.append(c ? c : "", len);
        if (c) JS_FreeCString(ctx, c);
        JS_FreeValue(ctx, s);
    }
    SafeLog(out.c_str());
    return JS_UNDEFINED;
}

// Math.random(): [0,1); Math.random(n): integer [1,n]; Math.random(m,n):
// integer [m,n]; Math.random(0): full random integer. Mirrors LuaHost's
// math.random calling conventions (one RNG draw per call, modulo projection).
JSValue JsHost::Impl::MathRandom(JSContext* ctx, JSValueConst this_val, int argc,
                                 JSValueConst* argv) {
    (void)this_val;
    JsHost* self = static_cast<JsHost*>(JS_GetContextOpaque(ctx));
    if (!self) return JS_ThrowInternalError(ctx, "invalid sandbox closure");
    const uint64_t rv = self->impl_->rng.Next();
    if (argc == 0) return JS_NewFloat64(ctx, RngFloat01(rv));
    double a0 = 0;
    if (JS_ToFloat64(ctx, &a0, argv[0]) != 0) return JS_EXCEPTION;
    if (argc == 1) {
        if (a0 == 0.0) return JS_NewFloat64(ctx, static_cast<double>(rv));
        return JS_NewFloat64(ctx, static_cast<double>(
                                      RandomInt(rv, 1, static_cast<int64_t>(a0))));
    }
    double a1 = 0;
    if (JS_ToFloat64(ctx, &a1, argv[1]) != 0) return JS_EXCEPTION;
    const int64_t low = static_cast<int64_t>(a0);
    const int64_t up = static_cast<int64_t>(a1);
    if (low > up) return JS_Throw(ctx, JS_NewString(ctx, "interval is empty"));
    return JS_NewFloat64(ctx, static_cast<double>(RandomInt(rv, low, up)));
}

// NMath.Random shares the host RNG with Math.random (mirrors LuaHost).
JSValue JsHost::Impl::NMathRandom(JSContext* ctx, JSValueConst this_val, int argc,
                                  JSValueConst* argv) {
    return MathRandom(ctx, this_val, argc, argv);
}

JSValue JsHost::Impl::NMathRandomRange(JSContext* ctx, JSValueConst this_val, int argc,
                                       JSValueConst* argv) {
    (void)this_val;
    JsHost* self = static_cast<JsHost*>(JS_GetContextOpaque(ctx));
    if (!self) return JS_ThrowInternalError(ctx, "invalid sandbox closure");
    const uint64_t rv = self->impl_->rng.Next();
    if (argc == 1) {
        double up = 0;
        if (JS_ToFloat64(ctx, &up, argv[0]) != 0) return JS_EXCEPTION;
        return JS_NewFloat64(ctx, static_cast<double>(
                                      RandomInt(rv, 1, static_cast<int64_t>(up))));
    }
    if (argc == 2) {
        double a0 = 0, a1 = 0;
        if (JS_ToFloat64(ctx, &a0, argv[0]) != 0 || JS_ToFloat64(ctx, &a1, argv[1]) != 0)
            return JS_EXCEPTION;
        const int64_t low = static_cast<int64_t>(a0);
        const int64_t up = static_cast<int64_t>(a1);
        if (low > up) return JS_Throw(ctx, JS_NewString(ctx, "interval is empty"));
        return JS_NewFloat64(ctx, static_cast<double>(RandomInt(rv, low, up)));
    }
    return JS_Throw(ctx, JS_NewString(ctx, "RandomRange expects 1 or 2 arguments"));
}

JSValue JsHost::Impl::NMathSeed(JSContext* ctx, JSValueConst this_val, int argc,
                                JSValueConst* argv) {
    (void)this_val;
    JsHost* self = static_cast<JsHost*>(JS_GetContextOpaque(ctx));
    if (!self) return JS_ThrowInternalError(ctx, "invalid sandbox closure");
    double seed = argc >= 1 ? 0 : static_cast<double>(kDefaultRngSeed);
    if (argc >= 1 && JS_ToFloat64(ctx, &seed, argv[0]) != 0) return JS_EXCEPTION;
    self->impl_->rng = core::Rng(static_cast<uint64_t>(seed));
    return JS_NewFloat64(ctx, seed); // mirrors LuaHost: returns the applied seed
}

JSValue JsHost::Impl::NMathTime(JSContext* ctx, JSValueConst this_val, int argc,
                                JSValueConst* argv) {
    (void)this_val;
    (void)argc;
    (void)argv;
    JsHost* self = static_cast<JsHost*>(JS_GetContextOpaque(ctx));
    if (!self) return JS_ThrowInternalError(ctx, "invalid sandbox closure");
    return JS_NewFloat64(ctx, self->impl_->simClock);
}

// QuickJS interrupt handler: aborts a runaway script after a generous budget.
int JsHost::Impl::Interrupt(JSRuntime* rt, void* opaque) {
    (void)rt;
    JsHost* self = static_cast<JsHost*>(opaque);
    if (!self) return 0;
    Impl& impl = *self->impl_;
    ++impl.interruptCalls;
    if (impl.interruptCalls > kInterruptBudget) {
        impl.interrupted = true;
        return 1; // abort the running script
    }
    return 0;
}

// ---------------------------------------------------------------------------
// Value <-> JSValue conversion. JS arrays map to the 1-based `array` sequence
// (index i lives at arr[i-1]); string-keyed properties map to `fields`.
// ---------------------------------------------------------------------------
JSValue PushValueImpl(JSContext* ctx, const Value& v, int depth,
                      std::unordered_set<const void*>& seen) {
    switch (v.type) {
        case Value::Type::Nil: return JS_UNDEFINED;
        case Value::Type::Number: return JS_NewFloat64(ctx, v.number);
        case Value::Type::String:
            return JS_NewStringLen(ctx, v.str.data(), v.str.size());
        case Value::Type::Bool: return JS_NewBool(ctx, v.boolean);
        case Value::Type::Table: {
            if (!v.table) return JS_NewObject(ctx);
            const void* ptr = v.table.get();
            if (depth >= kMaxConversionDepth || !seen.insert(ptr).second)
                return JS_UNDEFINED;
            JSValue obj = JS_NewArray(ctx);
            for (size_t i = 0; i < v.table->array.size(); ++i) {
                JSValue item = PushValueImpl(ctx, v.table->array[i], depth + 1, seen);
                JS_SetPropertyUint32(ctx, obj, static_cast<uint32_t>(i), item);
            }
            for (const auto& kv : v.table->fields) {
                JSValue val = PushValueImpl(ctx, kv.second, depth + 1, seen);
                JS_SetPropertyStr(ctx, obj, kv.first.c_str(), val);
            }
            return obj;
        }
    }
    return JS_UNDEFINED;
}

JSValue PushValue(JSContext* ctx, const Value& v) {
    std::unordered_set<const void*> seen;
    return PushValueImpl(ctx, v, 0, seen);
}

Value PopValueImpl(JSContext* ctx, JSValueConst val, int depth,
                   std::unordered_set<const void*>& seen) {
    if (JS_IsUndefined(val) || JS_IsNull(val)) return Value::Nil();
    if (JS_IsBool(val)) {
        const int b = JS_ToBool(ctx, val);
        if (b < 0) return Value::Nil();
        return Value::Bool(b != 0);
    }
    if (JS_IsNumber(val)) {
        double d = 0;
        if (JS_ToFloat64(ctx, &d, val) != 0) return Value::Nil();
        return Value::Num(d);
    }
    if (JS_IsString(val)) {
        size_t len = 0;
        const char* s = JS_ToCStringLen(ctx, &len, val);
        Value v = Value::Str(std::string(s ? s : "", len));
        if (s) JS_FreeCString(ctx, s);
        return v;
    }
    if (JS_IsObject(val)) {
        const void* ptr = JS_VALUE_GET_PTR(val);
        if (depth >= kMaxConversionDepth || !seen.insert(ptr).second) return Value::Nil();
        Value v = Value::Tbl();
        if (JS_IsArray(ctx, val)) {
            JSValue lenVal = JS_GetPropertyStr(ctx, val, "length");
            double lenD = 0;
            if (JS_ToFloat64(ctx, &lenD, lenVal) == 0 && lenD > 0) {
                const uint32_t len = static_cast<uint32_t>(lenD);
                for (uint32_t i = 0; i < len; ++i) {
                    JSValue item = JS_GetPropertyUint32(ctx, val, i);
                    v.table->array.push_back(PopValueImpl(ctx, item, depth + 1, seen));
                    JS_FreeValue(ctx, item);
                }
            }
            JS_FreeValue(ctx, lenVal);
        }
        // String-keyed properties (arrays may carry them too). Array indices
        // are already covered by the sequence part; skip pure-numeric keys.
        JSPropertyEnum* props = nullptr;
        uint32_t propCount = 0;
        if (JS_GetOwnPropertyNames(ctx, &props, &propCount, val,
                                   JS_GPN_STRING_MASK | JS_GPN_ENUM_ONLY) == 0) {
            for (uint32_t i = 0; i < propCount; ++i) {
                const char* key = JS_AtomToCString(ctx, props[i].atom);
                if (key && *key) {
                    bool numeric = true;
                    for (const char* p = key; *p; ++p) {
                        if (*p < '0' || *p > '9') {
                            numeric = false;
                            break;
                        }
                    }
                    if (!numeric) {
                        JSValue item = JS_GetProperty(ctx, val, props[i].atom);
                        v.table->fields.emplace_back(
                            key, PopValueImpl(ctx, item, depth + 1, seen));
                        JS_FreeValue(ctx, item);
                    }
                }
                if (key) JS_FreeCString(ctx, key);
            }
            for (uint32_t i = 0; i < propCount; ++i) JS_FreeAtom(ctx, props[i].atom);
            js_free(ctx, props);
        }
        return v;
    }
    return Value::Nil();
}

Value PopValue(JSContext* ctx, JSValueConst val) {
    std::unordered_set<const void*> seen;
    return PopValueImpl(ctx, val, 0, seen);
}

} // namespace neon::script
