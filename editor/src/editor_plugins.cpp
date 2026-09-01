#include "editor.hpp"
#include "editor_history.hpp"
#include "editor_util.hpp"

namespace neon::editor {

void EditorApp::PluginAddEntity(const std::string& meshKey, float x, float y, float z) {
    static int counter = 1;
    std::string name;
    if (meshKey.rfind("obj:", 0) == 0 || meshKey.rfind("gltf:", 0) == 0) {
        const std::string path = meshKey.substr(meshKey.find(':') + 1);
        const size_t slash = path.find_last_of("/\\");
        const size_t dot = path.find_last_of('.');
        const size_t begin = slash == std::string::npos ? 0 : slash + 1;
        const size_t len =
            (dot == std::string::npos || dot < begin) ? std::string::npos : dot - begin;
        name = path.substr(begin, len) + std::to_string(counter++);
    } else {
        name = meshKey + std::to_string(counter++);
    }
    SceneEntity e;
    e.name = name;
    e.meshKey = meshKey;
    e.pos = {x, y, z};
    if (ResolveMesh(e)) {
        ApplyMaterialParams(e);
        const size_t insertAt = entities_.size();
        history_.Push(std::make_unique<AddEntityCommand>(&entities_, e, insertAt));
        SetSelection(static_cast<int>(entities_.size()) - 1);
        NEON_LOG_INFO("Editor plugin spawned '%s' (%s) at (%.1f, %.1f, %.1f)", e.name.c_str(),
                      meshKey.c_str(), x, y, z);
    } else {
        NEON_LOG_ERROR("Editor plugin: cannot resolve mesh key '%s'", meshKey.c_str());
    }
}

std::string EditorApp::PluginBuildMesh(const std::string& name,
                                       const std::vector<math::Vec3>& verts,
                                       const std::vector<int>& indices) {
    if (name.empty() || verts.empty() || indices.empty() || indices.size() % 3 != 0) {
        NEON_LOG_ERROR("Editor plugin: buildMesh needs a name, verts and triangle indices");
        return {};
    }
    const std::string rel =
        projectDir_ + "/assets/generated/" + name + ".obj";
    const size_t slash = rel.find_last_of("/\\");
    if ((slash != std::string::npos && !EnsureDirs(rel.substr(0, slash))) ||
        (slash == std::string::npos)) {
        NEON_LOG_ERROR("Editor plugin: cannot create generated asset dir for '%s'", rel.c_str());
        return {};
    }
    std::ofstream out(rel, std::ios::binary);
    if (!out.is_open()) {
        NEON_LOG_ERROR("Editor plugin: cannot write '%s'", rel.c_str());
        return {};
    }
    for (const math::Vec3& v : verts)
        out << "v " << v.x << " " << v.y << " " << v.z << "\n";
    for (size_t i = 0; i < indices.size(); i += 3)
        out << "f " << indices[i] << " " << indices[i + 1] << " " << indices[i + 2] << "\n";
    out.close();
    NEON_LOG_INFO("Editor plugin: generated mesh asset '%s'", rel.c_str());
    return "obj:" + rel;
}

script::Value EditorApp::PluginSelectedEntity() const {
    if (selected_ < 0 || selected_ >= static_cast<int>(entities_.size()))
        return script::Value::Nil();
    const SceneEntity& e = entities_[static_cast<size_t>(selected_)];
    script::Value t = script::Value::Tbl();
    t.table->fields.emplace_back("name", script::Value::Str(e.name));
    t.table->fields.emplace_back("x", script::Value::Num(e.pos.x));
    t.table->fields.emplace_back("y", script::Value::Num(e.pos.y));
    t.table->fields.emplace_back("z", script::Value::Num(e.pos.z));
    return t;
}

script::Value EditorApp::PluginEntityList() const {
    script::Value out = script::Value::Tbl();
    for (const SceneEntity& e : entities_) {
        script::Value t = script::Value::Tbl();
        t.table->fields.emplace_back("name", script::Value::Str(e.name));
        t.table->fields.emplace_back("x", script::Value::Num(e.pos.x));
        t.table->fields.emplace_back("y", script::Value::Num(e.pos.y));
        t.table->fields.emplace_back("z", script::Value::Num(e.pos.z));
        t.table->fields.emplace_back("mesh", script::Value::Str(e.meshKey));
        out.table->array.push_back(std::move(t));
    }
    return out;
}

void EditorApp::PluginLog(const std::string& msg) {
    NEON_LOG_CAT(core::LogCategory::Script, core::LogLevel::Info, "plugin: %s", msg.c_str());
}

std::string EditorApp::PluginImportAsset(const std::string& srcPath) {
    if (srcPath.empty() || assetDir_.empty()) return {};
    const std::string name = BaseName(srcPath);
    if (name.empty() || name == "." || name == "..") return {};
    const std::string dst = assetDir_ + "/" + name;
    std::ifstream in(srcPath, std::ios::binary);
    std::ofstream out(dst, std::ios::binary);
    if (!in.is_open() || !out.is_open()) {
        NEON_LOG_ERROR("Editor plugin: cannot import '%s' -> '%s'", srcPath.c_str(),
                       dst.c_str());
        return {};
    }
    out << in.rdbuf();
    out.close();
    RefreshAssetDir();
    return ToProjectRelPath(dst, projectDir_);
}

void EditorApp::BuildPluginPanels() {
    if (!pluginMgr_) return;
    for (editor::PluginPanel& p : pluginMgr_->Panels()) pluginMgr_->DrawPanel(p);
}

// 插件管理面板（原 EditorApp::BuildPluginsPanel）已整体迁移为独立面板类
// editor/src/panels/plugins_panel.hpp/.cpp（PluginsPanel : IPanel，Task 8，
// 沿用 Task 2-7 样板）。EditorApp 在 OnCreate 注册它；editor_ui.cpp 的
// BuildImGuiUI 经 panels_.DrawAll(ctx_) 分发。原生插件列表状态
// （nativePlugins_/nativePluginsDir_）随面板迁入。

void EditorApp::BuildDebugOverlayPanel() {
    if (!showDebugOverlay_) return;
    if (ImGui::Begin("调试覆盖层", &showDebugOverlay_)) {
        ImGui::TextDisabled("F3 开关本面板; 图层实时作用在视口");
        ImGui::Checkbox("碰撞线框", &debugColliders_);
        ImGui::Checkbox("导航可行走区域", &debugNavMesh_);
        ImGui::Checkbox("光照探针", &debugProbes_);
        ImGui::Checkbox("音频源", &debugAudio_);
        ImGui::TextDisabled("提示: 碰撞/导航/音频在编辑与播放视口即时生效");
    }
    ImGui::End();
}

void EditorApp::DrawDebugOverlay(const gfx::Camera& cam) {
    (void)cam;
    if (!renderer_.Backend()) return;

    // G8-3 audio sources: translucent blue attenuation spheres at every entity
    // carrying an "audio" component. Radius = the component's attenuation
    // distance, so designers see how far a source is audible.
    if (debugAudio_) {
        for (const SceneEntity& e : entities_) {
            auto it = e.extraComponents.find("audio");
            if (it == e.extraComponents.end() || !it->second.IsObject()) continue;
            float radius = 10.0f;
            if (const core::Json* r = it->second.Get("radius")) {
                if (r->IsNumber()) radius = static_cast<float>(r->GetNumber());
            }
            renderer_.DrawSphere(e.pos, radius,
                                 gfx::Color{0.25f, 0.5f, 1.0f, 0.18f});
        }
    }

    // Navigation walkable area: translucent green (walkable) / red (blocked)
    // ground cells so the field is visible in the viewport, not just the panel.
    if (debugNavMesh_ && nav_.grid.Valid()) {
        static gfx::Mesh cell = gfx::Mesh::CreatePlane(renderer_, 1.0f, 1.0f, 1, 1, "navcell");
        if (!cell.Valid()) return;
        gfx::Material walk = gfx::Material::Unlit({}, gfx::Color{0.3f, 0.85f, 0.4f, 0.28f});
        walk.transparent = true;
        gfx::Material block = gfx::Material::Unlit({}, gfx::Color{0.9f, 0.3f, 0.3f, 0.28f});
        block.transparent = true;
        for (int y = 0; y < nav_.grid.Height(); ++y) {
            for (int x = 0; x < nav_.grid.Width(); ++x) {
                const math::Vec2 c = nav_.grid.CellToWorld(x, y);
                const math::Mat4 m = math::Mat4::Translation({c.x, 0.02f, c.y}) *
                                     math::Mat4::Scale({nav_.grid.CellSize(), 1.0f,
                                                        nav_.grid.CellSize()});
                renderer_.DrawMesh(cell, nav_.grid.Walkable(x, y) ? walk : block, m);
            }
        }
    }

    // Light probes: wireframe sphere markers tinted by probe irradiance, over a
    // field rebuilt lazily from the scene's 3D meshes' combined AABB. A single
    // representative light near the scene centre makes the 3D gradient visible;
    // real scene point lights would feed this list (G2-4).
    if (debugProbes_) {
        math::AABB bounds;
        bool haveBounds = false;
        for (const SceneEntity& e : entities_) {
            if (!e.mesh.Valid()) continue;
            const math::Mat4 model = math::Mat4::Translation(e.pos) * e.rot.ToMat4() *
                                     math::Mat4::Scale(e.scale);
            const math::AABB wb = math::TransformAABB(e.mesh.Bounds(), model);
            if (!haveBounds) {
                bounds = wb;
                haveBounds = true;
            } else {
                bounds.min.x = std::min(bounds.min.x, wb.min.x);
                bounds.min.y = std::min(bounds.min.y, wb.min.y);
                bounds.min.z = std::min(bounds.min.z, wb.min.z);
                bounds.max.x = std::max(bounds.max.x, wb.max.x);
                bounds.max.y = std::max(bounds.max.y, wb.max.y);
                bounds.max.z = std::max(bounds.max.z, wb.max.z);
            }
        }
        if (!haveBounds) {
            bounds = math::AABB{{-15, 0, -15}, {15, 5, 15}};
        }
        const bool same =
            debugProbeBounds_.min.x == bounds.min.x && debugProbeBounds_.max.x == bounds.max.x &&
            debugProbeBounds_.min.y == bounds.min.y && debugProbeBounds_.max.y == bounds.max.y &&
            debugProbeBounds_.min.z == bounds.min.z && debugProbeBounds_.max.z == bounds.max.z;
        if (debugProbeDirty_ || !same) {
            gfx::ProbeLightInput in;
            in.pointLights.push_back({(bounds.min + bounds.max) * 0.5f,
                                      gfx::Color{1.0f, 0.9f, 0.75f, 1.0f}, 6.0f,
                                      std::max(bounds.max.x - bounds.min.x, 8.0f)});
            gfx::BuildProbeField(bounds, debugProbeRes_, in, debugProbeField_);
            debugProbeBounds_ = bounds;
            debugProbeDirty_ = false;
        }
        const float markerR =
            std::max(0.08f, (bounds.max.x - bounds.min.x) / (2.0f * static_cast<float>(debugProbeRes_)));
        for (const gfx::IrradianceProbe& p : debugProbeField_) {
            const float y = math::Clamp(p.irradiance.y, 0.0f, 1.0f);
            renderer_.DrawSphere(p.pos, markerR,
                                 gfx::Color{math::Clamp(p.irradiance.x, 0.0f, 1.0f),
                                            y,
                                            math::Clamp(p.irradiance.z, 0.0f, 1.0f), 0.9f});
        }
    }
}

} // namespace neon::editor

