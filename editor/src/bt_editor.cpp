// Behavior tree visual editor (T4.4): the docked 行为树 panel UI.
//
// Canvas rendering + interaction (drag-move, link, delete), the node palette
// (drag or click-to-place), per-node param editing, toolbar save/open/new, and
// the play debug highlight. The pure node graph model lives inline in
// bt_editor.hpp so tests can include it headlessly; this file is the ImGui
// layer exercised by the editor smoke test.

#include "bt_editor.hpp"

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <direct.h>
#include <windows.h>
#else
#include <dirent.h>
#include <sys/stat.h>
#endif

#include "editor.hpp"
#include "imgui_internal.h"
#include "neon/bt/behavior_tree.hpp"
#include "neon/scene/scene_file.hpp"

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

bool EnsureDir(const std::string& path) {
#if defined(_WIN32)
    return _mkdir(path.c_str()) == 0 || errno == EEXIST;
#else
    return ::mkdir(path.c_str(), 0777) == 0 || errno == EEXIST;
#endif
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

void EditorApp::BtPushSnapshot(const btgraph::BtGraph& before) {
    if (GraphsEqual(before, btGraph_)) return; // no-op change: no undo step
    btHistory_.Push(std::make_unique<BtGraphSnapshotCommand>(&btGraph_, before, btGraph_));
}

std::string EditorApp::BtBehaviorsDir() const {
    std::string base = projectDir_.empty() ? "." : projectDir_;
    return base + "/assets/behaviors";
}

void EditorApp::BtRefreshBehaviorFiles() {
    btBehaviorFiles_ = ListBtFiles(BtBehaviorsDir());
}

void EditorApp::BtNewTree() {
    btGraph_ = btgraph::BtGraph{};
    btHistory_.Clear();
    btSelected_.clear();
    btPendingType_.clear();
    btFileName_ = "behavior";
    std::snprintf(btFileNameBuf_, sizeof(btFileNameBuf_), "%s", btFileName_.c_str());
    NEON_LOG_INFO("BtEditor: new empty tree");
}

bool EditorApp::BtSaveToFile(const std::string& path) {
    // The engine loader rejects empty composites ("sequence requires at least
    // one child"), childless decorators and a missing root. Refuse to write a
    // file the runtime cannot load, and say exactly which node is wrong. An
    // alternative (auto-injecting a `wait` placeholder) would silently change
    // the user's graph on save, so we prefer the explicit refusal.
    if (btGraph_.Empty()) {
        NEON_LOG_WARN("BtEditor: cannot save an empty tree (add a root node first)");
        return false;
    }
    for (const auto& n : btGraph_.Nodes()) {
        const int cap = bt::ChildCapacity(n.type);
        if (cap != -1 && cap != 1) continue; // actions / conditions take no children
        bool hasChild = false;
        for (const auto& l : btGraph_.Links())
            if (l.parent == n.id) {
                hasChild = true;
                break;
            }
        if (!hasChild) {
            NEON_LOG_WARN("BtEditor: '%s' needs %s child before saving (fix or delete it)",
                          n.type.c_str(), cap == 1 ? "exactly one" : "at least one");
            return false;
        }
    }

    std::string dir = path;
    size_t slash = dir.find_last_of("/\\");
    if (slash == std::string::npos) dir = ".";
    else dir = dir.substr(0, slash);
    EnsureDir(dir);
    std::ofstream out(path, std::ios::binary);
    if (!out.is_open()) {
        NEON_LOG_WARN("BtEditor: cannot write '%s'", path.c_str());
        return false;
    }
    out << btGraph_.Serialize();
    NEON_LOG_INFO("BtEditor: saved %zu nodes -> %s", btGraph_.NodeCount(), path.c_str());
    return true;
}

bool EditorApp::BtLoadFromFile(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in.is_open()) {
        NEON_LOG_WARN("BtEditor: cannot open '%s'", path.c_str());
        return false;
    }
    std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    std::string err;
    core::Json dom = core::Json::Parse(text, &err);
    if (dom.IsNull()) {
        NEON_LOG_WARN("BtEditor: '%s' is not valid JSON: %s", path.c_str(), err.c_str());
        return false;
    }
    btgraph::BtGraph g;
    if (!g.FromTreeJson(dom)) return false;
    btGraph_ = std::move(g);
    btHistory_.Clear();
    btSelected_.clear();
    btPendingType_.clear();
    NEON_LOG_INFO("BtEditor: loaded %zu nodes from %s", btGraph_.NodeCount(), path.c_str());
    return true;
}

void EditorApp::BtUpdatePlayHighlight() {
    btActivePath_.clear();
    if (!playActive_ || !play_) return;
    auto view = play_->World().ViewAll<scene::SceneBehaviorTree>();
    if (view.Size() == 0) return;
    ecs::Entity e = play_->World().EntityAt<scene::SceneBehaviorTree>(0);
    btActivePath_ = play_->ActiveTreePath(e);
}

void EditorApp::BuildBtPanel() {
    if (!showBt_) {
        btPanelFocused_ = false; // panel closed: never route undo to a stale focus
        return;
    }
    BtUpdatePlayHighlight();
    // The panel hosts a full node canvas: never let a stale saved size (or the
    // auto-size feedback loop between the canvas child and the window) shrink
    // it below a usable area.
    ImGui::SetNextWindowSize(ImVec2(1040.0f, 560.0f), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSizeConstraints(ImVec2(720.0f, 360.0f),
                                        ImVec2(FLT_MAX, FLT_MAX));
    if (!ImGui::Begin("行为树", &showBt_)) {
        btPanelFocused_ = false;
        ImGui::End();
        return;
    }
    btPanelFocused_ = ImGui::IsWindowFocused(ImGuiFocusedFlags_ChildWindows);
    BuildBtToolbar();

    const float availW = ImGui::GetContentRegionAvail().x;
    const float availH = ImGui::GetContentRegionAvail().y;
    ImGui::BeginChild("##bt_palette", ImVec2(180.0f, availH), ImGuiChildFlags_Borders);
    BuildBtPalette();
    ImGui::EndChild();
    ImGui::SameLine();
    ImGui::BeginChild("##bt_params", ImVec2(260.0f, availH), ImGuiChildFlags_Borders);
    BuildBtParams();
    ImGui::EndChild();
    ImGui::SameLine();
    const float canvasW = std::max(160.0f, availW - 180.0f - 260.0f);
    ImGui::BeginChild("##bt_canvas", ImVec2(canvasW, availH), ImGuiChildFlags_Borders);
    BuildBtCanvas();
    ImGui::EndChild();
    ImGui::End();
}

void EditorApp::BuildBtToolbar() {
    if (ImGui::Button("新建")) BtNewTree();
    ImGui::SameLine();
    if (ImGui::Button("打开") && btFileNameBuf_[0]) {
        if (!BtLoadFromFile(BtBehaviorsDir() + "/" + btFileName_ + ".bt.json"))
            NEON_LOG_WARN("BtEditor: failed to open '%s.bt.json'", btFileName_.c_str());
    }
    ImGui::SameLine();
    if (ImGui::Button("保存") && btFileNameBuf_[0]) {
        if (!BtSaveToFile(BtBehaviorsDir() + "/" + btFileName_ + ".bt.json"))
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
        btgraph::BtGraph before = btGraph_;
        btGraph_.LayoutTopDown();
        BtPushSnapshot(before);
    }
    ImGui::SameLine();
    if (ImGui::Button("撤销") && btHistory_.CanUndo()) btHistory_.Undo();
    ImGui::SameLine();
    if (ImGui::Button("重做") && btHistory_.CanRedo()) btHistory_.Redo();
    ImGui::SameLine();
    if (ImGui::Button("删除节点") && !btSelected_.empty()) {
        btgraph::BtGraph before = btGraph_;
        btGraph_.RemoveNode(btSelected_);
        btSelected_.clear();
        BtPushSnapshot(before);
    }
    ImGui::SameLine();
    if (playActive_ && !btActivePath_.empty()) {
        ImGui::TextColored(ImVec4(0.4f, 1.0f, 0.5f, 1.0f), "执行中 %s", btActivePath_.c_str());
    }
    ImGui::Separator();

    // Throttle the behaviors/ directory scan: it runs on panel open and every
    // ~1s of frames (60 @ 60fps), never per frame.
    const uint64_t now = TimeRef().frameIndex;
    if (now - btFilesRefreshFrame_ >= 60 || btBehaviorFiles_.empty()) {
        BtRefreshBehaviorFiles();
        btFilesRefreshFrame_ = now;
    }
    if (!btBehaviorFiles_.empty()) {
        ImGui::TextDisabled("assets/behaviors/");
        for (const auto& f : btBehaviorFiles_) {
            char label[512];
            std::snprintf(label, sizeof(label), "%s##btfile_%s", f.c_str(), f.c_str());
            if (ImGui::Selectable(label)) {
                if (!BtLoadFromFile(BtBehaviorsDir() + "/" + f))
                    NEON_LOG_WARN("BtEditor: failed to load '%s'", f.c_str());
            }
        }
        ImGui::Separator();
    }
}

void EditorApp::BuildBtPalette() {
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

void EditorApp::BuildBtCanvas() {
    btCanvasDrawn_ = false;
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    ImGui::InvisibleButton("##bt_canvas_region", ImVec2(kCanvasW, kCanvasH));
    const bool hovered = ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenBlockedByActiveItem);
    const ImVec2 toCanvas(origin.x - ImGui::GetScrollX(), origin.y - ImGui::GetScrollY());

    ImDrawList* dl = ImGui::GetWindowDrawList();
    const int vtxBefore = dl->VtxBuffer.Size;

    // --- links: parent bottom -> child top bezier ---
    for (const auto& l : btGraph_.Links()) {
        const btgraph::BtGraphNode* p = btGraph_.Find(l.parent);
        const btgraph::BtGraphNode* c = btGraph_.Find(l.child);
        if (!p || !c) continue;
        const ImVec2 p0(toCanvas.x + p->pos.x + kNodeW * 0.5f, toCanvas.y + p->pos.y + kNodeH);
        const ImVec2 p1(toCanvas.x + c->pos.x + kNodeW * 0.5f, toCanvas.y + c->pos.y);
        const ImVec2 cp0(p0.x, p0.y + 46.0f);
        const ImVec2 cp1(p1.x, p1.y - 46.0f);
        const bool active = playActive_ && btGraph_.TreeIdOf(c->id) == btActivePath_;
        dl->AddBezierCubic(p0, cp0, cp1, p1,
                           active ? IM_COL32(80, 255, 120, 255) : IM_COL32(150, 160, 180, 255),
                           2.0f);
    }

    // --- nodes ---
    for (const auto& n : btGraph_.Nodes()) {
        const ImVec2 pos(toCanvas.x + n.pos.x, toCanvas.y + n.pos.y);
        const ImRect rect(pos, ImVec2(pos.x + kNodeW, pos.y + kNodeH));
        const bool selected = btSelected_ == n.id;
        const bool active = playActive_ && btGraph_.TreeIdOf(n.id) == btActivePath_;
        const bt::NodeTypeInfo* info = FindTypeInfo(n.type);
        const ImU32 cat = CatColor(info ? info->category.c_str() : nullptr);

        dl->AddRectFilled(rect.Min, rect.Max, IM_COL32(40, 44, 56, 255), 6.0f);
        dl->AddRectFilled(rect.Min, ImVec2(rect.Max.x, rect.Min.y + 24.0f), cat, 6.0f);
        dl->AddRectFilled(ImVec2(rect.Min.x, rect.Min.y + 16.0f),
                          ImVec2(rect.Max.x, rect.Min.y + 24.0f), IM_COL32(40, 44, 56, 255));
        const ImU32 border = active ? IM_COL32(80, 255, 120, 255)
                                    : selected ? IM_COL32(255, 200, 80, 255)
                                               : IM_COL32(120, 130, 150, 255);
        dl->AddRect(rect.Min, rect.Max, border, 6.0f, ImDrawFlags_None,
                    (selected || active) ? 2.5f : 1.0f);

        const std::string title = n.name.empty() ? n.type : n.name;
        dl->AddText(ImVec2(pos.x + 7.0f, pos.y + 4.0f), IM_COL32(255, 255, 255, 255),
                    title.c_str());
        if (info) {
            std::string line;
            for (size_t i = 0; i < info->params.size() && i < 2; ++i) {
                if (!line.empty()) line += "  ";
                line += info->params[i].name + "=" + ArgText(n, info->params[i]);
            }
            if (!line.empty())
                dl->AddText(ImVec2(pos.x + 7.0f, pos.y + 30.0f),
                            IM_COL32(200, 205, 215, 255), line.c_str());
        }
    }
    // Vertices are the reliable signal: ImGui batches same-clip-rect items into
    // one command, so CmdBuffer may not grow even when nodes/links drew.
    btCanvasDrawn_ = dl->VtxBuffer.Size > vtxBefore;

    // --- interaction ---
    if (hovered) {
        const ImVec2 mouse = ImGui::GetIO().MousePos;
        const math::Vec2 cm(mouse.x - toCanvas.x, mouse.y - toCanvas.y);

        if (ImGui::BeginDragDropTarget()) {
            if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("BT_PALETTE")) {
                std::string type(static_cast<const char*>(payload->Data),
                                 static_cast<size_t>(payload->DataSize) - 1);
                btgraph::BtGraph before = btGraph_;
                const math::Vec2 p(Snap(cm.x - kNodeW * 0.5f), Snap(cm.y - kNodeH * 0.5f));
                const std::string id = btGraph_.AddNode(type, p);
                if (!id.empty()) {
                    btSelected_ = id;
                    BtPushSnapshot(before);
                }
                btPendingType_.clear();
            }
            ImGui::EndDragDropTarget();
        }

        const std::string hit = HitTest(btGraph_, cm);
        if (!btDragging_ && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
            BtCanvasClick(cm, ImGui::GetIO().KeyCtrl, ImGui::GetIO().KeyShift);
        }

        if (!btSelected_.empty() && ImGui::IsKeyPressed(ImGuiKey_Delete, false) &&
            !ImGui::GetIO().WantTextInput) {
            const btgraph::BtGraph before = btGraph_;
            btGraph_.RemoveNode(btSelected_);
            btSelected_.clear();
            BtPushSnapshot(before);
        }
    }

    // Drag-move continues even when the pointer leaves the canvas.
    if (btDragging_) {
        const ImVec2 mouse = ImGui::GetIO().MousePos;
        const math::Vec2 cm(mouse.x - toCanvas.x, mouse.y - toCanvas.y);
        if (ImGui::IsMouseDragging(ImGuiMouseButton_Left, 2.0f) && !btDragNode_.empty()) {
            const math::Vec2 delta(cm.x - btDragStart_.x, cm.y - btDragStart_.y);
            btGraph_.SetPos(btDragNode_, btNodeStartPos_ + delta);
        }
        if (ImGui::IsMouseReleased(ImGuiMouseButton_Left)) {
            const btgraph::BtGraphNode* nd = btGraph_.Find(btDragNode_);
            const bool moved = nd && (nd->pos.x != btNodeStartPos_.x ||
                                      nd->pos.y != btNodeStartPos_.y);
            if (moved && btHasGraphBeforeDrag_) BtPushSnapshot(btGraphBeforeDrag_);
            btHasGraphBeforeDrag_ = false;
            btDragging_ = false;
            btDragNode_.clear();
        }
    }
}

void EditorApp::BtCanvasClick(const math::Vec2& cm, bool ctrl, bool shift) {
    const std::string hit = HitTest(btGraph_, cm);
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
                const btgraph::BtGraph before = btGraph_;
                if (btGraph_.SetParent(hit, prev)) BtPushSnapshot(before);
            }
        } else if (shift) {
            // Shift+click: detach the clicked node from its parent.
            const btgraph::BtGraph before = btGraph_;
            btSelected_ = hit;
            if (btGraph_.SetParent(hit, "")) BtPushSnapshot(before);
        } else {
            // Plain click: select and begin a potential drag-move.
            btSelected_ = hit;
            btDragNode_ = hit;
            btDragStart_ = cm;
            if (const btgraph::BtGraphNode* hn = btGraph_.Find(hit))
                btNodeStartPos_ = hn->pos;
            btGraphBeforeDrag_ = btGraph_;
            btHasGraphBeforeDrag_ = true;
            btDragging_ = true;
        }
    } else {
        if (!btPendingType_.empty()) {
            const btgraph::BtGraph before = btGraph_;
            const math::Vec2 p(Snap(cm.x - kNodeW * 0.5f), Snap(cm.y - kNodeH * 0.5f));
            const std::string id = btGraph_.AddNode(btPendingType_, p);
            if (!id.empty()) {
                btSelected_ = id;
                BtPushSnapshot(before);
            }
            btPendingType_.clear();
        } else {
            btSelected_.clear();
        }
    }
}

void EditorApp::BuildBtParams() {
    if (btSelected_.empty()) {
        ImGui::TextDisabled("单击画布节点编辑参数");
        return;
    }
    const btgraph::BtGraphNode* n = btGraph_.Find(btSelected_);
    if (!n) {
        btSelected_.clear();
        return;
    }
    const bt::NodeTypeInfo* info = FindTypeInfo(n->type);
    ImGui::TextUnformatted(n->type.c_str());
    ImGui::TextDisabled("路径 %s", btGraph_.TreeIdOf(n->id).c_str());
    ImGui::Separator();

    char nameBuf[256];
    std::snprintf(nameBuf, sizeof(nameBuf), "%s", n->name.c_str());
    if (ImGui::InputText("名称", nameBuf, sizeof(nameBuf), ImGuiInputTextFlags_EnterReturnsTrue)) {
        const btgraph::BtGraph before = btGraph_;
        btGraph_.SetName(btSelected_, nameBuf);
        BtPushSnapshot(before);
    }
    ImGui::Separator();

    if (info && !info->params.empty()) {
        for (const auto& p : info->params) {
            switch (p.type) {
                case bt::ParamType::Number: BtParamNumber(*n, p); break;
                case bt::ParamType::String: BtParamString(*n, p); break;
                case bt::ParamType::Bool: BtParamBool(*n, p); break;
                case bt::ParamType::Object:
                case bt::ParamType::Array: BtParamJson(*n, p); break;
            }
        }
    } else {
        ImGui::TextDisabled("该节点没有参数");
    }
    ImGui::Separator();
    if (ImGui::Button("删除节点")) {
        const btgraph::BtGraph before = btGraph_;
        btGraph_.RemoveNode(btSelected_);
        btSelected_.clear();
        BtPushSnapshot(before);
    }
}

void EditorApp::BtParamNumber(const btgraph::BtGraphNode& n, const bt::ParamInfo& p) {
    const core::Json* v = n.args.Get(p.name);
    const double cur = (v && v->IsNumber()) ? v->GetNumber() : 0.0;
    float f = static_cast<float>(cur);
    ImGui::SetNextItemWidth(-1.0f);
    const std::string label = p.name + "##btarg_" + p.name;
    const bool edited = ImGui::DragFloat(label.c_str(), &f, 0.05f, 0.0f, 0.0f, "%.3f");
    // Capture the pre-drag graph on the frame the drag begins, so the undo step
    // reverts to the pre-drag value (one drag = one undo step).
    if (ImGui::IsItemActive() && !btArgDragOrigin_.count(p.name))
        btArgDragOrigin_[p.name] = btGraph_;
    if (edited) btGraph_.SetArg(n.id, p.name, JNum(static_cast<double>(f)));
    if (ImGui::IsItemDeactivatedAfterEdit() && btArgDragOrigin_.count(p.name)) {
        const btgraph::BtGraph before = btArgDragOrigin_[p.name];
        btArgDragOrigin_.erase(p.name);
        BtPushSnapshot(before);
    }
    if (p.required && !v) ImGui::TextColored(ImVec4(1.0f, 0.6f, 0.5f, 1.0f), "必填");
}

void EditorApp::BtParamString(const btgraph::BtGraphNode& n, const bt::ParamInfo& p) {
    const core::Json* v = n.args.Get(p.name);
    char buf[1024];
    std::snprintf(buf, sizeof(buf), "%s", v && v->IsString() ? v->GetString("").c_str() : "");
    ImGui::SetNextItemWidth(-1.0f);
    const std::string label = p.name + "##btarg_" + p.name;
    if (ImGui::InputText(label.c_str(), buf, sizeof(buf),
                         ImGuiInputTextFlags_EnterReturnsTrue)) {
        const btgraph::BtGraph before = btGraph_;
        btGraph_.SetArg(n.id, p.name, JStr(buf));
        BtPushSnapshot(before);
    }
}

void EditorApp::BtParamBool(const btgraph::BtGraphNode& n, const bt::ParamInfo& p) {
    const core::Json* v = n.args.Get(p.name);
    bool cur = v && v->IsBool() ? v->GetBool() : false;
    if (ImGui::Checkbox(p.name.c_str(), &cur)) {
        const btgraph::BtGraph before = btGraph_;
        btGraph_.SetArg(n.id, p.name, JBool(cur));
        BtPushSnapshot(before);
    }
}

void EditorApp::BtParamJson(const btgraph::BtGraphNode& n, const bt::ParamInfo& p) {
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
            const btgraph::BtGraph before = btGraph_;
            btGraph_.SetArg(n.id, p.name, parsed); // null value removes the key
            BtPushSnapshot(before);
        }
    }
}

} // namespace neon::editor
