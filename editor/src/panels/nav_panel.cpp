#include "panels/nav_panel.hpp"

// 导航面板实现 = 原 EditorApp::BuildNavPanel（panels_debug.inc:155-293）方法体
// 逐行迁移：EditorApp 成员（showNav_/nav_/projectDir_）改本类 visible_ /
// ctx.nav / ctx.projectDir。行为零变化。

#include <algorithm>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>
#if defined(_WIN32)
#include <direct.h>
#endif

#include "editor.hpp"
#include "editor_util.hpp"
#include "imgui.h"
#include "neon/assets/asset_manager.hpp"
#include "neon/assets/asset_path.hpp"
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

// Bakes a nav grid from the selected entity's glTF mesh (all nodes/primitives,
// node transform then the entity's TRS) via the engine's nav::BakeFromTriangles
// and saves it as a `.navgrid.json` the runtime loads through level.navgrid.
// Returns false and fills `outStatus` on any failure.
bool BakeNavGridFromSelection(EditorContext& ctx, float cell, float radius, float clearance,
                              const std::string& outRel, nav::NavGrid* outGrid,
                              std::string* outFullPath, std::string* outStatus) {
    auto fail = [&](const std::string& msg) {
        if (outStatus) *outStatus = msg;
        return false;
    };
    if (ctx.entities == nullptr || ctx.selected == nullptr)
        return fail("不可用: 无场景实体");
    const int sel = *ctx.selected;
    if (sel < 0 || sel >= static_cast<int>(ctx.entities->size()))
        return fail("请先在场景中选中地图实体");
    const SceneEntity& e = (*ctx.entities)[static_cast<size_t>(sel)];
    if (e.meshKey.rfind("gltf:", 0) != 0)
        return fail("选中实体不是 glTF 网格: " + e.meshKey);
    if (ctx.assetMgr == nullptr) return fail("不可用: AssetManager 为空");

    const std::string gltfPath = assets::NormalizeAssetPath(e.meshKey.substr(5));
    const assets::GltfAsset gltf = ctx.assetMgr->LoadGLTF(gltfPath);
    if (gltf.nodes.empty()) return fail("模型无网格: " + gltfPath);

    std::vector<math::Vec3> positions;
    std::vector<uint32_t> indices;
    for (const assets::GltfMeshNode& node : gltf.nodes) {
        const std::vector<gfx::Vertex3D>& verts = node.mesh.CpuVerts();
        if (verts.empty()) continue;
        const uint32_t base = static_cast<uint32_t>(positions.size());
        for (const gfx::Vertex3D& v : verts) {
            math::Vec3 p = node.transform.TransformPoint(v.pos);
            p = {p.x * e.scale.x, p.y * e.scale.y, p.z * e.scale.z};
            positions.push_back(e.pos + e.rot.Rotate(p));
        }
        if (!node.mesh.CpuIndicesU32().empty()) {
            for (uint32_t i : node.mesh.CpuIndicesU32()) indices.push_back(base + i);
        } else if (!node.mesh.CpuIndices().empty()) {
            for (uint16_t i : node.mesh.CpuIndices()) indices.push_back(base + i);
        } else {
            for (size_t i = 0; i < verts.size(); ++i)
                indices.push_back(base + static_cast<uint32_t>(i));
        }
    }
    if (indices.size() < 3) return fail("模型无三角面");

    nav::BakeParams params;
    params.cellSize = cell;
    params.agentRadius = radius;
    params.clearance = clearance;
    auto baked = nav::BakeFromTriangles(positions.data(), positions.size(), indices.data(),
                                        indices.size(), params);
    if (!baked.Ok()) return fail(baked.Error());
    auto json = baked.Value().ToJson();
    if (!json.Ok()) return fail(json.Error());

    std::string full = outRel;
    const std::string proj = ctx.projectDir ? *ctx.projectDir : std::string(".");
    if (!full.empty() && full[0] != '/' && full[0] != '\\' &&
        full.find(':') == std::string::npos)
        full = proj + "/" + outRel;
    const std::string dir = ParentPath(full);
    if (!dir.empty() && dir != "." && dir != "/" && !EnsureDirs(dir))
        return fail("无法创建目录: " + dir);
    if (!WriteFileUtf8(full, core::JsonWriter::WritePretty(json.Value())))
        return fail("写入失败: " + full);

    *outGrid = baked.Value();
    if (outFullPath) *outFullPath = full;
    const nav::NavGrid& g = baked.Value();
    size_t walkable = 0;
    for (int z = 0; z < g.Height(); ++z)
        for (int x = 0; x < g.Width(); ++x)
            if (g.Walkable(x, z)) ++walkable;
    const double total = static_cast<double>(g.Width()) * g.Height();
    char buf[512];
    std::snprintf(buf, sizeof(buf), "烘焙完成: %d x %d, 可行走 %.1f%%, 已保存 %s",
                  g.Width(), g.Height(), total > 0 ? 100.0 * walkable / total : 0.0,
                  full.c_str());
    if (outStatus) *outStatus = buf;
    NEON_LOG_INFO("nav bake: %s", buf);
    return true;
}

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

        if (ImGui::CollapsingHeader("从选中 Mesh 烘焙")) {
            ImGui::SetNextItemWidth(150.0f);
            ImGui::InputFloat("格子尺寸", &bakeCell_, 0.25f, 1.0f, "%.2f");
            ImGui::SetNextItemWidth(150.0f);
            ImGui::InputFloat("角色半径", &bakeRadius_, 0.1f, 0.5f, "%.2f");
            ImGui::SetNextItemWidth(150.0f);
            ImGui::InputFloat("障碍高度", &bakeClearance_, 0.1f, 0.5f, "%.2f");
            ImGui::SetNextItemWidth(320.0f);
            ImGui::InputText("输出路径", bakeOut_, sizeof(bakeOut_));
            if (ImGui::Button("从选中实体烘焙并保存")) {
                nav::NavGrid baked;
                std::string full, status;
                if (BakeNavGridFromSelection(ctx, bakeCell_, bakeRadius_, bakeClearance_,
                                             bakeOut_, &baked, &full, &status)) {
                    nav->grid = baked;
                    nav->assetPath = full;
                    nav->start = {-5, -5};
                    nav->goal = {-5, -5};
                    nav->path.clear();
                }
                bakeStatus_ = status;
            }
            if (!bakeStatus_.empty()) ImGui::TextWrapped("%s", bakeStatus_.c_str());
        }

        ImGui::Separator();
        ImGui::TextDisabled("左键: 翻转可行走 | Shift+左键: 起点 | Ctrl+左键: 终点");
        if (!nav->grid.Valid()) {
            ImGui::TextDisabled("未加载导航网格 (加载或新建)");
            ImGui::End();
            return;
        }
        // Preview cap: a 174x174 grid drawn cell-by-cell emits ~240k vertices
        // into one ImGui draw list, overflowing the 16-bit vertex index and
        // corrupting the text drawn after it. Aggregate into <=64x64 blocks so
        // the vertex count stays bounded while still showing the obstacle layout.
        const int maxBlocks = 64;
        const int stepX = std::max(1, (nav->grid.Width() + maxBlocks - 1) / maxBlocks);
        const int stepZ = std::max(1, (nav->grid.Height() + maxBlocks - 1) / maxBlocks);
        const int blocksW = (nav->grid.Width() + stepX - 1) / stepX;
        const int blocksH = (nav->grid.Height() + stepZ - 1) / stepZ;
        const float cellPx = std::max(
            3.0f, std::min(18.0f, 560.0f / static_cast<float>(std::max(blocksW, blocksH))));
        const ImVec2 canvasOrigin = ImGui::GetCursorScreenPos();
        const ImVec2 canvasSize(cellPx * blocksW, cellPx * blocksH);
        ImDrawList* dl = ImGui::GetWindowDrawList();
        auto blockWalkable = [&](int bx, int bz) {
            const int x0 = bx * stepX, y0 = bz * stepZ;
            const int x1 = std::min(x0 + stepX, nav->grid.Width());
            const int y1 = std::min(y0 + stepZ, nav->grid.Height());
            for (int y = y0; y < y1; ++y)
                for (int x = x0; x < x1; ++x)
                    if (nav->grid.Walkable(x, y)) return true;
            return false;
        };
        for (int bz = 0; bz < blocksH; ++bz) {
            for (int bx = 0; bx < blocksW; ++bx) {
                const ImVec2 a(canvasOrigin.x + bx * cellPx, canvasOrigin.y + bz * cellPx);
                const ImVec2 b(a.x + cellPx, a.y + cellPx);
                dl->AddRectFilled(a, b, blockWalkable(bx, bz) ? IM_COL32(30, 90, 40, 255)
                                                              : IM_COL32(150, 40, 40, 255));
                if (cellPx >= 6.0f) dl->AddRect(a, b, IM_COL32(20, 20, 20, 160));
            }
        }
        // A* path preview (yellow polyline; cell coords -> block pixels).
        auto cellToPx = [&](float cx, float cy, ImVec2* out) {
            out->x = canvasOrigin.x + (cx / static_cast<float>(stepX)) * cellPx;
            out->y = canvasOrigin.y + (cy / static_cast<float>(stepZ)) * cellPx;
        };
        if (!nav->path.empty()) {
            for (size_t i = 1; i < nav->path.size(); ++i) {
                ImVec2 p0, p1;
                cellToPx(nav->path[i - 1].x, nav->path[i - 1].y, &p0);
                cellToPx(nav->path[i].x, nav->path[i].y, &p1);
                dl->AddLine(p0, p1, IM_COL32(255, 220, 60, 255), 3.0f);
            }
        }
        ImGui::InvisibleButton("##nav_canvas", canvasSize);
        if (ImGui::IsItemHovered() && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
            const ImVec2 mouse = ImGui::GetIO().MousePos;
            const int bx = static_cast<int>((mouse.x - canvasOrigin.x) / cellPx);
            const int bz = static_cast<int>((mouse.y - canvasOrigin.y) / cellPx);
            if (bx >= 0 && bz >= 0 && bx < blocksW && bz < blocksH) {
                if (ImGui::GetIO().KeyShift) {
                    nav->start = {static_cast<float>(bx * stepX),
                                  static_cast<float>(bz * stepZ)};
                } else if (ImGui::GetIO().KeyCtrl) {
                    nav->goal = {static_cast<float>(bx * stepX),
                                 static_cast<float>(bz * stepZ)};
                } else {
                    // Flip every cell this block covers (large grids aggregate).
                    const bool to = !blockWalkable(bx, bz);
                    const int x0 = bx * stepX, y0 = bz * stepZ;
                    for (int y = y0; y < std::min(y0 + stepZ, nav->grid.Height()); ++y)
                        for (int x = x0; x < std::min(x0 + stepX, nav->grid.Width()); ++x)
                            nav->grid.SetWalkable(x, y, to);
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
            ImVec2 s;
            cellToPx(nav->start.x + 0.5f, nav->start.y + 0.5f, &s);
            dl->AddCircleFilled(s, std::max(3.0f, cellPx * 0.35f), IM_COL32(80, 220, 255, 255));
        }
        if (nav->goal.x >= 0) {
            ImVec2 s;
            cellToPx(nav->goal.x + 0.5f, nav->goal.y + 0.5f, &s);
            dl->AddCircleFilled(s, std::max(3.0f, cellPx * 0.35f), IM_COL32(255, 120, 80, 255));
        }
        ImGui::Text("起点 (%d,%d)  终点 (%d,%d)  路径 %zu 段",
                    static_cast<int>(nav->start.x), static_cast<int>(nav->start.y),
                    static_cast<int>(nav->goal.x), static_cast<int>(nav->goal.y),
                    nav->path.size());
    }
    ImGui::End();
}

} // namespace neon::editor
