#include "neon/script/input_map.hpp"

#include <algorithm>
#include <cstdlib>

namespace neon::script {

namespace {

std::string Lower(std::string s) {
    for (char& c : s)
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    return s;
}

bool HasKey(const std::vector<platform::Key>& keys, platform::Key k) {
    return std::find(keys.begin(), keys.end(), k) != keys.end();
}

} // namespace

platform::Key InputMap::KeyFromName(const std::string& raw) {
    const std::string name = Lower(raw);
    if (name == "space") return platform::Key::Space;
    if (name == "shift") return platform::Key::Shift;
    if (name == "ctrl" || name == "control") return platform::Key::Control;
    if (name == "alt") return platform::Key::Alt;
    if (name == "enter" || name == "return") return platform::Key::Enter;
    if (name == "esc" || name == "escape") return platform::Key::Escape;
    if (name == "tab") return platform::Key::Tab;
    if (name == "backspace") return platform::Key::Backspace;
    if (name == "up" || name == "arrowup") return platform::Key::ArrowUp;
    if (name == "down" || name == "arrowdown") return platform::Key::ArrowDown;
    if (name == "left" || name == "arrowleft") return platform::Key::ArrowLeft;
    if (name == "right" || name == "arrowright") return platform::Key::ArrowRight;
    if (name.size() == 1) {
        const char c = name[0];
        if (c >= 'a' && c <= 'z')
            return static_cast<platform::Key>(static_cast<int>(platform::Key::A) + (c - 'a'));
        if (c >= 'A' && c <= 'Z')
            return static_cast<platform::Key>(static_cast<int>(platform::Key::A) + (c - 'A'));
        if (c >= '0' && c <= '9')
            return static_cast<platform::Key>(static_cast<int>(platform::Key::D0) + (c - '0'));
    }
    if (name.size() >= 2 && name[0] == 'f') {
        const int n = std::atoi(name.c_str() + 1);
        if (n >= 1 && n <= 12)
            return static_cast<platform::Key>(static_cast<int>(platform::Key::F1) + (n - 1));
    }
    return platform::Key::Unknown;
}

std::string InputMap::KeyToName(platform::Key key) {
    switch (key) {
        case platform::Key::Space: return "Space";
        case platform::Key::Shift: return "Shift";
        case platform::Key::Control: return "Ctrl";
        case platform::Key::Alt: return "Alt";
        case platform::Key::Enter: return "Enter";
        case platform::Key::Escape: return "Esc";
        case platform::Key::Tab: return "Tab";
        case platform::Key::Backspace: return "Backspace";
        case platform::Key::ArrowUp: return "ArrowUp";
        case platform::Key::ArrowDown: return "ArrowDown";
        case platform::Key::ArrowLeft: return "ArrowLeft";
        case platform::Key::ArrowRight: return "ArrowRight";
        default: break;
    }
    const int k = static_cast<int>(key);
    if (key >= platform::Key::A && key <= platform::Key::Z)
        return std::string(1, static_cast<char>('A' + (k - static_cast<int>(platform::Key::A))));
    if (key >= platform::Key::D0 && key <= platform::Key::D9)
        return std::string(1, static_cast<char>('0' + (k - static_cast<int>(platform::Key::D0))));
    if (key >= platform::Key::F1 && key <= platform::Key::F12)
        return "F" + std::to_string(k - static_cast<int>(platform::Key::F1) + 1);
    return "?";
}

InputMap InputMap::Defaults() {
    InputMap m;
    auto axis = [&](const char* name, platform::Key pos, platform::Key neg) {
        InputAction a;
        a.name = name;
        a.positive = {pos};
        a.negative = {neg};
        m.actions_[name] = std::move(a);
        m.order_.push_back(name);
    };
    auto key = [&](const char* name, platform::Key k) {
        InputAction a;
        a.name = name;
        a.keys = {k};
        m.actions_[name] = std::move(a);
        m.order_.push_back(name);
    };
    axis("forward", platform::Key::W, platform::Key::S);
    axis("strafe", platform::Key::D, platform::Key::A);
    axis("vertical", platform::Key::E, platform::Key::Q);
    key("jump", platform::Key::Space);
    key("sprint", platform::Key::Shift);
    key("interact", platform::Key::F);
    key("fire1", platform::Key::D1);
    key("fire2", platform::Key::D2);
    return m;
}

bool InputMap::Load(const core::Json& root, std::string* err) {
    const core::Json* actions = root.Get("actions");
    if (!actions || !actions->IsObject()) {
        if (err) *err = "input map: missing \"actions\" object";
        return false;
    }
    for (const auto& kv : actions->Members()) {
        const std::string& name = kv.first;
        const core::Json& v = kv.second;
        InputAction a;
        a.name = name;
        if (v.IsArray()) {
            for (size_t i = 0; i < v.Size(); ++i) {
                const core::Json* e = v.At(i);
                if (!e || !e->IsString()) continue;
                const platform::Key k = KeyFromName(e->GetString());
                if (k != platform::Key::Unknown) a.keys.push_back(k);
            }
        } else if (v.IsObject()) {
            auto readKeys = [&](const char* key, std::vector<platform::Key>& out) {
                const core::Json* arr = v.Get(key);
                if (!arr || !arr->IsArray()) return;
                for (size_t i = 0; i < arr->Size(); ++i) {
                    const core::Json* e = arr->At(i);
                    if (!e || !e->IsString()) continue;
                    const platform::Key k = KeyFromName(e->GetString());
                    if (k != platform::Key::Unknown) out.push_back(k);
                }
            };
            readKeys("positive", a.positive);
            readKeys("negative", a.negative);
            readKeys("keys", a.keys);
        } else {
            if (err) *err = "input map: action '" + name + "' must be an array or object";
            return false;
        }
        if (a.keys.empty() && a.positive.empty() && a.negative.empty()) {
            if (err) *err = "input map: action '" + name + "' has no keys";
            return false;
        }
        if (actions_.find(name) == actions_.end()) order_.push_back(name);
        actions_[name] = std::move(a);
    }
    return true;
}

bool InputMap::Load(const std::string& json, std::string* err) {
    std::string parseErr;
    core::Json root = core::Json::Parse(json, &parseErr);
    if (!parseErr.empty()) {
        if (err) *err = "input map: " + parseErr;
        return false;
    }
    return Load(root, err);
}

const InputAction* InputMap::Find(const std::string& name) const {
    const auto it = actions_.find(name);
    return it == actions_.end() ? nullptr : &it->second;
}

bool InputMap::IsDown(const std::string& name, const platform::IInput& in) const {
    const InputAction* a = Find(name);
    if (!a) return false;
    for (platform::Key k : a->keys)
        if (in.IsDown(k)) return true;
    for (platform::Key k : a->positive)
        if (in.IsDown(k)) return true;
    return false;
}

bool InputMap::Pressed(const std::string& name, const platform::IInput& in) const {
    const InputAction* a = Find(name);
    if (!a) return false;
    for (platform::Key k : a->keys)
        if (in.Pressed(k)) return true;
    for (platform::Key k : a->positive)
        if (in.Pressed(k)) return true;
    return false;
}

bool InputMap::Released(const std::string& name, const platform::IInput& in) const {
    const InputAction* a = Find(name);
    if (!a) return false;
    for (platform::Key k : a->keys)
        if (in.Released(k)) return true;
    for (platform::Key k : a->positive)
        if (in.Released(k)) return true;
    return false;
}

float InputMap::Axis(const std::string& name, const platform::IInput& in) const {
    const InputAction* a = Find(name);
    if (!a) return 0.0f;
    const bool pos = std::any_of(a->positive.begin(), a->positive.end(),
                                 [&](platform::Key k) { return in.IsDown(k); });
    const bool neg = std::any_of(a->negative.begin(), a->negative.end(),
                                 [&](platform::Key k) { return in.IsDown(k); });
    return (pos ? 1.0f : 0.0f) - (neg ? 1.0f : 0.0f);
}

std::vector<std::string> InputMap::Names() const { return order_; }

bool InputMap::SetPrimaryKey(const std::string& name, platform::Key key) {
    auto it = actions_.find(name);
    if (it == actions_.end() || key == platform::Key::Unknown) return false;
    InputAction& a = it->second;
    if (!a.positive.empty()) {
        a.positive[0] = key;
    } else if (!a.keys.empty()) {
        a.keys[0] = key;
    } else {
        a.keys.push_back(key);
    }
    return true;
}

std::string InputMap::ToJson() const {
    core::Json root;
    root.type_ = core::Json::Type::Object;
    core::Json actions;
    actions.type_ = core::Json::Type::Object;
    for (const std::string& name : order_) {
        const InputAction& a = actions_.at(name);
        core::Json node;
        node.type_ = core::Json::Type::Object;
        auto keysJson = [](const std::vector<platform::Key>& keys) {
            core::Json arr;
            arr.type_ = core::Json::Type::Array;
            for (platform::Key k : keys) {
                core::Json s;
                s.type_ = core::Json::Type::String;
                s.string_ = KeyToName(k);
                arr.array_.push_back(std::move(s));
            }
            return arr;
        };
        node.object_["positive"] = keysJson(a.positive);
        node.object_["negative"] = keysJson(a.negative);
        node.object_["keys"] = keysJson(a.keys);
        actions.object_[name] = std::move(node);
    }
    root.object_["actions"] = std::move(actions);
    return core::JsonWriter::Write(root);
}

} // namespace neon::script
