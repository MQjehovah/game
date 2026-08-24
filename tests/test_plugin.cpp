#include <string>
#include <vector>

#if defined(_WIN32)
#include <direct.h>
#else
#include <sys/stat.h>
#endif

#include "neon/neon.hpp"
#include "neon/plugin/plugin.hpp"
#include "neon/plugin/runtime_plugin.hpp"
#include "neon/script/bindings.hpp"
#include "helpers.hpp"

using namespace neon;

namespace {

// Recursively create every directory in `path` (accepts '/' and '\').
bool EnsureDirs(const std::string& path) {
    auto mkOne = [](const std::string& p) {
#if defined(_WIN32)
        if (CreateDirectoryA(p.c_str(), nullptr)) return true;
        return GetLastError() == ERROR_ALREADY_EXISTS;
#else
        return ::mkdir(p.c_str(), 0777) == 0 || errno == EEXIST;
#endif
    };
    std::string acc;
    size_t i = 0;
    while (i <= path.size()) {
        size_t next = path.find_first_of("/\\", i);
        if (next == std::string::npos) next = path.size();
        const std::string comp = path.substr(i, next - i);
        i = next + 1;
        if (comp.empty() || comp == ".") continue;
        if (acc.empty()) acc = comp;
        else acc += "/" + comp;
        if (acc.size() == 2 && acc[1] == ':') continue; // drive root only
        if (!mkOne(acc)) return false;
    }
    return true;
}

void Write(const std::string& path, const std::string& text) {
    const size_t slash = path.find_last_of("/\\");
    if (slash != std::string::npos) {
        CHECK(EnsureDirs(path.substr(0, slash)));
    }
    CHECK(test::WriteFileAll(path, text));
}

const char* kLuaMod = R"(
function on_load()
  Plugin.SetVar("lua_tick", 0)
  Plugin.On("tick", function(dt)
    Plugin.SetVar("lua_tick", Plugin.GetVar("lua_tick") + 1)
  end)
  Plugin.OnCommand("lua_add", function(a, b)
    return a + b
  end)
  Plugin.Export("mul", function(a, b)
    return a * b
  end)
  Plugin.RegisterComponent("lua_comp", { label = "Lua 组件" })
end
function on_start()
  Plugin.SetVar("lua_started", true)
end
)";

const char* kJsMod = R"(
function on_load() {
  Plugin.SetVar("js_tick", 0);
  Plugin.On("tick", function (dt) {
    Plugin.SetVar("js_tick", Plugin.GetVar("js_tick") + 1);
  });
  Plugin.On("player_join", function (clientId) {
    Plugin.SetVar("joined", clientId);
  });
  Plugin.OnCommand("js_add", function (a, b) {
    return a + b;
  });
  Plugin.Export("add", function (a, b) {
    return a + b;
  });
}
)";

const char* kTooNew = R"(
function on_load()
  Plugin.Log("info", "should never load")
end
)";

struct PluginFixture {
    ecs::World world;
    physics::World physics;
    script::ScriptContext ctx;
    script::GameVars gameVars;

    PluginFixture() {
        ctx.world = &world;
        ctx.physics = &physics;
    }
};

} // namespace

// ---------------------------------------------------------------------------
// Manifest parsing / discovery
// ---------------------------------------------------------------------------

TEST(PluginManifestParseAndVersion) {
    plugin::PluginManifest m;
    std::string err;
    CHECK(m.LoadJson(R"({"id":"a","name":"A","version":"1.2.3","type":"editor",
                         "backend":"js","entry":"init.js","minEngineVersion":"0.1.0",
                         "requires":["core"],"permissions":["world","assets"]})",
                     &err));
    CHECK_EQ(m.id, std::string("a"));
    CHECK_EQ(m.name, std::string("A"));
    CHECK_EQ(m.version, std::string("1.2.3"));
    CHECK(m.type == plugin::PluginType::Editor);
    CHECK_EQ(m.backend, std::string("js"));
    CHECK_EQ(m.entry, std::string("init.js"));
    CHECK_EQ(m.minEngineVersion, std::string("0.1.0"));
    CHECK_EQ(m.requires.size(), 1u);
    CHECK_EQ(m.permissions.size(), 2u);

    // Defaults: type runtime, backend lua, version 0.0.0.
    plugin::PluginManifest d;
    CHECK(d.LoadJson(R"({"id":"x","entry":"init.lua","backend":"lua"})", &err));
    CHECK(d.type == plugin::PluginType::Runtime);
    CHECK_EQ(d.version, std::string("0.0.0"));
    CHECK(d.LoadJson(R"({"id":"m","type":"mod","entry":"m.lua","backend":"lua"})", &err));
    CHECK(d.type == plugin::PluginType::Runtime); // unknown types default safe

    // Invalid manifests fail.
    plugin::PluginManifest bad;
    CHECK(!bad.LoadJson(R"({"name":"no id"})", &err));
    CHECK(!bad.LoadJson(R"({"id":"a","entry":"x","backend":"python"})", &err));
    CHECK(!bad.LoadJson("not json", &err));

    // Version parsing.
    plugin::Version v;
    CHECK(plugin::ParseVersion("1.2.3", &v));
    CHECK_EQ(v.major, 1);
    CHECK_EQ(v.minor, 2);
    CHECK_EQ(v.patch, 3);
    CHECK(plugin::ParseVersion("2", &v));
    CHECK_EQ(v.minor, 0);
    CHECK(!plugin::ParseVersion("1.x", &v));
}

TEST(PluginDiscoverScansPluginDirs) {
    test::TempDir tmp;
    const std::string base = tmp.Str();
    Write(base + "/plugins/good/plugin.json",
          R"({"id":"good","type":"runtime","backend":"lua","entry":"init.lua"})");
    Write(base + "/plugins/bad/plugin.json", R"({"name":"missing id"})");
    Write(base + "/plugins/editor_one/plugin.json",
          R"({"id":"editor_one","type":"editor","backend":"js","entry":"init.js"})");

    const auto all = plugin::DiscoverPlugins(base);
    CHECK_EQ(all.size(), 2u); // invalid manifest skipped
    CHECK_EQ(all[0].id, std::string("editor_one")); // sorted by id
    CHECK_EQ(all[1].id, std::string("good"));
    CHECK(all[0].dir.find("editor_one") != std::string::npos);
}

// ---------------------------------------------------------------------------
// RuntimePluginManager: dual-language gameplay modules
// ---------------------------------------------------------------------------

TEST(RuntimePluginManagerLuaAndJsCoexist) {
    test::TempDir tmp;
    const std::string base = tmp.Str();
    Write(base + "/plugins/lua_mod/plugin.json",
          R"({"id":"lua_mod","type":"runtime","backend":"lua","entry":"init.lua"})");
    Write(base + "/plugins/lua_mod/init.lua", kLuaMod);
    Write(base + "/plugins/js_mod/plugin.json",
          R"({"id":"js_mod","type":"runtime","backend":"js","entry":"init.js"})");
    Write(base + "/plugins/js_mod/init.js", kJsMod);
    Write(base + "/plugins/too_new/plugin.json",
          R"({"id":"too_new","type":"runtime","backend":"lua","entry":"init.lua",
               "minEngineVersion":"99.0.0"})");
    Write(base + "/plugins/too_new/init.lua", kTooNew);

    PluginFixture fx;
    plugin::RuntimePluginManager mgr;
    plugin::RuntimePluginManager::Config cfg;
    cfg.baseDir = base;
    cfg.ctx = &fx.ctx;
    cfg.gameVars = &fx.gameVars;
    cfg.rngSeed = 12345;
#ifdef NEON_ENABLE_JS
    CHECK_EQ(mgr.Load(cfg), 2u); // lua_mod + js_mod (too_new gated by version)
#else
    CHECK_EQ(mgr.Load(cfg), 1u); // js backend not compiled -> js_mod skipped
#endif

    mgr.Start();
    CHECK(mgr.Running());

    // on_start ran for Lua.
    CHECK(fx.gameVars.Get("plugin:lua_mod:lua_started").type == script::Value::Type::Bool);

    // Three ticks -> the Lua backend's tick handler ran.
    mgr.Tick(1.0f / 60.0f);
    mgr.Tick(1.0f / 60.0f);
    mgr.Tick(1.0f / 60.0f);
    CHECK_EQ(fx.gameVars.Get("plugin:lua_mod:lua_tick").number, 3.0);
#ifdef NEON_ENABLE_JS
    CHECK_EQ(fx.gameVars.Get("plugin:js_mod:js_tick").number, 3.0);

    // Events dispatch to the subscribed backend.
    mgr.DispatchEvent("player_join", {script::Value::Num(42)});
    CHECK_EQ(fx.gameVars.Get("plugin:js_mod:joined").number, 42.0);
#endif

    // Commands.
    std::string err;
    CHECK(mgr.RunCommand("lua_add", {script::Value::Num(2), script::Value::Num(3)}, &err));
#ifdef NEON_ENABLE_JS
    CHECK(mgr.RunCommand("js_add", {script::Value::Num(20), script::Value::Num(22)}, &err));
#endif
    CHECK(!mgr.RunCommand("missing", {}, &err));

    // Module API exports.
    const auto mul = mgr.CallPluginApi("lua_mod", "mul", {script::Value::Num(6), script::Value::Num(7)});
    CHECK(mul.type == script::Value::Type::Number);
    CHECK_EQ(mul.number, 42.0);
#ifdef NEON_ENABLE_JS
    const auto add = mgr.CallPluginApi("js_mod", "add", {script::Value::Num(1), script::Value::Num(2)});
    CHECK(add.type == script::Value::Type::Number);
    CHECK_EQ(add.number, 3.0);
#endif

    // Scoped state.
    CHECK(fx.gameVars.Has("plugin:lua_mod:lua_tick"));
#ifdef NEON_ENABLE_JS
    CHECK(fx.gameVars.Has("plugin:js_mod:js_tick"));
#endif

    // Component schemas registered by plugins are recorded.
    CHECK(mgr.ComponentSchemas().count("lua_comp") == 1u);

#ifdef NEON_ENABLE_JS
    // Plugin.Info reports the ACTIVE plugin's identity (the manager routes
    // scoped state by which plugin is executing).
    {
        const char* whoamiJs = R"(
function on_load() {
  Plugin.OnCommand("whoami", function () {
    Plugin.SetVar("self_id", Plugin.Info().id);
    return true;
  });
}
)";
        Write(base + "/plugins/js_mod/init.js", whoamiJs);
        plugin::RuntimePluginManager mgr2;
        plugin::RuntimePluginManager::Config c2;
        c2.baseDir = base;
        c2.ctx = &fx.ctx;
        c2.gameVars = &fx.gameVars;
        CHECK_EQ(mgr2.Load(c2), 2u);
        std::string e2;
        CHECK(mgr2.RunCommand("whoami", {}, &e2));
        CHECK(fx.gameVars.Get("plugin:js_mod:self_id").type == script::Value::Type::String);
        CHECK_EQ(fx.gameVars.Get("plugin:js_mod:self_id").str, std::string("js_mod"));
        mgr2.Stop();
    }
#endif

    mgr.Stop();
    CHECK(!mgr.Running());
    CHECK_EQ(mgr.Count(), 0u); // hosts torn down, plugins cleared
}

TEST(RuntimePluginManagerSkipsEditorType) {
    test::TempDir tmp;
    const std::string base = tmp.Str();
    Write(base + "/plugins/editor_only/plugin.json",
          R"({"id":"editor_only","type":"editor","backend":"lua","entry":"init.lua"})");
    Write(base + "/plugins/editor_only/init.lua", kLuaMod);

    PluginFixture fx;
    plugin::RuntimePluginManager mgr;
    plugin::RuntimePluginManager::Config cfg;
    cfg.baseDir = base;
    cfg.ctx = &fx.ctx;
    cfg.gameVars = &fx.gameVars;
    CHECK_EQ(mgr.Load(cfg), 0u); // editor plugins never load at runtime
    mgr.Stop();
}
