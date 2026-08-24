#pragma once

#include <map>
#include <memory>
#include <string>
#include <vector>

#include "neon/plugin/plugin.hpp"
#include "neon/script/script.hpp"

namespace neon::scene {
struct ComponentSchema;
}

namespace neon::editor {

class EditorApp;

// One plugin-contributed panel window (docked like any built-in panel).
struct PluginPanel {
    std::string id;
    std::string title;
    std::string pluginId;
    script::IScriptHost* host = nullptr;
    uint64_t drawHandle = 0; // captured draw function
    bool opened = true;      // window open state (docked layout persistence)
};

// One plugin-contributed toolbar button.
struct PluginTool {
    std::string id;
    std::string label;
    std::string pluginId;
    script::IScriptHost* host = nullptr;
    uint64_t fn = 0;
};

// One plugin-contributed asset source (the "素材市场"): the plugin provides
// list() -> [{name,type,path}, ...] and import(path) -> project path.
struct PluginAssetSource {
    std::string id;
    std::string name;
    std::string pluginId;
    script::IScriptHost* host = nullptr;
    uint64_t listFn = 0;
    uint64_t importFn = 0;
};

// Loads editor plugins (type "editor") from <baseDir>/plugins. Each plugin's
// entry script runs with a `NeonEditor` API (panels/tools/asset sources/
// component schemas/ImGui widgets). Plugins share one host per backend and
// are fully isolated from the editor's own script hosts.
class EditorPluginManager {
public:
    EditorPluginManager();
    ~EditorPluginManager();
    EditorPluginManager(const EditorPluginManager&) = delete;
    EditorPluginManager& operator=(const EditorPluginManager&) = delete;

    // Wires the owning editor (for the native bindings). Must be called once
    // before Load.
    void Init(EditorApp* app);
    // Discovers and loads every editor plugin under <baseDir>/plugins.
    void Load(const std::string& baseDir);
    void Shutdown();

    size_t Count() const;
    const std::vector<plugin::PluginManifest>& Manifests() const;

    std::vector<PluginPanel>& Panels();
    std::vector<PluginTool>& Tools();
    std::vector<PluginAssetSource>& AssetSources();

    // The owning editor (for native bindings that touch scene state).
    EditorApp* App() const;

    // Invokes a panel's draw function inside its ImGui window (called from
    // BuildImGuiUI).
    void DrawPanel(PluginPanel& panel);

    // Native-binding entry points (public so the host-agnostic closures can
    // reach the manager).
    void OnPanel(const std::string& pluginId, const std::string& id,
                 const std::string& title, script::IScriptHost* host, uint64_t draw);
    void OnTool(const std::string& pluginId, const std::string& id,
                const std::string& label, script::IScriptHost* host, uint64_t fn);
    void OnAssetSource(const std::string& pluginId, const std::string& id,
                       const std::string& name, script::IScriptHost* host,
                       uint64_t listFn, uint64_t importFn);
    void OnComponentSchema(const std::string& name, const script::Value& schema);

    // Plugin-registered component schemas, converted to the editor's schema
    // format so the inspector renders them like built-in components.
    const scene::ComponentSchema* FindSchema(const std::string& name) const;
    const std::vector<scene::ComponentSchema>& Schemas() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace neon::editor
