#include "panels/log_panel.hpp"

// 日志面板实现 = 原 EditorApp::BuildLogPanel（panels_debug.inc）方法体逐行迁移：
// EditorApp 面板私有成员（logEntries_/logFilter_/logAutoScroll_）改本类成员，
// 日志读写走 core::Log 全局接口（GetRecentLogs/ClearLogs，与原来一致），
// 函数内 static wasAtBottom 改本类成员。行为零变化。

#include "imgui.h"

namespace neon::editor {

void LogPanel::Draw(EditorContext&) {
    // 本面板只消费 core::Log 全局环形缓冲，不读共享上下文。
    if (!visible_ || !*visible_) return;
    logEntries_ = core::GetRecentLogs(800);
    if (ImGui::Begin("日志", visible_)) {
        const char* filters[] = {"全部", "INFO+", "WARN+", "ERROR"};
        ImGui::SetNextItemWidth(80.0f);
        ImGui::Combo("级别", &logFilter_, filters, 4);
        ImGui::SameLine();
        ImGui::Checkbox("自动滚动", &logAutoScroll_);
        ImGui::SameLine();
        if (ImGui::Button("清空")) core::ClearLogs();
        ImGui::Separator();

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
        if (logAutoScroll_ && wasAtBottom_ && shown > 0) ImGui::SetScrollHereY(1.0f);
        wasAtBottom_ = ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 4.0f;
        ImGui::EndChild();
    }
    ImGui::End();
}

} // namespace neon::editor
