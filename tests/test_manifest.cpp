#include <string>
#include <vector>

#include "neon/neon.hpp"
#include "neon/scene/game_manifest.hpp"
#include "helpers.hpp"

using namespace neon;

// ---------------------------------------------------------------------------
// Full valid manifest: all fields parsed
// ---------------------------------------------------------------------------

TEST(ManifestFullValid) {
    const char* json = R"({
        "startScene": "scenes/level1.json",
        "window": {"w": 1280, "h": 720, "title": "My Game"},
        "packages": ["game.pack"],
        "title": "Neon Demo"
    })";
    auto res = scene::GameManifest::Load(json);
    CHECK(res.Ok());
    const scene::GameManifest& m = res.Value();
    CHECK_EQ(m.startScene, std::string("scenes/level1.json"));
    CHECK_EQ(m.window.w, 1280);
    CHECK_EQ(m.window.h, 720);
    CHECK_EQ(m.window.title, std::string("My Game"));
    CHECK_EQ(m.title, std::string("Neon Demo")); // top-level title preferred
    CHECK_EQ(m.packages.size(), 1u);
    CHECK_EQ(m.packages[0], std::string("game.pack"));
}

// ---------------------------------------------------------------------------
// startScene required and non-empty
// ---------------------------------------------------------------------------

TEST(ManifestMissingStartScene) {
    CHECK(!scene::GameManifest::Load(R"({"title": "X"})").Ok());
    CHECK(!scene::GameManifest::Load(R"({"window": {"w": 800, "h": 600}})").Ok());
    CHECK(!scene::GameManifest::Load(R"({"packages": []})").Ok());
}

TEST(ManifestEmptyStartScene) {
    CHECK(!scene::GameManifest::Load(R"({"startScene": ""})").Ok());
}

// ---------------------------------------------------------------------------
// Title fallback: no top-level title -> window.title; window.title missing is
// fine (empty title). Window defaults apply when absent.
// ---------------------------------------------------------------------------

TEST(ManifestTitleFallback) {
    auto res = scene::GameManifest::Load(R"({
        "startScene": "scenes/level1.json",
        "window": {"w": 800, "h": 600, "title": "Window Title"}
    })");
    CHECK(res.Ok());
    CHECK_EQ(res.Value().title, std::string("Window Title"));
    CHECK_EQ(res.Value().window.title, std::string("Window Title"));

    auto res2 = scene::GameManifest::Load(R"({"startScene": "scenes/level1.json"})");
    CHECK(res2.Ok());
    CHECK_EQ(res2.Value().title, std::string(""));
    CHECK_EQ(res2.Value().window.title, std::string(""));
    CHECK_EQ(res2.Value().window.w, 1280);
    CHECK_EQ(res2.Value().window.h, 720);
    CHECK_EQ(res2.Value().packages.size(), 0u);
}

// ---------------------------------------------------------------------------
// Invalid window settings
// ---------------------------------------------------------------------------

TEST(ManifestInvalidWindow) {
    CHECK(!scene::GameManifest::Load(R"({"startScene": "s", "window": {"w": 0}})").Ok());
    CHECK(!scene::GameManifest::Load(R"({"startScene": "s", "window": {"w": -4}})").Ok());
    CHECK(!scene::GameManifest::Load(R"({"startScene": "s", "window": {"h": 0}})").Ok());
    CHECK(!scene::GameManifest::Load(R"({"startScene": "s", "window": {"h": -1}})").Ok());
    CHECK(!scene::GameManifest::Load(R"({"startScene": "s", "window": {"w": "1280"}})").Ok());
    CHECK(!scene::GameManifest::Load(R"({"startScene": "s", "window": 5})").Ok());
}

// ---------------------------------------------------------------------------
// Packages: array of non-empty, non-duplicate strings
// ---------------------------------------------------------------------------

TEST(ManifestPackages) {
    CHECK(!scene::GameManifest::Load(R"({"startScene": "s", "packages": ["a.pack", "a.pack"]})").Ok());
    CHECK(!scene::GameManifest::Load(R"({"startScene": "s", "packages": [""]})").Ok());
    CHECK(!scene::GameManifest::Load(R"({"startScene": "s", "packages": [5]})").Ok());
    CHECK(!scene::GameManifest::Load(R"({"startScene": "s", "packages": "game.pack"})").Ok());

    auto res = scene::GameManifest::Load(R"({"startScene": "s", "packages": ["a.pack", "b.pack"]})");
    CHECK(res.Ok());
    CHECK_EQ(res.Value().packages.size(), 2u);
}

// ---------------------------------------------------------------------------
// Strict unknown-key rejection (top level and window)
// ---------------------------------------------------------------------------

TEST(ManifestUnknownTopLevel) {
    auto res = scene::GameManifest::Load(R"({"startScene": "s", "bogus": 1})");
    CHECK(!res.Ok());
    CHECK(res.Error().find("bogus") != std::string::npos);
}

TEST(ManifestUnknownWindowKey) {
    auto res = scene::GameManifest::Load(R"({"startScene": "s", "window": {"fullscreen": true}})");
    CHECK(!res.Ok());
    CHECK(res.Error().find("fullscreen") != std::string::npos);
}

TEST(ManifestNonObjectRoot) {
    CHECK(!scene::GameManifest::Load("this is not json").Ok());
    CHECK(!scene::GameManifest::Load("42").Ok());
}

// ---------------------------------------------------------------------------
// Round trip: Load -> ToJson -> Load -> equal fields
// ---------------------------------------------------------------------------

TEST(ManifestRoundTrip) {
    const char* json = R"({
        "startScene": "scenes/level1.json",
        "window": {"w": 1024, "h": 768, "title": "Window Title"},
        "packages": ["game.pack", "data.pack"],
        "title": "Neon Demo"
    })";
    auto a = scene::GameManifest::Load(json);
    CHECK(a.Ok());
    auto b = scene::GameManifest::Load(core::JsonWriter::Write(a.Value().ToJson()));
    CHECK(b.Ok());
    const scene::GameManifest& m = b.Value();
    CHECK_EQ(m.startScene, std::string("scenes/level1.json"));
    CHECK_EQ(m.window.w, 1024);
    CHECK_EQ(m.window.h, 768);
    CHECK_EQ(m.window.title, std::string("Window Title"));
    CHECK_EQ(m.title, std::string("Neon Demo"));
    CHECK_EQ(m.packages.size(), 2u);
    CHECK_EQ(m.packages[0], std::string("game.pack"));
    CHECK_EQ(m.packages[1], std::string("data.pack"));
}

// ---------------------------------------------------------------------------
// Validate() on a default-constructed (empty) manifest fails
// ---------------------------------------------------------------------------

TEST(ManifestValidateEmpty) {
    scene::GameManifest m;
    CHECK(!m.Validate().Ok());
    CHECK(!m.Validate().Error().empty());
}
