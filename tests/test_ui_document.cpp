#include <string>

#include "neon/neon.hpp"
#include "helpers.hpp"
#include "test_backend.hpp"

using namespace neon;

// Data-driven UI document: JSON round-trip, node find/hit-test.

TEST(UiDocumentJsonRoundTrip) {
    ui::UiDocument doc;
    doc.root.rect = {0, 0, 1280, 720};
    doc.root.color = {0.1f, 0.12f, 0.16f, 0.9f};
    ui::UiNode* panel = doc.root.AddChild(ui::UiNodeType::Panel, "Menu");
    panel->rect = {100, 100, 400, 300};
    ui::UiNode* btn = panel->AddChild(ui::UiNodeType::Button, "Start");
    btn->rect = {120, 130, 200, 48};
    btn->text = "开始游戏";
    ui::UiNode* bar = panel->AddChild(ui::UiNodeType::Bar, "Hp");
    bar->rect = {120, 200, 300, 18};
    bar->fill = 0.65f;

    const std::string json = doc.ToJson();
    CHECK(!json.empty());

    ui::UiDocument loaded;
    CHECK(loaded.LoadJson(json));
    CHECK_EQ(loaded.root.children.size(), 1u);
    ui::UiNode* menu = loaded.Find("Menu");
    CHECK(menu != nullptr);
    CHECK(menu->type == ui::UiNodeType::Panel);
    CHECK_NEAR(menu->rect.x, 100.0f, 1e-4f);
    CHECK_NEAR(menu->rect.w, 400.0f, 1e-4f);
    ui::UiNode* start = loaded.Find("Start");
    CHECK(start != nullptr);
    CHECK(start->type == ui::UiNodeType::Button);
    CHECK_EQ(start->text, "开始游戏");
    CHECK_NEAR(start->rect.y, 130.0f, 1e-4f);
    ui::UiNode* hp = loaded.Find("Hp");
    CHECK(hp != nullptr);
    CHECK_NEAR(hp->fill, 0.65f, 1e-4f);
    CHECK(hp->type == ui::UiNodeType::Bar);

    // Re-serializing the loaded doc must be stable.
    const std::string json2 = loaded.ToJson();
    ui::UiDocument loaded2;
    CHECK(loaded2.LoadJson(json2));
    CHECK(loaded2.Find("Start") != nullptr);
    CHECK_EQ(loaded2.Find("Start")->text, "开始游戏");
}

TEST(UiDocumentHitTest) {
    ui::UiDocument doc;
    doc.root.rect = {0, 0, 1280, 720};
    ui::UiNode* menu = doc.root.AddChild(ui::UiNodeType::Panel, "Menu");
    menu->rect = {50, 50, 300, 200};
    ui::UiNode* btn = menu->AddChild(ui::UiNodeType::Button, "Start");
    btn->rect = {70, 80, 120, 40};

    // Local rects: the button's absolute rect is (120,130)-(240,170).
    CHECK_NEAR(btn->AbsoluteRect().x, 120.0f, 1e-4f);
    CHECK_NEAR(btn->AbsoluteRect().y, 130.0f, 1e-4f);
    CHECK(doc.HitTest({130, 140}) == btn); // inside the button
    CHECK(doc.HitTest({200, 100}) == menu); // inside the panel, outside button
    CHECK(doc.HitTest({20, 20}) == &doc.root); // root panel
    CHECK(doc.HitTest({1000, 700}) == &doc.root);

    btn->visible = false;
    CHECK(doc.HitTest({130, 140}) == menu); // hidden button skipped

    CHECK(doc.Find("Start") == btn);
    CHECK(doc.Find("missing") == nullptr);
}

TEST(UiFilenameSuffixCheck) {
    // The editor's UI-file scan filters by suffix; the std::string compare
    // must match ".ui.json" on a plain ASCII filename.
    std::string name = "main.ui.json";
    CHECK_EQ(name.size(), 12u);
    // ".ui.json" is EIGHT characters: dot + "ui" + dot + "json".
    CHECK_EQ(name.compare(name.size() - 8, 8, ".ui.json"), 0);
    CHECK_EQ(name.substr(name.size() - 8), ".ui.json");
    CHECK_EQ(name.rfind(".ui.json"), 4u);
}

TEST(SystemCjkFontDynamicGlyphs) {
    // The dynamic-glyph path must rasterize common CJK text; a missing glyph
    // makes Measure return ~0 (text renders as nothing/solid blocks).
    test::HeadlessAssetFixture fx;
    gfx::Font font = fx.assets.LoadSystemCJKFont(24);
    if (!font.Valid()) return; // no system font in CI -> skip
    const math::Vec2 title = font.Measure("贪吃蛇", 44.0f);
    const math::Vec2 button = font.Measure("开始游戏", 22.0f);
    CHECK(title.x > 60.0f);  // 3 CJK glyphs at 44px
    CHECK(button.x > 50.0f); // 4 CJK glyphs at 22px
    CHECK(button.x < 160.0f);
}
