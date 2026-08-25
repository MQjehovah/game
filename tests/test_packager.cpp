// Task 4.6: one-click packaging (packager model).
//
// Exercises the pure validation/pack model in editor/src/packager.cpp: a sample
// project is built in a TempDir, validated and packed, and the produced
// game.pack is re-read with core::PackReader to assert the exact file set and
// byte-identical content. Each validation failure (missing game.json, missing
// startScene, missing asset, script syntax error, broken behavior-tree
// reference / inline tree, bad prefab, missing prefab reference, bad behavior
// file) is asserted individually, and packing refuses to write anything on
// validation errors.

#include <cstdio>
#include <string>
#include <vector>

#include "neon/core/pack.hpp"
#include "neon/neon.hpp"
#include "neon/scene/game_manifest.hpp"
#include "helpers.hpp"
#include "packager.hpp"

#if defined(_WIN32)
#include <direct.h>
#else
#include <sys/stat.h>
#endif

using namespace neon;
using namespace neon::editor::pack;

namespace {

bool Mkdir(const std::string& p) {
#if defined(_WIN32)
    return ::_mkdir(p.c_str()) == 0;
#else
    return ::mkdir(p.c_str(), 0777) == 0;
#endif
}

void Write(const std::string& path, const std::string& contents) {
    CHECK(test::WriteFileAll(path, contents));
}

const char kManifest[] = R"({"startScene": "scenes/main.json", "title": "Pack Test", "window": {"w": 800, "h": 600}})";

const char kScene[] = R"({"entities": [{"name": "Wolf", "prefab": "wolf", "components": {
  "transform": {"pos": [0,0,0], "rot": [0,0,0,1], "scale": [1,1,1]},
  "mesh": {"meshKey": "obj:assets/crate.obj", "material": {"albedoTex": "assets/wood.png"}},
  "script": {"backend": "lua", "path": "scripts/wolf.lua", "vars": {"aggro": 5}},
  "behaviorTree": {"tree": "bt:wolf_ai"}
}}]})";

const char kPrefab[] = R"({"components": {"health": {"hp": 50, "maxHp": 50}}})";
const char kTree[] = R"({"root": {"type": "sequence", "children": [{"type": "wait", "args": {"seconds": 0.5}}]}})";
const char kLua[] = "function on_update(ent, dt)\nend\n";
const char kObj[] = "v 0 0 0\nv 1 0 0\n";

// A complete, valid sample project: scene with prefab/script/BT refs, an OBJ
// asset and a PNG texture, a behavior tree and a script.
void BuildSampleProject(const std::string& proj) {
    const char* dirs[] = {"scenes", "prefabs", "behaviors", "scripts", "assets"};
    for (const char* d : dirs) {
        const bool ok = Mkdir(proj + "/" + d);
        CHECK(ok);
    }
    Write(proj + "/game.json", kManifest);
    Write(proj + "/scenes/main.json", kScene);
    Write(proj + "/prefabs/wolf.json", kPrefab);
    Write(proj + "/behaviors/wolf_ai.bt.json", kTree);
    Write(proj + "/scripts/wolf.lua", kLua);
    Write(proj + "/assets/crate.obj", kObj);
    Write(proj + "/assets/wood.png", std::string("\x89PNG\r\n\x1a\nfake png bytes", 20));
}

PackConfig DefaultCfg(const std::string& proj, const std::string& out) {
    PackConfig cfg;
    cfg.projectDir = proj;
    cfg.outDir = out;
    cfg.copyPlayer = false; // unit tests must not depend on the build tree
    return cfg;
}

bool AnyErrorContains(const PackageReport& r, const std::string& needle) {
    for (const std::string& e : r.errors)
        if (e.find(needle) != std::string::npos) return true;
    return false;
}

} // namespace

// ---------------------------------------------------------------------------
// Valid project -> validate + pack + PackReader round-trip
// ---------------------------------------------------------------------------

TEST(PackagerValidProjectPacks) {
    test::TempDir tmp;
    const std::string proj = tmp.Str();
    BuildSampleProject(proj);

    PackConfig cfg = DefaultCfg(proj, proj + "/out");
    PackageReport r = PackProject(cfg);
    if (!r.ok)
        for (const std::string& e : r.errors) std::printf("  PACK ERROR: %s\n", e.c_str());
    CHECK(r.ok);
    CHECK(r.errors.empty());
    CHECK(r.warnings.empty());
    CHECK_EQ(r.fileCount, 7u);
    CHECK_EQ(r.files.size(), 7u);

    std::string text;
    CHECK(test::ReadFileAll(cfg.outDir + "/game.pack", text));
    std::vector<uint8_t> bytes(text.begin(), text.end());
    core::PackReader reader(bytes);
    CHECK(reader.Valid());
    CHECK_EQ(reader.FileCount(), 7u);

    const char* kVirtual[] = {"game.json",          "scenes/main.json",
                              "prefabs/wolf.json",  "behaviors/wolf_ai.bt.json",
                              "scripts/wolf.lua",   "assets/crate.obj",
                              "assets/wood.png"};
    for (const char* vp : kVirtual) CHECK(reader.Has(vp));

    // Scene bytes are stored verbatim (byte-identical round-trip).
    auto scene = reader.Read("scenes/main.json");
    CHECK(scene.Ok());
    if (scene.Ok())
        CHECK_EQ(std::string(scene.Value().begin(), scene.Value().end()), std::string(kScene));

    // game.json is the normalized manifest (GameManifest::ToJson).
    auto manifest = reader.Read("game.json");
    CHECK(manifest.Ok());
    if (manifest.Ok()) {
        auto loaded = scene::GameManifest::Load(kManifest);
        CHECK(loaded.Ok());
        if (loaded.Ok())
            CHECK_EQ(std::string(manifest.Value().begin(), manifest.Value().end()),
                     core::JsonWriter::Write(loaded.Value().ToJson()));
    }

    // The run script is written and references the player + pack.
    CHECK(test::ReadFileAll(cfg.outDir + "/run.bat", text));
    CHECK(text.find("neon_game.exe --pack game.pack") != std::string::npos);

    // PackProject fills the report's output paths.
    CHECK_EQ(r.packPath, cfg.outDir + "/game.pack");
    CHECK_EQ(r.runScriptPath, cfg.outDir + "/run.bat");
    CHECK(r.playerPath.empty()); // copyPlayer=false
    CHECK(r.bytesWritten > 0u);
}

// P2-5: the packager emits release artifacts (update.json manifest with a
// checksum, install.bat, and an update.bat when an update host is configured).
TEST(PackagerReleaseArtifacts) {
    test::TempDir tmp;
    const std::string proj = tmp.Str();
    BuildSampleProject(proj);

    PackConfig cfg = DefaultCfg(proj, proj + "/out");
    cfg.version = "1.2.3";
    cfg.updateUrl = "https://example.com/neon";
    PackageReport r = PackProject(cfg);
    CHECK(r.ok);
    CHECK(!r.updatePath.empty());
    CHECK(!r.installPath.empty());

    std::string text;
    CHECK(test::ReadFileAll(cfg.outDir + "/update.json", text));
    std::string perr;
    core::Json upd = core::Json::Parse(text, &perr);
    CHECK(upd.IsObject());
    CHECK_EQ(upd.Get("version")->GetString(), "1.2.3");
    CHECK(upd.Get("packChecksum") != nullptr);
    CHECK_EQ(upd.Get("player")->GetString(), "neon_game.exe");

    CHECK(test::ReadFileAll(cfg.outDir + "/install.bat", text));
    CHECK(text.find("neon_game.exe") != std::string::npos);
    CHECK(text.find("CreateShortcut") != std::string::npos);

    CHECK(test::ReadFileAll(cfg.outDir + "/update.bat", text));
    CHECK(text.find("https://example.com/neon") != std::string::npos);
    CHECK(text.find("game.pack.new") != std::string::npos);
}

// ---------------------------------------------------------------------------
// Validation failures
// ---------------------------------------------------------------------------

TEST(PackagerMissingGameJson) {
    test::TempDir tmp; // empty project
    PackConfig cfg = DefaultCfg(tmp.Str(), tmp.Str() + "/out");
    PackageReport r = ValidateProject(cfg);
    CHECK(!r.ok);
    CHECK(AnyErrorContains(r, "game.json"));
    CHECK_EQ(r.files.size(), 0u);
}

TEST(PackagerMissingStartScene) {
    test::TempDir tmp;
    Write(tmp.Str() + "/game.json",
          "{\"startScene\": \"scenes/nope.json\", \"window\": {\"w\": 800, \"h\": 600}}");
    PackConfig cfg = DefaultCfg(tmp.Str(), tmp.Str() + "/out");
    PackageReport r = ValidateProject(cfg);
    CHECK(!r.ok);
    CHECK(AnyErrorContains(r, "startScene"));
}

TEST(PackagerMissingAsset) {
    test::TempDir tmp;
    BuildSampleProject(tmp.Str());
    const char scene[] = R"({"entities": [{"name": "Cube", "components": {
      "transform": {"pos": [0,0,0], "rot": [0,0,0,1], "scale": [1,1,1]},
      "mesh": {"meshKey": "obj:assets/gone.obj"}
    }}]})";
    Write(tmp.Str() + "/scenes/main.json", scene);
    PackConfig cfg = DefaultCfg(tmp.Str(), tmp.Str() + "/out");
    PackageReport r = ValidateProject(cfg);
    CHECK(!r.ok);
    CHECK(AnyErrorContains(r, "gone.obj"));
}

TEST(PackagerScriptSyntaxError) {
    test::TempDir tmp;
    BuildSampleProject(tmp.Str());
    Write(tmp.Str() + "/scripts/wolf.lua", "function on_update(ent, dt)\n  this is not lua !!!\nend\n");
    PackConfig cfg = DefaultCfg(tmp.Str(), tmp.Str() + "/out");
    PackageReport r = ValidateProject(cfg);
    CHECK(!r.ok);
    CHECK(AnyErrorContains(r, "wolf.lua"));
    CHECK(AnyErrorContains(r, "syntax error"));
}

TEST(PackagerBrokenBehaviorTreeRef) {
    test::TempDir tmp;
    BuildSampleProject(tmp.Str());
    const char scene[] = R"({"entities": [{"name": "N", "components": {
      "transform": {"pos": [0,0,0], "rot": [0,0,0,1], "scale": [1,1,1]},
      "behaviorTree": {"tree": "bt:nope"}
    }}]})";
    Write(tmp.Str() + "/scenes/main.json", scene);
    PackConfig cfg = DefaultCfg(tmp.Str(), tmp.Str() + "/out");
    PackageReport r = ValidateProject(cfg);
    CHECK(!r.ok);
    CHECK(AnyErrorContains(r, "nope"));
}

TEST(PackagerInlineBrokenTree) {
    test::TempDir tmp;
    BuildSampleProject(tmp.Str());
    const char scene[] = R"({"entities": [{"name": "N", "components": {
      "transform": {"pos": [0,0,0], "rot": [0,0,0,1], "scale": [1,1,1]},
      "behaviorTree": {"tree": "{not json"}
    }}]})";
    Write(tmp.Str() + "/scenes/main.json", scene);
    PackConfig cfg = DefaultCfg(tmp.Str(), tmp.Str() + "/out");
    PackageReport r = ValidateProject(cfg);
    CHECK(!r.ok);
    CHECK(AnyErrorContains(r, "behavior tree"));
}

TEST(PackagerBadPrefabFile) {
    test::TempDir tmp;
    BuildSampleProject(tmp.Str());
    Write(tmp.Str() + "/prefabs/bad.json", "not json at all");
    PackConfig cfg = DefaultCfg(tmp.Str(), tmp.Str() + "/out");
    PackageReport r = ValidateProject(cfg);
    CHECK(!r.ok);
    CHECK(AnyErrorContains(r, "prefab"));
}

TEST(PackagerMissingPrefabReference) {
    test::TempDir tmp;
    BuildSampleProject(tmp.Str());
    const char scene[] = R"({"entities": [{"name": "Ghost", "prefab": "ghost", "components": {
      "transform": {"pos": [0,0,0], "rot": [0,0,0,1], "scale": [1,1,1]}
    }}]})";
    Write(tmp.Str() + "/scenes/main.json", scene);
    PackConfig cfg = DefaultCfg(tmp.Str(), tmp.Str() + "/out");
    PackageReport r = ValidateProject(cfg);
    CHECK(!r.ok);
    CHECK(AnyErrorContains(r, "ghost"));
}

TEST(PackagerBadBehaviorFile) {
    test::TempDir tmp;
    BuildSampleProject(tmp.Str());
    Write(tmp.Str() + "/behaviors/bad.bt.json", "{}"); // no "root" node
    PackConfig cfg = DefaultCfg(tmp.Str(), tmp.Str() + "/out");
    PackageReport r = ValidateProject(cfg);
    CHECK(!r.ok);
    CHECK(AnyErrorContains(r, "bad.bt.json"));
}

// Packing fails closed: validation errors mean nothing is written to outDir.
TEST(PackagerFailsClosedWithoutWriting) {
    test::TempDir tmp;
    BuildSampleProject(tmp.Str());
    Write(tmp.Str() + "/scripts/wolf.lua", "broken !!!");
    PackConfig cfg = DefaultCfg(tmp.Str(), tmp.Str() + "/out");
    PackageReport r = PackProject(cfg);
    CHECK(!r.ok);
    std::string text;
    CHECK(!test::ReadFileAll(cfg.outDir + "/game.pack", text));
}

// The manifest / startScene reference alone drives the minimal pack: a project
// with only game.json + one scene packs cleanly (no prefabs/scripts/behaviors).
TEST(PackagerMinimalProject) {
    test::TempDir tmp;
    const std::string proj = tmp.Str();
    CHECK(Mkdir(proj + "/scenes"));
    Write(proj + "/game.json", kManifest);
    Write(proj + "/scenes/main.json",
          R"({"entities": [{"name": "P", "components": {
             "transform": {"pos": [0,0,0], "rot": [0,0,0,1], "scale": [1,1,1]},
             "mesh": {"meshKey": "cube"}
          }}]})");
    PackConfig cfg = DefaultCfg(proj, proj + "/out");
    PackageReport r = PackProject(cfg);
    if (!r.ok)
        for (const std::string& e : r.errors) std::printf("  PACK ERROR: %s\n", e.c_str());
    CHECK(r.ok);
    CHECK_EQ(r.fileCount, 2u); // game.json + scenes/main.json
    std::string text;
    CHECK(test::ReadFileAll(cfg.outDir + "/game.pack", text));
    core::PackReader reader(std::vector<uint8_t>(text.begin(), text.end()));
    CHECK(reader.Valid());
    CHECK(reader.Has("game.json"));
    CHECK(reader.Has("scenes/main.json"));
    CHECK(!reader.Has("assets/crate.obj"));
}

// ---------------------------------------------------------------------------
// Code-review fixes: startScene content validation, prefab refs, script refs,
// traversal containment, BOM tolerance.
// ---------------------------------------------------------------------------

// Gap 1: a startScene outside scenes/ (e.g. custom/main.json) must be validated
// with the same per-entity pass — a missing prefab/mesh inside it is fatal and
// packing fails closed, exactly like a scene under scenes/.
TEST(PackagerStartSceneCustomPathValidated) {
    test::TempDir tmp;
    const std::string proj = tmp.Str();
    CHECK(Mkdir(proj + "/custom"));
    Write(proj + "/game.json",
          "{\"startScene\": \"custom/main.json\", \"window\": {\"w\": 800, \"h\": 600}}");
    Write(proj + "/custom/main.json",
          R"({"entities": [{"name": "Ghost", "prefab": "ghost", "components": {
             "transform": {"pos": [0,0,0], "rot": [0,0,0,1], "scale": [1,1,1]},
             "mesh": {"meshKey": "obj:assets/nope.obj"}
          }}]})");
    PackConfig cfg = DefaultCfg(proj, proj + "/out");
    PackageReport r = ValidateProject(cfg);
    CHECK(!r.ok);
    CHECK(AnyErrorContains(r, "ghost"));    // missing prefab reference
    CHECK(AnyErrorContains(r, "nope.obj")); // missing mesh asset in the startScene

    PackageReport p = PackProject(cfg);
    CHECK(!p.ok);
    std::string text;
    CHECK(!test::ReadFileAll(cfg.outDir + "/game.pack", text));
}

// A startScene under scenes/ is still walked exactly once (dedupe against the
// scenes loop) — a broken scene there reports a single error set.
TEST(PackagerStartSceneUnderScenesNotDoubleWalked) {
    test::TempDir tmp;
    const std::string proj = tmp.Str();
    BuildSampleProject(proj);
    // Same missing mesh in the scene (which IS the startScene): the error must
    // appear once, not twice (once from the scenes loop + once from startScene).
    Write(proj + "/scenes/main.json",
          R"({"entities": [{"name": "N", "components": {
             "transform": {"pos": [0,0,0], "rot": [0,0,0,1], "scale": [1,1,1]},
             "mesh": {"meshKey": "obj:assets/gone.obj"}
          }}]})");
    PackConfig cfg = DefaultCfg(proj, proj + "/out");
    PackageReport r = ValidateProject(cfg);
    CHECK(!r.ok);
    size_t count = 0;
    for (const std::string& e : r.errors)
        if (e.find("gone.obj") != std::string::npos) ++count;
    CHECK_EQ(count, 1u); // walked once: no duplicate errors
}

// Gap 2a: a prefab referencing a mesh OUTSIDE assets/ is validated AND the file
// is collected into the pack (previously PACK OK but the entry was missing).
TEST(PackagerPrefabMeshCollected) {
    test::TempDir tmp;
    const std::string proj = tmp.Str();
    BuildSampleProject(proj);
    CHECK(Mkdir(proj + "/models"));
    Write(proj + "/prefabs/wolf.json",
          R"({"components": {"health": {"hp": 50, "maxHp": 50},
              "mesh": {"meshKey": "obj:models/crate.obj"}}})");
    Write(proj + "/models/crate.obj", kObj);
    PackConfig cfg = DefaultCfg(proj, proj + "/out");
    PackageReport r = PackProject(cfg);
    if (!r.ok)
        for (const std::string& e : r.errors) std::printf("  PACK ERROR: %s\n", e.c_str());
    CHECK(r.ok);
    std::string text;
    CHECK(test::ReadFileAll(cfg.outDir + "/game.pack", text));
    core::PackReader reader(std::vector<uint8_t>(text.begin(), text.end()));
    CHECK(reader.Valid());
    CHECK(reader.Has("models/crate.obj"));
}

// A prefab referencing a MISSING mesh must fail validation (previously PACK OK).
TEST(PackagerPrefabMissingMesh) {
    test::TempDir tmp;
    const std::string proj = tmp.Str();
    BuildSampleProject(proj);
    Write(proj + "/prefabs/wolf.json",
          R"({"components": {"mesh": {"meshKey": "obj:models/gone.obj"}}})");
    PackConfig cfg = DefaultCfg(proj, proj + "/out");
    PackageReport r = ValidateProject(cfg);
    CHECK(!r.ok);
    CHECK(AnyErrorContains(r, "gone.obj"));
}

// Gap 2b: a scene-referenced script OUTSIDE scripts/ is collected into the pack
// (previously it passed existence but was never packed).
TEST(PackagerScriptOutsideScriptsCollected) {
    test::TempDir tmp;
    const std::string proj = tmp.Str();
    BuildSampleProject(proj);
    CHECK(Mkdir(proj + "/ai"));
    Write(proj + "/ai/ai.lua", kLua);
    Write(proj + "/scenes/main.json",
          R"({"entities": [{"name": "N", "components": {
             "transform": {"pos": [0,0,0], "rot": [0,0,0,1], "scale": [1,1,1]},
             "script": {"backend": "lua", "path": "ai/ai.lua"}
          }}]})");
    PackConfig cfg = DefaultCfg(proj, proj + "/out");
    PackageReport r = PackProject(cfg);
    if (!r.ok)
        for (const std::string& e : r.errors) std::printf("  PACK ERROR: %s\n", e.c_str());
    CHECK(r.ok);
    std::string text;
    CHECK(test::ReadFileAll(cfg.outDir + "/game.pack", text));
    core::PackReader reader(std::vector<uint8_t>(text.begin(), text.end()));
    CHECK(reader.Valid());
    CHECK(reader.Has("ai/ai.lua"));
}

// A referenced out-of-scripts script with a syntax error is also caught.
TEST(PackagerScriptOutsideScriptsSyntaxError) {
    test::TempDir tmp;
    const std::string proj = tmp.Str();
    BuildSampleProject(proj);
    CHECK(Mkdir(proj + "/ai"));
    Write(proj + "/ai/ai.lua", "this is not lua !!!");
    Write(proj + "/scenes/main.json",
          R"({"entities": [{"name": "N", "components": {
             "transform": {"pos": [0,0,0], "rot": [0,0,0,1], "scale": [1,1,1]},
             "script": {"backend": "lua", "path": "ai/ai.lua"}
          }}]})");
    PackConfig cfg = DefaultCfg(proj, proj + "/out");
    PackageReport r = ValidateProject(cfg);
    CHECK(!r.ok);
    CHECK(AnyErrorContains(r, "ai/ai.lua"));
    CHECK(AnyErrorContains(r, "syntax error"));
}

// `..` refs are rejected so nothing escapes the project directory.
TEST(PackagerTraversalRejected) {
    test::TempDir tmp;
    const std::string proj = tmp.Str();
    BuildSampleProject(proj);
    Write(proj + "/scenes/main.json",
          R"({"entities": [{"name": "N", "components": {
             "transform": {"pos": [0,0,0], "rot": [0,0,0,1], "scale": [1,1,1]},
             "mesh": {"meshKey": "obj:../secret.obj"},
             "script": {"backend": "lua", "path": "../evil.lua"}
          }}]})");
    PackConfig cfg = DefaultCfg(proj, proj + "/out");
    PackageReport r = ValidateProject(cfg);
    CHECK(!r.ok);
    bool meshTraversal = false;
    bool scriptTraversal = false;
    for (const std::string& e : r.errors) {
        if (e.find("secret.obj") != std::string::npos) meshTraversal = true;
        if (e.find("evil.lua") != std::string::npos) scriptTraversal = true;
    }
    CHECK(meshTraversal);
    CHECK(scriptTraversal);
}

// A UTF-8 BOM in a scene file (Notepad/PowerShell default) no longer trips the
// JSON parser into a cryptic "unexpected character" error.
TEST(PackagerSceneBomTolerated) {
    test::TempDir tmp;
    const std::string proj = tmp.Str();
    CHECK(Mkdir(proj + "/scenes"));
    Write(proj + "/game.json", kManifest);
    Write(proj + "/scenes/main.json",
          std::string("\xEF\xBB\xBF", 3) +
              R"({"entities": [{"name": "P", "components": {
                 "transform": {"pos": [0,0,0], "rot": [0,0,0,1], "scale": [1,1,1]},
                 "mesh": {"meshKey": "cube"}
              }}]})");
    PackConfig cfg = DefaultCfg(proj, proj + "/out");
    PackageReport r = ValidateProject(cfg);
    if (!r.ok)
        for (const std::string& e : r.errors) std::printf("  PACK ERROR: %s\n", e.c_str());
    CHECK(r.ok);
    CHECK(r.errors.empty());
}

// G8-4 incremental packing: an unchanged project is detected via the per-file
// content-hash manifest and game.pack is kept (not rewritten).
TEST(PackagerIncrementalSkipsUnchanged) {
    test::TempDir tmp;
    const std::string proj = tmp.Str();
    BuildSampleProject(proj);
    PackConfig cfg = DefaultCfg(proj, proj + "/out");
    cfg.version = "0.1.0";

    PackageReport first = PackProject(cfg);
    CHECK(first.ok);
    CHECK(!first.unchanged);
    std::string pack1;
    CHECK(test::ReadFileAll(proj + "/out/game.pack", pack1));
    std::string manifest;
    CHECK(test::ReadFileAll(proj + "/out/pack_manifest.json", manifest));
    CHECK(manifest.find("wood.png") != std::string::npos);

    // Second run with identical inputs: skipped, and the pack on disk is the
    // same byte stream (kept, not rebuilt).
    PackageReport second = PackProject(cfg);
    CHECK(second.ok);
    CHECK(second.unchanged);
    CHECK(second.errors.empty());
    std::string pack2;
    CHECK(test::ReadFileAll(proj + "/out/game.pack", pack2));
    CHECK(pack1 == pack2);
}

// Changing any packed file's content invalidates the manifest and rebuilds.
TEST(PackagerIncrementalRebuildsOnChange) {
    test::TempDir tmp;
    const std::string proj = tmp.Str();
    BuildSampleProject(proj);
    PackConfig cfg = DefaultCfg(proj, proj + "/out");

    PackageReport first = PackProject(cfg);
    CHECK(first.ok);
    CHECK(!first.unchanged);
    std::string pack1;
    CHECK(test::ReadFileAll(proj + "/out/game.pack", pack1));

    Write(proj + "/assets/wood.png", "different texture bytes");
    PackageReport second = PackProject(cfg);
    CHECK(second.ok);
    CHECK(!second.unchanged);
    std::string pack2;
    CHECK(test::ReadFileAll(proj + "/out/game.pack", pack2));
    CHECK(pack1 != pack2);

    // And a third run is unchanged again (manifest re-synced).
    PackageReport third = PackProject(cfg);
    CHECK(third.ok);
    CHECK(third.unchanged);
}

// A version bump forces a rebuild even when file content is identical.
TEST(PackagerIncrementalVersionChangeRebuilds) {
    test::TempDir tmp;
    const std::string proj = tmp.Str();
    BuildSampleProject(proj);
    PackConfig cfg = DefaultCfg(proj, proj + "/out");
    cfg.version = "0.1.0";
    CHECK(PackProject(cfg).ok);

    cfg.version = "0.2.0";
    PackageReport bumped = PackProject(cfg);
    CHECK(bumped.ok);
    CHECK(!bumped.unchanged);

    // Back to the recorded version: unchanged again.
    cfg.version = "0.2.0";
    PackageReport again = PackProject(cfg);
    CHECK(again.ok);
    CHECK(again.unchanged);
}

// force=true bypasses the unchanged check (explicit rebuild action).
TEST(PackagerIncrementalForceRebuilds) {
    test::TempDir tmp;
    const std::string proj = tmp.Str();
    BuildSampleProject(proj);
    PackConfig cfg = DefaultCfg(proj, proj + "/out");
    CHECK(PackProject(cfg).ok);
    cfg.force = true;
    PackageReport forced = PackProject(cfg);
    CHECK(forced.ok);
    CHECK(!forced.unchanged);
}
