void EditorApp::FrameModelPreview() {
    math::AABB b;
    bool first = true;
    if (preview_.model) {
        for (const scene::SkinnedModel::Part& part : preview_.model->parts) {
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
        preview_.yaw = 0.0f;
        preview_.pitch = math::kPi * 0.5f;
    } else if (dx <= dy && dx <= dz) { // flattest in X -> look along +X
        preview_.yaw = math::kPi * 0.5f;
        preview_.pitch = 0.0f;
    } else { // flattest in Z (or balanced) -> look along +Z
        preview_.yaw = 0.0f;
        preview_.pitch = 0.0f;
    }
}

void EditorApp::OpenModelPreview(const std::string& path) {
    std::string p = path;
    const bool isFbx = p.rfind("fbx:", 0) == 0 || p.size() >= 4 &&
                       (p.compare(p.size() - 4, 4, ".fbx") == 0 ||
                        p.compare(p.size() - 4, 4, ".FBX") == 0);
    if (p.rfind("gltf:", 0) == 0) p = p.substr(5);
    if (p.rfind("fbx:", 0) == 0) p = p.substr(4);
    if (p.empty()) return;

    // FBX/OBJ static models have no skeleton/clips: build a single-part,
    // skeleton-less SkinnedModel so the preview's "static (non-skinned)" path
    // (RenderModelPreviewPanel, bones.empty()) draws it through the lit shader.
    if (isFbx) {
        assets::FbxAsset fbx = assetMgr_.LoadFBX(assets::NormalizeAssetPath(p));
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
        preview_.model = m;
        preview_.path = p;
        preview_.playing = true;
        preview_.time = 0.0f;
        preview_.clip = -1;
        FrameModelPreview();
        NEON_LOG_INFO("Model preview (FBX): '%s' (%zu parts)", p.c_str(), m->parts.size());
        return;
    }

    core::Result<scene::SkinnedModel> sm = scene::LoadSkinnedModel(assetMgr_, p);
    if (sm.Ok()) {
        preview_.model = std::make_shared<scene::SkinnedModel>(std::move(sm.Value()));
        preview_.path = p;
        preview_.playing = true;
        preview_.time = 0.0f;
        preview_.clip = preview_.model->clips.empty() ? -1
                        : preview_.model->defaultClip >= 0 ? preview_.model->defaultClip
                                                          : 0;
        // 自动初始视角: 相机正对模型最大面积面 (沿最小维度轴方向观察)。
        // 薄片模型 (banner/墙/栅栏等 Kenney 建筑) 默认斜视角以边缘示人,
        // 几乎不可见; 从最大面正对看就能看清。
        FrameModelPreview();
        NEON_LOG_INFO("Model preview: '%s' (%zu parts, %zu clips, %zu bones)", p.c_str(),
                      preview_.model->parts.size(), preview_.model->clips.size(),
                      preview_.model->skeleton.bones.size());
    } else {
        NEON_LOG_ERROR("Model preview: failed to load '%s': %s", p.c_str(),
                       sm.Error().c_str());
    }
}

void EditorApp::RenderModelPreviewPanel() {
    if (!showModelPreview_ || !preview_.model) return;
    gfx::IRenderBackend* b = renderer_.Backend();
    if (!b) return;
    const int w = static_cast<int>(preview_.screenRect.w);
    const int h = static_cast<int>(preview_.screenRect.h);
    if (w < 8 || h < 8) return;

    if (!preview_.rt.Valid() || preview_.rtW != w || preview_.rtH != h) {
        if (preview_.rt.Valid()) b->DestroyRenderTarget(preview_.rt);
        if (preview_.rtId != ImTextureID_Invalid) {
            gfx::ImGuiNeon_UnregisterTexture(preview_.rtColor);
            preview_.rtId = ImTextureID_Invalid;
        }
        preview_.rt = b->CreateRenderTarget(w, h, true, 0);
        preview_.rtColor = b->RenderTargetColorTexture(preview_.rt);
        preview_.rtW = w;
        preview_.rtH = h;
        if (preview_.rtColor.Valid())
            preview_.rtId = gfx::ImGuiNeon_RegisterTexture(preview_.rtColor);
    }
    if (!preview_.rt.Valid()) return;

    b->BindRenderTarget(preview_.rt);
    b->Clear({0.30f, 0.34f, 0.40f, 1.0f}, 1.0f);

    math::AABB bounds;
    bool first = true;
    for (const scene::SkinnedModel::Part& part : preview_.model->parts) {
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
                    math::Vec3{std::sin(preview_.yaw) * std::cos(preview_.pitch),
                               std::sin(preview_.pitch),
                               std::cos(preview_.yaw) * std::cos(preview_.pitch)} *
                        size * 2.6f;
    pcam.target = center;
    renderer_.SetCamera(pcam, static_cast<float>(w) / static_cast<float>(h));
    // SetCamera's CSM shadow pass may rebind the HDR/default target and reset
    // the viewport to the main scene rect (when the main viewport did not run
    // its own SetCamera this frame, e.g. it is covered or hidden). That would
    // silently steal our preview FBO - the model would draw into the main
    // window and the preview stays blank. Re-assert the FBO + full size here.
    b->BindRenderTarget(preview_.rt);
    b->SetViewport(0, 0, w, h);
    b->SetScissor(0, 0, w, h, true);
    renderer_.SetDirectionalLight({-0.4f, -1.0f, -0.3f}, {1.0f, 0.95f, 0.9f}, 0.5f);
    anim::Pose pose = preview_.model->skeleton.BindPose();
    if (preview_.clip >= 0 &&
        preview_.clip < static_cast<int>(preview_.model->clips.size())) {
        const anim::AnimationClip& clip =
            preview_.model->clips[static_cast<size_t>(preview_.clip)];
        clip.Sample(preview_.time, pose);
    }
    const std::vector<math::Mat4> bones =
        preview_.model->skeleton.ComputeBoneMatrices(pose);
    if (bones.empty()) {
        // Static (non-skinned) model: no skeleton to feed the skinned shader;
        // draw each part through the regular lit path.
        for (const scene::SkinnedModel::Part& part : preview_.model->parts)
            renderer_.DrawMesh(part.mesh, part.material, part.localTransform);
    } else {
        // Skinned: bones already map mesh-local vertices to world space
        // (globalJoint * inverseBind per the glTF skin spec). A mesh node's own
        // transform is NOT applied to a skinned mesh (it is baked into the
        // inverseBind), so multiplying part.localTransform would double-apply
        // it — CesiumMan's Z_UP root flips the rig (lying down). Identity here.
        for (const scene::SkinnedModel::Part& part : preview_.model->parts)
            renderer_.DrawSkinnedMesh(part.mesh, part.material, math::Mat4::Identity(), bones,
                                      static_cast<int>(bones.size()));
    }
    b->BindDefaultTarget();
    // The preview enabled a full-FBO scissor above (shadow-pass state can
    // bleed stale scissor/viewport). Disable it so the next frame's BeginFrame
    // clear and the EndFrame composite are not cropped to the preview rect
    // (which otherwise blanks the main viewport's models).
    b->SetScissor(0, 0, 0, 0, false);
}

void EditorApp::BuildModelPreviewPanel() {
    if (!showModelPreview_) return;
    ImGui::SetNextWindowSize(ImVec2(320.0f, 420.0f), ImGuiCond_FirstUseEver);
    if (ImGui::Begin("模型查看器", &showModelPreview_)) {
        // 窗口级拖放目标: 放在最前面, 保证"未打开模型"的提前 return 之前也
        // 生效 (之前目标在 EndChild 之后, 空状态根本执行不到 → 拖拽没反应)。
        // 资产面板拖入模型文件直接预览 (与资产面板的拖拽源 ASSET_MODEL 对接)。
        // 注意: 查看器的 LoadSkinnedModel 直读文件 (无项目前缀解析), 所以用
        // 资产面板的原始 CWD 相对路径 (projects/<p>/assets/...), 不要转项目相对。
        if (ImGui::BeginDragDropTarget()) {
            if (const ImGuiPayload* p = ImGui::AcceptDragDropPayload("ASSET_MODEL")) {
                const char* path = static_cast<const char*>(p->Data);
                NEON_LOG_INFO("Model preview: drop accepted '%s'", path ? path : "");
                if (path && *path) {
                    if (!assets::MeshFormatRegistry::Instance().FormatFromExt(path).empty()) {
                        std::snprintf(preview_.pathBuf, sizeof(preview_.pathBuf), "%s", path);
                        OpenModelPreview(preview_.pathBuf);
                    }
                }
            }
            ImGui::EndDragDropTarget();
        }
        ImGui::SetNextItemWidth(320.0f);
        ImGui::InputText("路径", preview_.pathBuf, sizeof(preview_.pathBuf));
        ImGui::SameLine();
        if (ImGui::Button("打开")) OpenModelPreview(preview_.pathBuf);
        ImGui::Separator();

        if (!preview_.model) {
            ImGui::TextDisabled("未打开模型 — 输入 .gltf 路径后点\"打开\"（右键拖拽旋转）");
            ImGui::End();
            return;
        }

        ImGui::Text("%s", preview_.path.c_str());
        ImGui::TextDisabled("%zu 部件 | %zu 骨骼 | %zu 动画",
                            preview_.model->parts.size(), preview_.model->skeleton.bones.size(),
                            preview_.model->clips.size());
        if (!preview_.model->clips.empty()) {
            std::vector<const char*> names;
            for (const anim::AnimationClip& c : preview_.model->clips) names.push_back(c.name.c_str());
            if (preview_.clip < 0 || preview_.clip >= static_cast<int>(names.size()))
                preview_.clip = 0;
            ImGui::SetNextItemWidth(220.0f);
            if (ImGui::Combo("动画", &preview_.clip, names.data(),
                             static_cast<int>(names.size()))) {
                preview_.time = 0.0f;
            }
            ImGui::SameLine();
            if (ImGui::Button(preview_.playing ? "暂停" : "播放")) preview_.playing = !preview_.playing;
            float t = preview_.time;
            if (ImGui::SliderFloat("时间", &t, 0.0f,
                                   std::max(0.01f, preview_.model->clips[static_cast<size_t>(preview_.clip)].duration))) {
                preview_.time = t;
            }
        }
        if (ImGui::Button("重置视角")) {
            preview_.yaw = 0.6f;
            preview_.pitch = 0.3f;
        }
        ImGui::SameLine();
        ImGui::TextDisabled("预览区右键拖拽旋转");

        // Preview area fills the panel's remaining space. Its screen rect is
        // consumed by RenderModelPreviewPanel (drawn after the main scene so
        // it coexists with the edit/play viewport). Height reduced by a few
        // px so the border+padding never overflows the parent window and
        // re-triggers its scrollbar.
        ImGui::BeginChild("##preview_area", ImVec2(0.0f, -6.0f), ImGuiChildFlags_Borders,
                          ImGuiWindowFlags_NoScrollbar);
        const ImVec2 pos = ImGui::GetWindowPos();
        const ImVec2 sz = ImGui::GetWindowSize();
        preview_.screenRect = {pos.x, pos.y, sz.x, sz.y};
        if (preview_.rtId != ImTextureID_Invalid)
            // FBO color texture is bottom-left origin; flip V so it displays
            // upright in ImGui (Image assumes top-left).
            ImGui::Image(preview_.rtId, ImVec2(sz.x, sz.y), ImVec2(0.0f, 1.0f),
                         ImVec2(1.0f, 0.0f));
        // 预览区子窗口也接收拖放 (拖到已渲染的模型画面上也能换模型)。
        if (ImGui::BeginDragDropTarget()) {
            if (const ImGuiPayload* p = ImGui::AcceptDragDropPayload("ASSET_MODEL")) {
                const char* path = static_cast<const char*>(p->Data);
                NEON_LOG_INFO("Model preview: drop accepted '%s'", path ? path : "");
                if (path && *path) {
                    if (!assets::MeshFormatRegistry::Instance().FormatFromExt(path).empty()) {
                        std::snprintf(preview_.pathBuf, sizeof(preview_.pathBuf), "%s", path);
                        OpenModelPreview(preview_.pathBuf);
                    }
                }
            }
            ImGui::EndDragDropTarget();
        }
        ImGui::EndChild();
    }
    ImGui::End();
}
