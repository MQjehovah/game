#pragma once

// Godot-style input actions: a script-facing NAME maps to one or more keys,
// configured in the project's input.json (data-driven, editable in the
// editor's input panel). Scripts query actions by name (ActionDown/Axis),
// never by physical key, so rebinding is pure data.
//
// JSON shape (project root, packed alongside game.json):
//   {
//     "actions": {
//       "move_forward": { "positive": ["W", "ArrowUp"], "negative": ["S", "ArrowDown"] },
//       "jump":         ["Space"],
//       "sprint":       ["Shift"]
//     }
//   }
// A plain array of key names means "any key triggers down/pressed"; an object
// with "positive"/"negative" also drives ActionAxis in [-1, 1]. Missing
// actions fall back to the engine's built-in defaults (forward/strafe/
// vertical/jump/...), so existing scripts keep working without a file.

#include <map>
#include <string>
#include <vector>

#include "neon/core/json.hpp"
#include "neon/platform/input.hpp"

namespace neon::script {

struct InputAction {
    std::string name;
    std::vector<platform::Key> keys;     // any of these triggers down/pressed
    std::vector<platform::Key> positive; // ActionAxis: +1 side
    std::vector<platform::Key> negative; // ActionAxis: -1 side
    // G7-3 timing rules (all optional):
    //   modifiers  - chord: every key here must be HELD for the action to
    //                register (e.g. "Ctrl" + F). Applies to every query.
    //   doubleTapMs- double-tap: the action fires on the 2nd press within this
    //                window (1st press is consumed). 0 = off.
    //   longPressMs- long-press: the action fires ONCE when the key has been
    //                held continuously for >= this many ms; Released fires when
    //                it is then let go. 0 = off.
    std::vector<platform::Key> modifiers;
    uint32_t doubleTapMs = 0;
    uint32_t longPressMs = 0;
};

class InputMap {
public:
    // Merges `json` actions onto the current map (defaults stay unless the
    // file overrides them). Returns false with *err on malformed JSON or an
    // unknown key name.
    bool Load(const std::string& json, std::string* err);
    bool Load(const core::Json& root, std::string* err);

    const InputAction* Find(const std::string& name) const;
    bool Has(const std::string& name) const { return Find(name) != nullptr; }
    // Mutable access (editor panel edits timing rules).
    InputAction* FindMutable(const std::string& name);

    bool IsDown(const std::string& name, const platform::IInput& in) const;
    bool Pressed(const std::string& name, const platform::IInput& in) const;
    bool Released(const std::string& name, const platform::IInput& in) const;
    float Axis(const std::string& name, const platform::IInput& in) const; // [-1, 1]

    // G7-3: advances the timing clock and recomputes double-tap / long-press
    // edges. Must be called every frame BEFORE the action queries when any
    // action uses timing rules; safe to call every frame regardless.
    void Update(float dt, const platform::IInput& in);
    // Clears timing + per-frame state (scene restart).
    void Reset();

    // Action names in insertion order (editor panel).
    std::vector<std::string> Names() const;
    // Replaces the action's primary binding (positive[0], else keys[0], else
    // appends to keys). Editor panel rebinding. Returns false when unknown.
    bool SetPrimaryKey(const std::string& name, platform::Key key);
    // Serializes all actions to the JSON shape (editor save).
    std::string ToJson() const;

    // Engine built-ins: forward/strafe/vertical axes + jump/sprint/interact
    // keys. The map starts from these and a project's input.json overrides.
    static InputMap Defaults();
    // Parses a key name ("W", "Space", "ArrowUp", "1", ...) or returns
    // Key::Unknown. Mirrors the Lua KeyFromName table.
    static platform::Key KeyFromName(const std::string& name);
    // Human-readable key name (editor panel display + JSON serialization).
    static std::string KeyToName(platform::Key key);

private:
    static bool ModifiersHeld(const InputAction& a, const platform::IInput& in);
    static bool HasTiming(const InputAction& a) {
        return a.doubleTapMs > 0 || a.longPressMs > 0;
    }

    std::map<std::string, InputAction> actions_;
    std::vector<std::string> order_;

    // G7-3 timing state. `timing_` tracks press edges / hold start per key so a
    // key shared by several actions keeps one coherent history. `frame_` holds
    // this frame's computed results for actions with timing rules (computed in
    // Update, read by the const queries; mutable for that split).
    struct KeyTiming {
        float lastPressMs = -1e9f;  // last press edge (double-tap window base)
        float pressStartMs = -1e9f; // when the key started being held
        bool longFired = false;     // long-press already fired for this hold
    };
    struct ActionFrame {
        bool down = false;
        bool pressed = false;
        bool released = false;
    };
    float timeMs_ = 0.0f;
    std::map<platform::Key, KeyTiming> timing_;
    mutable std::map<std::string, ActionFrame> frame_;
};

} // namespace neon::script
