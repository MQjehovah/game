#include "panels/nav_panel.hpp"

// 导航面板实现 = 原 EditorApp::BuildNavPanel（panels_debug.inc:155-293）方法体
// 逐行迁移：EditorApp 成员（showNav_/nav_/projectDir_）改本类 visible_ /
// ctx.nav / ctx.projectDir。行为零变化。

#include <algorithm>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <string>

#include "editor_util.hpp"
#include "imgui.h"
#include "neon/core/json.hpp"
#include "neon/core/log.hpp"

namespace neon::editor {

namespace {

// 保存时确保输出目录存在（原 panels.cpp 匿名命名空间的 MakeDirSingle；
// 与本面板同 TU 的本地副本，模式同 ScenePanel/AssetPanel 的 ToLower/FileName）。
bool MakeDirSingle(const std::string& path) {
#if defined(_WIN32)
    return _mkdir(path.c_str()) == 0 || errno == EEXIST;
#else
    return ::mkdir(path.c_str(), 0777) == 0 || errno == EEXIST;
#endif
}

} // namespace

void NavPanel::Draw(EditorContext& ctx) {
    NavState* nav = ctx.nav;
    if (!visible_ || !*visible_ || !nav) return;
    if (ImGui::Begin("导航", visible_)) {
        char navBuf[512];
        std::snprintf(navBuf, sizeof(navBuf), "%s", nav->assetPath.c_str());
        ImGui::SetNextItemWidth(260.0f);
        if (ImGui::InputText("导航资产", navBuf, sizeof(navBuf),
                             ImGuiInputTextFlags_EnterReturnsTrue)) {
            nav->assetPath = navBuf;
        }
        if (ImGui::Button("加载")) {
            std::ifstream in(nav->assetPath, std::ios::binary);
            if (!in.is_open()) {
                NEON_LOG_ERROR("Nav: cannot open '%s'", nav->assetPath.c_str());
            } else {
                std::string text((std::istreambuf_iterator<char>(in)),
                                 std::istreambuf_iterator<char>());
                auto r = nav::NavGrid::FromJson(text);
                if (!r.Ok()) {
                    NEON_LOG_ERROR("Nav: parse failed: %s", r.Error().c_str());
                } else {
                    nav->grid = r.Value();
                    nav->assetPath.clear();
                    nav->start = {-5, -5};
                    nav->goal = {-5, -5};
                    NEON_LOG_INFO("Nav: loaded '%s' (%dx%d)", navBuf, nav->grid.Width(),
                                  nav->grid.Height());
                }
            }
        }
        ImGui::SameLine();
        if (ImGui::Button("保存")) {
            if (!nav->grid.Valid()) {
                NEON_LOG_ERROR("Nav: nothing to save");
            } else {
                const std::string path =
                    nav->assetPath.empty() ? *ctx.projectDir + "/nav/grid.json" : nav->assetPath;
                const std::string dir = ParentPath(path);
                if (!dir.empty() && dir != "." && dir != "/") MakeDirSingle(dir);
                auto json = nav->grid.ToJson();
                if (json.Ok()) {
                    std::ofstream out(path, std::ios::binary);
                    if (out.is_open()) {
                        out << core::JsonWriter::WritePretty(json.Value());
                        nav->assetPath = path;
                        NEON_LOG_INFO("Nav: saved -> %s", path.c_str());
                    } else {
                        NEON_LOG_ERROR("Nav: cannot write '%s'", path.c_str());
                    }
                }
            }
        }
        ImGui::SameLine();
        if (ImGui::Button("新建 16x16")) nav->grid = nav::NavGrid::Create(16, 16, 1.0f, {0, 0});

        ImGui::Separator();
        ImGui::TextDisabled("左键: 翻转可行走 | Shift+左键: 起点 | Ctrl+左键: 终点");
        if (!nav->grid.Valid()) {
            ImGui::TextDisabled("未加载导航网格 (加载或新建)");
            ImGui::End();
            return;
        }
        const float cellPx = 18.0f;
        const ImVec2 canvasOrigin = ImGui::GetCursorScreenPos();
        const ImVec2 canvasSize(cellPx * nav->grid.Width(), cellPx * nav->grid.Height());
        ImDrawList* dl = ImGui::GetWindowDrawList();
        for (int y = 0; y < nav->grid.Height(); ++y) {
            for (int x = 0; x < nav->grid.Width(); ++x) {
                const ImVec2 a(canvasOrigin.x + x * cellPx, canvasOrigin.y + y * cellPx);
                const ImVec2 b(a.x + cellPx, a.y + cellPx);
                dl->AddRectFilled(a, b, nav->grid.Walkable(x, y)
                                            ? IM_COL32(30, 90, 40, 255)
                                            : IM_COL32(150, 40, 40, 255));
                dl->AddRect(a, b, IM_COL32(20, 20, 20, 160));
            }
        }
        // A* path preview (yellow polyline through cell centers).
        if (!nav->path.empty()) {
            for (size_t i = 1; i < nav->path.size(); ++i) {
                const math::Vec2& p0 = nav->path[i - 1];
                const math::Vec2& p1 = nav->path[i];
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
            if (nav->grid.InBounds(cx, cy)) {
                if (ImGui::GetIO().KeyShift) {
                    nav->start = {static_cast<float>(cx), static_cast<float>(cy)};
                } else if (ImGui::GetIO().KeyCtrl) {
                    nav->goal = {static_cast<float>(cx), static_cast<float>(cy)};
                } else {
                    nav->grid.SetWalkable(cx, cy, !nav->grid.Walkable(cx, cy));
                }
                // Recompute the path whenever the input state changes.
                nav->path.clear();
                if (nav->start.x >= 0 && nav->goal.x >= 0) {
                    nav->path = nav->grid.FindPath(
                        nav->grid.CellToWorld(static_cast<int>(nav->start.x),
                                             static_cast<int>(nav->start.y)),
                        nav->grid.CellToWorld(static_cast<int>(nav->goal.x),
                                             static_cast<int>(nav->goal.y)));
                    // Convert world -> canvas pixel cells for the preview.
                    for (size_t i = 0; i < nav->path.size(); ++i) {
                        math::Vec2& p = nav->path[i];
                        int cx2 = 0, cy2 = 0;
                        nav->grid.WorldToCell(p, &cx2, &cy2);
                        p = {static_cast<float>(cx2) + 0.5f,
                             static_cast<float>(cy2) + 0.5f};
                    }
                }
            }
        }
        if (nav->start.x >= 0) {
            dl->AddCircleFilled(
                ImVec2(canvasOrigin.x + (nav->start.x + 0.5f) * cellPx,
                       canvasOrigin.y + (nav->start.y + 0.5f) * cellPx),
                cellPx * 0.35f, IM_COL32(80, 220, 255, 255));
        }
        if (nav->goal.x >= 0) {
            dl->AddCircleFilled(
                ImVec2(canvasOrigin.x + (nav->goal.x + 0.5f) * cellPx,
                       canvasOrigin.y + (nav->goal.y + 0.5f) * cellPx),
                cellPx * 0.35f, IM_COL32(255, 120, 80, 255));
        }
        ImGui::Text("起点 (%d,%d)  终点 (%d,%d)  路径 %zu 段",
                    static_cast<int>(nav->start.x), static_cast<int>(nav->start.y),
                    static_cast<int>(nav->goal.x), static_cast<int>(nav->goal.y),
                    nav->path.size());
    }
    ImGui::End();
}

} // namespace neon::editor
