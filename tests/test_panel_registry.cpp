// 面板插件化核心（阶段 1）：EditorContext / IPanel / PanelRegistry。
// 注册表刻意 ImGui-free（DrawAll 只对可见面板调 Draw，窗口包裹是面板自己
// 的责任），所以这些单测不需要 ImGui context。生命周期计数写入测试栈上的
// 外部计数器——Shutdown 会销毁面板对象，销毁后仍可断言 OnClose 被调过。
#include <memory>
#include <utility>

#include "editor_context.hpp"
#include "panel_registry.hpp"
#include "helpers.hpp"

namespace {

using neon::editor::EditorContext;
using neon::editor::IPanel;
using neon::editor::PanelRegistry;

struct Counts {
    int open = 0;
    int close = 0;
    int draw = 0;
    int update = 0;
};

// 假面板：记录 OnOpen/OnClose/Draw/OnUpdate 到外部 Counts（可空 → 不记录）。
// nullFlag=true 时 VisibleFlag() 返回 nullptr（防御路径：视为不可见）。
struct FakePanel : public IPanel {
    std::string title;
    bool visible = true;
    bool nullFlag = false;
    Counts* counts = nullptr;
    FakePanel(std::string t, bool v, Counts* c)
        : title(std::move(t)), visible(v), counts(c) {}
    const char* Title() const override { return title.c_str(); }
    bool* VisibleFlag() override { return nullFlag ? nullptr : &visible; }
    void Draw(EditorContext&) override { if (counts) ++counts->draw; }
    void OnOpen(EditorContext&) override { if (counts) ++counts->open; }
    void OnClose() override { if (counts) ++counts->close; }
    void OnUpdate(float) override { if (counts) ++counts->update; }
};

} // namespace

TEST(PanelRegistryRegisterFindUnregister) {
    PanelRegistry reg;
    CHECK_EQ(reg.Count(), size_t{0});
    CHECK(reg.Find("A") == nullptr);

    Counts ca, cb;
    reg.Register(std::make_unique<FakePanel>("A", true, &ca));
    reg.Register(std::make_unique<FakePanel>("B", false, &cb));
    CHECK_EQ(reg.Count(), size_t{2});
    CHECK(reg.Find("A") != nullptr);
    CHECK(reg.Find("B") != nullptr);
    CHECK_EQ(std::string(reg.Find("A")->Title()), "A");
    CHECK(reg.Find("C") == nullptr);

    // 卸载：返回所有权并移除；标题不存在时返回 nullptr。
    std::unique_ptr<IPanel> out = reg.Unregister("A");
    CHECK(out != nullptr);
    CHECK_EQ(std::string(out->Title()), "A");
    CHECK_EQ(reg.Count(), size_t{1});
    CHECK(reg.Find("A") == nullptr);
    CHECK(reg.Unregister("A") == nullptr); // 重复卸载
    CHECK(reg.Unregister("missing") == nullptr);

    // 卸载后的面板可重新注册（独立加载/卸载）。
    reg.Register(std::move(out));
    CHECK_EQ(reg.Count(), size_t{2});
    CHECK(reg.Find("A") != nullptr);
}

TEST(PanelRegistryDrawAllOnlyVisible) {
    EditorContext ctx;
    PanelRegistry reg;
    Counts ca, cb, cc;
    reg.Register(std::make_unique<FakePanel>("A", true, &ca));   // 可见
    reg.Register(std::make_unique<FakePanel>("B", false, &cb));  // 不可见
    reg.Register(std::make_unique<FakePanel>("C", true, &cc));
    static_cast<FakePanel*>(reg.Find("C"))->nullFlag = true;     // VisibleFlag → null
    reg.DrawAll(ctx);
    CHECK_EQ(ca.draw, 1);
    CHECK_EQ(cb.draw, 0);
    CHECK_EQ(cc.draw, 0); // VisibleFlag() == null 视为不可见

    ca.draw = cb.draw = cc.draw = 0;
    static_cast<FakePanel*>(reg.Find("C"))->nullFlag = false;
    reg.DrawAll(ctx);
    CHECK_EQ(ca.draw, 1);
    CHECK_EQ(cb.draw, 0);
    CHECK_EQ(cc.draw, 1);
}

TEST(PanelRegistryLifecycle) {
    EditorContext ctx;
    PanelRegistry reg;
    Counts ca, cb;
    reg.Register(std::make_unique<FakePanel>("A", true, &ca));
    reg.Register(std::make_unique<FakePanel>("B", false, &cb));

    reg.OpenAll(ctx);
    CHECK_EQ(ca.open, 1);
    CHECK_EQ(cb.open, 1);

    // Shutdown 调全部 OnClose 并清空（面板对象随之销毁，外部计数仍可断言）。
    reg.Shutdown();
    CHECK_EQ(ca.close, 1);
    CHECK_EQ(cb.close, 1);
    CHECK_EQ(reg.Count(), size_t{0});
    CHECK(reg.Find("A") == nullptr);
    // Shutdown 后可继续复用（重新加载新的一组面板）。
    reg.Register(std::make_unique<FakePanel>("A2", true, &ca));
    CHECK_EQ(reg.Count(), size_t{1});
    reg.OpenAll(ctx);
    CHECK_EQ(ca.open, 2);
}

TEST(PanelRegistryUpdateAll) {
    EditorContext ctx;
    PanelRegistry reg;
    Counts ca, cb;
    reg.Register(std::make_unique<FakePanel>("A", true, &ca));
    reg.Register(std::make_unique<FakePanel>("B", false, &cb));
    reg.UpdateAll(1.0f / 60.0f);
    CHECK_EQ(ca.update, 1);
    CHECK_EQ(cb.update, 1); // OnUpdate 是非渲染更新，不按可见性过滤
    reg.UpdateAll(1.0f / 60.0f);
    CHECK_EQ(ca.update, 2);
    CHECK_EQ(cb.update, 2);
    // Draw 不受 UpdateAll 影响。
    CHECK_EQ(ca.draw, 0);
    CHECK_EQ(cb.draw, 0);
}
