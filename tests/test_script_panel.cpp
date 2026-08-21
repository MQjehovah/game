// Task 4.5: script panel (engine + editor pure model).
//
// Engine side (tested here): IScriptHost::CheckSyntax (valid -> true, invalid ->
// false with message + line) and LuaHost reusability after a failed Load. A
// successful CheckSyntax must not clobber the host's loaded chunk.
//
// Editor side: the ImGui-free script panel model (editor/src/script_panel_model.hpp)
// — recursive *.lua enumeration under <projectDir>/scripts/, per-file syntax
// checks surfacing the error message + line, and SceneScriptFields equality
// (the value the editor's undo command attaches/detaches).

#include <string>
#include <vector>

#include "neon/neon.hpp"
#include "neon/script/script.hpp"
#include "helpers.hpp"
#include "script_panel_model.hpp"

#if defined(_WIN32)
#include <direct.h>
#else
#include <sys/stat.h>
#endif

using namespace neon;
using namespace neon::editor;

namespace {

std::unique_ptr<script::IScriptHost> MakeHost() {
    auto host = script::CreateLuaHost();
    CHECK(host != nullptr);
    CHECK(host->Init());
    return host;
}

bool MakeTestDir(const std::string& p) {
#if defined(_WIN32)
    return ::_mkdir(p.c_str()) == 0;
#else
    return ::mkdir(p.c_str(), 0777) == 0;
#endif
}

core::Json JNum(double v) {
    core::Json j;
    j.type_ = core::Json::Type::Number;
    j.number_ = v;
    return j;
}

} // namespace

// ---------------------------------------------------------------------------
// IScriptHost::CheckSyntax
// ---------------------------------------------------------------------------

TEST(ScriptCheckSyntaxValid) {
    auto host = MakeHost();
    CHECK(host->CheckSyntax("function add(a, b) return a + b end"));
    CHECK(host->LastError().message.empty());
    host->Shutdown();
}

TEST(ScriptCheckSyntaxInvalid) {
    auto host = MakeHost();
    CHECK(!host->CheckSyntax("this is not lua !!!"));
    CHECK(!host->LastError().message.empty());
    CHECK(host->LastError().line > 0); // the error carries its source line
    host->Shutdown();
}

// A failed Load (syntax error) must leave the host fully usable: the next Load
// of a valid script still compiles and runs (the editor checks many files).
TEST(ScriptFailedLoadThenValidLoadReusable) {
    auto host = MakeHost();
    CHECK(!host->Load("this is not lua !!!"));
    CHECK(!host->LastError().message.empty());
    CHECK(host->LastError().line > 0);

    CHECK(host->Load("x = 10"));
    CHECK(host->Run().Ok());
    auto res = host->GetGlobal("x");
    CHECK(res.Ok());
    CHECK(res.Value().type == script::Value::Type::Number);
    CHECK_EQ(res.Value().number, 10.0);
    host->Shutdown();
}

// CheckSyntax is validation-only: unlike Load it must not replace the loaded
// chunk, so a host linting files keeps a running script intact.
TEST(ScriptCheckSyntaxDoesNotClobberLoadedChunk) {
    auto host = MakeHost();
    CHECK(host->Load("function f() return 42 end"));
    CHECK(host->Run().Ok());
    CHECK(host->HasFunction("f"));

    CHECK(!host->CheckSyntax("garbage !!!"));
    CHECK(!host->LastError().message.empty());

    auto res = host->Call("f", {});
    CHECK(res.Ok());
    CHECK(res.Value().type == script::Value::Type::Number);
    CHECK_EQ(res.Value().number, 42.0);
    host->Shutdown();
}

// ---------------------------------------------------------------------------
// editor: script file enumeration + per-file syntax checks (pure model)
// ---------------------------------------------------------------------------

TEST(ScriptPanelListLuaFilesRecursive) {
    test::TempDir tmp;
    const std::string base = tmp.Str();
    CHECK(MakeTestDir(base + "/scripts"));
    CHECK(MakeTestDir(base + "/scripts/sub"));
    CHECK(test::WriteFileAll(base + "/scripts/good.lua",
                             "function on_update(ent, dt)\n  Log('tick')\nend\n"));
    CHECK(test::WriteFileAll(base + "/scripts/broken.lua",
                             "function on_update(ent, dt)\n  this is not lua !!!\nend\n"));
    CHECK(test::WriteFileAll(base + "/scripts/sub/helper.lua", "return 1\n"));
    CHECK(test::WriteFileAll(base + "/scripts/readme.txt", "not a script"));

    std::vector<std::string> files;
    ListLuaFiles(base + "/scripts", "scripts", files);
    CHECK_EQ(files.size(), 3u); // only *.lua, recursive
    // Deterministic order + project-relative paths.
    CHECK_EQ(files[0], std::string("scripts/broken.lua"));
    CHECK_EQ(files[1], std::string("scripts/good.lua"));
    CHECK_EQ(files[2], std::string("scripts/sub/helper.lua"));

    CHECK_EQ(ScriptsDir("proj"), std::string("proj/scripts"));
    CHECK_EQ(ScriptsDir(""), std::string("./scripts"));
}

TEST(ScriptPanelCheckScriptFile) {
    test::TempDir tmp;
    const std::string base = tmp.Str();
    CHECK(MakeTestDir(base + "/scripts"));
    CHECK(test::WriteFileAll(base + "/scripts/good.lua",
                             "function on_update(ent, dt)\n  Log('tick')\nend\n"));
    CHECK(test::WriteFileAll(base + "/scripts/broken.lua",
                             "function on_update(ent, dt)\n  this is not lua !!!\nend\n"));

    auto host = MakeHost();
    ScriptCheckResult good = CheckScriptFile(*host, base, "scripts/good.lua");
    CHECK(good.ok);
    CHECK(good.message.empty());
    CHECK_EQ(good.line, 0);

    ScriptCheckResult broken = CheckScriptFile(*host, base, "scripts/broken.lua");
    CHECK(!broken.ok);
    CHECK(!broken.message.empty());
    CHECK(broken.line > 0); // the panel can show "错误 (行 N)"
    CHECK_EQ(broken.path, std::string("scripts/broken.lua"));

    // A missing / unreadable file surfaces as a failure, never a crash.
    ScriptCheckResult missing = CheckScriptFile(*host, base, "scripts/nope.lua");
    CHECK(!missing.ok);
    host->Shutdown();
}

// ---------------------------------------------------------------------------
// editor: SceneScriptFields (the value the attach/detach undo command edits)
// ---------------------------------------------------------------------------

TEST(ScriptFieldsEquality) {
    core::Json v1;
    v1.type_ = core::Json::Type::Object;
    v1.object_["n"] = JNum(1);
    core::Json v2;
    v2.type_ = core::Json::Type::Object;
    v2.object_["n"] = JNum(1);

    SceneScriptFields a{"lua", "scripts/wolf.lua", v1};
    SceneScriptFields b{"lua", "scripts/wolf.lua", v2};
    CHECK(ScriptFieldsEqual(a, b)); // same backend/path + equivalent vars

    b.path = "scripts/fox.lua";
    CHECK(!ScriptFieldsEqual(a, b));

    b.path = "scripts/wolf.lua";
    b.vars = core::Json{};
    CHECK(!ScriptFieldsEqual(a, b)); // vars differ (object vs null)
}
