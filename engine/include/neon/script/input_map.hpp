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

    bool IsDown(const std::string& name, const platform::IInput& in) const;
    bool Pressed(const std::string& name, const platform::IInput& in) const;
    bool Released(const std::string& name, const platform::IInput& in) const;
    float Axis(const std::string& name, const platform::IInput& in) const; // [-1, 1]

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
    std::map<std::string, InputAction> actions_;
    std::vector<std::string> order_;
};

} // namespace neon::script
