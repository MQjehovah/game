#include "panels/model_preview_panel.hpp"

// 模型查看器实现 = 原 panels_preview.inc 的四个方法（FrameModelPreview /
// OpenModelPreview / RenderModelPreviewPanel / BuildModelPreviewPanel）
// + 原 EditorApp::OnUpdate 的播放头推进块 + 原 editor_viewport 的预览鼠标块，
// 逐行迁移：EditorApp 成员（preview_/assetMgr_/renderer_/showModelPreview_）
// 改本类成员 / 构造注入的指针。行为零变化。

#include "neon/anim/anim.hpp"
#include "neon/assets/asset_manager.hpp"
#include "neon/assets/asset_path.hpp"
#include "neon/assets/mesh_format.hpp"
#include "neon/core/log.hpp"
#include "neon/gfx/imgui_neon.hpp"
#include "neon/platform/input.hpp"
#include "neon/scene/skinned_model.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace neon::editor {

void ModelPreviewPanel::FrameModelPreview() {
    math::AABB b;
    bool first = true;
    if (model) {
        for (const scene::SkinnedModel::Part& part : model->parts) {
            const math::AABB pb =
                math::TransformAABB(part.mesh.Bounds(), part.localTransform);
            if (first) {
                b = pb;
                first = false;
            } else {
                b.min = {std::min(b.min.x, pb.min.x), std::min(b.min.y, pb.min.y),
                         std::min(b.min.z, pb.min.z)};
                b.max = {std::max(b.max.x, pb.max.x), std::max(b.max.y, pb.max.y),
                         std::max(b.max.z, pb.max.z)};
            }
        }
    }
    if (first) b = {{-0.5f, -0.5f, -0.5f}, {0.5f, 0.5f, 0.5f}};
    const float dx = b.max.x - b.min.x;
    const float dy = b.max.y - b.min.y;
    const float dz = b.max.z - b.min.z;
    if (dy <= dx && dy <= dz) { // flattest in Y -> top view
        yaw = 0.0f;
        pitch = math::kPi * 0.5f;
    } else if (dx <= dy && dx <= dz) { // flattest in X -> look along +X
        yaw = math::kPi * 0.5f;
        pitch = 0.0f;
    } else { // flattest in Z (or balanced) -> look along +Z
        yaw = 0.0f;
        pitch = 0.0f;
    }
}

void ModelPreviewPanel::Open(const std::string& pathIn) {
    std::string p = pathIn;
    const bool isFbx = p.rfind("fbx:", 0) == 0 || p.size() >= 4 &&
                       (p.compare(p.size() - 4, 4, ".fbx") == 0 ||
                        p.compare(p.size() - 4, 4, ".FBX") == 0);
    if (p.rfind("gltf:", 0) == 0) p = p.substr(5);
    if (p.rfind("fbx:", 0) == 0) p = p.substr(4);
    if (p.empty()) return;

    // FBX/OBJ static models have no skeleton/clips: build a single-part,
    // skeleton-less SkinnedModel so the preview's "static (non-skinned)" path
    // (Render, bones.empty()) draws it through the lit shader.
    if (isFbx) {
        assets::FbxAsset fbx = assetMgr_->LoadFBX(assets::NormalizeAssetPath(p));
        if (fbx.nodes.empty()) {
            NEON_LOG_ERROR("Model preview: failed to load FBX '%s'", p.c_str());
            return;
        }
        auto m = std::make_shared<scene::SkinnedModel>();
        for (assets::FbxMeshNode& n : fbx.nodes) {
            scene::SkinnedModel::Part part;
            part.mesh = std::move(n.mesh);
            part.material = std::move(n.material);
            part.localTransform = math::Mat4::Identity();
            m->parts.push_back(std::move(part));
        }
        model = m;
        path = p;
        playing = true;
        time = 0.0f;
        clip = -1;
        FrameModelPreview();
        NEON_LOG_INFO("Model preview (FBX): '%s' (%zu parts)", p.c_str(), m->parts.size());
        return;
    }

    core::Result<scene::SkinnedModel> sm = scene::LoadSkinnedModel(*assetMgr_, p);
    if (sm.Ok()) {
        model = std::make_shared<scene::SkinnedModel>(std::move(sm.Value()));
        path = p;
        playing = true;
        time = 0.0f;
        clip = model->clips.empty() ? -1
                        : model->defaultClip >= 0 ? model->defaultClip
                                                        : 0;
        // 自动初始视角: 相机正对模型最大面积面 (沿最小维度轴方向观察)。
        // 薄片模型 (banner/墙/栅栏等 Kenney 建筑) 默认斜视角以边缘示人,
        // 几乎不可见; 从最大面正对看就能看清。
        FrameModelPreview();
        NEON_LOG_INFO("Model preview: '%s' (%zu parts, %zu clips, %zu bones)", p.c_str(),
                      model->parts.size(), model->clips.size(),
                      model->skeleton.bones.size());
    } else {
        NEON_LOG_ERROR("Model preview: failed to load '%s': %s", p.c_str(),
                       sm.Error().c_str());
    }
}

void ModelPreviewPanel::Render() {
    if (!visible_ || !*visible_ || !model) return;
    gfx::IRenderBackend* b = renderer_->Backend();
    if (!b) return;
    const int w = static_cast<int>(screenRect.w);
    const int h = static_cast<int>(screenRect.h);
    if (w < 8 || h < 8) return;

    if (!rt.Valid() || rtW != w || rtH != h) {
        if (rt.Valid()) b->DestroyRenderTarget(rt);
        if (rtId != ImTextureID_Invalid) {
            gfx::ImGuiNeon_UnregisterTexture(rtColor);
            rtId = ImTextureID_Invalid;
        }
        rt = b->CreateRenderTarget(w, h, true, 0);
        rtColor = b->RenderTargetColorTexture(rt);
        rtW = w;
        rtH = h;
        if (rtColor.Valid())
            rtId = gfx::ImGuiNeon_RegisterTexture(rtColor);
    }
    if (!rt.Valid()) return;

    b->BindRenderTarget(rt);
    b->Clear({0.30f, 0.34f, 0.40f, 1.0f}, 1.0f);

    math::AABB bounds;
    bool first = true;
    for (const scene::SkinnedModel::Part& part : model->parts) {
        // Use the WORLD-space bounds (mesh local bounds transformed by the
        // node transform): static models (Kenney .glb) often place their mesh
        // off the root (a translation/scale on the node), so framing against
        // the raw local bounds would aim the camera at empty space.
        const math::AABB pb =
            math::TransformAABB(part.mesh.Bounds(), part.localTransform);
        if (first) {
            bounds = pb;
            first = false;
        } else {
            bounds.min = {std::min(bounds.min.x, pb.min.x), std::min(bounds.min.y, pb.min.y),
                          std::min(bounds.min.z, pb.min.z)};
            bounds.max = {std::max(bounds.max.x, pb.max.x), std::max(bounds.max.y, pb.max.y),
                          std::max(bounds.max.z, pb.max.z)};
        }
    }
    const math::Vec3 center = (bounds.min + bounds.max) * 0.5f;
    const float size =
        std::max({bounds.max.x - bounds.min.x, bounds.max.y - bounds.min.y,
                  bounds.max.z - bounds.min.z, 0.5f});
    gfx::Camera pcam;
    pcam.position = center +
                    math::Vec3{std::sin(yaw) * std::cos(pitch),
                               std::sin(pitch),
                               std::cos(yaw) * std::cos(pitch)} *
                        size * 2.6f;
    pcam.target = center;
    renderer_->SetCamera(pcam, static_cast<float>(w) / static_cast<float>(h));
    // SetCamera's CSM shadow pass may rebind the HDR/default target and reset
    // the viewport to the main scene rect (when the main viewport did not run
    // its own SetCamera this frame, e.g. it is covered or hidden). That would
    // silently steal our preview FBO - the model would draw into the main
    // window and the preview stays blank. Re-assert the FBO + full size here.
    b->BindRenderTarget(rt);
    b->SetViewport(0, 0, w, h);
    b->SetScissor(0, 0, w, h, true);
    renderer_->SetDirectionalLight({-0.4f, -1.0f, -0.3f}, {1.0f, 0.95f, 0.9f}, 0.5f);
    anim::Pose pose = model->skeleton.BindPose();
    if (clip >= 0 && clip < static_cast<int>(model->clips.size())) {
        const anim::AnimationClip& c = model->clips[static_cast<size_t>(clip)];
        c.Sample(time, pose);
    }
    const std::vector<math::Mat4> bones =
        model->skeleton.ComputeBoneMatrices(pose);
    if (bones.empty()) {
        // Static (non-skinned) model: no skeleton to feed the skinned shader;
        // draw each part through the regular lit path.
        for (const scene::SkinnedModel::Part& part : model->parts)
            renderer_->DrawMesh(part.mesh, part.material, part.localTransform);
    } else {
        // Skinned: bones already map mesh-local vertices to world space
        // (globalJoint * inverseBind per the glTF skin spec). A mesh node's own
        // transform is NOT applied to a skinned mesh (it is baked into the
        // inverseBind), so multiplying part.localTransform would double-apply
        // it — CesiumMan's Z_UP root flips the rig (lying down). Identity here.
        for (const scene::SkinnedModel::Part& part : model->parts)
            renderer_->DrawSkinnedMesh(part.mesh, part.material, math::Mat4::Identity(), bones,
                                       static_cast<int>(bones.size()));
    }
    b->BindDefaultTarget();
    // The preview enabled a full-FBO scissor above (shadow-pass state can
    // bleed stale scissor/viewport). Disable it so the next frame's BeginFrame
    // clear and the EndFrame composite are not cropped to the preview rect
    // (which otherwise blanks the main viewport's models).
    b->SetScissor(0, 0, 0, 0, false);
}

void ModelPreviewPanel::Tick(float dt) {
    // 原 EditorApp::OnUpdate 的播放头推进块。
    if (!visible_ || !*visible_ || !model || !playing) return;
    float dur = model->clips.empty()
                    ? 1.0f
                    : model->clips[static_cast<size_t>(clip)].duration;
    if (dur > 0.0f) time = std::fmod(time + dt, dur);
}

bool ModelPreviewPanel::HandleViewportMouse(const platform::IInput& in) {
    // 原 editor_viewport UpdateViewport 的 preview 块：鼠标悬停预览区时右键
    // 拖拽旋转预览相机，并让调用方提前 return（主视口相机不驱动）。
    if (!visible_ || !*visible_ || !model || screenRect.w <= 0.0f) return false;
    const math::Vec2 mpx = in.MousePos();
    const bool overPreview =
        mpx.x >= screenRect.x && mpx.x <= screenRect.x + screenRect.w &&
        mpx.y >= screenRect.y && mpx.y <= screenRect.y + screenRect.h;
    if (!overPreview) return false;
    if (in.MouseDown(platform::MouseButton::Right)) {
        yaw += -in.MouseDelta().x * 0.005f;
        pitch = math::Clamp(pitch + -in.MouseDelta().y * 0.005f, -1.4f, 1.4f);
    }
    return true;
}

void ModelPreviewPanel::Draw(EditorContext&) {
    if (!visible_ || !*visible_) return;
    ImGui::SetNextWindowSize(ImVec2(320.0f, 420.0f), ImGuiCond_FirstUseEver);
    if (ImGui::Begin("模型查看器", visible_)) {
        // 窗口级拖放目标: 放在最前面, 保证"未打开模型"的提前 return 之前也
        // 生效 (之前目标在 EndChild 之后, 空状态根本执行不到 → 拖拽没反应)。
        // 资产面板拖入模型文件直接预览 (与资产面板的拖拽源 ASSET_MODEL 对接)。
        // 注意: 查看器的 LoadSkinnedModel 直读文件 (无项目前缀解析), 所以用
        // 资产面板的原始 CWD 相对路径 (projects/<p>/assets/...), 不要转项目相对。
        if (ImGui::BeginDragDropTarget()) {
            if (const ImGuiPayload* p = ImGui::AcceptDragDropPayload("ASSET_MODEL")) {
                const char* pathC = static_cast<const char*>(p->Data);
                NEON_LOG_INFO("Model preview: drop accepted '%s'", pathC ? pathC : "");
                if (pathC && *pathC) {
                    if (!assets::MeshFormatRegistry::Instance().FormatFromExt(pathC).empty()) {
                        std::snprintf(pathBuf, sizeof(pathBuf), "%s", pathC);
                        Open(pathBuf);
                    }
                }
            }
            ImGui::EndDragDropTarget();
        }
        ImGui::SetNextItemWidth(320.0f);
        ImGui::InputText("路径", pathBuf, sizeof(pathBuf));
        ImGui::SameLine();
        if (ImGui::Button("打开")) Open(pathBuf);
        ImGui::Separator();

        if (!model) {
            ImGui::TextDisabled("未打开模型 — 输入 .gltf 路径后点\"打开\"（右键拖拽旋转）");
            ImGui::End();
            return;
        }

        ImGui::Text("%s", path.c_str());
        ImGui::TextDisabled("%zu 部件 | %zu 骨骼 | %zu 动画",
                            model->parts.size(), model->skeleton.bones.size(),
                            model->clips.size());
        if (!model->clips.empty()) {
            std::vector<const char*> names;
            for (const anim::AnimationClip& c : model->clips) names.push_back(c.name.c_str());
            if (clip < 0 || clip >= static_cast<int>(names.size())) clip = 0;
            ImGui::SetNextItemWidth(220.0f);
            if (ImGui::Combo("动画", &clip, names.data(),
                             static_cast<int>(names.size()))) {
                time = 0.0f;
            }
            ImGui::SameLine();
            if (ImGui::Button(playing ? "暂停" : "播放")) playing = !playing;
            float t = time;
            if (ImGui::SliderFloat("时间", &t, 0.0f,
                                   std::max(0.01f, model->clips[static_cast<size_t>(clip)].duration))) {
                time = t;
            }
        }
        if (ImGui::Button("重置视角")) {
            yaw = 0.6f;
            pitch = 0.3f;
        }
        ImGui::SameLine();
        ImGui::TextDisabled("预览区右键拖拽旋转");

        // Preview area fills the panel's remaining space. Its screen rect is
        // consumed by Render (drawn after the main scene so it coexists with
        // the edit/play viewport). Height reduced by a few px so the
        // border+padding never overflows the parent window and re-triggers its
        // scrollbar.
        ImGui::BeginChild("##preview_area", ImVec2(0.0f, -6.0f), ImGuiChildFlags_Borders,
                          ImGuiWindowFlags_NoScrollbar);
        const ImVec2 pos = ImGui::GetWindowPos();
        const ImVec2 sz = ImGui::GetWindowSize();
        screenRect = {pos.x, pos.y, sz.x, sz.y};
        if (rtId != ImTextureID_Invalid)
            // FBO color texture is bottom-left origin; flip V so it displays
            // upright in ImGui (Image assumes top-left).
            ImGui::Image(rtId, ImVec2(sz.x, sz.y), ImVec2(0.0f, 1.0f),
                         ImVec2(1.0f, 0.0f));
        // 预览区子窗口也接收拖放 (拖到已渲染的模型画面上也能换模型)。
        if (ImGui::BeginDragDropTarget()) {
            if (const ImGuiPayload* p = ImGui::AcceptDragDropPayload("ASSET_MODEL")) {
                const char* pathC = static_cast<const char*>(p->Data);
                NEON_LOG_INFO("Model preview: drop accepted '%s'", pathC ? pathC : "");
                if (pathC && *pathC) {
                    if (!assets::MeshFormatRegistry::Instance().FormatFromExt(pathC).empty()) {
                        std::snprintf(pathBuf, sizeof(pathBuf), "%s", pathC);
                        Open(pathBuf);
                    }
                }
            }
            ImGui::EndDragDropTarget();
        }
        ImGui::EndChild();
    }
    ImGui::End();
}

} // namespace neon::editor
