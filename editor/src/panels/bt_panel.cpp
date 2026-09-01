// Behavior tree visual editor (T4.4) panel: the docked 行为树 panel UI.
//
// Canvas rendering + interaction (drag-move, link, delete), the node palette
// (drag or click-to-place), per-node param editing, toolbar save/open/new, and
// the play debug highlight. The pure node graph model lives inline in
// bt_editor.hpp so tests can include it headlessly; the ImGui layer (Task 18b:
// BtPanel : IPanel) lives here and is exercised by the editor smoke test.
//
// 迁移自原 EditorApp::BuildBtPanel/BuildBtToolbar/BuildBtPalette/BuildBtCanvas/
// BuildBtParams（bt_editor.cpp）方法体，行为零变化：EditorApp 成员（btGraph_
// /btHistory_/btFileName_/btSelected_/拖拽/视图变换等）改 ctx.btGraph 指针 /
// 本类成员 / ctx 回调（btLoadFromFile/btSaveToFile/playActiveTreePath）。

#include "panels/bt_panel.hpp"

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

#include "editor.hpp"
#include "imgui_internal.h"

namespace neon::editor {
namespace {

constexpr float kNodeW = 190.0f;
constexpr float kNodeH = 64.0f;
constexpr float kCanvasW = 4000.0f;
constexpr float kCanvasH = 4000.0f;
constexpr float kGrid = 16.0f;

// Node-type registry cache (one construction, shared by palette + params).
const std::vector<bt::NodeTypeInfo>& BtTypeInfos() {
    static const std::vector<bt::NodeTypeInfo> infos = bt::AllNodeTypes();
    return infos;
}
const bt::NodeTypeInfo* FindTypeInfo(const std::string& type) {
    for (const auto& ti : BtTypeInfos())
        if (ti.type == type) return &ti;
    return nullptr;
}

ImU32 CatColor(const char* cat) {
    if (!cat) return IM_COL32(90, 96, 110, 255);
    if (std::strcmp(cat, "composite") == 0) return IM_COL32(70, 120, 220, 255);
    if (std::strcmp(cat, "decorator") == 0) return IM_COL32(150, 90, 200, 255);
    if (std::strcmp(cat, "action") == 0) return IM_COL32(70, 170, 110, 255);
    return IM_COL32(210, 140, 70, 255); // condition
}

float Snap(float v) { return std::floor(v / kGrid + 0.5f) * kGrid; }

// Whether two graphs are identical including node positions (the serialized
// JSON omits positions, so it cannot detect node moves).
bool GraphsEqual(const btgraph::BtGraph& a, const btgraph::BtGraph& b) {
    if (a.NodeCount() != b.NodeCount() || a.LinkCount() != b.LinkCount()) return false;
    for (const auto& n : a.Nodes()) {
        const btgraph::BtGraphNode* o = b.Find(n.id);
        if (!o || o->type != n.type || o->name != n.name) return false;
        if (o->pos.x != n.pos.x || o->pos.y != n.pos.y) return false;
        if (core::JsonWriter::Write(o->args) != core::JsonWriter::Write(n.args)) return false;
    }
    for (const auto& l : a.Links()) {
        bool found = false;
        for (const auto& l2 : b.Links())
            if (l2.parent == l.parent && l2.child == l.child) {
                found = true;
                break;
            }
        if (!found) return false;
    }
    return true;
}

// Canvas node under a canvas-space point ("" when none). The canvas hit region
// is the node body; the top strip (category bar) is part of the body.
std::string HitTest(const btgraph::BtGraph& g, const math::Vec2& p) {
    for (auto it = g.Nodes().rbegin(); it != g.Nodes().rend(); ++it) {
        if (p.x >= it->pos.x && p.x <= it->pos.x + kNodeW && p.y >= it->pos.y &&
            p.y <= it->pos.y + kNodeH)
            return it->id;
    }
    return "";
}

// Short value text for a node's key params shown on the canvas body.
std::string ArgText(const btgraph::BtGraphNode& n, const bt::ParamInfo& p) {
    const core::Json* v = n.args.Get(p.name);
    if (!v) return "-";
    switch (p.type) {
        case bt::ParamType::Number: {
            char buf[32];
            std::snprintf(buf, sizeof(buf), "%g", v->GetNumber());
            return buf;
        }
        case bt::ParamType::String: return v->IsString() ? v->GetString() : "-";
        case bt::ParamType::Bool: return v->IsBool() ? (v->GetBool() ? "true" : "false") : "-";
        default: return "...";
    }
}

core::Json JNum(double v) {
    core::Json j;
    j.type_ = core::Json::Type::Number;
    j.number_ = v;
    return j;
}
core::Json JStr(const std::string& s) {
    core::Json j;
    j.type_ = core::Json::Type::String;
    j.string_ = s;
    return j;
}
core::Json JBool(bool b) {
    core::Json j;
    j.type_ = core::Json::Type::Bool;
    j.bool_ = b;
    return j;
}

std::vector<std::string> ListBtFiles(const std::string& dir) {
    std::vector<std::string> out;
#if defined(_WIN32)
    std::string pattern = dir + "/*.bt.json";
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(pattern.c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return out;
    do {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        out.push_back(fd.cFileName);
    } while (FindNextFileA(h, &fd));
    FindClose(h);
#else
    DIR* d = opendir(dir.c_str());
    if (d) {
        struct dirent* e;
        while ((e = readdir(d)) != nullptr) {
            std::string name = e->d_name;
            if (name.size() > 8 && name.compare(name.size() - 8, 8, ".bt.json") == 0)
                out.push_back(name);
        }
        closedir(d);
    }
#endif
    std::sort(out.begin(), out.end());
    return out;
}

inline float BtClamp(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }
inline float BtDist(float x1, float y1, float x2, float y2) {
    const float dx = x2 - x1, dy = y2 - y1;
    return std::sqrt(dx * dx + dy * dy);
}

} // namespace

// A whole-graph snapshot command: every BT editor mutation (add/remove/relink/
// move/arg edit) pushes the graph before + after, so one undo step restores the
// previous graph state. Graphs are small; copies are cheap.
class BtGraphSnapshotCommand : public Command {
public:
    BtGraphSnapshotCommand(btgraph::BtGraph* graph, btgraph::BtGraph before,
                           btgraph::BtGraph after)
        : graph_(graph), before_(std::move(before)), after_(std::move(after)) {}
    void Apply() override { *graph_ = after_; }
    void Undo() override { *graph_ = before_; }

private:
    btgraph::BtGraph* graph_;
    btgraph::BtGraph before_;
    btgraph::BtGraph after_;
};

void BtPanel::PushSnapshot(EditorContext& ctx, const btgraph::BtGraph& before) {
    if (GraphsEqual(before, *ctx.btGraph)) return; // no-op change: no undo step
    btHistory_.Push(std::make_unique<BtGraphSnapshotCommand>(ctx.btGraph, before, *ctx.btGraph));
}

std::string BtPanel::BehaviorsDir(const EditorContext& ctx) const {
    std::string base = (!ctx.projectDir || ctx.projectDir->empty()) ? "." : *ctx.projectDir;
    return base + "/assets/behaviors";
}

void BtPanel::RefreshBehaviorFiles(EditorContext& ctx) {
    btBehaviorFiles_ = ListBtFiles(BehaviorsDir(ctx));
}

void BtPanel::NewTree(EditorContext& ctx) {
    *ctx.btGraph = btgraph::BtGraph{};
    btHistory_.Clear();
    btSelected_.clear();
    btPendingType_.clear();
    btFileName_ = "behavior";
    std::snprintf(btFileNameBuf_, sizeof(btFileNameBuf_), "%s", btFileName_.c_str());
    NEON_LOG_INFO("BtEditor: new empty tree");
}

// Load + 面板自有状态重置：文件 IO（EditorApp::BtLoadFromFile，冒烟测试也调）
// 经 ctx.btLoadFromFile 回调，成功后再清历史/选区（与原 BtLoadFromFile 一致，
// 失败不动任何面板状态）。
bool BtPanel::LoadFromFile(EditorContext& ctx, const std::string& path) {
    if (!ctx.btLoadFromFile(path)) return false;
    btHistory_.Clear();
    btSelected_.clear();
    btPendingType_.clear();
    return true;
}

void BtPanel::UpdatePlayHighlight(EditorContext& ctx) {
    btActivePath_.clear();
    if (!(ctx.playActive && *ctx.playActive)) return;
    btActivePath_ = ctx.playActiveTreePath();
}

void BtPanel::Draw(EditorContext& ctx) {
    if (!*visible_) {
        btPanelFocused_ = false; // panel closed: never route undo to a stale focus
        return;
    }
    UpdatePlayHighlight(ctx);
    // The panel hosts a full node canvas: never let a stale saved size (or the
    // auto-size feedback loop between the canvas child and the window) shrink
    // it below a usable area.
    ImGui::SetNextWindowSize(ImVec2(1040.0f, 560.0f), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSizeConstraints(ImVec2(720.0f, 360.0f),
                                        ImVec2(FLT_MAX, FLT_MAX));
    if (!ImGui::Begin("行为树", visible_)) {
        btPanelFocused_ = false;
        ImGui::End();
        return;
    }
    btPanelFocused_ = ImGui::IsWindowFocused(ImGuiFocusedFlags_ChildWindows);
    BuildToolbar(ctx);

    const float availW = ImGui::GetContentRegionAvail().x;
    const float availH = ImGui::GetContentRegionAvail().y;
    // 节点参数面板: 仅在选中画布节点后出现在右侧 (未选中时画布占满).
    const bool hasSelection =
        !btSelected_.empty() && ctx.btGraph->Find(btSelected_) != nullptr;
    const float paramsW = hasSelection ? 260.0f : 0.0f;
    ImGui::BeginChild("##bt_palette", ImVec2(180.0f, availH), ImGuiChildFlags_Borders);
    BuildPalette(ctx);
    ImGui::EndChild();
    ImGui::SameLine();
    const float canvasW = std::max(160.0f, availW - 180.0f - paramsW);
    ImGui::BeginChild("##bt_canvas", ImVec2(canvasW, availH), ImGuiChildFlags_Borders);
    BuildCanvas(ctx);
    ImGui::EndChild();
    if (hasSelection) {
        ImGui::SameLine();
        ImGui::BeginChild("##bt_params", ImVec2(paramsW, availH), ImGuiChildFlags_Borders);
        BuildParams(ctx);
        ImGui::EndChild();
    }
    ImGui::End();
}

void BtPanel::BuildToolbar(EditorContext& ctx) {
    if (ImGui::Button("新建")) NewTree(ctx);
    ImGui::SameLine();
    if (ImGui::Button("打开") && btFileNameBuf_[0]) {
        if (!LoadFromFile(ctx, BehaviorsDir(ctx) + "/" + btFileName_ + ".bt.json"))
            NEON_LOG_WARN("BtEditor: failed to open '%s.bt.json'", btFileName_.c_str());
    }
    ImGui::SameLine();
    if (ImGui::Button("保存") && btFileNameBuf_[0]) {
        if (!ctx.btSaveToFile(BehaviorsDir(ctx) + "/" + btFileName_ + ".bt.json"))
            NEON_LOG_WARN("BtEditor: failed to save '%s.bt.json'", btFileName_.c_str());
    }
    ImGui::SameLine();
    ImGui::SetNextItemWidth(150.0f);
    if (ImGui::InputText("##btname", btFileNameBuf_, sizeof(btFileNameBuf_),
                         ImGuiInputTextFlags_EnterReturnsTrue)) {
        btFileName_ = btFileNameBuf_;
        if (btFileName_.empty()) btFileName_ = "behavior";
    }
    ImGui::SameLine();
    if (ImGui::Button("自动布局")) {
        btgraph::BtGraph before = *ctx.btGraph;
        ctx.btGraph->LayoutTopDown();
        PushSnapshot(ctx, before);
    }
    ImGui::SameLine();
    if (ImGui::Button("撤销") && btHistory_.CanUndo()) btHistory_.Undo();
    ImGui::SameLine();
    if (ImGui::Button("重做") && btHistory_.CanRedo()) btHistory_.Redo();
    ImGui::SameLine();
    if (ImGui::Button("删除节点") && !btSelected_.empty()) {
        btgraph::BtGraph before = *ctx.btGraph;
        ctx.btGraph->RemoveNode(btSelected_);
        btSelected_.clear();
        PushSnapshot(ctx, before);
    }
    ImGui::SameLine();
    if ((ctx.playActive && *ctx.playActive) && !btActivePath_.empty()) {
        ImGui::TextColored(ImVec4(0.4f, 1.0f, 0.5f, 1.0f), "执行中 %s", btActivePath_.c_str());
    }
    ImGui::Separator();

    // Throttle the behaviors/ directory scan: it runs on panel open and every
    // ~1s of frames (60 @ 60fps), never per frame.
    const uint64_t now = ctx.time->frameIndex;
    if (now - btFilesRefreshFrame_ >= 60 || btBehaviorFiles_.empty()) {
        RefreshBehaviorFiles(ctx);
        btFilesRefreshFrame_ = now;
    }
    if (!btBehaviorFiles_.empty()) {
        ImGui::TextDisabled("assets/behaviors/");
        for (const auto& f : btBehaviorFiles_) {
            char label[512];
            std::snprintf(label, sizeof(label), "%s##btfile_%s", f.c_str(), f.c_str());
            if (ImGui::Selectable(label)) {
                if (!LoadFromFile(ctx, BehaviorsDir(ctx) + "/" + f))
                    NEON_LOG_WARN("BtEditor: failed to load '%s'", f.c_str());
            }
        }
        ImGui::Separator();
    }
}

void BtPanel::BuildPalette(EditorContext& /*ctx*/) {
    const char* cats[] = {"composite", "decorator", "action", "condition"};
    const char* names[] = {"组合", "装饰", "行为", "条件"};
    for (int c = 0; c < 4; ++c) {
        if (!ImGui::CollapsingHeader(names[c], ImGuiTreeNodeFlags_DefaultOpen)) continue;
        for (const auto& ti : BtTypeInfos()) {
            if (ti.category != cats[c]) continue;
            const bool armed = btPendingType_ == ti.type;
            if (ImGui::Selectable(ti.type.c_str(), armed)) btPendingType_ = ti.type;
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", ti.type.c_str());
            if (ImGui::BeginDragDropSource(ImGuiDragDropFlags_SourceNoHoldToOpenOthers)) {
                ImGui::SetDragDropPayload("BT_PALETTE", ti.type.c_str(), ti.type.size() + 1);
                ImGui::Text("%s", ti.type.c_str());
                ImGui::EndDragDropSource();
            }
        }
    }
    ImGui::Separator();
    ImGui::TextDisabled("点击条目后在画布放置,\n或直接拖入画布.\nCtrl+点击连线,\nShift+点击断开");
}

void BtPanel::BuildCanvas(EditorContext& ctx) {
    btgraph::BtGraph& g = *ctx.btGraph;
    btCanvasDrawn_ = false;
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    const ImVec2 avail = ImGui::GetContentRegionAvail();
    // 无滚动条: 画布仅平移 (btPan_, 中键/空白拖拽) + 缩放 (btZoom_, 滚轮以鼠标为中心)。
    ImGui::InvisibleButton("##bt_canvas_region", avail);
    const bool hovered = ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenBlockedByActiveItem);

    const ImVec2 toScreen(origin.x + btPan_.x, origin.y + btPan_.y);
    auto w2s = [&](const ImVec2& wp) {
        return ImVec2(toScreen.x + wp.x * btZoom_, toScreen.y + wp.y * btZoom_);
    };
    auto s2w = [&](const ImVec2& sp) {
        return math::Vec2((sp.x - toScreen.x) / btZoom_, (sp.y - toScreen.y) / btZoom_);
    };

    // --- 锚点 (画布坐标): 输入=顶部中点, 输出=底部中点 ---
    auto inAnchor = [&](const btgraph::BtGraphNode& n) {
        return ImVec2(n.pos.x + kNodeW * 0.5f, n.pos.y);
    };
    auto outAnchor = [&](const btgraph::BtGraphNode& n) {
        return ImVec2(n.pos.x + kNodeW * 0.5f, n.pos.y + kNodeH);
    };

    ImDrawList* dl = ImGui::GetWindowDrawList();
    const int vtxBefore = dl->VtxBuffer.Size;

    // --- 连线: 父输出锚点 -> 子输入锚点 bezier ---
    for (const auto& l : g.Links()) {
        const btgraph::BtGraphNode* p = g.Find(l.parent);
        const btgraph::BtGraphNode* c = g.Find(l.child);
        if (!p || !c) continue;
        const ImVec2 p0 = w2s(outAnchor(*p));
        const ImVec2 p1 = w2s(inAnchor(*c));
        const float droop = 46.0f * btZoom_;
        const ImVec2 cp0(p0.x, p0.y + droop);
        const ImVec2 cp1(p1.x, p1.y - droop);
        const bool active =
            (ctx.playActive && *ctx.playActive) && g.TreeIdOf(c->id) == btActivePath_;
        dl->AddBezierCubic(p0, cp0, cp1, p1,
                           active ? IM_COL32(80, 255, 120, 255) : IM_COL32(150, 160, 180, 255),
                           2.0f * btZoom_);
        // 锚点圆点
        dl->AddCircleFilled(p0, 4.0f * btZoom_, IM_COL32(200, 210, 230, 255));
        dl->AddCircleFilled(p1, 4.0f * btZoom_, IM_COL32(200, 210, 230, 255));
    }

    // --- 蓄力连线中 (锚点拖拽): 从输出锚点到鼠标 ---
    const ImVec2 mouseScr = ImGui::GetIO().MousePos;
    if (btLinking_) {
        const btgraph::BtGraphNode* from = g.Find(btLinkFrom_);
        if (from) {
            const ImVec2 pa = w2s(outAnchor(*from));
            dl->AddBezierCubic(pa, ImVec2(pa.x, pa.y + 40.0f * btZoom_),
                               ImVec2(mouseScr.x, mouseScr.y + 30.0f), mouseScr,
                               IM_COL32(120, 200, 255, 220), 2.0f * btZoom_);
        }
    }

    // --- 节点 ---
    for (const auto& n : g.Nodes()) {
        const ImVec2 pos = w2s(ImVec2(n.pos.x, n.pos.y));
        const ImVec2 end = w2s(ImVec2(n.pos.x + kNodeW, n.pos.y + kNodeH));
        const bool selected = btSelected_ == n.id;
        const bool active =
            (ctx.playActive && *ctx.playActive) && g.TreeIdOf(n.id) == btActivePath_;
        const bt::NodeTypeInfo* info = FindTypeInfo(n.type);
        const ImU32 cat = CatColor(info ? info->category.c_str() : nullptr);

        dl->AddRectFilled(pos, end, IM_COL32(40, 44, 56, 255), 6.0f * btZoom_);
        dl->AddRectFilled(pos, ImVec2(end.x, pos.y + 24.0f * btZoom_), cat, 6.0f * btZoom_);
        dl->AddRectFilled(ImVec2(pos.x, pos.y + 16.0f * btZoom_),
                          ImVec2(end.x, pos.y + 24.0f * btZoom_),
                          IM_COL32(40, 44, 56, 255));
        const ImU32 border = active ? IM_COL32(80, 255, 120, 255)
                                    : selected ? IM_COL32(255, 200, 80, 255)
                                               : IM_COL32(120, 130, 150, 255);
        dl->AddRect(pos, end, border, 6.0f * btZoom_, ImDrawFlags_None,
                    (selected || active) ? 2.5f : 1.0f);

        const std::string title = n.name.empty() ? n.type : n.name;
        dl->AddText(nullptr, 13.0f * btZoom_, ImVec2(pos.x + 7.0f * btZoom_, pos.y + 4.0f * btZoom_),
                    IM_COL32(255, 255, 255, 255), title.c_str());
        if (info) {
            std::string line;
            for (size_t i = 0; i < info->params.size() && i < 2; ++i) {
                if (!line.empty()) line += "  ";
                line += info->params[i].name + "=" + ArgText(n, info->params[i]);
            }
            if (!line.empty())
                dl->AddText(nullptr, 12.0f * btZoom_,
                            ImVec2(pos.x + 7.0f * btZoom_, pos.y + 30.0f * btZoom_),
                            IM_COL32(200, 205, 215, 255), line.c_str());
        }
    }
    // Vertices are the reliable signal: ImGui batches same-clip-rect items into
    // one command, so CmdBuffer may not grow even when nodes/links drew.
    btCanvasDrawn_ = dl->VtxBuffer.Size > vtxBefore;

    // --- 交互 (视图变换后的世界坐标) ---
    const ImVec2 mouseScr2 = ImGui::GetIO().MousePos;
    const math::Vec2 cm = s2w(mouseScr2);

    // 滚轮缩放 (以鼠标为锚点)
    if (hovered) {
        const float wheel = ImGui::GetIO().MouseWheel;
        if (wheel != 0.0f) {
            const math::Vec2 anchor = s2w(mouseScr2);
            btZoom_ = BtClamp(btZoom_ * (wheel > 0 ? 1.15f : 1.0f / 1.15f), 0.4f, 2.5f);
            btPan_.x = mouseScr2.x - origin.x - anchor.x * btZoom_;
            btPan_.y = mouseScr2.y - origin.y - anchor.y * btZoom_;
        }
    }
    // 中键拖拽 / 空白左键拖拽 = 平移
    const bool panning =
        btCanvasDrawn_ &&
        ((hovered && ImGui::IsMouseDragging(ImGuiMouseButton_Middle, 4.0f)) ||
         (ImGui::IsMouseDragging(ImGuiMouseButton_Left, 4.0f) && btDragNode_.empty() &&
          btLinkFrom_.empty() && btSelected_.empty() && btPendingType_.empty()));
    if (panning) {
        btPan_.x += ImGui::GetIO().MouseDelta.x;
        btPan_.y += ImGui::GetIO().MouseDelta.y;
    }

    // 锚点命中 (屏幕像素距离)
    auto anchorHit = [&](float px, float py, float maxPx) -> std::string {
        std::string hitId;
        float best = maxPx;
        for (const auto& n : g.Nodes()) {
            const ImVec2 so = w2s(outAnchor(n));
            const float dOut = BtDist(px, py, so.x, so.y);
            if (dOut < best) { best = dOut; hitId = n.id; }
        }
        return hitId;
    };

    // 锚点拖拽连线: 释放在节点上 = 连接 (SetParent)
    if (btLinking_ && ImGui::IsMouseReleased(ImGuiMouseButton_Left)) {
        const std::string hit = HitTest(g, cm);
        if (!hit.empty() && hit != btLinkFrom_) {
            const btgraph::BtGraph before = g;
            if (g.SetParent(hit, btLinkFrom_)) PushSnapshot(ctx, before);
        }
        btLinking_ = false;
        btLinkFrom_.clear();
    }

    if (hovered && !btLinking_) {
        if (ImGui::BeginDragDropTarget()) {
            if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("BT_PALETTE")) {
                std::string type(static_cast<const char*>(payload->Data),
                                 static_cast<size_t>(payload->DataSize) - 1);
                btgraph::BtGraph before = g;
                const math::Vec2 p(Snap(cm.x - kNodeW * 0.5f), Snap(cm.y - kNodeH * 0.5f));
                const std::string id = g.AddNode(type, p);
                if (!id.empty()) {
                    btSelected_ = id;
                    PushSnapshot(ctx, before);
                }
                btPendingType_.clear();
            }
            ImGui::EndDragDropTarget();
        }

        const std::string hit = HitTest(g, cm);
        if (!btDragging_ && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
            // 输出锚点优先: 从锚点拖出连线
            if (!hit.empty()) {
                if (const btgraph::BtGraphNode* hn = g.Find(hit)) {
                    const ImVec2 so = w2s(outAnchor(*hn));
                    if (BtDist(mouseScr2.x, mouseScr2.y, so.x, so.y) < 10.0f * btZoom_) {
                        btLinking_ = true;
                        btLinkFrom_ = hit;
                    }
                }
            }
            if (!btLinking_) CanvasClick(ctx, cm, ImGui::GetIO().KeyCtrl, ImGui::GetIO().KeyShift);
        }

        if (!btSelected_.empty() && ImGui::IsKeyPressed(ImGuiKey_Delete, false) &&
            !ImGui::GetIO().WantTextInput) {
            const btgraph::BtGraph before = g;
            g.RemoveNode(btSelected_);
            btSelected_.clear();
            PushSnapshot(ctx, before);
        }
    }

    // 连线拖拽跟随 (即使指针离开画布)
    if (btLinking_) {
        if (ImGui::IsMouseReleased(ImGuiMouseButton_Left)) {
            btLinking_ = false;
            btLinkFrom_.clear();
        }
        return;
    }

    // 节点拖拽跟随 (即使指针离开画布)
    if (btDragging_) {
        const math::Vec2 cm2 = s2w(ImGui::GetIO().MousePos);
        if (ImGui::IsMouseDragging(ImGuiMouseButton_Left, 2.0f) && !btDragNode_.empty()) {
            const math::Vec2 delta(cm2.x - btDragStart_.x, cm2.y - btDragStart_.y);
            g.SetPos(btDragNode_, btNodeStartPos_ + delta);
        }
        if (ImGui::IsMouseReleased(ImGuiMouseButton_Left)) {
            const btgraph::BtGraphNode* nd = g.Find(btDragNode_);
            const bool moved = nd && (nd->pos.x != btNodeStartPos_.x ||
                                      nd->pos.y != btNodeStartPos_.y);
            if (moved && btHasGraphBeforeDrag_) PushSnapshot(ctx, btGraphBeforeDrag_);
            btHasGraphBeforeDrag_ = false;
            btDragging_ = false;
            btDragNode_.clear();
        }
    }
}

void BtPanel::CanvasClick(EditorContext& ctx, const math::Vec2& cm, bool ctrl, bool shift) {
    btgraph::BtGraph& g = *ctx.btGraph;
    const std::string hit = HitTest(g, cm);
    if (!hit.empty()) {
        if (ctrl) {
            // Ctrl+click a second node: link the PREVIOUSLY selected node as
            // the parent of the clicked one (select parent, ctrl-click child).
            // `prev` must be captured BEFORE btSelected_ is overwritten below,
            // otherwise this degenerates to SetParent(hit, hit) (self-parent,
            // always rejected).
            const std::string prev = btSelected_;
            btSelected_ = hit;
            if (!prev.empty() && prev != hit) {
                const btgraph::BtGraph before = g;
                if (g.SetParent(hit, prev)) PushSnapshot(ctx, before);
            }
        } else if (shift) {
            // Shift+click: detach the clicked node from its parent.
            const btgraph::BtGraph before = g;
            btSelected_ = hit;
            if (g.SetParent(hit, "")) PushSnapshot(ctx, before);
        } else {
            // Plain click: select and begin a potential drag-move.
            btSelected_ = hit;
            btDragNode_ = hit;
            btDragStart_ = cm;
            if (const btgraph::BtGraphNode* hn = g.Find(hit))
                btNodeStartPos_ = hn->pos;
            btGraphBeforeDrag_ = g;
            btHasGraphBeforeDrag_ = true;
            btDragging_ = true;
        }
    } else {
        if (!btPendingType_.empty()) {
            const btgraph::BtGraph before = g;
            const math::Vec2 p(Snap(cm.x - kNodeW * 0.5f), Snap(cm.y - kNodeH * 0.5f));
            const std::string id = g.AddNode(btPendingType_, p);
            if (!id.empty()) {
                btSelected_ = id;
                PushSnapshot(ctx, before);
            }
            btPendingType_.clear();
        } else {
            btSelected_.clear();
        }
    }
}

void BtPanel::BuildParams(EditorContext& ctx) {
    if (btSelected_.empty()) {
        ImGui::TextDisabled("单击画布节点编辑参数");
        return;
    }
    btgraph::BtGraph& g = *ctx.btGraph;
    const btgraph::BtGraphNode* n = g.Find(btSelected_);
    if (!n) {
        btSelected_.clear();
        return;
    }
    const bt::NodeTypeInfo* info = FindTypeInfo(n->type);
    ImGui::TextUnformatted(n->type.c_str());
    ImGui::TextDisabled("路径 %s", g.TreeIdOf(n->id).c_str());
    ImGui::Separator();

    char nameBuf[256];
    std::snprintf(nameBuf, sizeof(nameBuf), "%s", n->name.c_str());
    if (ImGui::InputText("名称", nameBuf, sizeof(nameBuf), ImGuiInputTextFlags_EnterReturnsTrue)) {
        const btgraph::BtGraph before = g;
        g.SetName(btSelected_, nameBuf);
        PushSnapshot(ctx, before);
    }
    ImGui::Separator();

    if (info && !info->params.empty()) {
        for (const auto& p : info->params) {
            switch (p.type) {
                case bt::ParamType::Number: ParamNumber(ctx, *n, p); break;
                case bt::ParamType::String: ParamString(ctx, *n, p); break;
                case bt::ParamType::Bool: ParamBool(ctx, *n, p); break;
                case bt::ParamType::Object:
                case bt::ParamType::Array: ParamJson(ctx, *n, p); break;
            }
        }
    } else {
        ImGui::TextDisabled("该节点没有参数");
    }
    ImGui::Separator();
    if (ImGui::Button("删除节点")) {
        const btgraph::BtGraph before = g;
        g.RemoveNode(btSelected_);
        btSelected_.clear();
        PushSnapshot(ctx, before);
    }
}

void BtPanel::ParamNumber(EditorContext& ctx, const btgraph::BtGraphNode& n,
                          const bt::ParamInfo& p) {
    const core::Json* v = n.args.Get(p.name);
    const double cur = (v && v->IsNumber()) ? v->GetNumber() : 0.0;
    float f = static_cast<float>(cur);
    ImGui::SetNextItemWidth(-1.0f);
    const std::string label = p.name + "##btarg_" + p.name;
    const bool edited = ImGui::DragFloat(label.c_str(), &f, 0.05f, 0.0f, 0.0f, "%.3f");
    // Capture the pre-drag graph on the frame the drag begins, so the undo step
    // reverts to the pre-drag value (one drag = one undo step).
    if (ImGui::IsItemActive() && !btArgDragOrigin_.count(p.name))
        btArgDragOrigin_[p.name] = *ctx.btGraph;
    if (edited) ctx.btGraph->SetArg(n.id, p.name, JNum(static_cast<double>(f)));
    if (ImGui::IsItemDeactivatedAfterEdit() && btArgDragOrigin_.count(p.name)) {
        const btgraph::BtGraph before = btArgDragOrigin_[p.name];
        btArgDragOrigin_.erase(p.name);
        PushSnapshot(ctx, before);
    }
    if (p.required && !v) ImGui::TextColored(ImVec4(1.0f, 0.6f, 0.5f, 1.0f), "必填");
}

void BtPanel::ParamString(EditorContext& ctx, const btgraph::BtGraphNode& n,
                          const bt::ParamInfo& p) {
    const core::Json* v = n.args.Get(p.name);
    char buf[1024];
    std::snprintf(buf, sizeof(buf), "%s", v && v->IsString() ? v->GetString("").c_str() : "");
    ImGui::SetNextItemWidth(-1.0f);
    const std::string label = p.name + "##btarg_" + p.name;
    if (ImGui::InputText(label.c_str(), buf, sizeof(buf),
                         ImGuiInputTextFlags_EnterReturnsTrue)) {
        const btgraph::BtGraph before = *ctx.btGraph;
        ctx.btGraph->SetArg(n.id, p.name, JStr(buf));
        PushSnapshot(ctx, before);
    }
}

void BtPanel::ParamBool(EditorContext& ctx, const btgraph::BtGraphNode& n,
                        const bt::ParamInfo& p) {
    const core::Json* v = n.args.Get(p.name);
    bool cur = v && v->IsBool() ? v->GetBool() : false;
    if (ImGui::Checkbox(p.name.c_str(), &cur)) {
        const btgraph::BtGraph before = *ctx.btGraph;
        ctx.btGraph->SetArg(n.id, p.name, JBool(cur));
        PushSnapshot(ctx, before);
    }
}

void BtPanel::ParamJson(EditorContext& ctx, const btgraph::BtGraphNode& n,
                        const bt::ParamInfo& p) {
    // Raw JSON scalar/object/array edit (used by blackboard_set / *_cmp "value").
    const core::Json* v = n.args.Get(p.name);
    char buf[1024];
    std::snprintf(buf, sizeof(buf), "%s", v ? core::JsonWriter::Write(*v).c_str() : "");
    ImGui::SetNextItemWidth(-1.0f);
    const std::string label = p.name + "##btarg_" + p.name;
    if (ImGui::InputText(label.c_str(), buf, sizeof(buf),
                         ImGuiInputTextFlags_EnterReturnsTrue)) {
        std::string perr;
        core::Json parsed = core::Json::Parse(buf, &perr);
        if (!perr.empty()) {
            NEON_LOG_WARN("BtEditor: invalid JSON for arg '%s': %s", p.name.c_str(),
                          perr.c_str());
        } else {
            const btgraph::BtGraph before = *ctx.btGraph;
            ctx.btGraph->SetArg(n.id, p.name, parsed); // null value removes the key
            PushSnapshot(ctx, before);
        }
    }
}

} // namespace neon::editor
