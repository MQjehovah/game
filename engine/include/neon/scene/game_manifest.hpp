#pragma once

#include <string>
#include <vector>

#include "neon/core/json.hpp"
#include "neon/core/result.hpp"

namespace neon::scene {

// Parsed game.json manifest: the single data-driven entry point that the
// neon_game player reads to boot a game and that the packager validates
// before bundling.
struct GameManifest {
    struct WindowSettings {
        int w = 1280;
        int h = 720;
        std::string title;
    };
    // Editor-facing export preset (Godot-style export presets in game.json):
    // the packager reads it for the build config; the editor writes it.
    struct ExportPreset {
        std::string platform = "windows"; // windows | linux | macos | web
        std::string icon;                 // project-relative icon path
        std::string description;          // one-line build description
    };

    std::string startScene;            // required; scene file the game boots into
    WindowSettings window;             // optional; defaults 1280x720
    std::vector<std::string> packages; // optional pack files; empty = loose files
    std::string title;                 // game display name (may equal window.title)
    ExportPreset exportPreset;         // optional editor export config

    // Parse + validate a game.json manifest. Strict: unknown keys at the top
    // level or inside "window" are rejected; startScene must be present and
    // non-empty; window w/h must be positive ints; packages must be an array
    // of non-empty, non-duplicate strings.
    static core::Result<GameManifest> Load(const std::string& jsonText);

    // Re-serialize the manifest back to a JSON DOM (round-trip + packager).
    core::Json ToJson() const;

    // Semantic validation of an in-memory manifest (startScene non-empty,
    // positive window size, sane package list).
    core::Status Validate() const;
};

} // namespace neon::scene
