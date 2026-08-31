void EditorApp::BuildLogPanel() {
    if (!showLog_) return;
    logEntries_ = core::GetRecentLogs(800);
    if (ImGui::Begin("日志", &showLog_)) {
        const char* filters[] = {"全部", "INFO+", "WARN+", "ERROR"};
        ImGui::SetNextItemWidth(80.0f);
        ImGui::Combo("级别", &logFilter_, filters, 4);
        ImGui::SameLine();
        ImGui::Checkbox("自动滚动", &logAutoScroll_);
        ImGui::SameLine();
        if (ImGui::Button("清空")) core::ClearLogs();
        ImGui::Separator();

        static bool wasAtBottom = true;
        ImGui::BeginChild("##log_list", ImVec2(0, 0), ImGuiChildFlags_Borders);
        const ImVec4 colors[4] = {
            ImVec4(0.55f, 0.58f, 0.65f, 1.0f), // debug
            ImVec4(0.90f, 0.95f, 1.00f, 1.0f), // info
            ImVec4(1.00f, 0.85f, 0.35f, 1.0f), // warn
            ImVec4(1.00f, 0.40f, 0.35f, 1.0f), // error
        };
        size_t shown = 0;
        for (const core::LogEntry& entry : logEntries_) {
            if (logFilter_ == 1 && entry.level < core::LogLevel::Info) continue;
            if (logFilter_ == 2 && entry.level < core::LogLevel::Warn) continue;
            if (logFilter_ == 3 && entry.level < core::LogLevel::Error) continue;
            ImGui::PushStyleColor(ImGuiCol_Text,
                                  colors[static_cast<int>(entry.level)]);
            ImGui::TextWrapped("%s", entry.text.c_str());
            ImGui::PopStyleColor();
            ++shown;
        }
        if (logAutoScroll_ && wasAtBottom && shown > 0) ImGui::SetScrollHereY(1.0f);
        wasAtBottom = ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 4.0f;
        ImGui::EndChild();
    }
    ImGui::End();
}

void EditorApp::BuildProfilerPanel() {
    if (!showProfiler_) return;
    if (ImGui::Begin("性能", &showProfiler_)) {
        const float ms = TimeRef().delta * 1000.0f;
        const gfx::Renderer::RenderStats& st = renderer_.Stats();
        profilerMs_[profilerMsHead_] = ms;
        profilerMsHead_ = (profilerMsHead_ + 1) % kProfilerSamples;

        ImGui::Text("帧时间 %.2f ms | %.1f FPS", ms, TimeRef().Fps());
        ImGui::Text("Draw 调用 %u | 三角形 %u | 实例 %u", st.drawCalls, st.triangles,
                    st.instances);

        const bool playing = playActive_ && play_ != nullptr;
        const size_t sceneEnts = entities_.size();
        const size_t playEnts = playing ? play_->EntityCount() : 0;
        const size_t bodies = playing ? play_->PhysicsBodyCount() : 0;
        const size_t trees = playing ? play_->BehaviorTreeCount() : 0;
        const size_t scripts = playing ? play_->ScriptCount() : 0;
        ImGui::Text("实体 %zu (播放 %zu) | 物理刚体 %zu", sceneEnts, playEnts, bodies);
        ImGui::Text("行为树 %zu | 脚本 %zu", trees, scripts);

        const assets::AssetStats a = assetMgr_.Stats();
        ImGui::Text("资产 %zu | 网格 %zu | 三角形 %zu", a.textures, a.meshes, a.meshTriangles);
        ImGui::Text("纹理内存 %.2f MB",
                    static_cast<double>(a.textureBytes) / (1024.0 * 1024.0));

        // G6-3: global heap tracking (operator new/delete overrides).
        const core::MemStats::Snapshot heap = core::MemStats::SnapshotNow();
        ImGui::Text("堆: 存活 %.2f MB | 峰值 %.2f MB | 分配 %llu 次",
                    static_cast<double>(heap.liveBytes) / (1024.0 * 1024.0),
                    static_cast<double>(heap.peakLiveBytes) / (1024.0 * 1024.0),
                    static_cast<unsigned long long>(heap.allocCount));

        // G6-1: driver-reported GPU memory budget/usage (0 = not reported).
        const gfx::IRenderBackend::GpuMemStats gpu = renderer_.GpuMemory();
        if (gpu.totalBytes > 0) {
            ImGui::Text("GPU 显存: 总 %.2f GB | 已用 %.2f MB",
                        static_cast<double>(gpu.totalBytes) / (1024.0 * 1024.0 * 1024.0),
                        static_cast<double>(gpu.usedBytes) / (1024.0 * 1024.0));
        } else {
            ImGui::Text("GPU 显存: 驱动未报告 (GL_NVX_gpu_memory_info / GL_ATI_meminfo 不可用)");
        }

        ImGui::Separator();
        // Plot the ring buffer in chronological order (oldest = profilerMsHead_)
        // so the graph reads left-to-right instead of jumping when the head
        // wraps around the fixed array.
        float wrapped[kProfilerSamples];
        for (int i = 0; i < kProfilerSamples; ++i)
            wrapped[i] = profilerMs_[(profilerMsHead_ + i) % kProfilerSamples];
        ImGui::PlotLines("##frame_ms", wrapped, kProfilerSamples, 0, "帧时间 (ms)",
                         0.0f, 40.0f, ImVec2(-1.0f, 88.0f));
        profilerDrawn_ = true;
    }
    ImGui::End();
}

void EditorApp::BuildViewportPanel() {
    ImGuiWindowFlags vpFlags = ImGuiWindowFlags_NoScrollbar |
                               ImGuiWindowFlags_NoScrollWithMouse |
                               ImGuiWindowFlags_NoCollapse |
                               ImGuiWindowFlags_NoBackground;
    // NOTE: deliberately NOT ImGuiWindowFlags_NoInputs. That flag disables
    // manual moving/resizing (imgui.cpp:7938), so the viewport could never be
    // undocked or re-docked elsewhere. The 3D camera reads the platform input
    // directly; ImGui just sees a normal (backgroundless) docked panel.
    if (ImGui::Begin("视口", nullptr, vpFlags)) {
        // The viewport is a normal dockable panel defaulting to the central
        // node. The user's saved layout (with a DockId) is always restored as
        //-is; only when the DockId was LOST (a previous session left it
        // floating) do we fall back to docking it in the center, on its first
        // frame - so an intentional mid-session undock is never yanked back.
        if (!viewportDockFallbackDone_) {
            viewportDockFallbackDone_ = true;
            ImGuiWindow* win = ImGui::GetCurrentWindow();
            if (dockspaceId_ && !win->DockId && !win->DockIsActive) {
                if (ImGuiDockNode* central = ImGui::DockBuilderGetCentralNode(dockspaceId_)) {
                    ImGui::DockBuilderDockWindow("视口", central->ID);
                    NEON_LOG_INFO("Editor: viewport DockId was lost; re-docked to the central node");
                }
            }
        }
        ImVec2 pos = ImGui::GetWindowPos();
        // GetWindowPos returns the OUTER frame (the dock tab bar included).
        // The design space must map into the VISIBLE content area instead, or
        // the game HUD's top rows end up hidden behind the tab bar.
        ImVec2 contentMin = ImGui::GetCursorScreenPos();
        ImVec2 contentMaxLocal = ImGui::GetContentRegionMax();
        const math::Rect2 contentRect{
            contentMin.x, contentMin.y,
            (pos.x + contentMaxLocal.x) - contentMin.x,
            (pos.y + contentMaxLocal.y) - contentMin.y};
        math::Vec2 uiPos = renderer_.ScreenToUI({contentRect.x, contentRect.y});
        float scale = renderer_.UIScale();
        viewportRect_ = {uiPos.x, uiPos.y, contentRect.w / scale, contentRect.h / scale};
        viewportScreenRect_ = contentRect;

        // Play: the game's own view fills the viewport; the editor hint rows
        // (camera help + scene stats) would draw on top of it - hide both.
        if (!playActive_) {
            ImGui::TextColored(ImVec4(0.65f, 0.85f, 1.0f, 1.0f),
                               "右键旋转 | 中键平移 | 滚轮缩放 | 左键拾取");
            const char* camLabel = viewCam_ == ViewCam::Top
                                   ? "顶视 (正交)"
                                   : viewCam_ == ViewCam::Front ? "前视 (正交)" : "透视";
            std::string physInfo;
            if (play_ && play_->PhysicsBodyCount() > 0)
                physInfo = " | 物理 " + std::to_string(play_->PhysicsBodyCount());
            ImGui::TextDisabled("%s | 实体 %zu%s | 目标 (%.1f, %.1f, %.1f) | 距离 %.1f", camLabel,
                                entities_.size(), physInfo.c_str(), camTarget_.x, camTarget_.y,
                                camTarget_.z, camDist_);
        }
        // Transform gizmo for the selected entity (drawn into this window's
        // draw list; interacts via ImGui's mouse state).
        DrawTransformGizmo();
        // 模型拖到 3D 视口 → 打开模型查看器 (资产面板拖拽源的 ASSET_MODEL)。
        // 注意: 层级面板已用 ASSET_MODEL 添加实体, 这里只做预览兜底, 避免
        // 用户拖到视口没反应的困惑。
        if (ImGui::BeginDragDropTarget()) {
            if (const ImGuiPayload* p = ImGui::AcceptDragDropPayload("ASSET_MODEL")) {
                const char* path = static_cast<const char*>(p->Data);
                NEON_LOG_INFO("Viewport: model dropped '%s'", path ? path : "");
                if (path && *path) {
                    showModelPreview_ = true;
                    std::snprintf(previewPathBuf_, sizeof(previewPathBuf_), "%s", path);
                    OpenModelPreview(previewPathBuf_);
                }
            }
            // 资产面板的预置体 (assets/prefabs/*.json) 拖到 3D 视口 → 生成实例。
            if (const ImGuiPayload* p = ImGui::AcceptDragDropPayload("ASSET_PREFAB")) {
                const char* path = static_cast<const char*>(p->Data);
                NEON_LOG_INFO("Viewport: prefab dropped '%s'", path ? path : "");
                if (path && *path) {
                    std::string nm = FileName(path);
                    const size_t dot = nm.find_last_of('.');
                    if (dot != std::string::npos) nm = nm.substr(0, dot);
                    if (!nm.empty()) AddEntity("prefab:" + nm);
                }
            }
            ImGui::EndDragDropTarget();
        }
    }
    ImGui::End();
}

// Navigation tool: edit a 2D walkability grid (.navgrid.json), place a start
// and goal, and preview the A* path. The runtime/scripts consume the same
// asset format (neon::nav::NavGrid).
void EditorApp::BuildNavPanel() {
    if (!showNav_) return;
    if (ImGui::Begin("导航", &showNav_)) {
        char navBuf[512];
        std::snprintf(navBuf, sizeof(navBuf), "%s", navAssetPath_.c_str());
        ImGui::SetNextItemWidth(260.0f);
        if (ImGui::InputText("导航资产", navBuf, sizeof(navBuf),
                             ImGuiInputTextFlags_EnterReturnsTrue)) {
            navAssetPath_ = navBuf;
        }
        if (ImGui::Button("加载")) {
            std::ifstream in(navAssetPath_, std::ios::binary);
            if (!in.is_open()) {
                NEON_LOG_ERROR("Nav: cannot open '%s'", navAssetPath_.c_str());
            } else {
                std::string text((std::istreambuf_iterator<char>(in)),
                                 std::istreambuf_iterator<char>());
                auto r = nav::NavGrid::FromJson(text);
                if (!r.Ok()) {
                    NEON_LOG_ERROR("Nav: parse failed: %s", r.Error().c_str());
                } else {
                    navGrid_ = r.Value();
                    navAssetPath_.clear();
                    navStart_ = {-5, -5};
                    navGoal_ = {-5, -5};
                    NEON_LOG_INFO("Nav: loaded '%s' (%dx%d)", navBuf, navGrid_.Width(),
                                  navGrid_.Height());
                }
            }
        }
        ImGui::SameLine();
        if (ImGui::Button("保存")) {
            if (!navGrid_.Valid()) {
                NEON_LOG_ERROR("Nav: nothing to save");
            } else {
                const std::string path =
                    navAssetPath_.empty() ? projectDir_ + "/nav/grid.json" : navAssetPath_;
                const std::string dir = ParentPath(path);
                if (!dir.empty() && dir != "." && dir != "/") MakeDirSingle(dir);
                auto json = navGrid_.ToJson();
                if (json.Ok()) {
                    std::ofstream out(path, std::ios::binary);
                    if (out.is_open()) {
                        out << core::JsonWriter::WritePretty(json.Value());
                        navAssetPath_ = path;
                        NEON_LOG_INFO("Nav: saved -> %s", path.c_str());
                    } else {
                        NEON_LOG_ERROR("Nav: cannot write '%s'", path.c_str());
                    }
                }
            }
        }
        ImGui::SameLine();
        if (ImGui::Button("新建 16x16")) navGrid_ = nav::NavGrid::Create(16, 16, 1.0f, {0, 0});

        ImGui::Separator();
        ImGui::TextDisabled("左键: 翻转可行走 | Shift+左键: 起点 | Ctrl+左键: 终点");
        if (!navGrid_.Valid()) {
            ImGui::TextDisabled("未加载导航网格 (加载或新建)");
            ImGui::End();
            return;
        }
        const float cellPx = 18.0f;
        const ImVec2 canvasOrigin = ImGui::GetCursorScreenPos();
        const ImVec2 canvasSize(cellPx * navGrid_.Width(), cellPx * navGrid_.Height());
        ImDrawList* dl = ImGui::GetWindowDrawList();
        for (int y = 0; y < navGrid_.Height(); ++y) {
            for (int x = 0; x < navGrid_.Width(); ++x) {
                const ImVec2 a(canvasOrigin.x + x * cellPx, canvasOrigin.y + y * cellPx);
                const ImVec2 b(a.x + cellPx, a.y + cellPx);
                dl->AddRectFilled(a, b, navGrid_.Walkable(x, y)
                                            ? IM_COL32(30, 90, 40, 255)
                                            : IM_COL32(150, 40, 40, 255));
                dl->AddRect(a, b, IM_COL32(20, 20, 20, 160));
            }
        }
        // A* path preview (yellow polyline through cell centers).
        if (!navPath_.empty()) {
            for (size_t i = 1; i < navPath_.size(); ++i) {
                const math::Vec2& p0 = navPath_[i - 1];
                const math::Vec2& p1 = navPath_[i];
                dl->AddLine(ImVec2(canvasOrigin.x + p0.x * cellPx,
                                   canvasOrigin.y + p0.y * cellPx),
                            ImVec2(canvasOrigin.x + p1.x * cellPx,
                                   canvasOrigin.y + p1.y * cellPx),
                            IM_COL32(255, 220, 60, 255), 3.0f);
            }
        }
        ImGui::InvisibleButton("##nav_canvas", canvasSize);
        if (ImGui::IsItemHovered() && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
            const ImVec2 mouse = ImGui::GetIO().MousePos;
            const int cx = static_cast<int>((mouse.x - canvasOrigin.x) / cellPx);
            const int cy = static_cast<int>((mouse.y - canvasOrigin.y) / cellPx);
            if (navGrid_.InBounds(cx, cy)) {
                if (ImGui::GetIO().KeyShift) {
                    navStart_ = {static_cast<float>(cx), static_cast<float>(cy)};
                } else if (ImGui::GetIO().KeyCtrl) {
                    navGoal_ = {static_cast<float>(cx), static_cast<float>(cy)};
                } else {
                    navGrid_.SetWalkable(cx, cy, !navGrid_.Walkable(cx, cy));
                }
                // Recompute the path whenever the input state changes.
                navPath_.clear();
                if (navStart_.x >= 0 && navGoal_.x >= 0) {
                    navPath_ = navGrid_.FindPath(
                        navGrid_.CellToWorld(static_cast<int>(navStart_.x),
                                             static_cast<int>(navStart_.y)),
                        navGrid_.CellToWorld(static_cast<int>(navGoal_.x),
                                             static_cast<int>(navGoal_.y)));
                    // Convert world -> canvas pixel cells for the preview.
                    for (size_t i = 0; i < navPath_.size(); ++i) {
                        math::Vec2& p = navPath_[i];
                        int cx2 = 0, cy2 = 0;
                        navGrid_.WorldToCell(p, &cx2, &cy2);
                        p = {static_cast<float>(cx2) + 0.5f,
                             static_cast<float>(cy2) + 0.5f};
                    }
                }
            }
        }
        if (navStart_.x >= 0) {
            dl->AddCircleFilled(
                ImVec2(canvasOrigin.x + (navStart_.x + 0.5f) * cellPx,
                       canvasOrigin.y + (navStart_.y + 0.5f) * cellPx),
                cellPx * 0.35f, IM_COL32(80, 220, 255, 255));
        }
        if (navGoal_.x >= 0) {
            dl->AddCircleFilled(
                ImVec2(canvasOrigin.x + (navGoal_.x + 0.5f) * cellPx,
                       canvasOrigin.y + (navGoal_.y + 0.5f) * cellPx),
                cellPx * 0.35f, IM_COL32(255, 120, 80, 255));
        }
        ImGui::Text("起点 (%d,%d)  终点 (%d,%d)  路径 %zu 段",
                    static_cast<int>(navStart_.x), static_cast<int>(navStart_.y),
                    static_cast<int>(navGoal_.x), static_cast<int>(navGoal_.y),
                    navPath_.size());
    }
    ImGui::End();
}

// Data-driven UI editor (Godot/Unity-style): browse ui/*.ui.json documents,
// edit the node tree + node properties, and preview the result in the main
// viewport (1:1 design pixels). Saved JSON is consumed at runtime by the
// UIShow/UIClicked Lua bindings.
