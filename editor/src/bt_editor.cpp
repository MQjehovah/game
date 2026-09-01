// Behavior tree visual editor (T4.4): the pure node graph model lives inline
// in bt_editor.hpp so tests can include it headlessly. This file now holds the
// EditorApp-level .bt.json file IO (save/load) only — the canvas UI migrated to
// panels/bt_panel.cpp (Task 18b: BtPanel : IPanel), and the panel reaches the
// file IO through EditorContext::btLoadFromFile / btSaveToFile callbacks.
// BtLoadFromFile/BtSaveToFile stay as EditorApp methods because the editor
// smoke test calls them directly.

#include "bt_editor.hpp"

#include <cerrno>
#include <fstream>
#include <iterator>
#include <string>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <direct.h>
#else
#include <sys/stat.h>
#endif

#include "editor.hpp"
#include "neon/bt/behavior_tree.hpp"
#include "neon/core/json.hpp"

namespace neon::editor {
namespace {

bool EnsureDir(const std::string& path) {
#if defined(_WIN32)
    return _mkdir(path.c_str()) == 0 || errno == EEXIST;
#else
    return ::mkdir(path.c_str(), 0777) == 0 || errno == EEXIST;
#endif
}

} // namespace

bool EditorApp::BtSaveToFile(const std::string& path) {
    // The engine loader rejects empty composites ("sequence requires at least
    // one child"), childless decorators and a missing root. Refuse to write a
    // file the runtime cannot load, and say exactly which node is wrong. An
    // alternative (auto-injecting a `wait` placeholder) would silently change
    // the user's graph on save, so we prefer the explicit refusal.
    if (btGraph_.Empty()) {
        NEON_LOG_WARN("BtEditor: cannot save an empty tree (add a root node first)");
        return false;
    }
    for (const auto& n : btGraph_.Nodes()) {
        const int cap = bt::ChildCapacity(n.type);
        if (cap != -1 && cap != 1) continue; // actions / conditions take no children
        bool hasChild = false;
        for (const auto& l : btGraph_.Links())
            if (l.parent == n.id) {
                hasChild = true;
                break;
            }
        if (!hasChild) {
            NEON_LOG_WARN("BtEditor: '%s' needs %s child before saving (fix or delete it)",
                          n.type.c_str(), cap == 1 ? "exactly one" : "at least one");
            return false;
        }
    }

    std::string dir = path;
    size_t slash = dir.find_last_of("/\\");
    if (slash == std::string::npos) dir = ".";
    else dir = dir.substr(0, slash);
    EnsureDir(dir);
    std::ofstream out(path, std::ios::binary);
    if (!out.is_open()) {
        NEON_LOG_WARN("BtEditor: cannot write '%s'", path.c_str());
        return false;
    }
    out << btGraph_.Serialize();
    NEON_LOG_INFO("BtEditor: saved %zu nodes -> %s", btGraph_.NodeCount(), path.c_str());
    return true;
}

bool EditorApp::BtLoadFromFile(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in.is_open()) {
        NEON_LOG_WARN("BtEditor: cannot open '%s'", path.c_str());
        return false;
    }
    std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    std::string err;
    core::Json dom = core::Json::Parse(text, &err);
    if (dom.IsNull()) {
        NEON_LOG_WARN("BtEditor: '%s' is not valid JSON: %s", path.c_str(), err.c_str());
        return false;
    }
    btgraph::BtGraph g;
    if (!g.FromTreeJson(dom)) return false;
    btGraph_ = std::move(g);
    // NOTE: history/selection reset on load is the BtPanel's job (Task 18b);
    // the panel clears its own btHistory_/btSelected_/btPendingType_ after a
    // successful load, so interactive behavior is unchanged.
    NEON_LOG_INFO("BtEditor: loaded %zu nodes from %s", btGraph_.NodeCount(), path.c_str());
    return true;
}

} // namespace neon::editor
