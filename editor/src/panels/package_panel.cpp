#include "panels/package_panel.hpp"

// 打包面板实现 = 原 EditorApp::BuildPackagePanel（panels_world.inc:197-313）
// 方法体逐行迁移：EditorApp 成员（showPackage_/projectDirBuf_/projectDir_/package_）
// 改本类 visible_ / 局部缓冲 / ctx.projectDir / 本类成员；SaveEditorConfig/RunPackage
// 经 ctx 回调。行为零变化。

#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>

#include "editor.hpp"
#include "imgui.h"
#include "neon/core/json.hpp"
#include "neon/core/log.hpp"

namespace neon::editor {

void PackagePanel::Draw(EditorContext& ctx) {
    if (!visible_ || !*visible_) return;
    if (ImGui::Begin("打包", visible_)) {
        ImGui::TextDisabled("项目目录 (game.json + assets/ 内容根：scenes/ prefabs/ scripts/ ui/ locales/)");
        char projBuf[512];
        std::snprintf(projBuf, sizeof(projBuf), "%s", ctx.projectDir->c_str());
        if (ImGui::InputText("##pack_proj", projBuf, sizeof(projBuf),
                             ImGuiInputTextFlags_EnterReturnsTrue)) {
            *ctx.projectDir = projBuf;
            if (ctx.projectDir->empty()) *ctx.projectDir = ".";
            ctx.saveEditorConfig();
        }
        ImGui::TextDisabled("输出目录");
        ImGui::SetNextItemWidth(-1.0f);
        ImGui::InputText("##pack_out", outDirBuf, sizeof(outDirBuf));
        ImGui::Separator();
        // Export presets (Godot-style): edit game.json's "export" block.
        ImGui::TextUnformatted("导出配置 (game.json \"export\")");
        static std::string expProj;
        static char expPlatform[64] = "windows";
        static char expIcon[512] = {};
        static char expDesc[512] = {};
        if (expProj != *ctx.projectDir) {
            expProj = *ctx.projectDir;
            std::strcpy(expPlatform, "windows");
            expIcon[0] = '\0';
            expDesc[0] = '\0';
            std::ifstream in(*ctx.projectDir + "/game.json", std::ios::binary);
            if (in.is_open()) {
                std::string text((std::istreambuf_iterator<char>(in)),
                                 std::istreambuf_iterator<char>());
                std::string err;
                core::Json root = core::Json::Parse(text, &err);
                if (const core::Json* ex = root.Get("export")) {
                    if (const core::Json* p = ex->Get("platform"))
                        std::strncpy(expPlatform, p->GetString().c_str(),
                                     sizeof(expPlatform) - 1);
                    if (const core::Json* i = ex->Get("icon"))
                        std::strncpy(expIcon, i->GetString().c_str(), sizeof(expIcon) - 1);
                    if (const core::Json* d = ex->Get("description"))
                        std::strncpy(expDesc, d->GetString().c_str(), sizeof(expDesc) - 1);
                }
            }
        }
        ImGui::SetNextItemWidth(130.0f);
        ImGui::InputText("平台", expPlatform, sizeof(expPlatform));
        ImGui::SetNextItemWidth(-1.0f);
        ImGui::InputText("图标", expIcon, sizeof(expIcon));
        ImGui::SetNextItemWidth(-1.0f);
        ImGui::InputText("说明", expDesc, sizeof(expDesc));
        if (ImGui::Button("保存导出配置")) {
            std::ifstream in(*ctx.projectDir + "/game.json", std::ios::binary);
            std::string text((std::istreambuf_iterator<char>(in)),
                             std::istreambuf_iterator<char>());
            std::string err;
            core::Json root = core::Json::Parse(text, &err);
            if (!root.IsObject()) {
                NEON_LOG_ERROR("Export: cannot read '%s/game.json',", ctx.projectDir->c_str());
            } else {
                core::Json ex;
                ex.type_ = core::Json::Type::Object;
                auto str = [](const char* s) {
                    core::Json j;
                    j.type_ = core::Json::Type::String;
                    j.string_ = s;
                    return j;
                };
                ex.object_["platform"] = str(expPlatform);
                if (expIcon[0]) ex.object_["icon"] = str(expIcon);
                if (expDesc[0]) ex.object_["description"] = str(expDesc);
                root.object_["export"] = ex;
                std::ofstream out(*ctx.projectDir + "/game.json", std::ios::binary);
                if (out.is_open()) {
                    out << core::JsonWriter::WritePretty(root);
                    NEON_LOG_INFO("Export: preset saved -> %s/game.json", ctx.projectDir->c_str());
                } else {
                    NEON_LOG_ERROR("Export: cannot write '%s/game.json'", ctx.projectDir->c_str());
                }
            }
        }
        ImGui::SameLine();
        ImGui::TextDisabled("windows | linux | macos | web");
        ImGui::Separator();
        if (ImGui::Button("一键打包")) {
            ctx.runPackage(outDirBuf, report);
            ran = true;
        }
        ImGui::SameLine();
        ImGui::TextDisabled("生成 game.pack + run.bat + neon_game.exe");
        ImGui::Separator();

        if (ran) {
            if (report.ok) {
                ImGui::TextColored(ImVec4(0.4f, 0.9f, 0.5f, 1.0f), "打包成功");
            } else {
                ImGui::TextColored(ImVec4(1.0f, 0.45f, 0.4f, 1.0f), "打包失败 (%zu 个错误)",
                                   report.errors.size());
            }
            ImGui::TextDisabled("文件: %zu    字节: %zu", report.fileCount,
                                report.bytesWritten);
            if (!report.packPath.empty()) ImGui::TextUnformatted(report.packPath.c_str());
            if (!report.runScriptPath.empty())
                ImGui::TextUnformatted(report.runScriptPath.c_str());
            if (!report.playerPath.empty())
                ImGui::TextUnformatted(report.playerPath.c_str());
            if (!report.errors.empty()) {
                ImGui::TextUnformatted("错误:");
                for (const std::string& e : report.errors)
                    ImGui::TextColored(ImVec4(1.0f, 0.45f, 0.4f, 1.0f), "- %s", e.c_str());
            }
            if (!report.warnings.empty()) {
                ImGui::TextUnformatted("警告:");
                for (const std::string& w : report.warnings)
                    ImGui::TextColored(ImVec4(0.9f, 0.8f, 0.4f, 1.0f), "- %s", w.c_str());
            }
            ImGui::Separator();
        }
        ImGui::TextDisabled(
            "neon_game.exe 为数据驱动播放器（读取 game.pack）。\n"
            "若尚未构建 neon_game，播放器复制会给出警告，打包仍会生成 game.pack 与 run.bat。");
    }
    ImGui::End();
}

} // namespace neon::editor
