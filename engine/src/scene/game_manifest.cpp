#include "neon/scene/game_manifest.hpp"

#include <algorithm>
#include <utility>

namespace neon::scene {
namespace {

// Rejects an object that contains any key outside `allowed`. `where` names the
// object in error messages (e.g. "manifest 'window'").
bool CheckKeys(const core::Json& obj, const std::vector<std::string>& allowed,
               const std::string& where, std::string* err) {
    for (const auto& [key, val] : obj.Members()) {
        (void)val;
        if (std::find(allowed.begin(), allowed.end(), key) == allowed.end()) {
            *err = "manifest: " + where + " has unknown field '" + key + "'";
            return false;
        }
    }
    return true;
}

core::Json MakeObject() {
    core::Json j;
    j.type_ = core::Json::Type::Object;
    return j;
}

core::Json MakeString(const std::string& s) {
    core::Json j;
    j.type_ = core::Json::Type::String;
    j.string_ = s;
    return j;
}

core::Json MakeInt(int v) {
    core::Json j;
    j.type_ = core::Json::Type::Number;
    j.number_ = static_cast<double>(v);
    return j;
}

} // namespace

core::Result<GameManifest> GameManifest::Load(const std::string& jsonText) {
    std::string perr;
    core::Json root = core::Json::Parse(jsonText, &perr);
    if (root.IsNull() && !perr.empty())
        return core::Result<GameManifest>::Err("manifest: JSON parse error: " + perr);
    if (!root.IsObject())
        return core::Result<GameManifest>::Err("manifest: root must be a JSON object");
    if (!CheckKeys(root, {"startScene", "window", "packages", "title"}, "manifest", &perr))
        return core::Result<GameManifest>::Err(perr);

    GameManifest m;

    const core::Json* start = root.Get("startScene");
    if (!start || !start->IsString() || start->GetString().empty())
        return core::Result<GameManifest>::Err(
            "manifest: 'startScene' must be a non-empty string");
    m.startScene = start->GetString();

    if (const core::Json* title = root.Get("title")) {
        if (!title->IsString())
            return core::Result<GameManifest>::Err("manifest: 'title' must be a string");
        m.title = title->GetString();
    }

    if (const core::Json* win = root.Get("window")) {
        if (!win->IsObject())
            return core::Result<GameManifest>::Err("manifest: 'window' must be an object");
        if (!CheckKeys(*win, {"w", "h", "title"}, "manifest 'window'", &perr))
            return core::Result<GameManifest>::Err(perr);
        if (const core::Json* w = win->Get("w")) {
            if (!w->IsNumber() || w->GetNumber() != static_cast<double>(w->GetInt()) ||
                w->GetInt() <= 0)
                return core::Result<GameManifest>::Err(
                    "manifest: 'window.w' must be a positive integer");
            m.window.w = w->GetInt();
        }
        if (const core::Json* h = win->Get("h")) {
            if (!h->IsNumber() || h->GetNumber() != static_cast<double>(h->GetInt()) ||
                h->GetInt() <= 0)
                return core::Result<GameManifest>::Err(
                    "manifest: 'window.h' must be a positive integer");
            m.window.h = h->GetInt();
        }
        if (const core::Json* wt = win->Get("title")) {
            if (!wt->IsString())
                return core::Result<GameManifest>::Err(
                    "manifest: 'window.title' must be a string");
            m.window.title = wt->GetString();
        }
    }

    // Title resolution: top-level 'title' wins; absent/empty -> window.title.
    if (m.title.empty()) m.title = m.window.title;
    if (m.window.title.empty()) m.window.title = m.title;

    if (const core::Json* pkgs = root.Get("packages")) {
        if (!pkgs->IsArray())
            return core::Result<GameManifest>::Err("manifest: 'packages' must be an array");
        for (size_t i = 0; i < pkgs->Size(); ++i) {
            const core::Json* p = pkgs->At(i);
            if (!p || !p->IsString() || p->GetString().empty())
                return core::Result<GameManifest>::Err(
                    "manifest: 'packages' entry at index " + std::to_string(i) +
                    " must be a non-empty string");
            const std::string& name = p->GetString();
            if (std::find(m.packages.begin(), m.packages.end(), name) != m.packages.end())
                return core::Result<GameManifest>::Err(
                    "manifest: duplicate package '" + name + "'");
            m.packages.push_back(name);
        }
    }

    core::Status s = m.Validate();
    if (!s.Ok()) return core::Result<GameManifest>::Err(s.Error());
    return core::Result<GameManifest>::Ok(std::move(m));
}

core::Json GameManifest::ToJson() const {
    core::Json root = MakeObject();
    root.object_["startScene"] = MakeString(startScene);
    root.object_["title"] = MakeString(title);
    core::Json win = MakeObject();
    win.object_["w"] = MakeInt(window.w);
    win.object_["h"] = MakeInt(window.h);
    win.object_["title"] = MakeString(window.title);
    root.object_["window"] = std::move(win);
    if (!packages.empty()) {
        core::Json arr;
        arr.type_ = core::Json::Type::Array;
        for (const std::string& p : packages) arr.array_.push_back(MakeString(p));
        root.object_["packages"] = std::move(arr);
    }
    return root;
}

core::Status GameManifest::Validate() const {
    if (startScene.empty())
        return core::Status::Err("manifest: 'startScene' must be a non-empty string");
    if (window.w <= 0 || window.h <= 0)
        return core::Status::Err("manifest: window w/h must be positive");
    for (const std::string& p : packages) {
        if (p.empty()) return core::Status::Err("manifest: package names must be non-empty");
    }
    for (size_t i = 0; i < packages.size(); ++i) {
        for (size_t j = i + 1; j < packages.size(); ++j) {
            if (packages[i] == packages[j])
                return core::Status::Err("manifest: duplicate package '" + packages[i] + "'");
        }
    }
    return core::Status::Ok(true);
}

} // namespace neon::scene
