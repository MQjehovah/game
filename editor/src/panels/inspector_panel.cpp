#include "panels/inspector_panel.hpp"

// 属性面板实现 = 原 EditorApp::BuildInspectorPanel（panels_inspector.inc）方法体
// 逐行迁移：EditorApp 成员访问改经 EditorContext（ctx.xxx / 局部引用），EditorApp
// 方法调用改 ctx 注入回调，函数内 static 面板私有状态改本类成员。行为零变化。
// 节点类型表（kNodeTypes）与自动类型标签（EntityTypeLabel/TypeLabel）原是
// panels.cpp 匿名命名空间的局部助手（仅本面板用到，随迁移带入）。

#include <cmath>
#include <cstdio>
#include <set>
#include <string>
#include <vector>

#include "editor.hpp"
#include "editor_history.hpp"
#include "editor_util.hpp"

#include "imgui.h"
#include "neon/assets/mesh_format.hpp"
#include "neon/core/json.hpp"
#include "neon/gfx/imgui_neon.hpp"

namespace neon::editor {

namespace {

// 原 panels.cpp 匿名命名空间里的局部助手（仅本面板用到，随迁移带入）。
std::string TypeLabel(const std::string& key) {
    if (key.empty()) return "实体";
    if (key == "terrain") return "地形";
    if (key == "helmet") return "头盔 (glTF PBR)";
    if (key == "cube") return "方块";
    if (key == "sphere") return "球体";
    if (key == "plane") return "平面";
    if (key == "hero") return "英雄";
    if (key == "wolf") return "狼";
    if (key == "npc" || key.compare(0, 4, "npc:") == 0) return "村民";
    if (key == "house") return "房屋";
    if (key == "bush") return "灌木";
    if (key == "rock") return "岩石";
    if (key == "water") return "水面";
    if (key == "road") return "道路";
    if (key == "tree") return "松树 (OBJ)";
    // File-backed mesh formats get their registered display label (obj/gltf/
    // fbx/...). Adding a format auto-appears here.
    if (const std::string p = assets::MeshFormatRegistry::Instance().MatchPrefix(key); !p.empty())
        return assets::MeshFormatRegistry::Instance().DisplayName(p);
    return key;
}

// Unity-like entity type: what the selected object IS, derived from its
// components / mesh kind (plant/zombie from the 2D canvas, sprite, prefab,
// or the mesh type).
std::string EntityTypeLabel(const SceneEntity& e) {
    if (!e.nodeType.empty()) return e.nodeType;
    if (e.extraComponents.count("plant")) return "植物";
    if (e.extraComponents.count("zombie")) return "僵尸";
    if (!e.spriteTex.empty()) return "精灵";
    if (!e.prefab.empty()) return "预制体: " + e.prefab;
    return TypeLabel(e.meshKey);
}

// Node type table (P1-1): the combo list for the inspector.
const char* kNodeTypes[] = {"Node", "MeshInstance3D", "Camera3D",
                            "CharacterBody", "Sprite", "Light3D"};

} // namespace

void InspectorPanel::Draw(EditorContext& ctx) {
    // 过渡期逃生舱：EditMeshKeyCommand 需要 EditorApp*（undo/redo 内经它调
    // ResolveMesh/ApplyMaterialParams）。OnCreate 注入，Draw 期必然有效。
    auto* app = static_cast<EditorApp*>(ctx.editorApp);
    std::vector<SceneEntity>& entities = *ctx.entities;
    std::set<int>& selection = *ctx.selection;
    int& selected = *ctx.selected;
    HistoryManager& history = *ctx.history;
    bool& sceneDirty = *ctx.sceneDirty;
    scene::PrefabLibrary& prefabLib = *ctx.prefabLib;
    assets::AssetManager& assetMgr = *ctx.assetMgr;
    const std::string& projectDir = *ctx.projectDir;
    if (!visible_ || !*visible_ || selected < 0 ||
        selected >= static_cast<int>(entities.size())) {
        return;
    }
    if (ImGui::Begin("属性", visible_)) {
        SceneEntity& e = entities[static_cast<size_t>(selected)];
        // P2-editor UX: multi-selection banner + batch operations. Field edits
        // below always target the ACTIVE (last-clicked) entity.
        if (selection.size() > 1) {
            ImGui::TextColored(ImVec4(0.7f, 0.85f, 1.0f, 1.0f),
                               "多选: %zu 个实体 (编辑作用于 \"%s\")", selection.size(),
                               e.name.c_str());
            ImGui::SameLine();
            if (ImGui::SmallButton("批量复制")) {
                history.Push(std::make_unique<MultiDuplicateEntityCommand>(
                    &entities, ctx.selectedIndices()));
                ctx.setSelection(static_cast<int>(entities.size()) - 1);
            }
            ImGui::SameLine();
            if (ImGui::SmallButton("批量删除")) {
                history.Push(std::make_unique<MultiDeleteEntityCommand>(
                    &entities, ctx.selectedIndices()));
                ctx.clampSelection();
            }
            ImGui::Separator();
        }
        char buf[160];
        std::snprintf(buf, sizeof(buf), "%s", e.name.c_str());
        // Every field edit routes through the undo/redo command stack; the old
        // value is captured before the widget mutates the entity.
        if (ImGui::InputText("名称", buf, sizeof(buf))) {
            const std::string oldName = e.name;
            e.name = buf;
            history.Push(std::make_unique<EditPropertyCommand<std::string>>(
                &entities, selected, ApplyNameProp, oldName, e.name,
                /*mergeable=*/false)); // one undo step per keystroke
        }
        // Scene-tree parent is edited via the hierarchy panel (drag to
        // reparent); it is intentionally not shown here, the tree already
        // visualizes the parent-child relationship.
        // Node type table (P1-1): explicit type overrides the auto-derived
        // label; the inspector renders type-specific sections below.
        {
            const std::string current = e.nodeType.empty() ? EntityTypeLabel(e) : e.nodeType;
            int sel = 0;
            for (int i = 0; i < static_cast<int>(sizeof(kNodeTypes) / sizeof(kNodeTypes[0])); ++i)
                if (current == kNodeTypes[i]) sel = i;
            if (ImGui::Combo("类型", &sel, kNodeTypes,
                             static_cast<int>(sizeof(kNodeTypes) / sizeof(kNodeTypes[0])))) {
                const std::string oldType = e.nodeType;
                e.nodeType = kNodeTypes[sel];
                history.Push(std::make_unique<EditPropertyCommand<std::string>>(
                    &entities, selected, ApplyNodeTypeProp, oldType, e.nodeType,
                    /*mergeable=*/false));
            }
            if (!e.nodeType.empty())
                ImGui::TextDisabled("自动类型: %s", EntityTypeLabel(e).c_str());
        }
        if (e.nodeType == "Camera3D") {
            const float oldFov = e.cameraFov;
            if (ImGui::DragFloat("视野 (度)", &e.cameraFov, 0.5f, 20.0f, 120.0f)) {
                history.Push(std::make_unique<EditPropertyCommand<float>>(
                    &entities, selected, ApplyCameraFovProp, oldFov, e.cameraFov));
            }
            const bool oldOrtho = e.cameraOrtho;
            if (ImGui::Checkbox("正交相机", &e.cameraOrtho)) {
                history.Push(std::make_unique<EditPropertyCommand<bool>>(
                    &entities, selected, ApplyCameraOrthoProp, oldOrtho, e.cameraOrtho));
            }
            if (e.cameraOrtho) {
                if (ImGui::DragFloat("正交尺寸", &e.cameraOrthoSize, 0.1f, 0.1f, 2000.0f))
                    sceneDirty = true;
            }
            if (ImGui::DragFloat("画面比例", &e.cameraAspect, 0.005f, 0.0f, 4.0f, "%.3f")) {
                if (e.cameraAspect > 0.001f && e.cameraAspect < 0.5f) e.cameraAspect = 0.5f;
                sceneDirty = true;
            }
            ImGui::TextDisabled("画面比例: 运行时游戏区的宽高比(0=默认16:9); 蓝框即此视野预览");
            ImGui::TextDisabled("将相机实体选中并设为视图: 使用右上角相机菜单的\"跟随选中\"");
            ImGui::Separator();
        }
        if (e.hasLight) {
            if (ImGui::CollapsingHeader("光源##light", ImGuiTreeNodeFlags_DefaultOpen)) {
                static const char* kLt[] = {"方向光", "点光源", "环境光"};
                const int typeIdx = e.light.type == "point" ? 1 : (e.light.type == "ambient" ? 2 : 0);
                int sel = typeIdx;
                if (ImGui::Combo("类型##lt", &sel, kLt, 3)) {
                    e.light.type = sel == 1 ? "point" : (sel == 2 ? "ambient" : "directional");
                    sceneDirty = true;
                }
                if (e.light.type == "directional") {
                    if (ImGui::DragFloat3("方向##lt", &e.light.sunDir.x, 0.05f)) sceneDirty = true;
                    if (ImGui::DragFloat("强度##lt", &e.light.intensity, 0.05f, 0.0f, 10.0f))
                        sceneDirty = true;
                } else if (e.light.type == "point") {
                    if (ImGui::DragFloat("半径##lt", &e.light.radius, 0.1f, 0.1f, 500.0f))
                        sceneDirty = true;
                    if (ImGui::DragFloat("强度##lt", &e.light.intensity, 0.05f, 0.0f, 10.0f))
                        sceneDirty = true;
                } else { // ambient
                    if (ImGui::DragFloat("强度##lt", &e.light.ambientStrength, 0.01f, 0.0f, 2.0f))
                        sceneDirty = true;
                }
                float col[4] = {e.light.color.r, e.light.color.g, e.light.color.b, e.light.color.a};
                if (ImGui::ColorEdit4("颜色##lt", col)) {
                    e.light.color = {col[0], col[1], col[2], col[3]};
                    sceneDirty = true;
                }
                ImGui::Separator();
            }
        }
        if (e.nodeType == "Sprite" && e.spriteTex.empty()) {
            ImGui::TextColored(ImVec4(0.8f, 0.85f, 1.0f, 1.0f),
                               "精灵类型: 在下方\"精灵\"区块设置贴图");
        }
        if (!e.spriteTex.empty()) {
            ImGui::TextDisabled("精灵贴图: %s", e.spriteTex.c_str());
            const SpriteFlipValue oldFlip{e.spriteFlipX, e.spriteFlipY};
            bool fx = e.spriteFlipX, fy = e.spriteFlipY;
            ImGui::Checkbox("水平翻转", &fx);
            ImGui::SameLine();
            ImGui::Checkbox("垂直翻转", &fy);
            if (fx != e.spriteFlipX || fy != e.spriteFlipY) {
                e.spriteFlipX = fx;
                e.spriteFlipY = fy;
                history.Push(std::make_unique<EditPropertyCommand<SpriteFlipValue>>(
                    &entities, selected, ApplySpriteFlip, oldFlip, SpriteFlipValue{fx, fy}));
            }
            ImGui::Separator();
        }

        if (!e.prefab.empty()) {
            ImGui::TextDisabled("预置体: %s", e.prefab.c_str());
        }
        ImGui::Separator();
        // G5-4-4(项1): prefab instance overrides. The instance stores only the
        // field-level diff against the template; 重置为预制体 reverts every
        // component override (transform/name kept — they are per-instance).
        if (!e.prefab.empty() && prefabLib.Has(e.prefab)) {
            if (ImGui::CollapsingHeader("预制体实例##prefab", ImGuiTreeNodeFlags_DefaultOpen)) {
                ImGui::TextDisabled("模板: %s", e.prefab.c_str());
                if (ImGui::Button("重置为预制体")) {
                    SceneEntity fresh = ctx.materializePrefab(e.prefab, e.pos);
                    fresh.id = e.id;
                    fresh.name = e.name;
                    fresh.pos = e.pos;
                    fresh.rot = e.rot;
                    fresh.scale = e.scale;
                    fresh.parentId = e.parentId;
                    fresh.prefab = e.prefab;
                    if (ctx.resolveMesh(fresh)) {
                        ctx.applyMaterialParams(fresh);
                        history.Push(std::make_unique<ReplaceEntityCommand>(
                            &entities, selected, std::move(fresh)));
                    }
                }
                ImGui::TextDisabled("实例字段按模板继承; 编辑字段即为覆盖(保存时仅写入覆盖)");
            }
            ImGui::Separator();
        }
        // Unity-style: every entity is a type + a list of components. The
        // default components (变换/网格/生命) are ordinary blocks in this same
        // list; transform is mandatory like Unity's Transform, mesh and health
        // are removable.
        if (ImGui::CollapsingHeader("组件", ImGuiTreeNodeFlags_DefaultOpen)) {
        if (ImGui::CollapsingHeader("变换##transform", ImGuiTreeNodeFlags_DefaultOpen)) {
        const math::Vec3 oldPos = e.pos;
        if (ImGui::DragFloat3("位置", &e.pos.x, 0.1f)) {
            history.Push(std::make_unique<EditTransformCommand>(
                &entities, selected, oldPos, e.rot, e.scale, e.pos, e.rot, e.scale,
                EditTransformCommand::kPos));
        }
        math::Vec3 euler = e.rot.ToMat4().TransformDir({0, 0, -1});
        float rotDeg = std::atan2(euler.x, -euler.z) * math::kRadToDeg;
        const math::Quat oldRot = e.rot;
        if (ImGui::DragFloat("旋转 Y", &rotDeg, 0.5f, -180.0f, 180.0f)) {
            e.rot = math::Quat::FromEuler(0, rotDeg * math::kDegToRad, 0);
            history.Push(std::make_unique<EditTransformCommand>(
                &entities, selected, e.pos, oldRot, e.scale, e.pos, e.rot, e.scale,
                EditTransformCommand::kRot));
        }
        const math::Vec3 oldScale = e.scale;
        if (ImGui::DragFloat3("缩放", &e.scale.x, 0.05f, 0.05f, 50.0f)) {
            history.Push(std::make_unique<EditTransformCommand>(
                &entities, selected, e.pos, e.rot, oldScale, e.pos, e.rot, e.scale,
                EditTransformCommand::kScale));
        }
        const float oldZ = e.zOrder;
        if (ImGui::DragFloat("Z 排序", &e.zOrder, 0.1f, -10000.0f, 10000.0f)) {
            history.Push(std::make_unique<EditPropertyCommand<float>>(
                &entities, selected, ApplyZOrderProp, oldZ, e.zOrder));
        }
        }
        // 网格 (MeshRenderer-like): mesh key + material + textures in one
        // block. Hidden for sprites (the sprite quad replaces it); removable -
        // the entity becomes a logical/sprite-only object, and 网格 reappears
        // in the 添加组件 dropdown to re-add it.
        if (e.spriteTex.empty() && !e.meshKey.empty()) {
        // Collapsed by default: the mesh block (key + material + 4 texture
        // slots) is tall and pushed 生命/脚本 below the panel's visible area,
        // making their remove buttons unreachable without scrolling.
        const bool meshOpen = ImGui::CollapsingHeader("网格##mesh", ImGuiTreeNodeFlags_None);
        if (meshOpen && !e.meshKey.empty()) {
        char meshBuf[2048];
        std::snprintf(meshBuf, sizeof(meshBuf), "%s", e.meshKey.c_str());
        if (ImGui::InputText("网格键", meshBuf, sizeof(meshBuf),
                             ImGuiInputTextFlags_EnterReturnsTrue)) {
            const std::string newKey(meshBuf);
            if (newKey != e.meshKey) {
                const std::string oldKey = e.meshKey;
                history.Push(std::make_unique<EditMeshKeyCommand>(
                    app, &entities, selected, oldKey, newKey));
            }
        }
        // Drag a model from the asset panel to replace the mesh.
        if (ImGui::BeginDragDropTarget()) {
            if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("ASSET_MODEL")) {
                std::string path(static_cast<const char*>(payload->Data),
                                 static_cast<size_t>(payload->DataSize));
                if (!path.empty() && path.back() == '\0') path.pop_back();
                if (!path.empty()) {
                    const std::string prefix =
                        assets::MeshFormatRegistry::Instance().FormatFromExt(path);
                    if (prefix.empty()) return; // not a mesh model
                    const std::string key = prefix + ":" + path;
                    const std::string oldKey = e.meshKey;
                    history.Push(std::make_unique<EditMeshKeyCommand>(
                        app, &entities, selected, oldKey, key));
                }
            }
            ImGui::EndDragDropTarget();
        }
        if (e.mesh.Valid()) {
            ImGui::TextDisabled("%u 三角形", e.mesh.TriangleCount());
            const math::AABB& b = e.mesh.Bounds();
            ImGui::TextDisabled("包围盒 (%.1f, %.1f, %.1f) ~ (%.1f, %.1f, %.1f)", b.min.x,
                                b.min.y, b.min.z, b.max.x, b.max.y, b.max.z);
        }
        ImGui::Separator();
        if (!e.materialRef.empty()) {
            ImGui::TextDisabled("材质球: %s", e.materialRef.c_str());
        }
        {
            std::snprintf(matNameBuf_, sizeof(matNameBuf_), "%s", e.name.c_str());
            ImGui::SetNextItemWidth(150.0f);
            ImGui::InputText("材质球名", matNameBuf_, sizeof(matNameBuf_));
            ImGui::SameLine();
            if (ImGui::Button("另存为材质球")) {
                std::string name(matNameBuf_);
                if (!name.empty()) {
                    const size_t dot = name.find_last_of('.');
                    if (dot != std::string::npos) name = name.substr(0, dot);
                    ctx.saveMaterialAsset(name);
                }
            }
        }
        // P2-6 custom shader (fragment .glsl) with hot reload: compiled
        // against the built-in unlit vertex contract; the file is re-watched
        // by PollHotReload (--hot) and the 重编译 button recompiles now.
        {
            std::snprintf(shaderBuf_, sizeof(shaderBuf_), "%s", e.shaderPath.c_str());
            ImGui::SetNextItemWidth(320.0f);
            if (ImGui::InputText("着色器 (.glsl 片元)", shaderBuf_, sizeof(shaderBuf_))) {
                const std::string oldPath = e.shaderPath;
                e.shaderPath = shaderBuf_;
                history.Push(std::make_unique<EditPropertyCommand<std::string>>(
                    &entities, selected, ApplyShaderPathProp, oldPath, e.shaderPath,
                    /*mergeable=*/false));
                ctx.reloadEntityShader(e);
            }
            ImGui::SameLine();
            if (ImGui::Button("重编译")) ctx.reloadEntityShader(e);
            if (!e.shaderPath.empty())
                ImGui::TextDisabled(e.customShader.Valid() ? "已编译 ✓ (GL)"
                                                           : "未编译 / 后端不支持自定义着色器");
        }
        // Drag a material-ball asset from the asset panel onto the entity.
        if (ImGui::BeginDragDropTarget()) {
            if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("ASSET_MATERIAL")) {
                std::string path(static_cast<const char*>(payload->Data),
                                 static_cast<size_t>(payload->DataSize));
                if (!path.empty() && path.back() == '\0') path.pop_back();
                if (!path.empty()) ctx.applyMaterialAsset(path);
            }
            ImGui::EndDragDropTarget();
        }
        const gfx::Color oldTint = e.tint;
        if (ImGui::ColorEdit3("颜色", &e.tint.r)) {
            e.material.tint = e.tint;
            history.Push(std::make_unique<EditPropertyCommand<gfx::Color>>(
                &entities, selected, ApplyColorProp, oldTint, e.tint));
        }
        const float oldMetallic = e.metallic;
        if (ImGui::DragFloat("金属度", &e.metallic, 0.01f, 0.0f, 1.0f)) {
            e.material.metallic = e.metallic;
            history.Push(std::make_unique<EditPropertyCommand<float>>(
                &entities, selected, ApplyMetallicProp, oldMetallic, e.metallic));
        }
        const float oldRoughness = e.roughness;
        if (ImGui::DragFloat("粗糙度", &e.roughness, 0.01f, 0.0f, 1.0f)) {
            e.material.roughness = e.roughness;
            history.Push(std::make_unique<EditPropertyCommand<float>>(
                &entities, selected, ApplyRoughnessProp, oldRoughness, e.roughness));
        }
        const float oldUv = e.uvRepeat;
        if (ImGui::DragFloat("UV 重复", &e.uvRepeat, 0.05f, 0.1f, 64.0f, "%.1f")) {
            e.material.uvRepeat = e.uvRepeat;
            history.Push(std::make_unique<EditPropertyCommand<float>>(
                &entities, selected, ApplyUvRepeatProp, oldUv, e.uvRepeat));
        }
        const float oldAO = e.ao;
        if (ImGui::DragFloat("环境光遮蔽", &e.ao, 0.01f, 0.0f, 1.0f)) {
            e.material.aoStrength = e.ao;
            history.Push(std::make_unique<EditPropertyCommand<float>>(
                &entities, selected, ApplyAOProp, oldAO, e.ao));
        }
        const float oldEmissiveIntensity = e.emissiveIntensity;
        if (ImGui::DragFloat("自发光强度", &e.emissiveIntensity, 0.05f, 0.0f, 5.0f)) {
            e.material.emissiveIntensity = e.emissiveIntensity;
            history.Push(std::make_unique<EditPropertyCommand<float>>(
                &entities, selected, ApplyEmissiveIntensityProp, oldEmissiveIntensity,
                e.emissiveIntensity));
        }
        // One slot per PBR texture: thumbnail preview, editable path (Enter to
        // commit), clear button, and a drag-drop target from the asset panel.
        // Every change routes through the undo history as a texture-slot edit.
        auto textureSlot = [&](const char* label, std::string& path,
                              gfx::TextureHandle& handle,
                              void (*apply)(SceneEntity&, const TextureSlotValue&)) {
            ImTextureID tid = ImTextureID_Invalid;
            if (handle.Valid()) tid = gfx::ImGuiNeon_RegisterTexture(handle);
            const ImVec2 previewSize(22.0f, 22.0f);
            // The thumbnail is also the drop target for textures dragged from
            // the asset panel.
            if (tid != ImTextureID_Invalid)
                ImGui::ImageButton(("##thumb_" + std::string(label)).c_str(), tid,
                                   previewSize);
            else
                ImGui::Button(("##thumb_" + std::string(label)).c_str(), previewSize);
            if (ImGui::BeginDragDropTarget()) {
                if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("ASSET_TEXTURE")) {
                    std::string newPath(static_cast<const char*>(payload->Data),
                                        static_cast<size_t>(payload->DataSize));
                    if (!newPath.empty() && newPath.back() == '\0') newPath.pop_back();
                    if (!newPath.empty() && newPath != path) {
                        const TextureSlotValue oldVal{path, handle};
                        gfx::Texture tex = assetMgr.LoadTexture(newPath);
                        if (tex.Valid()) {
                            const TextureSlotValue newVal{newPath, tex.Handle()};
                            history.Push(std::make_unique<EditPropertyCommand<TextureSlotValue>>(
                                &entities, selected, apply, oldVal, newVal));
                        } else {
                            NEON_LOG_WARN("Editor: dropped texture '%s' failed to load", newPath.c_str());
                        }
                    }
                }
                ImGui::EndDragDropTarget();
            }
            ImGui::SameLine();
            char buf[2048];
            std::snprintf(buf, sizeof(buf), "%s", path.c_str());
            ImGui::SetNextItemWidth(-190.0f);
            if (ImGui::InputText((std::string("##path_") + label).c_str(), buf, sizeof(buf),
                                 ImGuiInputTextFlags_EnterReturnsTrue)) {
                const std::string newPath(buf);
                if (newPath != path) {
                    const TextureSlotValue oldVal{path, handle};
                    gfx::Texture tex =
                        newPath.empty() ? gfx::Texture{} : assetMgr.LoadTexture(newPath);
                    if (newPath.empty() || tex.Valid()) {
                        // Store the project-relative path (so it survives a save
                        // + restart and resolves through the runtime's
                        // FullAssetPath), even though LoadTexture used `newPath`.
                        const std::string storePath = ToProjectRelPath(newPath, projectDir);
                        const TextureSlotValue newVal{storePath, tex.Handle()};
                        history.Push(std::make_unique<EditPropertyCommand<TextureSlotValue>>(
                            &entities, selected, apply, oldVal, newVal));
                    } else {
                        NEON_LOG_WARN("Editor: texture '%s' failed to load (slot '%s')",
                                      newPath.c_str(), label);
                    }
                }
            }
            ImGui::SameLine();
            if (ImGui::Button((std::string("清除##") + label).c_str())) {
                const TextureSlotValue oldVal{path, handle};
                const TextureSlotValue newVal{"", gfx::TextureHandle{}};
                history.Push(std::make_unique<EditPropertyCommand<TextureSlotValue>>(
                    &entities, selected, apply, oldVal, newVal));
            }
            // P2-editor UX: drag a texture asset from the 资源 panel onto the
            // slot to assign it.
            if (ImGui::BeginDragDropTarget()) {
                if (const ImGuiPayload* p =
                        ImGui::AcceptDragDropPayload("ASSET_TEXTURE")) {
                    const char* src = static_cast<const char*>(p->Data);
                    if (src && *src) {
                        const std::string newPath(src);
                        gfx::Texture tex = assetMgr.LoadTexture(newPath);
                        if (tex.Valid()) {
                            // Store project-relative so the path survives a save
                            // + restart and resolves via FullAssetPath in play.
                            const std::string storePath = ToProjectRelPath(newPath, projectDir);
                            const TextureSlotValue oldVal{path, handle};
                            history.Push(std::make_unique<EditPropertyCommand<TextureSlotValue>>(
                                &entities, selected, apply, oldVal,
                                TextureSlotValue{storePath, tex.Handle()}));
                        } else {
                            NEON_LOG_WARN("Editor: texture '%s' failed to load (slot '%s')",
                                          newPath.c_str(), label);
                        }
                    }
                }
                ImGui::EndDragDropTarget();
            }
            // Trailing label (like the material rows): thumb / input / clear
            // stay aligned regardless of label length.
            ImGui::SameLine();
            ImGui::TextDisabled("%s", label);
        };
        textureSlot("漫反射", e.albedoTex, e.material.albedo, ApplyAlbedoTexSlot);
        textureSlot("金属度/粗糙度", e.mrTex, e.material.metallicRoughness, ApplyMRTexSlot);
        textureSlot("环境光遮蔽图", e.aoTex, e.material.occlusion, ApplyAOTexSlot);
        textureSlot("自发光图", e.emissiveTex, e.material.emissive, ApplyEmissiveTexSlot);
        ImGui::Separator();
        if (ImGui::Button("移除网格")) {
            history.Push(std::make_unique<EditMeshKeyCommand>(
                app, &entities, selected, e.meshKey, ""));
        }
        }
        }
        // P2-1 ground decal: a flat textured quad projected onto the ground.
        // A component like mesh/health; only shows when a decal texture is set
        // (or via 添加组件 -> 贴花 to add one).
        if (ImGui::CollapsingHeader("贴花##decal")) {
            std::snprintf(decalBuf_, sizeof(decalBuf_), "%s", e.decalTex.c_str());
            ImGui::SetNextItemWidth(300.0f);
            if (ImGui::InputText("贴图", decalBuf_, sizeof(decalBuf_))) {
                const std::string old = e.decalTex;
                e.decalTex = decalBuf_;
                e.decalMesh = {};
                history.Push(std::make_unique<EditPropertyCommand<std::string>>(
                    &entities, selected, ApplyDecalTexProp, old, e.decalTex,
                    /*mergeable=*/false));
            }
            if (!e.decalTex.empty()) {
                const float oldSize = e.decalSize;
                if (ImGui::DragFloat("尺寸", &e.decalSize, 0.1f, 0.1f, 100.0f)) {
                    e.decalMesh = {};
                    history.Push(std::make_unique<EditPropertyCommand<float>>(
                        &entities, selected, ApplyDecalSizeProp, oldSize, e.decalSize));
                }
                const float oldAlpha = e.decalAlpha;
                if (ImGui::DragFloat("不透明度", &e.decalAlpha, 0.01f, 0.0f, 1.0f)) {
                    history.Push(std::make_unique<EditPropertyCommand<float>>(
                        &entities, selected, ApplyDecalAlphaProp, oldAlpha, e.decalAlpha));
                }
            } else {
                ImGui::TextDisabled("填入贴图路径后保存/导出即生成地面贴花");
            }
            ImGui::Separator();
        }
        auto makeNum = [](double v) {
            core::Json j;
            j.type_ = core::Json::Type::Number;
            j.number_ = v;
            return j;
        };
        auto makeStr = [](const std::string& s) {
            core::Json j;
            j.type_ = core::Json::Type::String;
            j.string_ = s;
            return j;
        };
        auto makeBool = [](bool v) {
            core::Json j;
            j.type_ = core::Json::Type::Bool;
            j.bool_ = v;
            return j;
        };
        auto makeArr = [&](const std::vector<double>& v) {
            core::Json j;
            j.type_ = core::Json::Type::Array;
            for (double x : v) j.array_.push_back(makeNum(x));
            return j;
        };
        // G5-4-4: ONE schema-driven field editor for every component (built-in
        // and data). Renders a component's fields from its ComponentSchema and
        // pushes an EditComponentCommand (Json-based undo through the canonical
        // bridge) per changed field. Built-in components (health/groups) pass a
        // bridged component JSON; data components pass extraComponents[name].
        auto renderSchemaFields = [&](const scene::ComponentSchema& schema,
                                      core::Json& compData) {
            for (const scene::FieldSchema& f : schema.fields) {
                if (!compData.IsObject()) compData.type_ = core::Json::Type::Object;
                core::Json& node = compData.object_[f.key];
                if (node.IsNull()) node = makeNum(f.def);
                const core::Json oldField = node;
                bool changed = false;
                switch (f.type) {
                    case scene::FieldType::Number: {
                        float v = static_cast<float>(node.IsNumber() ? node.GetNumber() : f.def);
                        if (ImGui::DragFloat(f.label.c_str(), &v, static_cast<float>(f.step),
                                             static_cast<float>(f.min),
                                             static_cast<float>(f.max)))
                            node = makeNum(static_cast<double>(v)), changed = true;
                        break;
                    }
                    case scene::FieldType::Int: {
                        int v = node.IsNumber() ? static_cast<int>(node.GetNumber())
                                                : static_cast<int>(f.def);
                        if (ImGui::DragInt(f.label.c_str(), &v, 1, static_cast<int>(f.min),
                                           static_cast<int>(f.max)))
                            node = makeNum(v), changed = true;
                        break;
                    }
                    case scene::FieldType::Bool: {
                        bool v = node.IsBool() ? node.GetBool() : false;
                        if (ImGui::Checkbox(f.label.c_str(), &v))
                            node = makeBool(v), changed = true;
                        break;
                    }
                    case scene::FieldType::String: {
                        char buf[1024];
                        std::snprintf(buf, sizeof(buf), "%s",
                                      node.IsString() ? node.GetString().c_str() : "");
                        if (ImGui::InputText(f.label.c_str(), buf, sizeof(buf))) {
                            node = makeStr(buf);
                            changed = true;
                        }
                        break;
                    }
                    case scene::FieldType::Vec3: {
                        float v[3] = {static_cast<float>(f.def), static_cast<float>(f.def),
                                      static_cast<float>(f.def)};
                        if (node.IsArray() && node.Size() == 3) {
                            for (int i = 0; i < 3; ++i)
                                v[i] = static_cast<float>(node.At(static_cast<size_t>(i))
                                                              ->GetNumber());
                        }
                        if (ImGui::DragFloat3(f.label.c_str(), v, static_cast<float>(f.step),
                                              static_cast<float>(f.min),
                                              static_cast<float>(f.max))) {
                            node = makeArr({v[0], v[1], v[2]});
                            changed = true;
                        }
                        break;
                    }
                    case scene::FieldType::Color: {
                        float col[4] = {1, 1, 1, 1};
                        if (node.IsString()) {
                            gfx::Color c = ColorFromHex(node.GetString());
                            col[0] = c.r;
                            col[1] = c.g;
                            col[2] = c.b;
                        }
                        if (ImGui::ColorEdit3(f.label.c_str(), col)) {
                            char hex[16];
                            std::snprintf(hex, sizeof(hex), "#%02X%02X%02X",
                                          static_cast<int>(col[0] * 255.0f),
                                          static_cast<int>(col[1] * 255.0f),
                                          static_cast<int>(col[2] * 255.0f));
                            node = makeStr(hex);
                            changed = true;
                        }
                        break;
                    }
                    case scene::FieldType::Enum: {
                        int sel = 0;
                        if (node.IsString() && f.options) {
                            for (int i = 0; i < f.optionCount; ++i)
                                if (node.GetString() == f.options[i]) sel = i;
                        }
                        if (ImGui::Combo(f.label.c_str(), &sel, f.options, f.optionCount)) {
                            node = makeStr(f.options[sel]);
                            changed = true;
                        }
                        break;
                    }
                    case scene::FieldType::Resource: {
                        std::string path = node.IsString() ? node.GetString() : "";
                        char buf[1024];
                        std::snprintf(buf, sizeof(buf), "%s", path.c_str());
                        ImGui::SetNextItemWidth(-1.0f);
                        if (ImGui::InputText(f.label.c_str(), buf, sizeof(buf),
                                             ImGuiInputTextFlags_EnterReturnsTrue)) {
                            node = makeStr(buf);
                            changed = true;
                        }
                        const char* payloadKind =
                            f.resourceKind && std::string(f.resourceKind) == "model"
                                ? "ASSET_MODEL"
                                : f.resourceKind && std::string(f.resourceKind) == "script"
                                      ? "ASSET_SCRIPT"
                                      : "ASSET_TEXTURE";
                        if (ImGui::BeginDragDropTarget()) {
                            if (const ImGuiPayload* payload =
                                    ImGui::AcceptDragDropPayload(payloadKind)) {
                                std::string dropped(static_cast<const char*>(payload->Data),
                                                    static_cast<size_t>(payload->DataSize));
                                if (!dropped.empty() && dropped.back() == '\0')
                                    dropped.pop_back();
                                if (!dropped.empty()) {
                                    node = makeStr(dropped);
                                    changed = true;
                                }
                            }
                            ImGui::EndDragDropTarget();
                        }
                        break;
                    }
                    case scene::FieldType::Json:
                        ImGui::TextDisabled("%s: %s", f.label.c_str(),
                                            core::JsonWriter::Write(node).c_str());
                        break;
                }
                if (changed) {
                    history.Push(std::make_unique<EditComponentCommand>(
                        &entities, selected, schema.name, f.key, oldField, node));
                }
            }
        };

        // 生命: built-in component (maxHp > 0 = attached), rendered through the
        // SAME schema editor as data components — the canonical bridge maps the
        // health fields to the flattened hp/maxHp (G5-4-4).
        if (ComponentPresent(e, "health")) {
            const scene::ComponentSchema* schema = scene::FindComponentSchema("health");
            if (schema) {
                const bool hOpen = ImGui::CollapsingHeader(
                    (schema->label + "##health").c_str(), ImGuiTreeNodeFlags_DefaultOpen);
                if (hOpen) {
                    // G5-4-4(项4): read the component from the runtime World
                    // when the entity is hosted there (single representation);
                    // fall back to the editor's flattened fields otherwise.
                    core::Json compData;
                    if (e.id != 0) {
                        if (auto wj = scene::SceneFile::EntityToJson((*ctx.sceneWorld), e.id); wj.Ok()) {
                            if (const core::Json* c = wj.Value().Get("components"))
                                if (const core::Json* h = c->Get("health")) compData = *h;
                        }
                    }
                    if (!compData.IsObject()) compData = ComponentJson(e, "health");
                    renderSchemaFields(*schema, compData);
                    ApplyComponentJson(entities[static_cast<size_t>(selected)], "health",
                                       compData);
                    ImGui::Separator();
                    if (ImGui::Button("移除生命")) {
                        history.Push(std::make_unique<ComponentJsonCommand>(
                            &entities, selected, "health", ComponentJson(e, "health"),
                            core::Json{}));
                    }
                }
            }
        }
        // Script components: ordinary component blocks (schema backend/path/
        // vars), each with its own remove button - exactly like the
        // schema-driven components below. Multiple scripts = multiple blocks.
        {
            const scene::ComponentSchema* scriptSchema =
                scene::FindComponentSchema("script");
            for (size_t si = 0; si < e.scripts.size(); ++si) {
                SceneScriptFields& f = e.scripts[si];
                const std::string header = "脚本##script_" + std::to_string(si);
                const bool open = ImGui::CollapsingHeader(
                    header.c_str(), ImGuiTreeNodeFlags_DefaultOpen);
                if (!open) continue;
                if (!scriptSchema) {
                    ImGui::TextWrapped("%s", core::JsonWriter::Write(f.vars).c_str());
                    continue;
                }
                for (const scene::FieldSchema& fs : scriptSchema->fields) {
                    if (fs.type == scene::FieldType::Enum) {
                        int sel = 0;
                        if (fs.options)
                            for (int o = 0; o < fs.optionCount; ++o)
                                if (f.backend == fs.options[o]) sel = o;
                        if (ImGui::Combo(fs.label.c_str(), &sel, fs.options,
                                         fs.optionCount)) {
                            const ScriptFieldEdit oldV{si, fs.key, makeStr(f.backend)};
                            const ScriptFieldEdit newV{si, fs.key,
                                                       makeStr(fs.options[sel])};
                            history.Push(std::make_unique<
                                EditPropertyCommand<ScriptFieldEdit>>(
                                &entities, selected, ApplyScriptField, oldV, newV,
                                /*mergeable=*/false));
                        }
                    } else if (fs.type == scene::FieldType::Resource) {
                        char buf[1024];
                        std::snprintf(buf, sizeof(buf), "%s", f.path.c_str());
                        ImGui::SetNextItemWidth(-1.0f);
                        if (ImGui::InputText(fs.label.c_str(), buf, sizeof(buf),
                                             ImGuiInputTextFlags_EnterReturnsTrue)) {
                            const ScriptFieldEdit oldV{si, fs.key, makeStr(f.path)};
                            const ScriptFieldEdit newV{si, fs.key, makeStr(buf)};
                            history.Push(std::make_unique<
                                EditPropertyCommand<ScriptFieldEdit>>(
                                &entities, selected, ApplyScriptField, oldV, newV,
                                /*mergeable=*/false));
                        }
                        const char* payloadKind =
                            fs.resourceKind && std::string(fs.resourceKind) == "script"
                                ? "ASSET_SCRIPT"
                                : "ASSET_TEXTURE";
                        if (ImGui::BeginDragDropTarget()) {
                            if (const ImGuiPayload* payload =
                                    ImGui::AcceptDragDropPayload(payloadKind)) {
                                std::string dropped(
                                    static_cast<const char*>(payload->Data),
                                    static_cast<size_t>(payload->DataSize));
                                if (!dropped.empty() && dropped.back() == '\0')
                                    dropped.pop_back();
                                if (!dropped.empty()) {
                                    const ScriptFieldEdit oldV{si, fs.key, makeStr(f.path)};
                                    const ScriptFieldEdit newV{
                                        si, fs.key,
                                        makeStr(ToProjectRelPath(dropped, projectDir))};
                                    history.Push(std::make_unique<
                                        EditPropertyCommand<ScriptFieldEdit>>(
                                        &entities, selected, ApplyScriptField, oldV, newV,
                                        /*mergeable=*/false));
                                }
                            }
                            ImGui::EndDragDropTarget();
                        }
                    } else if (fs.type == scene::FieldType::Json) {
                        ImGui::TextDisabled("%s: %s", fs.label.c_str(),
                                            core::JsonWriter::Write(f.vars).c_str());
                    }
                }
                ImGui::Separator();
                if (ImGui::Button("移除脚本")) {
                    std::vector<SceneScriptFields> newList = e.scripts;
                    newList.erase(newList.begin() + static_cast<ptrdiff_t>(si));
                    history.Push(std::make_unique<
                        EditPropertyCommand<std::vector<SceneScriptFields>>>(
                        &entities, selected, ApplyScriptList, e.scripts, newList,
                        /*mergeable=*/false));
                    break; // the list changed; re-render next frame
                }
            }
            // Dragging a .lua onto the component section mounts a new script.
            if (ImGui::BeginDragDropTarget()) {
                if (const ImGuiPayload* payload =
                        ImGui::AcceptDragDropPayload("ASSET_SCRIPT")) {
                    const char* path = static_cast<const char*>(payload->Data);
                    if (path && selected >= 0 &&
                        selected < static_cast<int>(entities.size())) {
                        std::vector<SceneScriptFields> newList = e.scripts;
                        newList.push_back({"lua", ToProjectRelPath(path, projectDir), {}});
                        history.Push(std::make_unique<
                            EditPropertyCommand<std::vector<SceneScriptFields>>>(
                            &entities, selected, ApplyScriptList, e.scripts, newList,
                            /*mergeable=*/false));
                    }
                }
                ImGui::EndDragDropTarget();
            }
        }
        for (auto it = e.extraComponents.begin(); it != e.extraComponents.end(); ++it) {
            const std::string& compName = it->first;
            core::Json& compData = it->second;
            if (compName == "plant" || compName == "zombie") continue; // 2D canvas edits
            // camera 组件是 nodeType=Camera3D 的序列化载体 (加载时数据并入
            // e.cameraFov 等字段, 播放时再导出回去) — 节点区已展示同一份数据,
            // 组件列表里重复显示会误导 (改一处丢一处)。
            if (compName == "camera" && e.nodeType == "Camera3D") continue;
            // "type" 组件同理: 它存储 nodeType 本身, 已由上方的类型下拉编辑。
            if (compName == "type" && !e.nodeType.empty()) continue;
            const scene::ComponentSchema* schema = scene::FindComponentSchema(compName);
            if (!schema && ctx.pluginMgr) schema = ctx.pluginMgr->FindSchema(compName);
            const std::string header =
                schema ? (schema->label + "##" + compName) : (compName + "##raw");
            const bool open =
                ImGui::CollapsingHeader(header.c_str(), ImGuiTreeNodeFlags_DefaultOpen);
            if (!open) continue;
            if (!schema) {
                ImGui::TextWrapped("%s", core::JsonWriter::Write(compData).c_str());
                continue;
            }
            renderSchemaFields(*schema, compData);
            ImGui::Separator();
            if (ImGui::Button("移除组件")) {
                history.Push(std::make_unique<AddComponentCommand>(
                    &entities, selected, compName, compData, /*remove=*/true));
                break; // the command already removed the component
            }
        }
        // 添加组件: every component type is here - default components that
        // were removed (网格/生命) reappear, scripts are multi-instance, and
        // schema components (刚体/行为树/...) work as before. Transform is
        // mandatory (Unity's Transform) and never listed.
        {
            const auto& allSchemas = scene::AllComponentSchemas();
            std::vector<const scene::ComponentSchema*> addable;
            for (const scene::ComponentSchema& s : allSchemas) {
                if (s.name == "transform" || s.name == "name" ||
                    s.name == "plant" || s.name == "zombie")
                    continue;
                if (s.name == "mesh" && (!e.meshKey.empty() || !e.spriteTex.empty()))
                    continue; // already has a renderer
                if (s.name == "health" && e.maxHp > 0.0f)
                    continue; // already attached
                if (e.extraComponents.count(s.name)) continue; // already present
                addable.push_back(&s);
            }
            // Plugin-registered component schemas (NeonEditor.registerComponent).
            if (ctx.pluginMgr) {
                for (const scene::ComponentSchema& s : ctx.pluginMgr->Schemas()) {
                    if (s.name == "transform" || e.extraComponents.count(s.name)) continue;
                    addable.push_back(&s);
                }
            }
            if (!addable.empty()) {
                if (addCompSel_ >= static_cast<int>(addable.size())) addCompSel_ = 0;
                std::vector<const char*> addLabels;
                for (const scene::ComponentSchema* s : addable)
                    addLabels.push_back(s->label.c_str());
                ImGui::SetNextItemWidth(110.0f);
                ImGui::Combo("##add_component", &addCompSel_, addLabels.data(),
                             static_cast<int>(addLabels.size()));
                ImGui::SameLine();
                if (ImGui::Button("添加组件")) {
                    const scene::ComponentSchema* schema =
                        addable[static_cast<size_t>(addCompSel_)];
                    if (schema->name == "script") {
                        // 新脚本块: 空路径, 在块内编辑 (或从资产面板拖入脚本)。
                        std::vector<SceneScriptFields> newList = e.scripts;
                        newList.push_back({"lua", "", {}});
                        history.Push(std::make_unique<
                            EditPropertyCommand<std::vector<SceneScriptFields>>>(
                            &entities, selected, ApplyScriptList, e.scripts, newList,
                            /*mergeable=*/false));
                    } else if (schema->name == "mesh") {
                        // Re-add the mesh renderer (default cube).
                        history.Push(std::make_unique<EditMeshKeyCommand>(
                            app, &entities, selected, "", "cube"));
                    } else if (schema->name == "health") {
                        const HealthValue oldV{e.hp, e.maxHp};
                        history.Push(std::make_unique<EditPropertyCommand<HealthValue>>(
                            &entities, selected, ApplyHealth, oldV, HealthValue{100, 100},
                            /*mergeable=*/false));
                    } else {
                        core::Json data;
                        data.type_ = core::Json::Type::Object;
                        for (const scene::FieldSchema& f : schema->fields) {
                            switch (f.type) {
                                case scene::FieldType::Number:
                                case scene::FieldType::Int:
                                    data.object_[f.key] = makeNum(f.def);
                                    break;
                                case scene::FieldType::Bool:
                                    data.object_[f.key] = makeBool(f.def != 0.0);
                                    break;
                                case scene::FieldType::Vec3:
                                    data.object_[f.key] = makeArr({f.def, f.def, f.def});
                                    break;
                                case scene::FieldType::Color:
                                    data.object_[f.key] = makeStr("#FFFFFF");
                                    break;
                                case scene::FieldType::Json: {
                                    core::Json o;
                                    o.type_ = core::Json::Type::Object;
                                    data.object_[f.key] = std::move(o);
                                    break;
                                }
                                case scene::FieldType::Enum:
                                    // Default to the first option so enum
                                    // components are valid immediately.
                                    data.object_[f.key] = makeStr(
                                        f.options && f.optionCount > 0 ? f.options[0] : "");
                                    break;
                                default:
                                    data.object_[f.key] = makeStr("");
                                    break;
                            }
                        }
                        history.Push(std::make_unique<AddComponentCommand>(
                            &entities, selected, schema->name, std::move(data),
                            /*remove=*/false));
                    }
                }
            }
        }
        }
    }
    ImGui::End();
}

} // namespace neon::editor
