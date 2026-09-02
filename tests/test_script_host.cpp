#include "helpers.hpp"

// E (multi-language host): CreateScriptHost(kind) is the single factory for a
// language-named script host, so "lua" / "js" / "python" are switched the same
// way (and a not-yet-built backend falls back to nullptr). Hosts are usable for
// BOTH gameplay and editor tooling — the same IScriptHost gets a different
// binding set from whichever side wires it.

#include "neon/script/script.hpp"

using namespace neon::script;

TEST(script_host_factory_lua) {
    // "lua" is always built.
    std::unique_ptr<IScriptHost> host = CreateScriptHost("lua");
    CHECK(host != nullptr);
    if (host) {
        CHECK(host->Init());
        CHECK(host->Load("return 7"));
        auto r = host->Run();
        CHECK(r.Ok());
        if (r.Ok()) {
            CHECK(r.Value().type == Value::Type::Number);
            CHECK_EQ(r.Value().number, 7.0);
        }
    }
}

TEST(script_host_factory_unknown_or_unbuilt) {
    // "python" is a dedicated E workstream (CPython + determinism sandbox); it is
    // not built yet, and unknown names return nullptr so callers fall back to Lua.
    CHECK(CreateScriptHost("python") == nullptr);
    CHECK(CreateScriptHost("bogus") == nullptr);
}
