#pragma once

// One-click project packager (T4.6): validates a NeonEngine project directory
// and bundles it into a distributable game — a `game.pack` (via core::PackWriter),
// a `run.bat` launcher and a copied player executable. The pure, ImGui-free
// logic lives in packager.cpp so the CLI (`--package`), the 打包 panel and the
// unit tests share one code path (tests link editor/src/packager.cpp, exactly
// like they link editor/src/history.cpp).
//
// Validation rules (see packager.cpp for details):
//   * game.json exists and GameManifest::Load accepts it; startScene resolves
//     to an existing file and its content is validated with the same per-entity
//     pass as scenes/ (whatever path it lives at).
//   * every scenes/*.json parses (SceneFile::Parse); entity prefab references
//     resolve to prefabs/<name>.json; mesh "obj:"/"gltf:" keys and material
//     texture slots resolve to existing files; script.path files exist; the
//     behaviorTree "tree" (inline JSON, or "bt:<name>" -> behaviors/<name>.bt.json)
//     loads via bt::BehaviorTree.
//   * every prefabs/*.json parses AND its component templates are walked for
//     mesh/script/behaviorTree references (prefab-referenced assets and scripts
//     are validated and collected, not just instance components); every
//     behaviors/*.bt.json parses.
//   * every scripts/*.lua plus every scene/prefab-referenced lua script passes
//     IScriptHost::CheckSyntax (when enabled).
//   * ".." path segments are rejected in asset/script/behaviorTree/startScene
//     references so nothing can escape the project directory.
// Fatal findings are collected into PackageReport::errors (packing is refused);
// non-fatal notes go to ::warnings (packing still proceeds).
//
// Packed file set (virtual paths, forward slashes): game.json (the manifest,
// re-serialized via GameManifest::ToJson), every scenes/*.json, prefabs/*.json,
// behaviors/*.bt.json and scripts/*.lua (extension match is case-insensitive),
// every file under assets/, plus any asset or script referenced by a scene or
// prefab that lives outside those directories (e.g. an "obj:models/foo.obj"
// mesh key or a "shared/ai.lua" script path).

#include <string>
#include <vector>

namespace neon::editor::pack {

// Outcome of a validate / pack run. Errors are fatal; warnings are not.
struct PackageReport {
    bool ok = false;
    std::vector<std::string> files;     // virtual paths written into the pack
    std::vector<std::string> warnings;  // non-fatal messages
    std::vector<std::string> errors;    // fatal validation / IO messages
    std::string packPath;      // "<outDir>/game.pack" ("" when not written)
    std::string runScriptPath; // "<outDir>/run.bat"   ("" when not written)
    std::string playerPath;    // "<outDir>/neon_game.exe" ("" when not copied)
    std::string updatePath;    // P2-5: "<outDir>/update.json" ("" when not written)
    std::string installPath;   // P2-5: "<outDir>/install.bat" ("" when not written)
    size_t fileCount = 0;
    size_t bytesWritten = 0;
    // G8-4 incremental packing: true when the previous run's per-file content
    // hashes matched exactly, so game.pack was kept instead of rebuilt.
    bool unchanged = false;
};

struct PackConfig {
    std::string projectDir;   // project root ("" = ".")
    std::string outDir;       // output directory ("" -> error)
    std::string playerSource; // exe to copy as neon_game.exe ("" = "build/neon_game.exe")
    bool copyPlayer = true;   // false skips the player copy (tests / CI)
    bool checkScriptSyntax = true; // run Lua CheckSyntax on every scripts/*.lua
    // P2-5 release metadata: version string + update host used by update.json
    // and the generated update.bat ("" disables the auto-update script).
    std::string version = "0.1.0";
    std::string updateUrl;
    // G8-4: when true, skip the unchanged-content check and always rebuild
    // game.pack (used by explicit "rebuild" actions / tests).
    bool force = false;
};

// Validation pass only: loads game.json, parses scenes/prefabs/behaviors and
// syntax-checks scripts, collecting the exact file set that would be packed.
// Never writes anything.
PackageReport ValidateProject(const PackConfig& cfg);

// Validate then write <outDir>/game.pack + run.bat and copy the player. When
// validation finds errors, ok=false and nothing is written. Missing player
// source is a warning, never a failure (T4.7 builds the real neon_game.exe).
PackageReport PackProject(const PackConfig& cfg);

} // namespace neon::editor::pack
