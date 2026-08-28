#include "editor_plugin.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <sstream>

#include "imgui.h"

#include "neon/core/log.hpp"
#include "neon/scene/component_schema.hpp"

#include "editor.hpp"

namespace neon::editor {
namespace {

// Active-plugin context: native NeonEditor.* bindings need to know WHICH
// plugin is registering (for ownership). The manager sets this before every
// captured call and clears it after.
constexpr const char* kActivePluginKey = "neon_active_plugin";

std::string ActivePlugin(script::IScriptHost& host) {
    const auto v = host.GetGlobal(kActivePluginKey);
    return v.Ok() && v.Value().type == script::Value::Type::String ? v.Value().str
                                                                   : std::string();
}

void SetActivePlugin(script::IScriptHost& host, const std::string& id) {
    host.SetGlobal(kActivePluginKey, script::Value::Str(id));
}

// ---------------------------------------------------------------------------
// NeonEditor.* registration bindings
// ---------------------------------------------------------------------------

script::Value NativePanel(script::IScriptHost& host, void* user) {
    auto* mgr = static_cast<EditorPluginManager*>(user);
    const std::string id = host.GetArg(0).type == script::Value::Type::String
                               ? host.GetArg(0).str
                               : "";
    const std::string title = host.GetArg(1).type == script::Value::Type::String
                                  ? host.GetArg(1).str
                                  : id;
    const auto cap = host.CaptureStackFunction(2);
    if (id.empty() || !cap.Ok()) {
        host.SetError("NeonEditor.panel(id, title, drawFn) requires a draw function");
        return script::Value::Nil();
    }
    if (mgr) mgr->OnPanel(ActivePlugin(host), id, title, &host, cap.Value());
    return script::Value::Nil();
}

script::Value NativeTool(script::IScriptHost& host, void* user) {
    auto* mgr = static_cast<EditorPluginManager*>(user);
    const std::string id = host.GetArg(0).type == script::Value::Type::String
                               ? host.GetArg(0).str
                               : "";
    const std::string label = host.GetArg(1).type == script::Value::Type::String
                                  ? host.GetArg(1).str
                                  : id;
    const auto cap = host.CaptureStackFunction(2);
    if (id.empty() || !cap.Ok()) {
        host.SetError("NeonEditor.tool(id, label, fn) requires a function");
        return script::Value::Nil();
    }
    if (mgr) mgr->OnTool(ActivePlugin(host), id, label, &host, cap.Value());
    return script::Value::Nil();
}

script::Value NativeAssetSource(script::IScriptHost& host, void* user) {
    auto* mgr = static_cast<EditorPluginManager*>(user);
    const std::string id = host.GetArg(0).type == script::Value::Type::String
                               ? host.GetArg(0).str
                               : "";
    const std::string name = host.GetArg(1).type == script::Value::Type::String
                                 ? host.GetArg(1).str
                                 : id;
    const auto list = host.CaptureStackFunction(2);
    const auto importFn = host.CaptureStackFunction(3);
    if (id.empty() || !list.Ok() || !importFn.Ok()) {
        host.SetError("NeonEditor.assetSource(id, name, listFn, importFn) requires two functions");
        return script::Value::Nil();
    }
    if (mgr)
        mgr->OnAssetSource(ActivePlugin(host), id, name, &host, list.Value(),
                           importFn.Value());
    return script::Value::Nil();
}

script::Value NativeRegisterComponent(script::IScriptHost& host, void* user) {
    auto* mgr = static_cast<EditorPluginManager*>(user);
    const std::string name = host.GetArg(0).type == script::Value::Type::String
                                 ? host.GetArg(0).str
                                 : "";
    if (mgr && !name.empty()) mgr->OnComponentSchema(name, host.GetArg(1));
    return script::Value::Nil();
}

// ---------------------------------------------------------------------------
// NeonEditor scene / mesh bindings (forward to public EditorApp methods)
// ---------------------------------------------------------------------------

EditorApp* AppFrom(script::IScriptHost& host, void* user) {
    auto* mgr = static_cast<EditorPluginManager*>(user);
    return mgr ? mgr->App() : nullptr;
}

script::Value NativeBuildMesh(script::IScriptHost& host, void* user) {
    EditorApp* app = AppFrom(host, user);
    if (!app) return script::Value::Nil();
    const std::string name = host.GetArg(0).type == script::Value::Type::String
                                 ? host.GetArg(0).str
                                 : "generated";
    // verts: array of {x,y,z}; indices: 1-based triangle indices.
    std::vector<math::Vec3> verts;
    std::vector<int> indices;
    const script::Value vv = host.GetArg(1);
    if (vv.type == script::Value::Type::Table) {
        for (const script::Value& it : vv.table->array) {
            if (it.type != script::Value::Type::Table) continue;
            math::Vec3 p;
            for (const auto& kv : it.table->fields) {
                if (kv.second.type != script::Value::Type::Number) continue;
                if (kv.first == "x") p.x = static_cast<float>(kv.second.number);
                if (kv.first == "y") p.y = static_cast<float>(kv.second.number);
                if (kv.first == "z") p.z = static_cast<float>(kv.second.number);
            }
            verts.push_back(p);
        }
    }
    const script::Value iv = host.GetArg(2);
    if (iv.type == script::Value::Type::Table) {
        for (const script::Value& it : iv.table->array)
            if (it.type == script::Value::Type::Number)
                indices.push_back(static_cast<int>(it.number));
    }
    if (verts.empty() || indices.empty()) {
        host.SetError("NeonEditor.buildMesh(name, verts, indices): verts/indices required");
        return script::Value::Nil();
    }
    return script::Value::Str(app->PluginBuildMesh(name, verts, indices));
}

script::Value NativeSpawn(script::IScriptHost& host, void* user) {
    EditorApp* app = AppFrom(host, user);
    if (!app) return script::Value::Nil();
    const std::string meshKey = host.GetArg(0).type == script::Value::Type::String
                                    ? host.GetArg(0).str
                                    : "";
    const script::Value p = host.GetArg(1);
    math::Vec3 pos;
    if (p.type == script::Value::Type::Table) {
        for (const auto& kv : p.table->fields) {
            if (kv.second.type != script::Value::Type::Number) continue;
            if (kv.first == "x") pos.x = static_cast<float>(kv.second.number);
            if (kv.first == "y") pos.y = static_cast<float>(kv.second.number);
            if (kv.first == "z") pos.z = static_cast<float>(kv.second.number);
        }
    }
    if (!meshKey.empty()) app->PluginAddEntity(meshKey, pos.x, pos.y, pos.z);
    return script::Value::Nil();
}

script::Value NativeSelected(script::IScriptHost& host, void* user) {
    EditorApp* app = AppFrom(host, user);
    return app ? app->PluginSelectedEntity() : script::Value::Nil();
}

script::Value NativeEntities(script::IScriptHost& host, void* user) {
    EditorApp* app = AppFrom(host, user);
    return app ? app->PluginEntityList() : script::Value::Nil();
}

script::Value NativeLog(script::IScriptHost& host, void* user) {
    EditorApp* app = AppFrom(host, user);
    if (app && host.GetArg(0).type == script::Value::Type::String)
        app->PluginLog(host.GetArg(0).str);
    return script::Value::Nil();
}

script::Value NativeImportAsset(script::IScriptHost& host, void* user) {
    EditorApp* app = AppFrom(host, user);
    if (!app || host.GetArg(0).type != script::Value::Type::String)
        return script::Value::Nil();
    return script::Value::Str(app->PluginImportAsset(host.GetArg(0).str));
}

script::Value NativeListDir(script::IScriptHost& host, void* user) {
    EditorApp* app = AppFrom(host, user);
    if (!app || host.GetArg(0).type != script::Value::Type::String)
        return script::Value::Nil();
    const std::vector<std::string> files = app->PluginListDir(host.GetArg(0).str);
    script::Value out = script::Value::Tbl();
    for (const std::string& f : files) out.table->array.push_back(script::Value::Str(f));
    return out;
}

// ---------------------------------------------------------------------------
// NeonEditor.ui.* ImGui widget bindings (curated subset)
// ---------------------------------------------------------------------------

script::Value UIButton(script::IScriptHost& host, void* /*user*/) {
    return script::Value::Bool(ImGui::Button(
        host.GetArg(0).type == script::Value::Type::String ? host.GetArg(0).str.c_str()
                                                           : ""));
}

script::Value UISmallButton(script::IScriptHost& host, void* /*user*/) {
    return script::Value::Bool(ImGui::SmallButton(
        host.GetArg(0).type == script::Value::Type::String ? host.GetArg(0).str.c_str()
                                                           : ""));
}

script::Value UICheckbox(script::IScriptHost& host, void* /*user*/) {
    // Checkbox(label, current) -> newValue (plugins keep their own state).
    bool v = host.GetArg(1).type == script::Value::Type::Bool ? host.GetArg(1).boolean
                                                              : false;
    if (ImGui::Checkbox(
            host.GetArg(0).type == script::Value::Type::String ? host.GetArg(0).str.c_str()
                                                               : "",
            &v))
        return script::Value::Bool(v);
    return script::Value::Nil();
}

script::Value UIText(script::IScriptHost& host, void* /*user*/) {
    if (host.GetArg(0).type == script::Value::Type::String)
        ImGui::TextUnformatted(host.GetArg(0).str.c_str());
    return script::Value::Nil();
}

script::Value UITextDisabled(script::IScriptHost& host, void* /*user*/) {
    if (host.GetArg(0).type == script::Value::Type::String)
        ImGui::TextDisabled("%s", host.GetArg(0).str.c_str());
    return script::Value::Nil();
}

script::Value UITextColored(script::IScriptHost& host, void* /*user*/) {
    const script::Value& t = host.GetArg(4);
    if (t.type != script::Value::Type::String) return script::Value::Nil();
    ImGui::TextColored(ImVec4(static_cast<float>(host.GetArg(0).number),
                              static_cast<float>(host.GetArg(1).number),
                              static_cast<float>(host.GetArg(2).number),
                              static_cast<float>(host.GetArg(3).number)),
                       "%s", t.str.c_str());
    return script::Value::Nil();
}

script::Value UISameLine(script::IScriptHost& host, void* /*user*/) {
    ImGui::SameLine();
    return script::Value::Nil();
}

script::Value UISeparator(script::IScriptHost& host, void* /*user*/) {
    ImGui::Separator();
    return script::Value::Nil();
}

script::Value UISliderFloat(script::IScriptHost& host, void* /*user*/) {
    float v = static_cast<float>(host.GetArg(1).number);
    if (ImGui::SliderFloat(host.GetArg(0).type == script::Value::Type::String
                               ? host.GetArg(0).str.c_str()
                               : "",
                           &v, static_cast<float>(host.GetArg(2).number),
                           static_cast<float>(host.GetArg(3).number)))
        return script::Value::Num(v);
    return script::Value::Nil();
}

script::Value UIDragFloat(script::IScriptHost& host, void* /*user*/) {
    float v = static_cast<float>(host.GetArg(1).number);
    const float speed = host.ArgCount() > 2 ? static_cast<float>(host.GetArg(2).number) : 0.1f;
    const float lo = host.ArgCount() > 3 ? static_cast<float>(host.GetArg(3).number) : 0.0f;
    const float hi = host.ArgCount() > 4 ? static_cast<float>(host.GetArg(4).number) : 0.0f;
    if (ImGui::DragFloat(host.GetArg(0).type == script::Value::Type::String
                             ? host.GetArg(0).str.c_str()
                             : "",
                         &v, speed, lo, hi))
        return script::Value::Num(v);
    return script::Value::Nil();
}

script::Value UIDragFloat3(script::IScriptHost& host, void* /*user*/) {
    const script::Value& cur = host.GetArg(1);
    float v[3] = {0, 0, 0};
    if (cur.type == script::Value::Type::Table) {
        for (const auto& kv : cur.table->fields) {
            if (kv.second.type != script::Value::Type::Number) continue;
            if (kv.first == "x") v[0] = static_cast<float>(kv.second.number);
            if (kv.first == "y") v[1] = static_cast<float>(kv.second.number);
            if (kv.first == "z") v[2] = static_cast<float>(kv.second.number);
        }
    }
    if (ImGui::DragFloat3(host.GetArg(0).type == script::Value::Type::String
                              ? host.GetArg(0).str.c_str()
                              : "",
                          v, 0.1f)) {
        script::Value out = script::Value::Tbl();
        out.table->fields.emplace_back("x", script::Value::Num(v[0]));
        out.table->fields.emplace_back("y", script::Value::Num(v[1]));
        out.table->fields.emplace_back("z", script::Value::Num(v[2]));
        return out;
    }
    return script::Value::Nil();
}

script::Value UIInputText(script::IScriptHost& host, void* /*user*/) {
    const std::string label = host.GetArg(0).type == script::Value::Type::String
                                  ? host.GetArg(0).str
                                  : "";
    const std::string cur = host.GetArg(1).type == script::Value::Type::String
                                ? host.GetArg(1).str
                                : "";
    char buf[1024];
    std::snprintf(buf, sizeof(buf), "%s", cur.c_str());
    if (ImGui::InputText(label.c_str(), buf, sizeof(buf)))
        return script::Value::Str(buf);
    return script::Value::Nil();
}

script::Value UICombo(script::IScriptHost& host, void* /*user*/) {
    const std::string label = host.GetArg(0).type == script::Value::Type::String
                                  ? host.GetArg(0).str
                                  : "";
    std::vector<const char*> items;
    const script::Value& opts = host.GetArg(1);
    if (opts.type == script::Value::Type::Table) {
        for (const script::Value& it : opts.table->array)
            if (it.type == script::Value::Type::String) items.push_back(it.str.c_str());
    }
    if (items.empty()) return script::Value::Nil();
    int sel = static_cast<int>(host.GetArg(2).number);
    if (ImGui::Combo(label.c_str(), &sel, items.data(), static_cast<int>(items.size())))
        return script::Value::Num(sel);
    return script::Value::Nil();
}

script::Value UICollapsingHeader(script::IScriptHost& host, void* /*user*/) {
    return script::Value::Bool(ImGui::CollapsingHeader(
        host.GetArg(0).type == script::Value::Type::String ? host.GetArg(0).str.c_str()
                                                           : ""));
}

script::Value UITreeNode(script::IScriptHost& host, void* /*user*/) {
    return script::Value::Bool(ImGui::TreeNode(
        host.GetArg(0).type == script::Value::Type::String ? host.GetArg(0).str.c_str()
                                                           : ""));
}

script::Value UITreePop(script::IScriptHost& host, void* /*user*/) {
    ImGui::TreePop();
    return script::Value::Nil();
}

script::Value UISetNextItemWidth(script::IScriptHost& host, void* /*user*/) {
    ImGui::SetNextItemWidth(static_cast<float>(host.GetArg(0).number));
    return script::Value::Nil();
}

script::Value UIBeginTable(script::IScriptHost& host, void* /*user*/) {
    return script::Value::Bool(ImGui::BeginTable(
        host.GetArg(0).type == script::Value::Type::String ? host.GetArg(0).str.c_str() : "",
        static_cast<int>(host.GetArg(1).number)));
}

script::Value UITableSetupColumn(script::IScriptHost& host, void* /*user*/) {
    ImGui::TableSetupColumn(host.GetArg(0).type == script::Value::Type::String
                                ? host.GetArg(0).str.c_str()
                                : "");
    return script::Value::Nil();
}

script::Value UITableHeadersRow(script::IScriptHost& host, void* /*user*/) {
    ImGui::TableHeadersRow();
    return script::Value::Nil();
}

script::Value UITableNextRow(script::IScriptHost& host, void* /*user*/) {
    ImGui::TableNextRow();
    return script::Value::Nil();
}

script::Value UITableSetColumnIndex(script::IScriptHost& host, void* /*user*/) {
    return script::Value::Bool(
        ImGui::TableSetColumnIndex(static_cast<int>(host.GetArg(0).number)));
}

script::Value UITableEnd(script::IScriptHost& host, void* /*user*/) {
    ImGui::EndTable();
    return script::Value::Nil();
}

script::Value UIColorEdit4(script::IScriptHost& host, void* /*user*/) {
    float c[4] = {static_cast<float>(host.GetArg(1).number),
                  static_cast<float>(host.GetArg(2).number),
                  static_cast<float>(host.GetArg(3).number),
                  static_cast<float>(host.GetArg(4).number)};
    if (ImGui::ColorEdit4(host.GetArg(0).type == script::Value::Type::String
                              ? host.GetArg(0).str.c_str()
                              : "",
                          c)) {
        script::Value out = script::Value::Tbl();
        out.table->array.push_back(script::Value::Num(c[0]));
        out.table->array.push_back(script::Value::Num(c[1]));
        out.table->array.push_back(script::Value::Num(c[2]));
        out.table->array.push_back(script::Value::Num(c[3]));
        return out;
    }
    return script::Value::Nil();
}

script::Value UIProgressBar(script::IScriptHost& host, void* /*user*/) {
    ImGui::ProgressBar(static_cast<float>(host.GetArg(0).number),
                       ImVec2(static_cast<float>(host.GetArg(1).number),
                              static_cast<float>(host.GetArg(2).number)));
    return script::Value::Nil();
}

script::Value UITooltip(script::IScriptHost& host, void* /*user*/) {
    if (ImGui::IsItemHovered() && host.GetArg(0).type == script::Value::Type::String)
        ImGui::SetTooltip("%s", host.GetArg(0).str.c_str());
    return script::Value::Nil();
}

script::Value UIIsItemHovered(script::IScriptHost& host, void* /*user*/) {
    return script::Value::Bool(ImGui::IsItemHovered());
}

script::Value UIBeginChild(script::IScriptHost& host, void* /*user*/) {
    return script::Value::Bool(ImGui::BeginChild(
        host.GetArg(0).type == script::Value::Type::String ? host.GetArg(0).str.c_str() : "",
        ImVec2(static_cast<float>(host.GetArg(1).number),
               static_cast<float>(host.GetArg(2).number)),
        true));
}

script::Value UIEndChild(script::IScriptHost& host, void* /*user*/) {
    ImGui::EndChild();
    return script::Value::Nil();
}

script::Value UIRadioButton(script::IScriptHost& host, void* /*user*/) {
    return script::Value::Bool(ImGui::RadioButton(
        host.GetArg(0).type == script::Value::Type::String ? host.GetArg(0).str.c_str() : "",
        static_cast<int>(host.GetArg(1).number) != 0));
}

} // namespace

// ---------------------------------------------------------------------------
// EditorPluginManager
// ---------------------------------------------------------------------------

struct EditorPluginManager::Impl {
    EditorApp* app = nullptr;
    std::unique_ptr<script::IScriptHost> luaHost;
    std::unique_ptr<script::IScriptHost> jsHost;
    std::vector<plugin::PluginManifest> manifests;
    std::vector<PluginPanel> panels;
    std::vector<PluginTool> tools;
    std::vector<PluginAssetSource> sources;
    std::vector<scene::ComponentSchema> extraSchemas;
    std::map<std::string, const scene::ComponentSchema*> schemaIndex;
    // Enum option string storage backing FieldSchema::options pointers.
    std::vector<std::vector<std::string>> enumStorage;
    std::vector<std::vector<const char*>> enumPtrs;

    script::IScriptHost* HostFor(const std::string& backend, EditorPluginManager* mgr) {
        if (backend == "js") {
            if (!jsHost) {
                jsHost = script::CreateJsHost();
                if (jsHost && jsHost->Init()) RegisterApi(*jsHost, mgr);
            }
            return jsHost.get();
        }
        if (!luaHost) {
            luaHost = script::CreateLuaHost();
            if (luaHost && luaHost->Init()) RegisterApi(*luaHost, mgr);
        }
        return luaHost.get();
    }

    void RegisterApi(script::IScriptHost& host, EditorPluginManager* mgr) {
        host.RegisterField("NeonEditor", "panel", &NativePanel, mgr);
        host.RegisterField("NeonEditor", "tool", &NativeTool, mgr);
        host.RegisterField("NeonEditor", "assetSource", &NativeAssetSource, mgr);
        host.RegisterField("NeonEditor", "registerComponent", &NativeRegisterComponent, mgr);
        host.RegisterField("NeonEditor", "buildMesh", &NativeBuildMesh, mgr);
        host.RegisterField("NeonEditor", "spawn", &NativeSpawn, mgr);
        host.RegisterField("NeonEditor", "selected", &NativeSelected, mgr);
        host.RegisterField("NeonEditor", "entities", &NativeEntities, mgr);
        host.RegisterField("NeonEditor", "log", &NativeLog, mgr);
        host.RegisterField("NeonEditor", "importAsset", &NativeImportAsset, mgr);
        host.RegisterField("NeonEditor", "listDir", &NativeListDir, mgr);
        // ui.* widget bindings (dotted-path registration creates the table).
        host.RegisterField("NeonEditor.ui", "Button", &UIButton, mgr);
        host.RegisterField("NeonEditor.ui", "SmallButton", &UISmallButton, mgr);
        host.RegisterField("NeonEditor.ui", "Checkbox", &UICheckbox, mgr);
        host.RegisterField("NeonEditor.ui", "Text", &UIText, mgr);
        host.RegisterField("NeonEditor.ui", "TextDisabled", &UITextDisabled, mgr);
        host.RegisterField("NeonEditor.ui", "TextColored", &UITextColored, mgr);
        host.RegisterField("NeonEditor.ui", "SameLine", &UISameLine, mgr);
        host.RegisterField("NeonEditor.ui", "Separator", &UISeparator, mgr);
        host.RegisterField("NeonEditor.ui", "SliderFloat", &UISliderFloat, mgr);
        host.RegisterField("NeonEditor.ui", "DragFloat", &UIDragFloat, mgr);
        host.RegisterField("NeonEditor.ui", "DragFloat3", &UIDragFloat3, mgr);
        host.RegisterField("NeonEditor.ui", "InputText", &UIInputText, mgr);
        host.RegisterField("NeonEditor.ui", "Combo", &UICombo, mgr);
        host.RegisterField("NeonEditor.ui", "CollapsingHeader", &UICollapsingHeader, mgr);
        host.RegisterField("NeonEditor.ui", "TreeNode", &UITreeNode, mgr);
        host.RegisterField("NeonEditor.ui", "TreePop", &UITreePop, mgr);
        host.RegisterField("NeonEditor.ui", "SetNextItemWidth", &UISetNextItemWidth, mgr);
        host.RegisterField("NeonEditor.ui", "BeginTable", &UIBeginTable, mgr);
        host.RegisterField("NeonEditor.ui", "TableSetupColumn", &UITableSetupColumn, mgr);
        host.RegisterField("NeonEditor.ui", "TableHeadersRow", &UITableHeadersRow, mgr);
        host.RegisterField("NeonEditor.ui", "TableNextRow", &UITableNextRow, mgr);
        host.RegisterField("NeonEditor.ui", "TableSetColumnIndex", &UITableSetColumnIndex, mgr);
        host.RegisterField("NeonEditor.ui", "TableEnd", &UITableEnd, mgr);
        host.RegisterField("NeonEditor.ui", "ColorEdit4", &UIColorEdit4, mgr);
        host.RegisterField("NeonEditor.ui", "ProgressBar", &UIProgressBar, mgr);
        host.RegisterField("NeonEditor.ui", "Tooltip", &UITooltip, mgr);
        host.RegisterField("NeonEditor.ui", "IsItemHovered", &UIIsItemHovered, mgr);
        host.RegisterField("NeonEditor.ui", "BeginChild", &UIBeginChild, mgr);
        host.RegisterField("NeonEditor.ui", "EndChild", &UIEndChild, mgr);
        host.RegisterField("NeonEditor.ui", "RadioButton", &UIRadioButton, mgr);
    }
};

EditorPluginManager::EditorPluginManager() : impl_(std::make_unique<Impl>()) {}

EditorPluginManager::~EditorPluginManager() { Shutdown(); }

void EditorPluginManager::Init(EditorApp* app) { impl_->app = app; }

size_t EditorPluginManager::Count() const { return impl_->manifests.size(); }

const std::vector<plugin::PluginManifest>& EditorPluginManager::Manifests() const {
    return impl_->manifests;
}

const std::vector<scene::ComponentSchema>& EditorPluginManager::Schemas() const {
    return impl_->extraSchemas;
}

std::vector<PluginPanel>& EditorPluginManager::Panels() { return impl_->panels; }
std::vector<PluginTool>& EditorPluginManager::Tools() { return impl_->tools; }
std::vector<PluginAssetSource>& EditorPluginManager::AssetSources() { return impl_->sources; }

EditorApp* EditorPluginManager::App() const { return impl_->app; }

void EditorPluginManager::Load(const std::string& baseDir) {
    Shutdown(); // re-load: fresh hosts, fresh registrations
    std::vector<plugin::PluginManifest> all = plugin::DiscoverPlugins(baseDir);
    for (const plugin::PluginManifest& m : all) {
        if (m.type != plugin::PluginType::Editor) continue;
        // D1: engine version gate (the runtime side has one; the editor
        // previously loaded plugins with no version check at all).
        if (!m.minEngineVersion.empty()) {
            plugin::Version minV, engV;
            if (plugin::ParseVersion(m.minEngineVersion, &minV) &&
                plugin::ParseVersion(plugin::kEngineVersion, &engV)) {
                const bool atLeast =
                    engV.major > minV.major ||
                    (engV.major == minV.major &&
                     (engV.minor > minV.minor ||
                      (engV.minor == minV.minor && engV.patch >= minV.patch)));
                if (!atLeast) {
                    NEON_LOG_WARN("Editor plugin '%s' needs engine >= %s (have %s); skipped",
                                  m.id.c_str(), m.minEngineVersion.c_str(),
                                  plugin::kEngineVersion);
                    continue;
                }
            }
        }
        script::IScriptHost* host = impl_->HostFor(m.backend, this);
        if (!host) {
            NEON_LOG_ERROR("Editor plugin '%s': host for '%s' unavailable; skipped",
                           m.id.c_str(), m.backend.c_str());
            continue;
        }
        const std::string source = [&]() {
            std::ifstream in(m.dir + "/" + m.entry, std::ios::binary);
            if (!in.is_open()) return std::string();
            std::stringstream ss;
            ss << in.rdbuf();
            return ss.str();
        }();
        if (source.empty()) {
            NEON_LOG_ERROR("Editor plugin '%s': cannot read '%s/%s'; skipped", m.id.c_str(),
                           m.dir.c_str(), m.entry.c_str());
            continue;
        }
        if (!host->Load(source) || !host->Run().Ok()) {
            NEON_LOG_ERROR("Editor plugin '%s': load failed: %s; skipped", m.id.c_str(),
                           host->LastError().message.c_str());
            continue;
        }
        const auto load = host->CaptureFunction("on_load");
        if (load.Ok()) {
            SetActivePlugin(*host, m.id);
            const auto res = host->CallCaptured(load.Value(), {});
            if (!res.Ok()) {
                NEON_LOG_ERROR("Editor plugin '%s': on_load failed: %s", m.id.c_str(),
                               host->LastError().message.c_str());
                continue;
            }
        }
        impl_->manifests.push_back(m);
        NEON_LOG_INFO("Editor plugin '%s' v%s (%s) loaded", m.id.c_str(), m.version.c_str(),
                      m.backend.c_str());
    }
}

void EditorPluginManager::Shutdown() {
    if (impl_->luaHost) {
        impl_->luaHost->Shutdown();
        impl_->luaHost.reset();
    }
    if (impl_->jsHost) {
        impl_->jsHost->Shutdown();
        impl_->jsHost.reset();
    }
    impl_->manifests.clear();
    impl_->panels.clear();
    impl_->tools.clear();
    impl_->sources.clear();
    impl_->extraSchemas.clear();
    impl_->schemaIndex.clear();
    impl_->enumStorage.clear();
    impl_->enumPtrs.clear();
}

void EditorPluginManager::DrawPanel(PluginPanel& panel) {
    if (!panel.opened || !panel.host || panel.drawHandle == 0) return;
    const std::string winName = panel.title + "##plugin_" + panel.id;
    if (ImGui::Begin(winName.c_str(), &panel.opened)) {
        SetActivePlugin(*panel.host, panel.pluginId);
        const auto res = panel.host->CallCaptured(panel.drawHandle, {});
        if (!res.Ok()) {
            NEON_LOG_ERROR("Editor plugin panel '%s' draw failed: %s", panel.id.c_str(),
                           panel.host->LastError().message.c_str());
        }
    }
    ImGui::End();
}

void EditorPluginManager::OnPanel(const std::string& pluginId, const std::string& id,
                                  const std::string& title, script::IScriptHost* host,
                                  uint64_t draw) {
    for (PluginPanel& p : impl_->panels) {
        if (p.id == id && p.pluginId == pluginId) return; // already registered
    }
    PluginPanel p;
    p.id = id;
    p.title = title;
    p.pluginId = pluginId;
    p.host = host;
    p.drawHandle = draw;
    impl_->panels.push_back(std::move(p));
    NEON_LOG_INFO("Editor plugin '%s' registered panel '%s'", pluginId.c_str(), id.c_str());
}

void EditorPluginManager::OnTool(const std::string& pluginId, const std::string& id,
                                 const std::string& label, script::IScriptHost* host,
                                 uint64_t fn) {
    for (PluginTool& t : impl_->tools) {
        if (t.id == id && t.pluginId == pluginId) return;
    }
    PluginTool t;
    t.id = id;
    t.label = label;
    t.pluginId = pluginId;
    t.host = host;
    t.fn = fn;
    impl_->tools.push_back(std::move(t));
}

void EditorPluginManager::OnAssetSource(const std::string& pluginId, const std::string& id,
                                        const std::string& name, script::IScriptHost* host,
                                        uint64_t listFn, uint64_t importFn) {
    for (PluginAssetSource& s : impl_->sources) {
        if (s.id == id && s.pluginId == pluginId) return;
    }
    PluginAssetSource s;
    s.id = id;
    s.name = name;
    s.pluginId = pluginId;
    s.host = host;
    s.listFn = listFn;
    s.importFn = importFn;
    impl_->sources.push_back(std::move(s));
}

void EditorPluginManager::OnComponentSchema(const std::string& name,
                                            const script::Value& schema) {
    scene::ComponentSchema out;
    out.name = name;
    out.label = name;
    if (schema.type == script::Value::Type::Table) {
        for (const auto& kv : schema.table->fields) {
            if (kv.first == "label" && kv.second.type == script::Value::Type::String) {
                out.label = kv.second.str;
                continue;
            }
            if (kv.first != "fields" || kv.second.type != script::Value::Type::Table) continue;
            for (const script::Value& fv : kv.second.table->array) {
                if (fv.type != script::Value::Type::Table) continue;
                scene::FieldSchema f;
                std::string type = "string";
                for (const auto& fk : fv.table->fields) {
                    if (fk.first == "key" && fk.second.type == script::Value::Type::String)
                        f.key = fk.second.str;
                    else if (fk.first == "label" && fk.second.type == script::Value::Type::String)
                        f.label = fk.second.str;
                    else if (fk.first == "type" && fk.second.type == script::Value::Type::String)
                        type = fk.second.str;
                    else if (fk.first == "def" && fk.second.type == script::Value::Type::Number)
                        f.def = fk.second.number;
                    else if (fk.first == "min" && fk.second.type == script::Value::Type::Number)
                        f.min = fk.second.number;
                    else if (fk.first == "max" && fk.second.type == script::Value::Type::Number)
                        f.max = fk.second.number;
                    else if (fk.first == "step" && fk.second.type == script::Value::Type::Number)
                        f.step = fk.second.number;
                    else if (fk.first == "options" &&
                             fk.second.type == script::Value::Type::Table) {
                        std::vector<std::string> opts;
                        for (const script::Value& o : fk.second.table->array)
                            if (o.type == script::Value::Type::String) opts.push_back(o.str);
                        if (!opts.empty()) {
                            impl_->enumStorage.push_back(std::move(opts));
                            std::vector<const char*> ptrs;
                            for (const std::string& o : impl_->enumStorage.back())
                                ptrs.push_back(o.c_str());
                            impl_->enumPtrs.push_back(std::move(ptrs));
                            f.options = impl_->enumPtrs.back().data();
                            f.optionCount = static_cast<int>(impl_->enumPtrs.back().size());
                            type = "enum";
                        }
                    }
                }
                if (f.key.empty()) continue;
                if (f.label.empty()) f.label = f.key;
                if (type == "number") f.type = scene::FieldType::Number;
                else if (type == "int") f.type = scene::FieldType::Int;
                else if (type == "bool") f.type = scene::FieldType::Bool;
                else if (type == "string") f.type = scene::FieldType::String;
                else if (type == "vec3") f.type = scene::FieldType::Vec3;
                else if (type == "color") f.type = scene::FieldType::Color;
                else if (type == "enum") f.type = scene::FieldType::Enum;
                else if (type == "resource") {
                    f.type = scene::FieldType::Resource;
                    for (const auto& fk : fv.table->fields)
                        if (fk.first == "kind" && fk.second.type == script::Value::Type::String)
                            f.resourceKind = nullptr; // keep simple: texture default
                }
                out.fields.push_back(std::move(f));
            }
        }
    }
    impl_->extraSchemas.push_back(std::move(out));
    impl_->schemaIndex[impl_->extraSchemas.back().name] = &impl_->extraSchemas.back();
    NEON_LOG_INFO("Editor plugin registered component schema '%s'", name.c_str());
}

const scene::ComponentSchema* EditorPluginManager::FindSchema(const std::string& name) const {
    const auto it = impl_->schemaIndex.find(name);
    return it == impl_->schemaIndex.end() ? nullptr : it->second;
}

} // namespace neon::editor
