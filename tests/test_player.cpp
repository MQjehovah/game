#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "neon/neon.hpp"
#include "neon/core/pack.hpp"
#include "neon/scene/game_manifest.hpp"
#include "neon/scene/game_runtime.hpp"
#include "neon/script/bindings.hpp"
#include "helpers.hpp"
#include "packager.hpp"

using namespace neon;

namespace {

// A scriptable fake input state: the engine's IInput API with explicit control
// of the keys a test cares about. Everything else returns neutral values.
struct MockInput : platform::IInput {
    bool downW = false;
    bool downA = false;
    bool downD = false;
    bool downS = false;
    bool downSpace = false;
    math::Vec2 mouseDelta{};

    void HandleEvent(const platform::InputEvent&) override {}
    bool IsDown(platform::Key key) const override {
        switch (key) {
            case platform::Key::W: return downW;
            case platform::Key::A: return downA;
            case platform::Key::D: return downD;
            case platform::Key::S: return downS;
            case platform::Key::Space: return downSpace;
            default: return false;
        }
    }
    bool Pressed(platform::Key) const override { return false; }
    bool Released(platform::Key) const override { return false; }
    bool MouseDown(platform::MouseButton) const override { return false; }
    bool MousePressed(platform::MouseButton) const override { return false; }
    bool MouseReleased(platform::MouseButton) const override { return false; }
    math::Vec2 MousePos() const override { return {}; }
    math::Vec2 MouseDelta() const override { return mouseDelta; }
    float WheelDelta() const override { return 0.0f; }
    void EndFrame() override {}
};

std::vector<uint8_t> ToBytes(const std::string& s) {
    return std::vector<uint8_t>(s.begin(), s.end());
}

std::string BytesToString(const std::vector<uint8_t>& b) {
    return std::string(b.begin(), b.end());
}

// Create a directory (and its parents) for test fixtures that need subdirs the
// TempDir does not create. `path` must end with a separator so the final
// segment is created too.
bool MakeDirAll(const std::string& path) {
    if (path.empty()) return false;
    std::string full = path.back() == '/' || path.back() == '\\' ? path : path + "/";
#if defined(_WIN32)
    size_t start = 0;
    while (true) {
        const size_t slash = full.find_first_of("/\\", start);
        if (slash == std::string::npos) break;
        const std::string comp = full.substr(0, slash);
        if (!comp.empty() && !(comp.size() == 2 && comp[1] == ':')) {
            if (!CreateDirectoryA(comp.c_str(), nullptr) && GetLastError() != ERROR_ALREADY_EXISTS)
                return false;
        }
        start = slash + 1;
    }
    return true;
#else
    size_t start = 0;
    while (true) {
        const size_t slash = full.find('/', start);
        if (slash == std::string::npos) break;
        const std::string comp = full.substr(0, slash);
        if (!comp.empty() && ::mkdir(comp.c_str(), 0700) != 0 && errno != EEXIST) return false;
        start = slash + 1;
    }
    return true;
#endif
}

// The named tree a "bt:cycle" reference resolves to: sequence [wait(0.5),
// blackboard_set]. Lives at <base>/behaviors/cycle.bt.json on disk.
const char* kCycleBt =
    "{\"root\":{\"type\":\"sequence\",\"children\":["
    "{\"type\":\"wait\",\"args\":{\"seconds\":0.5}},"
    "{\"type\":\"blackboard_set\",\"args\":{\"key\":\"bt_done\",\"value\":true}}]}}";

const char* kBtScene = R"({
  "entities": [
    {"name": "Walker", "components": {
      "transform": {"pos": [0, 0, 0]},
      "behaviorTree": {"tree": "bt:cycle"}
    }}
  ]
})";

const char* kInputLua = R"(
function on_start(e)
  SetVar("fwd", InputAxis("forward"))
  SetVar("strafe", InputAxis("strafe"))
  SetVar("space", InputKey("space"))
  SetVar("mx", InputMouseX())
  SetVar("my", InputMouseY())
end
)";

const char* kInputScene = R"({
  "entities": [
    {"name": "Probe", "components": {
      "transform": {"pos": [0, 0, 0]},
      "script": {"backend": "lua", "path": "ai.lua"}
    }}
  ]
})";

} // namespace

// ---------------------------------------------------------------------------
// bt:<name> resolution in GameRuntime::AttachTrees
// ---------------------------------------------------------------------------

// A "bt:cycle" reference loads behaviors/cycle.bt.json from scriptBaseDir and
// ticks (wait + blackboard write), same as an inline tree would.
TEST(PlayerBtNamedReferenceLoadsFromDisk) {
    test::TempDir tmp;
    CHECK(MakeDirAll(tmp.Str() + "/behaviors"));
    CHECK(test::WriteFileAll(tmp.Str() + "/behaviors/cycle.bt.json", kCycleBt));

    scene::GameRuntime runtime;
    scene::GameRuntimeConfig cfg;
    cfg.scriptBaseDir = tmp.Str();
    core::Status st = runtime.Start(kBtScene, cfg);
    CHECK(st.Ok());
    CHECK_EQ(runtime.BehaviorTreeCount(), 1u);

    ecs::Entity btEnt;
    {
        auto view = runtime.World().ViewAll<scene::SceneBehaviorTree>();
        CHECK_EQ(view.Size(), 1u);
        if (view.Size() == 1u) btEnt = runtime.World().EntityAt<scene::SceneBehaviorTree>(0);
    }
    CHECK(btEnt.IsValid());

    for (int i = 0; i < 120; ++i) runtime.Tick(1.0f / 60.0f); // 2.0s > wait 0.5s
    script::Value done = runtime.EntityBlackboardValue(btEnt, "bt_done");
    CHECK(done.type == script::Value::Type::Bool);
    CHECK(done.boolean);
    runtime.Stop();
}

// The named reference can also go through the readScript override (the pack
// reader path), so packed games resolve bt: files exactly like loose projects.
TEST(PlayerBtNamedReferenceThroughReadScriptOverride) {
    scene::GameRuntime runtime;
    scene::GameRuntimeConfig cfg;
    cfg.readScript = [](const std::string& path) {
        if (path.find("behaviors/cycle.bt.json") != std::string::npos) return std::string(kCycleBt);
        return std::string();
    };
    core::Status st = runtime.Start(kBtScene, cfg);
    CHECK(st.Ok());
    CHECK_EQ(runtime.BehaviorTreeCount(), 1u);
    for (int i = 0; i < 120; ++i) runtime.Tick(1.0f / 60.0f); // must not crash
    runtime.Stop();
}

// A missing named tree is non-fatal: the entity stays, the tree is skipped.
TEST(PlayerBtNamedMissingIsNonFatal) {
    scene::GameRuntime runtime;
    core::Status st = runtime.Start(kBtScene, scene::GameRuntimeConfig{});
    CHECK(st.Ok());
    CHECK_EQ(runtime.BehaviorTreeCount(), 0u);
    CHECK_EQ(runtime.EntityCount(), 1u);
    runtime.Tick(1.0f / 60.0f); // must not crash
}

// A tree reference that is neither inline JSON nor "bt:<name>" is skipped.
TEST(PlayerBtMalformedReferenceIsNonFatal) {
    const char* scene = R"({
      "entities": [
        {"name": "Bad", "components": {
          "transform": {"pos": [0, 0, 0]},
          "behaviorTree": {"tree": "gibberish"}
        }}
      ]
    })";
    scene::GameRuntime runtime;
    core::Status st = runtime.Start(scene, scene::GameRuntimeConfig{});
    CHECK(st.Ok());
    CHECK_EQ(runtime.BehaviorTreeCount(), 0u);
    runtime.Tick(1.0f / 60.0f);
}

// ---------------------------------------------------------------------------
// Input bindings (ScriptContext.input -> InputAxis/InputKey/InputMouse)
// ---------------------------------------------------------------------------

TEST(PlayerInputBindingAxisKeyMouse) {
    auto host = script::CreateLuaHost();
    script::ScriptContext ctx;
    MockInput mock;
    mock.downW = true;
    mock.downD = true;
    mock.downSpace = true;
    mock.mouseDelta = {3.0f, -2.0f};
    ctx.input = &mock;
    CHECK(host != nullptr);
    CHECK(host->Init());
    script::RegisterEngineBindings(*host, ctx);

    CHECK(host->Load(kInputLua));
    CHECK(host->Run().Ok());
    CHECK(host->Call("on_start", {}).Ok());
    CHECK_EQ(ctx.gameVars.Get("fwd").number, 1.0);
    CHECK_EQ(ctx.gameVars.Get("strafe").number, 1.0);
    CHECK_EQ(ctx.gameVars.Get("space").number, 1.0);
    CHECK_NEAR(ctx.gameVars.Get("mx").number, 3.0, 1e-6);
    CHECK_NEAR(ctx.gameVars.Get("my").number, -2.0, 1e-6);
    host->Shutdown();
}

// Opposite-direction keys cancel: W+S -> 0, unheld keys -> 0.
TEST(PlayerInputBindingAxisCancels) {
    auto host = script::CreateLuaHost();
    script::ScriptContext ctx;
    MockInput mock;
    mock.downW = true;
    mock.downS = true; // forward + backward cancel
    ctx.input = &mock;
    CHECK(host != nullptr);
    CHECK(host->Init());
    script::RegisterEngineBindings(*host, ctx);
    CHECK(host->Load(kInputLua));
    CHECK(host->Run().Ok());
    CHECK(host->Call("on_start", {}).Ok());
    CHECK_EQ(ctx.gameVars.Get("fwd").number, 0.0);
    CHECK_EQ(ctx.gameVars.Get("strafe").number, 0.0);
    CHECK_EQ(ctx.gameVars.Get("space").number, 0.0);
    host->Shutdown();
}

// Null input degrades to zeros instead of crashing (headless hosts).
TEST(PlayerInputBindingNullInputIsZero) {
    auto host = script::CreateLuaHost();
    script::ScriptContext ctx; // input intentionally null
    CHECK(host != nullptr);
    CHECK(host->Init());
    script::RegisterEngineBindings(*host, ctx);
    CHECK(host->Load(kInputLua));
    CHECK(host->Run().Ok());
    CHECK(host->Call("on_start", {}).Ok());
    CHECK_EQ(ctx.gameVars.Get("fwd").number, 0.0);
    CHECK_EQ(ctx.gameVars.Get("strafe").number, 0.0);
    CHECK_EQ(ctx.gameVars.Get("space").number, 0.0);
    CHECK_EQ(ctx.gameVars.Get("mx").number, 0.0);
    CHECK_EQ(ctx.gameVars.Get("my").number, 0.0);
    host->Shutdown();
}

// End-to-end wiring: the GameRuntimeConfig.input pointer reaches scripts, so a
// data-driven scene reacts to live input during the tick loop.
TEST(PlayerGameRuntimeWiresInputToScripts) {
    MockInput mock;
    mock.downSpace = true;
    scene::GameRuntime runtime;
    scene::GameRuntimeConfig cfg;
    cfg.input = &mock;
    cfg.readScript = [](const std::string& path) {
        return path == "ai.lua" ? std::string(kInputLua) : std::string();
    };
    CHECK(runtime.Start(kInputScene, cfg).Ok());
    CHECK_EQ(runtime.ScriptCount(), 1u);
    // on_start ran during Start with the live input state.
    CHECK_EQ(runtime.GameVars().Get("space").number, 1.0);
    CHECK_EQ(runtime.GameVars().Get("fwd").number, 0.0);
    runtime.Stop();
}

// ---------------------------------------------------------------------------
// Pack unpacking (PackWriter -> PackReader -> core::Unpack -> file bytes)
// ---------------------------------------------------------------------------

TEST(PlayerUnpackRoundTrip) {
    test::TempDir dst;
    const std::string sceneJson = "{\"entities\":[]}";
    const std::string lua = "function on_update(e, dt) end";
    const std::string bt = "{\"root\":{\"type\":\"wait\"}}";

    core::PackWriter w;
    CHECK(w.AddFile("game.json", ToBytes("{\"startScene\":\"scenes/main.json\"}")).Ok());
    CHECK(w.AddFile("scenes/main.json", ToBytes(sceneJson)).Ok());
    CHECK(w.AddFile("scripts/ai.lua", ToBytes(lua)).Ok());
    CHECK(w.AddFile("behaviors/cycle.bt.json", ToBytes(bt)).Ok());
    std::vector<uint8_t> pack = w.Build();

    core::PackReader reader(pack);
    CHECK(reader.Valid());
    core::Status st = core::Unpack(reader, dst.Str());
    CHECK(st.Ok());

    std::string out;
    CHECK(test::ReadFileAll(dst.Str() + "/game.json", out));
    CHECK_EQ(out, std::string("{\"startScene\":\"scenes/main.json\"}"));
    CHECK(test::ReadFileAll(dst.Str() + "/scenes/main.json", out));
    CHECK_EQ(out, sceneJson);
    CHECK(test::ReadFileAll(dst.Str() + "/scripts/ai.lua", out));
    CHECK_EQ(out, lua);
    CHECK(test::ReadFileAll(dst.Str() + "/behaviors/cycle.bt.json", out));
    CHECK_EQ(out, bt);
}

// Binary blobs survive unpacking byte-for-byte.
TEST(PlayerUnpackBinaryBytesMatch) {
    test::TempDir dst;
    std::vector<uint8_t> bin(70000);
    uint32_t state = 7u;
    for (size_t i = 0; i < bin.size(); ++i) {
        state = state * 1664525u + 1013904223u;
        bin[i] = static_cast<uint8_t>(state >> 24);
    }
    core::PackWriter w;
    CHECK(w.AddFile("assets/data.bin", bin).Ok());
    std::vector<uint8_t> pack = w.Build();

    core::PackReader reader(pack);
    CHECK(core::Unpack(reader, dst.Str()).Ok());

    std::vector<char> out;
    CHECK(test::ReadFileAll(dst.Str() + "/assets/data.bin", out));
    CHECK_EQ(out.size(), bin.size());
    if (out.size() == bin.size()) {
        for (size_t i = 0; i < bin.size(); ++i) {
            CHECK_EQ(static_cast<unsigned char>(out[i]), bin[i]);
        }
    }
}

// A hostile pack entry that escapes the destination dir is rejected.
TEST(PlayerUnpackRejectsTraversalPath) {
    test::TempDir dst;
    core::PackWriter w;
    CHECK(w.AddFile("../escape.txt", ToBytes("evil")).Ok());
    std::vector<uint8_t> pack = w.Build();
    core::PackReader reader(pack);
    CHECK(reader.Valid());
    core::Status st = core::Unpack(reader, dst.Str());
    CHECK(!st.Ok());
    CHECK(st.Error().find("unsafe") != std::string::npos);
    // The escaping file must not exist outside the destination.
    std::string out;
    CHECK(!test::ReadFileAll("escape.txt", out));
}

// An invalid reader fails the unpack up front.
TEST(PlayerUnpackInvalidReaderFails) {
    test::TempDir dst;
    std::vector<uint8_t> garbage(64, 0);
    core::PackReader bad(garbage);
    CHECK(!bad.Valid());
    core::Status st = core::Unpack(bad, dst.Str());
    CHECK(!st.Ok());
    CHECK(!st.Error().empty());
}

// ---------------------------------------------------------------------------
// Lua UTF-8 BOM + runtime traversal guards (review fixes)
// ---------------------------------------------------------------------------

// A BOM'd Lua source (the packager stores raw bytes, and Notepad/PowerShell
// prepend a BOM) must load through GameRuntime::AttachScripts and run.
TEST(PlayerRuntimeBomScriptRuns) {
    const char* scene = R"({
      "entities": [
        {"name": "Counter", "components": {
          "transform": {"pos": [0, 0, 0]},
          "script": {"backend": "lua", "path": "ai.lua"}
        }}
      ]
    })";
    const char* lua =
        "\xEF\xBB\xBF" // UTF-8 BOM
        "function on_start(e)\n"
        "  SetVar(\"started\", true)\n"
        "end\n"
        "function on_update(e, dt)\n"
        "  local t = GetVar(\"ticks\")\n"
        "  if t == nil then t = 0 end\n"
        "  SetVar(\"ticks\", t + 1)\n"
        "end\n";
    scene::GameRuntime runtime;
    scene::GameRuntimeConfig cfg;
    cfg.readScript = [&](const std::string& p) {
        return p == "ai.lua" ? std::string(lua) : std::string();
    };
    core::Status st = runtime.Start(scene, cfg);
    CHECK(st.Ok());
    CHECK_EQ(runtime.ScriptCount(), 1u);
    CHECK(runtime.GameVars().Get("started").boolean);
    for (int i = 0; i < 60; ++i) runtime.Tick(1.0f / 60.0f);
    CHECK_EQ(runtime.GameVars().Get("ticks").number, 60.0);
    runtime.Stop();
}

// A script path with ".." is rejected at load time (defense-in-depth): the
// entity stays, the script is skipped, Start still succeeds.
TEST(PlayerScriptUnsafePathSkipped) {
    const char* scene = R"({
      "entities": [
        {"name": "Evil", "components": {
          "transform": {"pos": [0, 0, 0]},
          "script": {"backend": "lua", "path": "../evil.lua"}
        }}
      ]
    })";
    scene::GameRuntime runtime;
    scene::GameRuntimeConfig cfg;
    cfg.readScript = [](const std::string&) { return std::string("function on_update(e, dt) end"); };
    core::Status st = runtime.Start(scene, cfg);
    CHECK(st.Ok());
    CHECK_EQ(runtime.ScriptCount(), 0u);
    CHECK_EQ(runtime.EntityCount(), 1u);
    runtime.Tick(1.0f / 60.0f); // must not crash
}

// A "bt:" name with ".." is rejected: no tree, Start still succeeds.
TEST(PlayerBtUnsafeNameSkipped) {
    const char* scene = R"({
      "entities": [
        {"name": "Evil", "components": {
          "transform": {"pos": [0, 0, 0]},
          "behaviorTree": {"tree": "bt:../../secret"}
        }}
      ]
    })";
    scene::GameRuntime runtime;
    scene::GameRuntimeConfig cfg;
    cfg.readScript = [](const std::string&) { return std::string("{\"root\":{\"type\":\"wait\"}}"); };
    core::Status st = runtime.Start(scene, cfg);
    CHECK(st.Ok());
    CHECK_EQ(runtime.BehaviorTreeCount(), 0u);
    runtime.Tick(1.0f / 60.0f);
}

// core::IsUnsafeRelPath rejects ".." / absolute forms and accepts clean ones.
TEST(PlayerIsUnsafeRelPath) {
    CHECK(core::IsUnsafeRelPath("../x.lua"));
    CHECK(core::IsUnsafeRelPath("a/../../x.lua"));
    CHECK(core::IsUnsafeRelPath("/etc/passwd"));
    CHECK(core::IsUnsafeRelPath("C:/evil.lua"));
    CHECK(core::IsUnsafeRelPath(".."));
    CHECK(!core::IsUnsafeRelPath("scripts/ai.lua"));
    CHECK(!core::IsUnsafeRelPath("cycle"));
    CHECK(!core::IsUnsafeRelPath("a/b/c.lua"));
    CHECK(!core::IsUnsafeRelPath(""));
}

// ---------------------------------------------------------------------------
// Committed sample project: package -> unpack -> headless GameRuntime run.
// The full "edit -> package -> run" loop, reproducible in CI. Reads
// tests/data/neon_game_sample/ from the repo root (ctest runs there).
// ---------------------------------------------------------------------------

TEST(PlayerCommittedSamplePackUnpackRun) {
    const std::string sample = "tests/data/neon_game_sample";

    test::TempDir outTmp;
    neon::editor::pack::PackConfig pcfg;
    pcfg.projectDir = sample;
    pcfg.outDir = outTmp.Str() + "/out";
    pcfg.copyPlayer = false; // tests must not depend on the build tree
    neon::editor::pack::PackageReport rep = neon::editor::pack::PackProject(pcfg);
    if (!rep.ok)
        for (const std::string& e : rep.errors) std::printf("  PACK ERROR: %s\n", e.c_str());
    CHECK(rep.ok);
    CHECK(rep.errors.empty());
    CHECK_EQ(rep.fileCount, 5u); // game.json + scene + prefab + behavior + script

    std::string packText;
    CHECK(test::ReadFileAll(pcfg.outDir + "/game.pack", packText));
    std::vector<uint8_t> packBytes(packText.begin(), packText.end());
    core::PackReader reader(packBytes);
    CHECK(reader.Valid());
    CHECK(reader.Has("game.json"));
    CHECK(reader.Has("scenes/main.json"));
    CHECK(reader.Has("prefabs/pillar.json"));
    CHECK(reader.Has("behaviors/cycle.bt.json"));
    CHECK(reader.Has("scripts/ai.lua"));

    test::TempDir runTmp;
    CHECK(core::Unpack(reader, runTmp.Str()).Ok());

    std::string manifestText;
    CHECK(test::ReadFileAll(runTmp.Str() + "/game.json", manifestText));
    auto manifest = scene::GameManifest::Load(manifestText);
    CHECK(manifest.Ok());
    if (!manifest.Ok()) return;
    CHECK_EQ(manifest.Value().startScene, std::string("scenes/main.json"));

    std::string sceneJson;
    CHECK(test::ReadFileAll(runTmp.Str() + "/" + manifest.Value().startScene, sceneJson));

    scene::GameRuntime runtime;
    scene::GameRuntimeConfig rcfg;
    rcfg.scriptBaseDir = runTmp.Str();
    rcfg.assetBaseDir = runTmp.Str();
    core::Status st = runtime.Start(sceneJson, rcfg);
    if (!st.Ok()) std::printf("  RUNTIME ERROR: %s\n", st.Error().c_str());
    CHECK(st.Ok());
    // Ground + Pillar(prefab) + Spinner(script) + Beacon(behavior tree).
    CHECK_EQ(runtime.EntityCount(), 4u);
    CHECK_EQ(runtime.ScriptCount(), 1u);
    CHECK_EQ(runtime.BehaviorTreeCount(), 1u);
    CHECK(runtime.GameVars().Get("started").boolean);

    for (int i = 0; i < 120; ++i) runtime.Tick(1.0f / 60.0f); // 2.0s
    // The script ran: ticks incremented once per tick.
    CHECK_EQ(runtime.GameVars().Get("ticks").number, 120.0);

    // The bt:cycle named tree loaded from the unpacked behaviors/ and wrote
    // its blackboard entry after the 0.5s wait.
    ecs::Entity btEnt;
    {
        auto view = runtime.World().ViewAll<scene::SceneBehaviorTree>();
        CHECK_EQ(view.Size(), 1u);
        if (view.Size() == 1u) btEnt = runtime.World().EntityAt<scene::SceneBehaviorTree>(0);
    }
    CHECK(btEnt.IsValid());
    script::Value done = runtime.EntityBlackboardValue(btEnt, "bt_done");
    CHECK(done.type == script::Value::Type::Bool);
    CHECK(done.boolean);

    runtime.Stop();
}
