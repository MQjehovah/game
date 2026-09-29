// 流程图（蓝图式玩法编排）面板实现。模型见 flow_panel.hpp 头注释。

#include "panels/flow_panel.hpp"

#include <algorithm>
#include <sys/stat.h>

#include "neon/script/script.hpp"

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace neon::editor {
namespace {

constexpr float kGrid = 16.0f;
constexpr float kNodeW = 176.0f;
constexpr float kTitleH = 24.0f;


core::Json JsonStr(const std::string& s) {
    core::Json j;
    j.type_ = core::Json::Type::String;
    j.string_ = s;
    return j;
}
core::Json JsonNum(double v) {
    core::Json j;
    j.type_ = core::Json::Type::Number;
    j.number_ = v;
    return j;
}

bool FileExists(const std::string& p) {
    struct _stat64 st;
    return _stat64(p.c_str(), &st) == 0;
}

bool ListFlowFiles(const std::string& dir, std::vector<std::string>& out) {
    out.clear();
    std::string pattern = dir + "/*.flow.json";
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(pattern.c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return false;
    do {
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) {
            std::string n = fd.cFileName;
            // 去掉 .flow.json 后缀
            const size_t cut = n.rfind(".flow.json");
            if (cut != std::string::npos) n = n.substr(0, cut);
            out.push_back(n);
        }
    } while (FindNextFileA(h, &fd));
    FindClose(h);
    std::sort(out.begin(), out.end());
    return true;
}

bool ReadTextFile(const std::string& path, std::string& out) {
    std::ifstream in(path, std::ios::binary);
    if (!in.is_open()) return false;
    out.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    return true;
}

// 数值外观的字符串存为数字（Lua 比较 "5" < 5 会炸；$var 保持字符串）
core::Json ArgFromText(const std::string& text) {
    if (!text.empty() && text[0] == '$') return JsonStr(text);
    char* end = nullptr;
    const double v = std::strtod(text.c_str(), &end);
    if (end && *end == '\0' && !text.empty()) return JsonNum(v);
    return JsonStr(text);
}
std::string TextFromArg(const core::Json& v) {
    if (v.IsNumber()) {
        char buf[32];
        std::snprintf(buf, sizeof(buf), "%g", v.GetNumber());
        return buf;
    }
    return v.GetString();
}

} // namespace

// ---------------------------------------------------------------------------
// 类型目录
// ---------------------------------------------------------------------------
const std::vector<FlowTypeInfo>& FlowTypes() {
    static const std::vector<FlowTypeInfo> types = {
        {"flow/setvar", "设置变量", "流程", {"exec", nullptr, nullptr},
         {{"var", 's'}, {"value", 's'}, {nullptr, 0}}, IM_COL32(70, 120, 220, 255)},
        {"flow/addvar", "变量自增", "流程", {"exec", nullptr, nullptr},
         {{"var", 's'}, {"value", 'n'}, {nullptr, 0}}, IM_COL32(70, 120, 220, 255)},
        {"flow/branch", "条件分支", "流程", {"true", "false", nullptr},
         {{"var", 's'}, {"op", 'o'}, {"value", 's'}, {nullptr, 0}}, IM_COL32(210, 140, 70, 255)},
        {"game/toast", "提示文字", "玩法", {"exec", nullptr, nullptr},
         {{"text", 's'}, {"duration", 'n'}, {nullptr, 0}}, IM_COL32(70, 170, 110, 255)},
        {"game/horde", "尸群刷新", "玩法", {"exec", nullptr, nullptr},
         {{"count", 'n'}, {nullptr, 0}}, IM_COL32(70, 170, 110, 255)},
        {"game/loot", "搜刮材料", "玩法", {"exec", nullptr, nullptr},
         {{"mat", 's'}, {"min", 'n'}, {"max", 'n'}, {nullptr, 0}}, IM_COL32(70, 170, 110, 255)},
        {"game/fuel", "油量增减", "玩法", {"exec", nullptr, nullptr},
         {{"amount", 'n'}, {nullptr, 0}}, IM_COL32(70, 170, 110, 255)},
        {"game/durability", "耐久增减", "玩法", {"exec", nullptr, nullptr},
         {{"amount", 'n'}, {nullptr, 0}}, IM_COL32(70, 170, 110, 255)},
        {"game/animparam", "动画参数", "玩法", {"exec", nullptr, nullptr},
         {{"target", 's'}, {"param", 's'}, {"value", 'n'}, {nullptr, 0}}, IM_COL32(70, 170, 110, 255)},
    };
    return types;
}

const FlowTypeInfo* FindFlowType(const std::string& type) {
    for (const auto& t : FlowTypes())
        if (type == t.type) return &t;
    return nullptr;
}

// ---------------------------------------------------------------------------
// JSON 装载 / 保存
// ---------------------------------------------------------------------------
bool FlowLoadJson(FlowGraph& g, const std::string& text, std::string* err) {
    core::Json def = core::Json::Parse(text, err);
    if (!def.IsObject()) {
        if (err && err->empty()) *err = "not a JSON object";
        return false;
    }
    g = FlowGraph{};
    if (const core::Json* n = def.Get("name")) g.name = n->GetString("untitled");
    if (const core::Json* vars = def.Get("vars")) {
        for (const auto& [k, v] : vars->Members()) g.vars.push_back({k, v});
    }
    if (const core::Json* entry = def.Get("entry")) {
        for (size_t i = 0; i < entry->Size(); ++i) {
            const core::Json* e = entry->At(i);
            if (!e) continue;
            FlowEntry fe;
            fe.type = e->Get("type") ? e->Get("type")->GetString() : "signal";
            fe.name = e->Get("name") ? e->Get("name")->GetString() : "";
            fe.interval = e->Get("interval") ? e->Get("interval")->GetNumber() : 0.0;
            fe.gate = e->Get("gate") ? e->Get("gate")->GetString() : "";
            fe.node = e->Get("node") ? e->Get("node")->GetInt() : 0;
            g.entries.push_back(fe);
        }
    }
    const core::Json* pos = nullptr;
    if (const core::Json* ed = def.Get("editor"))
        if (const core::Json* p = ed->Get("positions")) pos = p;
    if (const core::Json* nodes = def.Get("nodes")) {
        for (size_t i = 0; i < nodes->Size(); ++i) {
            const core::Json* nd = nodes->At(i);
            if (!nd) continue;
            FlowNode fn;
            fn.id = nd->Get("id") ? nd->Get("id")->GetInt() : 0;
            fn.type = nd->Get("type") ? nd->Get("type")->GetString() : "";
            fn.args = JsonObj();
            for (const auto& [k, v] : nd->Members()) {
                if (k == "id" || k == "type") continue;
                fn.args.object_[k] = v;
            }
            if (pos) {
                if (const core::Json* pt = pos->Get(std::to_string(fn.id))) {
                    if (pt->Size() >= 2) {
                        fn.x = static_cast<float>(pt->At(0)->GetNumber(fn.x));
                        fn.y = static_cast<float>(pt->At(1)->GetNumber(fn.y));
                    }
                }
            }
            g.nodes.push_back(fn);
            g.nextId = std::max(g.nextId, fn.id + 1);
        }
    }
    if (const core::Json* links = def.Get("links")) {
        for (size_t i = 0; i < links->Size(); ++i) {
            const core::Json* lk = links->At(i);
            if (!lk) continue;
            FlowLink fl;
            fl.from = lk->Get("from") ? lk->Get("from")->GetInt() : 0;
            fl.out = lk->Get("out") ? lk->Get("out")->GetString("exec") : "exec";
            fl.to = lk->Get("to") ? lk->Get("to")->GetInt() : 0;
            g.links.push_back(fl);
        }
    }
    return true;
}

std::string FlowSaveJson(const FlowGraph& g) {
    core::Json def{core::Json::Type::Object};
    def.object_["name"] = JsonStr(g.name);
    core::Json vars{core::Json::Type::Object};
    for (const auto& v : g.vars) vars.object_[v.name] = v.value;
    def.object_["vars"] = vars;

    core::Json entry{core::Json::Type::Array};
    for (const auto& e : g.entries) {
        core::Json je{core::Json::Type::Object};
        je.object_["type"] = JsonStr(e.type);
        if (e.type == "signal") je.object_["name"] = JsonStr(e.name);
        if (e.type == "timer") {
            je.object_["interval"] = JsonNum(e.interval);
            if (!e.gate.empty()) je.object_["gate"] = JsonStr(e.gate);
        }
        je.object_["node"] = JsonNum(static_cast<double>(e.node));
        entry.array_.push_back(je);
    }
    def.object_["entry"] = entry;

    core::Json nodes{core::Json::Type::Array};
    for (const auto& n : g.nodes) {
        core::Json jn{core::Json::Type::Object};
        jn.object_["id"] = JsonNum(static_cast<double>(n.id));
        jn.object_["type"] = JsonStr(n.type);
        if (n.args.IsObject())
            for (const auto& [k, v] : n.args.Members()) jn.object_[k] = v;
        nodes.array_.push_back(jn);
    }
    def.object_["nodes"] = nodes;

    core::Json links{core::Json::Type::Array};
    for (const auto& l : g.links) {
        core::Json jl{core::Json::Type::Object};
        jl.object_["from"] = JsonNum(static_cast<double>(l.from));
        jl.object_["out"] = JsonStr(l.out);
        jl.object_["to"] = JsonNum(static_cast<double>(l.to));
        links.array_.push_back(jl);
    }
    def.object_["links"] = links;

    core::Json positions{core::Json::Type::Object};
    for (const auto& n : g.nodes) {
        core::Json pt{core::Json::Type::Array};
        pt.array_.push_back(JsonNum(static_cast<double>(n.x)));
        pt.array_.push_back(JsonNum(static_cast<double>(n.y)));
        positions.object_[std::to_string(n.id)] = pt;
    }
    core::Json editor{core::Json::Type::Object};
    editor.object_["positions"] = positions;
    def.object_["editor"] = editor;

    return core::JsonWriter::WritePretty(def);
}

// ---------------------------------------------------------------------------
// 面板
// ---------------------------------------------------------------------------
void FlowPanel::Draw(EditorContext& ctx) {
    if (!ImGui::Begin(Title(), VisibleFlag())) {
        ImGui::End();
        return;
    }
    if (!loaded_ && ctx.projectDir) {
        Load(ctx, fileName_); // 首帧自动载入当前文件
        loaded_ = true;
    }
    RefreshFiles(ctx);
    ImGui::PushStyleColor(ImGuiCol_ChildBg, IM_COL32(30, 32, 38, 255));
    Toolbar(ctx);
    if (ImGui::BeginChild("flow_body", ImVec2(0, 0), 0,
                          ImGuiWindowFlags_NoScrollWithMouse)) {
        // 左：调色板（固定宽）；右：画布 + 检视（上下）
        ImGui::PushStyleColor(ImGuiCol_ChildBg, IM_COL32(38, 40, 46, 255));
        ImGui::BeginChild("flow_left", ImVec2(196, 0));
        Palette(ctx);
        ImGui::EndChild();
        ImGui::PopStyleColor();
        ImGui::SameLine();
        ImGui::BeginChild("flow_right", ImVec2(0, 0), 0,
                          ImGuiWindowFlags_NoScrollbar);
        ImGui::BeginChild("flow_canvas", ImVec2(0, -260), 0,
                          ImGuiWindowFlags_NoScrollbar);
        Canvas(ctx);
        ImGui::EndChild();
        Inspector(ctx);
        ImGui::EndChild();
    }
    ImGui::EndChild();
    ImGui::PopStyleColor();
    ImGui::End();
}

// ---------------------------------------------------------------------------
// 撤销/重做（整图快照，一次交互一步）
// ---------------------------------------------------------------------------
namespace {
class FlowSnapshotCmd : public Command {
public:
    FlowSnapshotCmd(FlowPanel* panel, FlowGraph before, FlowGraph after)
        : panel_(panel), before_(std::move(before)), after_(std::move(after)) {}
    void Apply() override { panel_->RestoreSnapshot(after_); }
    void Undo() override { panel_->RestoreSnapshot(before_); }

private:
    FlowPanel* panel_;
    FlowGraph before_, after_;
};
} // namespace

void FlowPanel::PushHistory(const FlowGraph& before) {
    const FlowGraph after = Snapshot();
    history_.Push(std::make_unique<FlowSnapshotCmd>(this, before, after));
}
void FlowPanel::BeginInteraction() {
    beforeInteract_ = Snapshot();
    hasBeforeInteract_ = true;
}
void FlowPanel::CommitInteraction() {
    if (!hasBeforeInteract_) return;
    hasBeforeInteract_ = false;
    // 有变化才入栈（对比序列化文本）
    if (FlowSaveJson(beforeInteract_) != FlowSaveJson(graph_)) {
        PushHistory(beforeInteract_);
    }
}

// ---------------------------------------------------------------------------
// 运行期调试桥
// ---------------------------------------------------------------------------
int FlowPanel::PollDebugNode(EditorContext& ctx) {
    debugNode_ = 0;
    if (!ctx.playActive || !*ctx.playActive || !ctx.playScriptHost) return 0;
    script::IScriptHost* host = ctx.playScriptHost();
    if (!host) return 0;
    auto r = host->GetGlobal("FLOW_DEBUG_NODE");
    if (!r.Ok()) return 0;
    const std::string s = r.Value().str;
    const size_t c = s.rfind(':');
    if (c == std::string::npos) return 0;
    debugGraph_ = s.substr(0, c);
    debugNode_ = std::atoi(s.substr(c + 1).c_str());
    return debugNode_;
}

void FlowPanel::RefreshFiles(EditorContext& ctx) {
    if (filesRefreshFrame_ && ImGui::GetFrameCount() - filesRefreshFrame_ < 120) return;
    filesRefreshFrame_ = ImGui::GetFrameCount();
    ListFlowFiles(FlowDir(ctx), files_);
}

std::string FlowPanel::FlowDir(const EditorContext& ctx) const {
    return ctx.projectDir ? *ctx.projectDir + "/assets/flow" : "assets/flow";
}

bool FlowPanel::Load(const EditorContext& ctx, const std::string& file) {
    fileName_ = file;
    std::string text;
    if (!ReadTextFile(FlowDir(ctx) + "/" + file + ".flow.json", text)) {
        graph_ = FlowGraph{};
        graph_.name = file;
        std::snprintf(nameBuf_, sizeof(nameBuf_), "%s", graph_.name.c_str());
        return false;
    }
    std::string err;
    if (!FlowLoadJson(graph_, text, &err)) {
        ImGui::TextDisabled("load failed: %s", err.c_str());
        return false;
    }
    std::snprintf(nameBuf_, sizeof(nameBuf_), "%s", graph_.name.c_str());
    selNode_ = 0;
    selEntry_ = -1;
    selVar_ = -1;
    return true;
}

bool FlowPanel::Save(const EditorContext& ctx) {
    graph_.name = nameBuf_;
    const std::string path = FlowDir(ctx) + "/" + fileName_ + ".flow.json";
    std::ofstream out(path, std::ios::binary);
    if (!out.is_open()) return false;
    const std::string text = FlowSaveJson(graph_);
    out.write(text.data(), static_cast<std::streamsize>(text.size()));
    // 播放中：热重载进运行图（保留运行变量/累积器，可边玩边调）
    hotReloadNote_.clear();
    if (ctx.playActive && *ctx.playActive && ctx.playScriptHost) {
        if (script::IScriptHost* host = ctx.playScriptHost()) {
            const auto r = host->Call("FLOW_RELOAD", {script::Value::Str(fileName_)});
            hotReloadNote_ = r.Ok() && r.Value().boolean ? "已热重载"
                                                             : "热重载失败(图未在运行?)";
        }
    }
    return true;
}

void FlowPanel::Toolbar(EditorContext& ctx) {
    if (ImGui::BeginCombo("文件", fileName_.c_str())) {
        for (const auto& f : files_) {
            if (ImGui::Selectable(f.c_str(), f == fileName_)) Load(ctx, f);
        }
        ImGui::EndCombo();
    }
    ImGui::SameLine();
    if (ImGui::Button("新建")) {
        char buf[64];
        std::snprintf(buf, sizeof(buf), "flow%d", static_cast<int>(files_.size() + 1));
        fileName_ = buf;
        graph_ = FlowGraph{};
        graph_.name = buf;
        std::snprintf(nameBuf_, sizeof(nameBuf_), "%s", buf);
    }
    ImGui::SameLine();
    if (ImGui::Button("保存") || ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_S)) {
        if (!Save(ctx)) ImGui::TextDisabled("保存失败（目录不存在？）");
        filesRefreshFrame_ = 0;
    }
    ImGui::SameLine();
    ImGui::BeginDisabled(!history_.CanUndo());
    if (ImGui::Button("撤销") || ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_Z))
        history_.Undo();
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::BeginDisabled(!history_.CanRedo());
    if (ImGui::Button("重做") || ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_Y))
        history_.Redo();
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::SetNextItemWidth(160);
    ImGui::InputText("##name", nameBuf_, sizeof(nameBuf_));
    ImGui::SameLine();
    ImGui::TextDisabled("%zu 节点 · %zu 连线 · %zu 入口 · %zu 变量", graph_.nodes.size(),
                        graph_.links.size(), graph_.entries.size(), graph_.vars.size());
    if (!hotReloadNote_.empty()) {
        ImGui::SameLine();
        ImGui::TextColored(ImVec4(0.5f, 1.f, 0.6f, 1), "%s", hotReloadNote_.c_str());
    }
    if (!pendingType_.empty() || !pendingEntry_.empty()) {
        ImGui::SameLine();
        ImGui::TextColored(ImVec4(0.4f, 1.0f, 0.5f, 1), "点击画布放置: %s",
                           !pendingType_.empty() ? pendingType_.c_str() : pendingEntry_.c_str());
    }
}

void FlowPanel::Palette(EditorContext& ctx) {
    ImGui::TextUnformatted("节点");
    ImGui::Separator();
    const char* lastCat = nullptr;
    for (const auto& t : FlowTypes()) {
        if (lastCat && std::strcmp(lastCat, t.category) != 0) ImGui::Separator();
        lastCat = t.category;
        ImGui::PushStyleColor(ImGuiCol_Header, t.color & 0x00FFFFFF | 0xFF000000);
        ImGui::Selectable(t.label);
        ImGui::PopStyleColor();
        if (ImGui::IsItemActivated()) {
            pendingType_ = t.type;
            pendingEntry_.clear();
        }
    }
    ImGui::Separator();
    ImGui::TextUnformatted("入口");
    const char* entries[] = {"signal 信号", "timer 定时", "tick 每帧"};
    for (const char* e : entries) {
        if (ImGui::Selectable(e)) {
            pendingEntry_ = std::string(e).substr(0, 5);
            pendingType_.clear();
        }
    }
}

void FlowPanel::Canvas(EditorContext& ctx) {
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    const ImVec2 size = ImGui::GetContentRegionAvail();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImGui::InvisibleButton("flow_canvas_grip", size,
                           ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight |
                               ImGuiButtonFlags_MouseButtonMiddle);
    const bool hover = ImGui::IsItemHovered();
    const ImVec2 mouse = ImGui::GetIO().MousePos;

    // 世界 <-> 屏幕
    auto ToScreen = [&](const ImVec2& w) {
        return ImVec2(origin.x + (w.x * zoom_ + pan_.x), origin.y + (w.y * zoom_ + pan_.y));
    };
    auto ToWorld = [&](const ImVec2& s) {
        return ImVec2((s.x - origin.x - pan_.x) / zoom_, (s.y - origin.y - pan_.y) / zoom_);
    };

    // 网格
    const ImU32 gridCol = IM_COL32(45, 48, 55, 255);
    for (float x = std::fmod(pan_.x, kGrid * zoom_); x < size.x; x += kGrid * zoom_)
        dl->AddLine(ImVec2(origin.x + x, origin.y), ImVec2(origin.x + x, origin.y + size.y), gridCol);
    for (float y = std::fmod(pan_.y, kGrid * zoom_); y < size.y; y += kGrid * zoom_)
        dl->AddLine(ImVec2(origin.x, origin.y + y), ImVec2(origin.x + size.x, origin.y + y), gridCol);

    // 缩放
    if (hover) {
        const float wheel = ImGui::GetIO().MouseWheel;
        if (wheel != 0.f) {
            const ImVec2 before = ToWorld(mouse);
            zoom_ = std::clamp(zoom_ * (1.f + wheel * 0.1f), 0.3f, 2.5f);
            const ImVec2 after = ToWorld(mouse);
            pan_.x += (before.x - after.x) * zoom_;
            pan_.y += (before.y - after.y) * zoom_;
        }
    }

    // 布局：每节点屏幕矩形 + 引脚位置
    hits_.clear();
    hits_.reserve(graph_.nodes.size());
    for (const auto& n : graph_.nodes) {
        Hit h;
        h.id = n.id;
        h.info = FindFlowType(n.type);
        const float params = h.info ? [&] {
            int c = 0;
            for (auto p = h.info->params; p->key; ++p) ++c;
            return static_cast<float>(c);
        }() : 1.f;
        const float nodeH = kTitleH + params * 22.f + 8.f;
        h.min = ToScreen(ImVec2(n.x, n.y));
        h.max = ImVec2(h.min.x + kNodeW * zoom_, h.min.y + nodeH * zoom_);
        h.inPos = ImVec2(h.min.x, h.min.y + kTitleH * 0.5f * zoom_ + 6.f);
        int oi = 0;
        if (h.info)
            for (auto o = h.info->outs; *o && oi < 3; ++o, ++oi)
                h.outPos[oi] = ImVec2(h.max.x, h.min.y + (kTitleH + 14.f + oi * 20.f) * zoom_ + 6.f);
        hits_.push_back(h);
    }
    auto HitNode = [&](const ImVec2& s) -> Hit* {
        for (auto& h : hits_)
            if (s.x >= h.min.x && s.x <= h.max.x && s.y >= h.min.y && s.y <= h.max.y) return &h;
        return nullptr;
    };

    // 连线（先画在节点下层）
    for (const auto& l : graph_.links) {
        const Hit* a = nullptr;
        const Hit* b = nullptr;
        for (const auto& h : hits_) {
            if (h.id == l.from) a = &h;
            if (h.id == l.to) b = &h;
        }
        if (!a || !b) continue;
        int oi = 0;
        ImVec2 from = a->outPos[0];
        if (a->info)
            for (auto o = a->info->outs; *o && oi < 3; ++o, ++oi)
                if (l.out == *o) from = a->outPos[oi];
        const ImVec2 to = b->inPos;
        const ImVec2 c1(from.x + 50.f * zoom_, from.y), c2(to.x - 50.f * zoom_, to.y);
        const bool hot = selNode_ == l.from || selNode_ == l.to;
        dl->AddBezierCubic(from, c1, c2, to, hot ? IM_COL32(255, 200, 90, 255) : IM_COL32(150, 160, 175, 220),
                           2.5f);
    }
    // 正在拖的连线
    if (linking_) {
        ImVec2 from{};
        for (const auto& h : hits_)
            if (h.id == linkFrom_) {
                int oi = 0;
                from = h.outPos[0];
                if (h.info)
                    for (auto o = h.info->outs; *o && oi < 3; ++o, ++oi)
                        if (linkOut_ == *o) from = h.outPos[oi];
            }
        dl->AddBezierCubic(from, ImVec2(from.x + 50, from.y), ImVec2(mouse.x - 50, mouse.y), mouse,
                           IM_COL32(120, 220, 140, 220), 2.f);
    }

    // 节点
    badges_.clear();
    for (size_t hi = 0; hi < hits_.size(); ++hi) {
        const Hit& h = hits_[hi];
        const FlowNode& n = graph_.nodes[hi];
        const bool sel = selNode_ == h.id;
        const ImU32 header = h.info ? h.info->color : IM_COL32(90, 96, 110, 255);
        dl->AddRectFilled(h.min, h.max, IM_COL32(44, 47, 54, 245), 6.0f);
        dl->AddRectFilled(h.min, ImVec2(h.max.x, h.min.y + kTitleH * zoom_), header, 6.0f);
        dl->AddRect(h.min, h.max,
                    sel ? IM_COL32(255, 210, 90, 255) : IM_COL32(70, 74, 84, 255), 6.0f,
                    sel ? 2.5f : 1.0f);
        const char* label = h.info ? h.info->label : n.type.c_str();
        dl->AddText(ImVec2(h.min.x + 8, h.min.y + 5 * zoom_), IM_COL32(20, 22, 26, 255), label);
        // 参数预览
        if (h.info) {
            float py = h.min.y + kTitleH * zoom_ + 6.f;
            for (auto p = h.info->params; p->key; ++p) {
                const core::Json* v = n.args.Get(p->key);
                char line[96];
                std::snprintf(line, sizeof(line), "%s: %s", p->key,
                              v ? TextFromArg(*v).c_str() : "-");
                dl->AddText(ImVec2(h.min.x + 8, py), IM_COL32(200, 205, 215, 255), line);
                py += 22.f * zoom_;
            }
        }
        // 引脚
        dl->AddCircleFilled(h.inPos, 5.f * zoom_ + 2.f, IM_COL32(200, 205, 215, 255));
        if (h.info) {
            int oi = 0;
            for (auto o = h.info->outs; *o && oi < 3; ++o, ++oi) {
                dl->AddCircleFilled(h.outPos[oi], 5.f * zoom_ + 2.f, header);
                dl->AddText(ImVec2(h.outPos[oi].x - 42.f * zoom_, h.outPos[oi].y - 7.f),
                            IM_COL32(200, 205, 215, 255), *o);
            }
        }
        // 入口徽标（绿色小牌，可点击选中）
        int badge = 0;
        for (size_t ei = 0; ei < graph_.entries.size(); ++ei) {
            const auto& e = graph_.entries[ei];
            if (e.node != n.id) continue;
            char txt[96];
            if (e.type == "signal") std::snprintf(txt, sizeof(txt), "> %s", e.name.c_str());
            else if (e.type == "timer") std::snprintf(txt, sizeof(txt), "T %.1fs", e.interval);
            else std::snprintf(txt, sizeof(txt), "tick");
            if (!e.gate.empty())
                std::snprintf(txt + std::strlen(txt), sizeof(txt) - std::strlen(txt), " [ %s ]",
                              e.gate.c_str());
            const ImVec2 tl(h.min.x - 8.f, h.min.y - 22.f - badge * 18.f);
            dl->AddText(tl, IM_COL32(110, 230, 130, 255), txt);
            Badge b;
            b.entry = static_cast<int>(ei);
            b.min = tl;
            b.max = ImVec2(tl.x + ImGui::CalcTextSize(txt).x, tl.y + ImGui::GetTextLineHeight());
            badges_.push_back(b);
            ++badge;
        }
    }

    // ---- 连线命中（采样贝塞尔距离） ----
    auto HitLink = [&](const ImVec2& m) -> int {
        for (size_t li = 0; li < graph_.links.size(); ++li) {
            const FlowLink& l = graph_.links[li];
            const Hit* a = nullptr;
            const Hit* b = nullptr;
            for (const auto& h : hits_) {
                if (h.id == l.from) a = &h;
                if (h.id == l.to) b = &h;
            }
            if (!a || !b) continue;
            ImVec2 from = a->outPos[0];
            int oi = 0;
            if (a->info)
                for (auto o = a->info->outs; *o && oi < 3; ++o, ++oi)
                    if (l.out == *o) from = a->outPos[oi];
            const ImVec2 to = b->inPos;
            const ImVec2 c1(from.x + 50.f * zoom_, from.y), c2(to.x - 50.f * zoom_, to.y);
            float best = 1e9f;
            for (int s = 0; s <= 12; ++s) {
                const float t = static_cast<float>(s) / 12.0f;
                const float u = 1.0f - t;
                const ImVec2 pt(u * u * u * from.x + 3 * u * u * t * c1.x + 3 * u * t * t * c2.x +
                                    t * t * t * to.x,
                                u * u * u * from.y + 3 * u * u * t * c1.y + 3 * u * t * t * c2.y +
                                    t * t * t * to.y);
                const float dx = m.x - pt.x, dy = m.y - pt.y;
                best = std::min(best, dx * dx + dy * dy);
            }
            if (best < 64.f) return static_cast<int>(li);
        }
        return -1;
    };

    // ---- 运行期高亮 ----
    const int dbg = PollDebugNode(ctx);

    // ---- 左键 ----
    if (hover && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
        // 入口徽标优先（点徽标=选中该入口，进检视编辑）
        for (const auto& b : badges_) {
            if (mouse.x >= b.min.x - 4 && mouse.x <= b.max.x + 4 && mouse.y >= b.min.y - 4 &&
                mouse.y <= b.max.y + 4) {
                selEntry_ = b.entry;
                selNode_ = 0;
                return;
            }
        }
        Hit* hit = HitNode(mouse);
        if (!pendingType_.empty() && !hit) {
            BeginInteraction();
            FlowNode n;
            n.id = graph_.AllocId();
            n.type = pendingType_;
            const ImVec2 w = ToWorld(mouse);
            n.x = std::floor(w.x / kGrid + 0.5f) * kGrid - kNodeW * 0.5f;
            n.y = std::floor(w.y / kGrid + 0.5f) * kGrid - kTitleH * 0.5f;
            graph_.nodes.push_back(n);
            selNode_ = n.id;
            selEntry_ = -1;
            pendingType_.clear();
            CommitInteraction();
        } else if (!pendingEntry_.empty() && hit) {
            BeginInteraction();
            FlowEntry e;
            e.type = pendingEntry_;
            if (e.type == "signal") e.name = "new_signal";
            if (e.type == "timer") e.interval = 5.0;
            e.node = hit->id;
            graph_.entries.push_back(e);
            pendingEntry_.clear();
            CommitInteraction();
        } else if (hit) {
            selNode_ = hit->id;
            selEntry_ = -1;
            int oi = 0;
            if (hit->info)
                for (auto o = hit->info->outs; *o && oi < 3; ++o, ++oi) {
                    const float dx = mouse.x - hit->outPos[oi].x, dy = mouse.y - hit->outPos[oi].y;
                    if (dx * dx + dy * dy < (10.f * zoom_ + 6.f) * (10.f * zoom_ + 6.f)) {
                        linking_ = true;
                        linkFrom_ = hit->id;
                        linkOut_ = *o;
                    }
                }
            if (!linking_) {
                BeginInteraction();
                dragging_ = true;
                dragNode_ = hit->id;
                dragStart_ = mouse;
                nodeStartPos_ = ImVec2(
                    graph_.nodes[static_cast<size_t>(hit - hits_.data())].x,
                    graph_.nodes[static_cast<size_t>(hit - hits_.data())].y);
            }
        } else {
            selNode_ = 0;
            selEntry_ = -1;
            panning_ = true;
            panStart_ = mouse;
            panOrigin_ = pan_;
        }
    }
    if (linking_ && !ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
        Hit* hit = HitNode(mouse);
        if (hit && hit->id != linkFrom_) {
            BeginInteraction();
            // 每个 out 一条链；每个节点一条入链（运行时只跟第一个匹配）
            for (size_t i = 0; i < graph_.links.size();) {
                if (graph_.links[i].to == hit->id ||
                    (graph_.links[i].from == linkFrom_ && graph_.links[i].out == linkOut_))
                    graph_.links.erase(graph_.links.begin() + static_cast<long>(i));
                else
                    ++i;
            }
            FlowLink l;
            l.from = linkFrom_;
            l.out = linkOut_;
            l.to = hit->id;
            graph_.links.push_back(l);
            CommitInteraction();
        }
        linking_ = false;
    }
    if (dragging_ && !ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
        dragging_ = false;
        CommitInteraction();
    }
    if (dragging_ && ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
        const ImVec2 d((mouse.x - dragStart_.x) / zoom_, (mouse.y - dragStart_.y) / zoom_);
        for (auto& n : graph_.nodes)
            if (n.id == dragNode_) {
                n.x = std::floor((nodeStartPos_.x + d.x) / kGrid + 0.5f) * kGrid;
                n.y = std::floor((nodeStartPos_.y + d.y) / kGrid + 0.5f) * kGrid;
            }
    }
    if (panning_ && ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
        pan_.x = panOrigin_.x + (mouse.x - panStart_.x);
        pan_.y = panOrigin_.y + (mouse.y - panStart_.y);
    } else {
        panning_ = false;
    }

    // ---- 右键菜单 ----
    if (hover && ImGui::IsMouseClicked(ImGuiMouseButton_Right)) {
        Hit* hit = HitNode(mouse);
        const int linkIdx = hit ? -1 : HitLink(mouse);
        ImGui::OpenPopup("flow_ctx");
        ctxNode_ = hit ? hit->id : 0;
        ctxLink_ = linkIdx;
        ctxMouse_ = ToWorld(mouse);
    }
    if (ImGui::BeginPopup("flow_ctx")) {
        if (ctxNode_) {
          FlowNode* cn = graph_.Find(ctxNode_);
          if (cn) {
            const FlowTypeInfo* ti = FindFlowType(cn->type);
            ImGui::TextDisabled("%s", ti ? ti->label : cn->type.c_str());
            if (ImGui::MenuItem("复制节点")) {
                BeginInteraction();
                FlowNode dup = *cn;
                dup.id = graph_.AllocId();
                dup.x += 48.f;
                dup.y += 48.f;
                graph_.nodes.push_back(dup);
                selNode_ = dup.id;
                CommitInteraction();
            }
            if (ImGui::MenuItem("删除节点")) {
                BeginInteraction();
                graph_.RemoveNode(ctxNode_);
                if (selNode_ == ctxNode_) selNode_ = 0;
                CommitInteraction();
            }
          }
        } else if (ctxLink_ >= 0 && ctxLink_ < static_cast<int>(graph_.links.size())) {
            if (ImGui::MenuItem("删除连线")) {
                BeginInteraction();
                graph_.links.erase(graph_.links.begin() + ctxLink_);
                CommitInteraction();
            }
        } else {
            if (ImGui::BeginMenu("在此放置")) {
                for (const auto& t : FlowTypes())
                    if (ImGui::MenuItem(t.label)) {
                        BeginInteraction();
                        FlowNode n;
                        n.id = graph_.AllocId();
                        n.type = t.type;
                        n.x = std::floor(ctxMouse_.x / kGrid + 0.5f) * kGrid - kNodeW * 0.5f;
                        n.y = std::floor(ctxMouse_.y / kGrid + 0.5f) * kGrid - kTitleH * 0.5f;
                        graph_.nodes.push_back(n);
                        selNode_ = n.id;
                        CommitInteraction();
                    }
                ImGui::EndMenu();
            }
        }
        ImGui::EndPopup();
    }

    // ---- 删除键 ----
    if (hover && selNode_ && ImGui::IsKeyPressed(ImGuiKey_Delete, false)) {
        BeginInteraction();
        graph_.RemoveNode(selNode_);
        selNode_ = 0;
        CommitInteraction();
    }

    // ---- 调试高亮（运行期） ----
    if (dbg && debugGraph_ == fileName_) {
        for (const auto& h : hits_)
            if (h.id == dbg) {
                const float pulse = 0.5f + 0.5f * std::sin(ImGui::GetTime() * 6.0f);
                ImU32 c = IM_COL32(120, 255, 140, static_cast<int>(120 + 120 * pulse));
                dl->AddRect(ImVec2(h.min.x - 3, h.min.y - 3), ImVec2(h.max.x + 3, h.max.y + 3), c,
                            8.0f, 0, 3.0f);
                dl->AddText(ImVec2(h.min.x + 4, h.max.y + 2), IM_COL32(120, 255, 140, 255),
                            "● executing");
            }
    }
}

void FlowPanel::Inspector(EditorContext& ctx) {
    // 左列：节点参数 / 入口编辑；右列：变量 + 入口清单
    ImGui::BeginChild("insp_left", ImVec2(ImGui::GetContentRegionAvail().x * 0.5f - 4, 0),
                      0);
    ImGui::TextColored(ImVec4(0.7f, 0.8f, 1.f, 1), "检视");
    ImGui::Separator();
    FlowNode* nodeSel = selNode_ ? graph_.Find(selNode_) : nullptr;
    if (nodeSel) {
        const FlowTypeInfo* info = FindFlowType(nodeSel->type);
        ImGui::Text("%s  (#%d)", info ? info->label : nodeSel->type.c_str(), nodeSel->id);
        ImGui::TextDisabled("%s", nodeSel->type.c_str());
        ImGui::Spacing();
        if (info) {
            for (auto p = info->params; p->key; ++p) {
                ImGui::PushID(p->key);
                ImGui::SetNextItemWidth(-60);
                const core::Json* slot = nodeSel->args.Get(p->key);
                if (p->kind == 'o') {
                    static const char* ops[] = {"<", "<=", ">", ">=", "==", "~="};
                    std::string curv = slot ? slot->GetString("==") : "==";
                    int cur = 4;
                    for (int i = 0; i < 6; ++i)
                        if (curv == ops[i]) cur = i;
                    if (ImGui::Combo(p->key, &cur, ops, 6))
                        nodeSel->args.object_["op"] = JsonStr(ops[cur]);
                } else {
                    char buf[128];
                    std::snprintf(buf, sizeof(buf), "%s", slot ? TextFromArg(*slot).c_str() : "");
                    if (p->kind == 'n') {
                        float v = slot && slot->IsNumber() ? static_cast<float>(slot->GetNumber()) : 0.f;
                        if (ImGui::DragFloat(p->key, &v, 0.1f, 0.f, 0.f, "%.2f"))
                            nodeSel->args.object_[p->key] = JsonNum(static_cast<double>(v));
                    } else if (ImGui::InputText(p->key, buf, sizeof(buf),
                                                ImGuiInputTextFlags_EnterReturnsTrue)) {
                        nodeSel->args.object_[p->key] = ArgFromText(buf);
                    }
                }
                ImGui::PopID();
            }
        }
        ImGui::Spacing();
        if (ImGui::Button("删除节点")) {
            graph_.RemoveNode(selNode_);
            selNode_ = 0;
        }
    } else if (selEntry_ >= 0 && selEntry_ < static_cast<int>(graph_.entries.size())) {
        FlowEntry& e = graph_.entries[static_cast<size_t>(selEntry_)];
        ImGui::Text("入口： %s", e.type.c_str());
        if (e.type == "signal") {
            char nb[96];
            std::snprintf(nb, sizeof(nb), "%s", e.name.c_str());
            ImGui::SetNextItemWidth(-60);
            if (ImGui::InputText("信号名", nb, sizeof(nb), ImGuiInputTextFlags_EnterReturnsTrue))
                e.name = nb;
        } else if (e.type == "timer") {
            ImGui::SetNextItemWidth(-60);
            float iv = static_cast<float>(e.interval);
            if (ImGui::DragFloat("间隔秒", &iv, 0.1f, 0.05f, 600.f, "%.1f")) e.interval = iv;
            char gb[64];
            std::snprintf(gb, sizeof(gb), "%s", e.gate.c_str());
            ImGui::SetNextItemWidth(-60);
            if (ImGui::InputText("变量门(可空)", gb, sizeof(gb),
                                 ImGuiInputTextFlags_EnterReturnsTrue))
                e.gate = gb;
        }
        ImGui::TextDisabled("绑定节点 #%d（画布上点节点重接）", e.node);
        if (ImGui::Button("删除入口")) {
            graph_.entries.erase(graph_.entries.begin() + selEntry_);
            selEntry_ = -1;
        }
    } else {
        ImGui::TextDisabled("选中节点或入口徽标编辑（点击节点上方绿色入口文字）");
    }
    ImGui::EndChild();
    ImGui::SameLine();
    ImGui::BeginChild("insp_right", ImVec2(0, 0), 0);

    // 变量
    ImGui::TextColored(ImVec4(1.f, 0.85f, 0.6f, 1), "变量");
    ImGui::Separator();
    static char newName[64] = "";
    ImGui::SetNextItemWidth(90);
    ImGui::InputText("##vname", newName, sizeof(newName));
    ImGui::SameLine();
    if (ImGui::Button("+") && newName[0]) {
        graph_.vars.push_back({newName, JsonNum(0.0)});
        newName[0] = '\0';
    }
    for (size_t i = 0; i < graph_.vars.size(); ++i) {
        ImGui::PushID(static_cast<int>(i));
        FlowVar& v = graph_.vars[i];
        char nb[64];
        std::snprintf(nb, sizeof(nb), "%s", v.name.c_str());
        ImGui::SetNextItemWidth(90);
        if (ImGui::InputText("##n", nb, sizeof(nb), ImGuiInputTextFlags_EnterReturnsTrue))
            v.name = nb;
        ImGui::SameLine();
        ImGui::SetNextItemWidth(80);
        if (v.value.IsNumber()) {
            float f = static_cast<float>(v.value.GetNumber());
            if (ImGui::DragFloat("##v", &f, 0.1f, 0.f, 0.f, "%.2f")) v.value = JsonNum(static_cast<double>(f));
        } else {
            char b[96];
            std::snprintf(b, sizeof(b), "%s", v.value.GetString().c_str());
            if (ImGui::InputText("##v", b, sizeof(b), ImGuiInputTextFlags_EnterReturnsTrue))
                v.value = ArgFromText(b);
        }
        ImGui::SameLine();
        if (ImGui::SmallButton("x")) {
            graph_.vars.erase(graph_.vars.begin() + static_cast<long>(i));
            ImGui::PopID();
            break;
        }
        ImGui::PopID();
    }

    // 入口清单
    ImGui::Spacing();
    ImGui::TextColored(ImVec4(0.6f, 1.f, 0.7f, 1), "入口");
    ImGui::Separator();
    for (size_t i = 0; i < graph_.entries.size(); ++i) {
        const FlowEntry& e = graph_.entries[static_cast<size_t>(i)];
        char line[128];
        if (e.type == "signal") std::snprintf(line, sizeof(line), "signal %s -> #%d", e.name.c_str(), e.node);
        else if (e.type == "timer")
            std::snprintf(line, sizeof(line), "timer %.1fs%s -> #%d", e.interval,
                          e.gate.empty() ? "" : ("[" + e.gate + "]").c_str(), e.node);
        else std::snprintf(line, sizeof(line), "tick -> #%d", e.node);
        const bool sel = selEntry_ == static_cast<int>(i);
        if (ImGui::Selectable(line, sel)) {
            selEntry_ = static_cast<int>(i);
            selNode_ = 0;
        }
    }
    ImGui::TextDisabled("入口绑定：调色板选入口类型后点目标节点");
    ImGui::EndChild();
}

} // namespace neon::editor
