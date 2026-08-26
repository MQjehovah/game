#include <set>
#include <string>

#include "neon/neon.hpp"
#include "neon/script/input_map.hpp"
#include "helpers.hpp"

using namespace neon;

namespace {

// A frame-stepped IInput mock: keys move in/out of `held`, with Pressed/Released
// edge sets that EndFrame() clears — mirroring the real input layer.
struct FrameInput : platform::IInput {
    std::set<platform::Key> held;
    std::set<platform::Key> pressedEdge;
    std::set<platform::Key> releasedEdge;
    void HandleEvent(const platform::InputEvent&) override {}
    bool IsDown(platform::Key k) const override { return held.count(k) != 0; }
    bool Pressed(platform::Key k) const override { return pressedEdge.count(k) != 0; }
    bool Released(platform::Key k) const override { return releasedEdge.count(k) != 0; }
    bool MouseDown(platform::MouseButton) const override { return false; }
    bool MousePressed(platform::MouseButton) const override { return false; }
    bool MouseReleased(platform::MouseButton) const override { return false; }
    math::Vec2 MousePos() const override { return {}; }
    math::Vec2 MouseDelta() const override { return {}; }
    float WheelDelta() const override { return 0.0f; }
    void EndFrame() override {
        pressedEdge.clear();
        releasedEdge.clear();
    }

    void Press(platform::Key k) {
        held.insert(k);
        pressedEdge.insert(k);
    }
    void Release(platform::Key k) {
        held.erase(k);
        releasedEdge.insert(k);
    }
    // Advances the map one frame at `dt` seconds, then clears edges.
    void Step(script::InputMap& map, float dt) {
        map.Update(dt, *this);
        EndFrame();
    }
};

} // namespace

// G7-3: chord modifiers — every declared modifier must be held for the action
// to register; without the chord it is fully suppressed.
TEST(InputMapChordModifiers) {
    script::InputMap map;
    std::string err;
    CHECK(map.Load(R"({"actions":{
        "map": {"keys":["M"], "modifiers":["Ctrl"]}
    }})", &err));
    FrameInput in;

    in.Press(platform::Key::M);          // M without Ctrl
    CHECK(!map.Pressed("map", in));
    CHECK(!map.IsDown("map", in));
    in.EndFrame();

    in.Press(platform::Key::Control);    // now hold Ctrl
    CHECK(!map.Pressed("map", in));      // M press edge already consumed
    in.Press(platform::Key::M);          // re-press with Ctrl held
    CHECK(map.IsDown("map", in));
    CHECK(map.Pressed("map", in));
    in.EndFrame();

    in.Release(platform::Key::Control);  // releasing the chord kills it
    CHECK(!map.Pressed("map", in));
}

// G7-3: double-tap — the 1st press within the window is consumed (no edge); the
// 2nd press inside the window fires; a later press starts a fresh window.
TEST(InputMapDoubleTap) {
    script::InputMap map;
    std::string err;
    CHECK(map.Load(R"({"actions":{
        "dodge": {"keys":["Space"], "doubleTapMs":200}
    }})", &err));
    FrameInput in;

    in.Press(platform::Key::Space);      // 1st tap: suppressed
    in.Step(map, 1.0f / 60.0f);          // frame: press edge processed
    CHECK(!map.Pressed("dodge", in));
    CHECK(map.IsDown("dodge", in));      // still physically down
    in.Release(platform::Key::Space);
    in.Step(map, 1.0f / 60.0f);

    in.Step(map, 0.1f);                  // 100ms later, still within window
    in.Press(platform::Key::Space);      // 2nd tap fires
    in.Step(map, 1.0f / 60.0f);
    CHECK(map.Pressed("dodge", in));
    in.Release(platform::Key::Space);
    in.Step(map, 1.0f / 60.0f);

    in.Step(map, 0.3f);                  // wait past the window
    in.Press(platform::Key::Space);      // fresh 1st tap: suppressed again
    in.Step(map, 1.0f / 60.0f);
    CHECK(!map.Pressed("dodge", in));
}

// G7-3: long-press — no edge until the key has been held continuously for the
// threshold; then it fires once, IsDown stays true, and release reports.
TEST(InputMapLongPress) {
    script::InputMap map;
    std::string err;
    CHECK(map.Load(R"({"actions":{
        "sprint": {"keys":["Shift"], "longPressMs":300}
    }})", &err));
    FrameInput in;

    in.Press(platform::Key::Shift);
    for (int i = 0; i < 10; ++i) in.Step(map, 0.01f); // 100ms
    CHECK(!map.Pressed("sprint", in));
    CHECK(map.IsDown("sprint", in));

    bool fired = false;                          // fires on the crossing frame
    for (int i = 0; i < 30; ++i) {               // +300ms total
        in.Step(map, 0.01f);
        if (map.Pressed("sprint", in)) fired = true;
    }
    CHECK(fired);                                // once, at the 300ms crossing
    in.Step(map, 0.01f);
    CHECK(!map.Pressed("sprint", in));           // not again while held
    CHECK(map.IsDown("sprint", in));

    in.Release(platform::Key::Shift);
    in.Step(map, 0.01f);
    CHECK(map.Released("sprint", in));           // release after firing

    // A short hold (< threshold) must never fire or report release.
    in.Press(platform::Key::Shift);
    for (int i = 0; i < 10; ++i) in.Step(map, 0.01f); // 100ms
    CHECK(!map.Pressed("sprint", in));
    in.Release(platform::Key::Shift);
    in.Step(map, 0.01f);
    CHECK(!map.Released("sprint", in));
}

// G7-3: timing rules survive the JSON round-trip and the runtime Reset.
TEST(InputMapTimingRoundTrip) {
    script::InputMap map;
    std::string err;
    CHECK(map.Load(R"({"actions":{
        "dash": {"keys":["D"], "doubleTapMs":180},
        "focus": {"keys":["F"], "longPressMs":500, "modifiers":["Alt"]}
    }})", &err));

    const std::string json = map.ToJson();
    script::InputMap reloaded;
    CHECK(reloaded.Load(json, &err));
    const script::InputAction* dash = reloaded.Find("dash");
    CHECK(dash != nullptr);
    if (dash) CHECK_EQ(dash->doubleTapMs, 180u);
    const script::InputAction* focus = reloaded.Find("focus");
    CHECK(focus != nullptr);
    if (focus) {
        CHECK_EQ(focus->longPressMs, 500u);
        CHECK_EQ(focus->modifiers.size(), 1u);
        CHECK(focus->modifiers[0] == platform::Key::Alt);
    }

    // Reset clears the timing clock/edges (scene restart semantics): a long
    // press that crossed the threshold before Reset must not fire afterwards.
    FrameInput in;
    in.Press(platform::Key::F);
    in.Press(platform::Key::Alt);
    bool fired = false;
    for (int i = 0; i < 60; ++i) {               // 600ms held (Step clears edges)
        in.Step(reloaded, 0.01f);
        if (reloaded.Pressed("focus", in)) fired = true;
    }
    CHECK(fired);
    reloaded.Reset();
    in.EndFrame();
    CHECK(!reloaded.Pressed("focus", in));
}
