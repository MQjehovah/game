#pragma once

// 流程图（蓝图式玩法编排）面板 — 与 lc_flow.lua 运行时同格式的可视化编辑器。
//
// 数据模型与 JSON 格式（projects/rv/assets/flow/*.flow.json）一一对应：
//   { name, vars, entry:[{type:signal|timer|tick, name?, interval?, gate?, node}],
//     nodes:[{id, type, ...args}], links:[{from, out, to}]（in 恒为 exec）,
//     editor:{ positions:{"id":[x,y]} } }
// 节点参数存 core::Json（字符串/数字），保存时写回除 id/type 外的字段。
//
// 面板自治（ImGui 边界同其它面板）：调色板选类型 → 画布点击放置；输出锚点
// 拖到目标节点连线（每个 out 只许一条链、每个节点只许一条入链，与运行时
// “沿第一个匹配 out 深走”的语义一致）；选中节点在下方面板编参数；变量与
// 入口在侧栏编辑。Ctrl+S / 工具栏保存到 assets/flow/<name>.flow.json。

#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

#include "editor_context.hpp"
#include "history.hpp"
#include "imgui.h"
#include "neon/core/json.hpp"

namespace neon::editor {

// 构造空 JSON 对象（Json 无 Type 构造函数，花括号单参初始化不合法）
inline core::Json JsonObj() {
    core::Json j;
    j.type_ = core::Json::Type::Object;
    return j;
}

// --- 数据模型（纯数据，无 ImGui 依赖） --------------------------------------
struct FlowNode {
    int id = 0;
    std::string type;
    core::Json args = JsonObj();
    float x = 0.f, y = 0.f;
};
struct FlowLink {
    int from = 0, to = 0;
    std::string out = "exec";
};
struct FlowEntry {
    std::string type;      // signal | timer | tick
    std::string name;      // signal 名
    double interval = 0.0; // timer
    std::string gate;      // 变量门（非空时 gate 变量为假则暂停）
    int node = 0;
};
struct FlowVar {
    std::string name;
    core::Json value;
};
// 分组框（UE 蓝图注释盒）：编辑器元数据，运行时忽略；成员按"节点矩形完全在
// 组内"判定，拖组 = 组+成员一起动。
struct FlowGroup {
    std::string title = "分组";
    float x = 0.f, y = 0.f, w = 320.f, h = 160.f;
    int rgb = 0x4a7dc9; // 边框/标题颜色
};

struct FlowGraph {
    std::string name = "untitled";
    std::vector<FlowVar> vars;
    std::vector<FlowEntry> entries;
    std::vector<FlowNode> nodes;
    std::vector<FlowLink> links;
    int nextId = 1;

    FlowNode* Find(int id) {
        for (auto& n : nodes)
            if (n.id == id) return &n;
        return nullptr;
    }
    int AllocId() { return nextId++; }
    void RemoveNode(int id) {
        for (size_t i = 0; i < nodes.size(); ++i)
            if (nodes[i].id == id) {
                nodes.erase(nodes.begin() + static_cast<long>(i));
                break;
            }
        for (size_t i = 0; i < links.size();) {
            if (links[i].from == id || links[i].to == id)
                links.erase(links.begin() + static_cast<long>(i));
            else
                ++i;
        }
        for (auto& e : entries)
            if (e.node == id) e.node = 0; // 入口保留但解绑（编辑器可再接）
    }
};

// --- 节点类型目录（调色板 + 引脚 + 参数规格；与 lc_flow/lc_flowgame 对齐） ---
struct FlowParam {
    const char* key;
    char kind; // s=文本 n=数值 o=比较符
};
struct FlowTypeInfo {
    const char* type;
    const char* label;
    const char* category; // 流程 / 玩法
    const char* outs[3];  // 输出引脚名（nullptr 结尾）
    FlowParam params[5];  // 参数（{nullptr,0} 结尾）
    ImU32 color;
};
const std::vector<FlowTypeInfo>& FlowTypes();
const FlowTypeInfo* FindFlowType(const std::string& type);
// 扫描项目 scripts 下的 LC.flow.describe(...) 声明，并入类型目录（静态目录优先）
void ScanFlowDescriptions(const std::string& scriptsDir);

// --- JSON 装载/保存（与 lc_flow.load 读取的格式逐字段一致） ------------------
bool FlowLoadJson(FlowGraph& g, const std::string& text, std::string* err = nullptr);
std::string FlowSaveJson(const FlowGraph& g);

// --- 面板 --------------------------------------------------------------------
class FlowPanel : public IPanel {
public:
    explicit FlowPanel(bool* visibleFlag) : visible_(visibleFlag) {}

    const char* Title() const override { return "流程图"; }
    bool* VisibleFlag() override { return visible_; }
    void Draw(EditorContext& ctx) override;
    // 撤销命令回调（FlowSnapshotCmd 需要公开访问）
    void RestoreSnapshot(const FlowGraph& g) { graph_ = g; }

private:
    // UI 分区
    void Toolbar(EditorContext& ctx);
    void Palette(EditorContext& ctx);
    void Canvas(EditorContext& ctx);
    void Inspector(EditorContext& ctx); // 参数 / 变量 / 入口
    void RefreshFiles(EditorContext& ctx);

    // 文件
    std::string FlowDir(const EditorContext& ctx) const;
    bool Load(const EditorContext& ctx, const std::string& file);
    bool Save(const EditorContext& ctx);

    // 撤销/重做（整图快照）
    FlowGraph Snapshot() const { return graph_; }
    void PushHistory(const FlowGraph& before);
    void BeginInteraction();   // 交互开始时抓快照（拖动/改参数前）
    void CommitInteraction();  // 交互结束且有变化时入栈
    // 运行期调试：读 FLOW_DEBUG_NODE（"图名:节点id"）→ 高亮；返回节点 id
    int PollDebugNode(EditorContext& ctx);

    bool* visible_ = nullptr;

    FlowGraph graph_;
    HistoryManager history_;
    FlowGraph beforeInteract_;
    bool hasBeforeInteract_ = false;
    std::string fileName_ = "raid_wave"; // 无扩展名
    std::string hotReloadNote_;          // 保存时热重载的结果提示
    char nameBuf_[128]{};
    std::vector<std::string> files_;
    uint64_t filesRefreshFrame_ = 0;
    bool loaded_ = false;

    // 画布视图
    float zoom_ = 1.0f;
    ImVec2 pan_{40.f, 40.f};
    // 选中：节点 id > 0；入口 selEntry_ >= 0；变量 selVar_ >= 0
    int selNode_ = 0;
    int selEntry_ = -1;
    int selVar_ = -1;
    // 交互状态
    bool dragging_ = false;
    int dragNode_ = 0;
    ImVec2 dragStart_{}, nodeStartPos_{};
    bool panning_ = false;
    ImVec2 panStart_{}, panOrigin_{};
    bool linking_ = false;
    int linkFrom_ = 0;
    std::string linkOut_ = "exec";
    ImVec2 linkMouse_{};
    // 放置armed：pendingType_ 非空=节点；pendingEntry_ 非空=入口（点到节点上绑定）
    std::string pendingType_;
    std::string pendingEntry_;
    // 简易布局缓存（本帧每个节点的屏幕矩形/引脚位置）
    struct Hit {
        int id = 0;
        ImVec2 min{}, max{};
        ImVec2 inPos{};             // exec-in 引脚
        ImVec2 outPos[3]{};         // 各输出引脚
        const FlowTypeInfo* info = nullptr;
    };
    struct Badge {
        int entry = -1;
        ImVec2 min{}, max{};
    };
    std::vector<Hit> hits_;
    std::vector<Badge> badges_;
    // 运行期调试（PollDebugNode 缓存）
    int debugNode_ = 0;
    std::string debugGraph_;
    // 右键菜单上下文
    int ctxNode_ = 0;
    int ctxLink_ = -1;
    ImVec2 ctxMouse_{};
};

} // namespace neon::editor
