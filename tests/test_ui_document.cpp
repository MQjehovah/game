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

// G3-5: 9-slice math — corners keep their size, edges stretch along one axis,
// the center fills the remainder, and the UVs map the source texture slices.
TEST(UiNineSliceLayout) {
    // A 100x100 texture with a 20px border over a 200x80 dest rect.
    const math::Rect2 rect{10, 20, 200, 80};
    ui::NineSliceQuad q[9];
    CHECK(ui::ComputeNineSlice(rect, 20.0f, 100.0f, 100.0f, q));

    // Corners stay 20x20 and sit at the four corners of the dest rect.
    CHECK_NEAR(q[0].dest.x, 10.0f, 1e-4f);  CHECK_NEAR(q[0].dest.y, 20.0f, 1e-4f);
    CHECK_NEAR(q[0].dest.w, 20.0f, 1e-4f);  CHECK_NEAR(q[0].dest.h, 20.0f, 1e-4f);
    CHECK_NEAR(q[2].dest.x, 190.0f, 1e-4f); CHECK_NEAR(q[2].dest.y, 20.0f, 1e-4f);
    CHECK_NEAR(q[6].dest.x, 10.0f, 1e-4f);  CHECK_NEAR(q[6].dest.y, 80.0f, 1e-4f);
    CHECK_NEAR(q[8].dest.x, 190.0f, 1e-4f); CHECK_NEAR(q[8].dest.y, 80.0f, 1e-4f);

    // Top edge: full width minus corners, 20px tall.
    CHECK_NEAR(q[1].dest.x, 30.0f, 1e-4f);
    CHECK_NEAR(q[1].dest.w, 160.0f, 1e-4f);
    CHECK_NEAR(q[1].dest.h, 20.0f, 1e-4f);

    // Left edge: 20px wide, full height minus corners.
    CHECK_NEAR(q[3].dest.x, 10.0f, 1e-4f);
    CHECK_NEAR(q[3].dest.w, 20.0f, 1e-4f);
    CHECK_NEAR(q[3].dest.h, 40.0f, 1e-4f);

    // Center: the remainder.
    CHECK_NEAR(q[4].dest.x, 30.0f, 1e-4f);
    CHECK_NEAR(q[4].dest.y, 40.0f, 1e-4f);
    CHECK_NEAR(q[4].dest.w, 160.0f, 1e-4f);
    CHECK_NEAR(q[4].dest.h, 40.0f, 1e-4f);

    // UVs: corner quads sample the texture corners (0/1), center samples the
    // interior (0.2..0.8 in a 100px texture with a 20px border).
    CHECK_NEAR(q[0].uv0.x, 0.0f, 1e-4f); CHECK_NEAR(q[0].uv1.x, 0.2f, 1e-4f);
    CHECK_NEAR(q[0].uv1.y, 1.0f, 1e-4f);
    CHECK_NEAR(q[4].uv0.x, 0.2f, 1e-4f); CHECK_NEAR(q[4].uv1.x, 0.8f, 1e-4f);
    CHECK_NEAR(q[4].uv0.y, 0.2f, 1e-4f); CHECK_NEAR(q[4].uv1.y, 0.8f, 1e-4f);

    // Dest rect smaller than 2*slice on either axis -> false.
    ui::NineSliceQuad tiny[9];
    CHECK(!ui::ComputeNineSlice({0, 0, 30, 200}, 20.0f, 100.0f, 100.0f, tiny));
}

// G3-5: the 9-slice border survives the UI-document JSON round-trip.
TEST(UiNineSliceJsonRoundTrip) {
    ui::UiDocument doc;
    doc.root.rect = {0, 0, 1280, 720};
    ui::UiNode* frame = doc.root.AddChild(ui::UiNodeType::Panel, "Frame");
    frame->rect = {100, 100, 400, 300};
    frame->sprite = "assets/ui/frame.png";
    frame->slice = 24.0f;

    ui::UiDocument loaded;
    CHECK(loaded.LoadJson(doc.ToJson()));
    ui::UiNode* lf = loaded.Find("Frame");
    CHECK(lf != nullptr);
    if (lf) {
        CHECK_EQ(lf->sprite, "assets/ui/frame.png");
        CHECK_NEAR(lf->slice, 24.0f, 1e-4f);
    }
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

// G3-5: rich text parsing — [color:#rrggbb]..[/color] splits a label into
// colored spans; plain text stays one span in the base color; malformed tags
// render literally.
TEST(UiRichTextParse) {
    const gfx::Color base{1.0f, 1.0f, 1.0f, 1.0f};
    const gfx::Color red{1.0f, 0.0f, 0.0f, 1.0f};
    const gfx::Color green{0.0f, 1.0f, 0.0f, 1.0f};

    // Plain: one span in the base color.
    {
        const std::vector<ui::RichSpan> spans = ui::ParseRichText("hello", base);
        CHECK_EQ(spans.size(), 1u);
        if (spans.size() == 1u) {
            CHECK_EQ(spans[0].text, "hello");
            CHECK_NEAR(spans[0].color.r, 1.0f, 1e-4f);
        }
    }

    // Mixed: colored span + plain span.
    {
        const std::vector<ui::RichSpan> spans =
            ui::ParseRichText("[color:#ff0000]A[/color]B", base);
        CHECK_EQ(spans.size(), 2u);
        if (spans.size() == 2u) {
            CHECK_EQ(spans[0].text, "A");
            CHECK_NEAR(spans[0].color.r, 1.0f, 1e-4f);
            CHECK_NEAR(spans[0].color.g, 0.0f, 1e-4f);
            CHECK_NEAR(spans[0].color.b, 0.0f, 1e-4f);
            CHECK_EQ(spans[1].text, "B");
            CHECK_NEAR(spans[1].color.r, base.r, 1e-4f);
        }
    }

    // "color=" form + closing restores the base color.
    {
        const std::vector<ui::RichSpan> spans =
            ui::ParseRichText("[color=#00ff00]X[/color]Y", base);
        CHECK_EQ(spans.size(), 2u);
        if (spans.size() == 2u) {
            CHECK_NEAR(spans[0].color.g, 1.0f, 1e-4f);
            CHECK_NEAR(spans[0].color.r, 0.0f, 1e-4f);
        }
    }

    // Malformed tag renders literally (one span, base color).
    {
        const std::vector<ui::RichSpan> spans = ui::ParseRichText("[color:#zz]x", base);
        CHECK_EQ(spans.size(), 1u);
        if (spans.size() == 1u) CHECK_EQ(spans[0].text, "[color:#zz]x");
    }
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
