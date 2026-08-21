#include "editor.hpp"

#include <algorithm>
#include <cctype>
#include <cstdio>
#if defined(_WIN32)
#include <direct.h>
#endif

#include "editor_history.hpp"
#include "imgui_internal.h"
#include "neon/gfx/imgui_neon.hpp"

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace neon::editor {
namespace {

std::string TypeLabel(const std::string& key) {
    if (key == "terrain") return "地形";
    if (key == "helmet") return "头盔 (glTF PBR)";
    if (key == "cube") return "方块";
    if (key == "tree") return "松树 (OBJ)";
    if (key.rfind("obj:", 0) == 0) return "OBJ 模型";
    if (key.rfind("gltf:", 0) == 0) return "glTF 模型";
    return key;
}

std::string ToLower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

std::string GetCurrentDir();
std::string ParentPath(const std::string& p);
std::string FileName(const std::string& p);
std::string FileStem(const std::string& p);
std::string FileExt(const std::string& p);

bool IsImageExt(const std::string& name) {
    std::string ext = ToLower(FileExt(name));
    return ext == ".png" || ext == ".jpg" || ext == ".jpeg" || ext == ".bmp" || ext == ".tga";
}

bool IsModelExt(const std::string& name) {
    std::string ext = ToLower(FileExt(name));
    return ext == ".obj" || ext == ".gltf";
}

std::string GetCurrentDir() {
    char buf[4096];
#if defined(_WIN32)
    if (_getcwd(buf, sizeof(buf))) return std::string(buf);
#else
    if (::getcwd(buf, sizeof(buf))) return std::string(buf);
#endif
    return ".";
}

std::string ParentPath(const std::string& p) {
    size_t pos = p.find_last_of("/\\");
    if (pos == std::string::npos) return p;
    if (pos == 0) return p.substr(0, 1);
    return p.substr(0, pos);
}

std::string FileName(const std::string& p) {
    size_t pos = p.find_last_of("/\\");
    return pos == std::string::npos ? p : p.substr(pos + 1);
}

std::string FileStem(const std::string& p) {
    std::string name = FileName(p);
    size_t dot = name.find_last_of('.');
    return dot == std::string::npos ? name : name.substr(0, dot);
}

std::string FileExt(const std::string& p) {
    std::string name = FileName(p);
    size_t dot = name.find_last_of('.');
    return dot == std::string::npos ? std::string() : name.substr(dot);
}

#if defined(_WIN32)
std::string WideToUtf8(const std::wstring& w) {
    if (w.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), nullptr, 0,
                                nullptr, nullptr);
    std::string out(static_cast<size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), &out[0], n, nullptr,
                        nullptr);
    return out;
}

std::wstring Utf8ToWide(const std::string& s) {
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring out(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), &out[0], n);
    return out;
}
#endif

bool ListDirectory(const std::string& dir, std::vector<AssetEntry>& out) {
#if defined(_WIN32)
    std::wstring pattern = Utf8ToWide(dir) + L"\\*";
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(pattern.c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return false;
    do {
        std::wstring name = fd.cFileName;
        if (name == L"." || name == L"..") continue;
        AssetEntry e;
        e.name = WideToUtf8(name);
        e.path = dir + "/" + e.name;
        e.isDir = (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
        if (!e.isDir) {
            ULARGE_INTEGER sz;
            sz.LowPart = fd.nFileSizeLow;
            sz.HighPart = fd.nFileSizeHigh;
            e.size = sz.QuadPart;
        }
        out.push_back(std::move(e));
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    return true;
#else
    DIR* d = opendir(dir.c_str());
    if (!d) return false;
    struct dirent* ent;
    while ((ent = readdir(d)) != nullptr) {
        std::string name = ent->d_name;
        if (name == "." || name == "..") continue;
        std::string path = dir + "/" + name;
        struct stat st;
        if (stat(path.c_str(), &st) != 0) continue;
        AssetEntry e;
        e.name = name;
        e.path = path;
        e.isDir = S_ISDIR(st.st_mode);
        if (!e.isDir) e.size = static_cast<uint64_t>(st.st_size);
        out.push_back(std::move(e));
    }
    closedir(d);
    return true;
#endif
}

} // namespace

void EditorApp::InitToolPanels() {
    assetDir_ = GetCurrentDir();
    RefreshAssetDir();
}

void EditorApp::RefreshAssetDir() {
    assetEntries_.clear();
    selectedAsset_ = -1;
    ListDirectory(assetDir_, assetEntries_);
    std::sort(assetEntries_.begin(), assetEntries_.end(),
              [](const AssetEntry& a, const AssetEntry& b) {
                  if (a.isDir != b.isDir) return a.isDir;
                  return ToLower(a.name) < ToLower(b.name);
              });
}

void EditorApp::ImportAssetPath(const std::string& path) {
    std::string lower = ToLower(path);
    if (lower.rfind(".obj") != std::string::npos) {
        AddEntity("obj:" + path);
        NEON_LOG_INFO("Editor: imported OBJ '%s'", path.c_str());
    } else if (lower.rfind(".gltf") != std::string::npos) {
        AddEntity("gltf:" + path);
        NEON_LOG_INFO("Editor: imported glTF '%s'", path.c_str());
    } else if (IsImageExt(path)) {
        gfx::Texture tex = assetMgr_.LoadTexture(path);
        if (tex.Valid()) {
            previewTexture_ = tex;
            if (previewTexId_ != ImTextureID_Invalid) {
                gfx::ImGuiNeon_UnregisterTexture(previewTexture_.Handle());
            }
            previewTexId_ = gfx::ImGuiNeon_RegisterTexture(previewTexture_.Handle());
            NEON_LOG_INFO("Editor: preview texture '%s' (%dx%d)", path.c_str(), tex.Width(),
                          tex.Height());
        }
    } else {
        NEON_LOG_INFO("Editor: '%s' (%llu bytes) - no import action",
                      path.c_str(), static_cast<unsigned long long>(0));
    }
}

void EditorApp::ImportSelectedAsset() {
    if (selectedAsset_ >= 0 && selectedAsset_ < static_cast<int>(assetEntries_.size()) &&
        !assetEntries_[static_cast<size_t>(selectedAsset_)].isDir) {
        ImportAssetPath(assetEntries_[static_cast<size_t>(selectedAsset_)].path);
    }
}

void EditorApp::BuildScenePanel() {
    if (!showHierarchy_) return;
    if (ImGui::Begin("场景", &showHierarchy_)) {
        static int addType = 0;
        const char* types[] = {"地形", "头盔", "方块", "松树"};
        ImGui::SetNextItemWidth(86.0f);
        ImGui::Combo("##addtype", &addType, types, 4);
        ImGui::SameLine();
        if (ImGui::Button("添加")) {
            const char* keys[] = {"terrain", "helmet", "cube", "tree"};
            AddEntity(keys[addType]);
        }
        ImGui::SameLine();
        if (ImGui::Button("复制") && selected_ >= 0) {
            history_.Push(std::make_unique<DuplicateEntityCommand>(
                &entities_, static_cast<size_t>(selected_)));
            SetSelection(static_cast<int>(entities_.size()) - 1);
        }
        ImGui::SameLine();
        if (ImGui::Button("删除") && selected_ >= 0 &&
            selected_ < static_cast<int>(entities_.size())) {
            history_.Push(std::make_unique<DeleteEntityCommand>(
                &entities_, static_cast<size_t>(selected_)));
            if (selected_ >= static_cast<int>(entities_.size()))
                SetSelection(static_cast<int>(entities_.size()) - 1);
        }
        ImGui::SameLine();
        if (ImGui::Button("↑") && selected_ > 0) {
            history_.Push(std::make_unique<ReorderEntityCommand>(
                &entities_, static_cast<size_t>(selected_), static_cast<size_t>(selected_ - 1)));
            SetSelection(selected_ - 1);
        }
        ImGui::SameLine();
        if (ImGui::Button("↓") && selected_ >= 0 &&
            selected_ < static_cast<int>(entities_.size()) - 1) {
            history_.Push(std::make_unique<ReorderEntityCommand>(
                &entities_, static_cast<size_t>(selected_), static_cast<size_t>(selected_ + 1)));
            SetSelection(selected_ + 1);
        }
        ImGui::Separator();
        ImGui::BeginChild("##scene_list", ImVec2(0, 0), ImGuiChildFlags_Borders);
        for (size_t i = 0; i < entities_.size(); ++i) {
            char label[256];
            std::snprintf(label, sizeof(label), "%s##scene_%zu",
                          entities_[i].name.c_str(), i);
            if (ImGui::Selectable(label, selected_ == static_cast<int>(i))) {
                SetSelection(static_cast<int>(i));
            }
        }
        ImGui::EndChild();
    }
    ImGui::End();
}

void EditorApp::BuildAssetPanel() {
    if (!showAssets_) return;
    if (ImGui::Begin("资产", &showAssets_)) {
        if (ImGui::SmallButton("..")) {
            std::string parent = ParentPath(assetDir_);
            if (parent != assetDir_) {
                assetDir_ = parent;
                RefreshAssetDir();
            }
        }
        ImGui::SameLine();
        if (ImGui::SmallButton("assets")) {
            assetDir_ = GetCurrentDir() + "/assets";
            RefreshAssetDir();
        }
        ImGui::SameLine();
        if (ImGui::SmallButton("根目录")) {
            assetDir_ = GetCurrentDir();
            RefreshAssetDir();
        }
        ImGui::SameLine();
        if (ImGui::SmallButton("刷新")) RefreshAssetDir();
        ImGui::SameLine();
        ImGui::TextUnformatted(assetDir_.c_str());
        ImGui::Separator();

        ImGui::BeginChild("##asset_list", ImVec2(0, -170.0f), ImGuiChildFlags_Borders);
        for (size_t i = 0; i < assetEntries_.size(); ++i) {
            const AssetEntry& e = assetEntries_[i];
            char label[320];
            std::snprintf(label, sizeof(label), "%s%s##asset_%zu",
                          e.isDir ? "[D] " : "    ", e.name.c_str(), i);
            if (ImGui::Selectable(label, selectedAsset_ == static_cast<int>(i))) {
                selectedAsset_ = static_cast<int>(i);
            }
            if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(0)) {
                if (e.isDir) {
                    assetDir_ = e.path;
                    RefreshAssetDir();
                } else {
                    ImportAssetPath(e.path);
                }
            }
            // Image assets can be dragged onto a material texture slot in the
            // inspector ("材质" section) to assign the texture.
            if (!e.isDir && IsImageExt(e.name) && ImGui::BeginDragDropSource()) {
                ImGui::SetDragDropPayload("ASSET_TEXTURE", e.path.c_str(), e.path.size() + 1);
                ImGui::Text("%s", e.name.c_str());
                ImGui::EndDragDropSource();
            }
        }
        ImGui::EndChild();

        if (selectedAsset_ >= 0 &&
            selectedAsset_ < static_cast<int>(assetEntries_.size())) {
            const AssetEntry& e = assetEntries_[static_cast<size_t>(selectedAsset_)];
            ImGui::TextUnformatted(e.name.c_str());
            ImGui::TextDisabled("%s", e.isDir ? "目录" : e.path.c_str());
            if (!e.isDir) {
                ImGui::SameLine();
                ImGui::TextDisabled("%.1f KB", static_cast<double>(e.size) / 1024.0);
                if (IsModelExt(e.name)) {
                    if (ImGui::Button("导入到场景")) ImportAssetPath(e.path);
                } else if (IsImageExt(e.name) && previewTexId_ != ImTextureID_Invalid) {
                    ImGui::Image(previewTexId_, ImVec2(140.0f, 140.0f));
                    ImGui::SameLine();
                    ImGui::TextDisabled("%dx%d", previewTexture_.Width(),
                                        previewTexture_.Height());
                }
            }
        }
    }
    ImGui::End();
}

void EditorApp::BuildResourcePanel() {
    if (!showResources_) return;
    if (ImGui::Begin("资源", &showResources_)) {
        assets::AssetStats s = assetMgr_.Stats();
        ImGui::Text("纹理 %zu | 网格 %zu | 字体 %zu", s.textures, s.meshes, s.fonts);
        ImGui::Text("纹理内存 %.2f MB | 三角形 %zu",
                    static_cast<double>(s.textureBytes) / (1024.0 * 1024.0),
                    s.meshTriangles);
        ImGui::Separator();
        if (ImGui::BeginTabBar("##res_tabs")) {
            if (ImGui::BeginTabItem("纹理")) {
                ImGui::BeginChild("##res_tex");
                for (const auto& kv : assetMgr_.Textures()) {
                    if (!kv.second.Valid()) continue;
                    ImGui::Text("%s", kv.first.c_str());
                    ImGui::SameLine();
                    ImGui::TextDisabled("%dx%d", kv.second.Width(), kv.second.Height());
                }
                ImGui::EndChild();
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem("网格")) {
                ImGui::BeginChild("##res_mesh");
                for (const auto& kv : assetMgr_.Meshes()) {
                    if (!kv.second.Valid()) continue;
                    ImGui::Text("%s", kv.first.c_str());
                    ImGui::SameLine();
                    ImGui::TextDisabled("%u 三角", kv.second.TriangleCount());
                }
                ImGui::EndChild();
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem("字体")) {
                ImGui::BeginChild("##res_font");
                for (const auto& kv : assetMgr_.Fonts()) {
                    if (!kv.second.Valid()) continue;
                    ImGui::Text("%s (%dpx)", kv.first.first.c_str(), kv.first.second);
                }
                ImGui::EndChild();
                ImGui::EndTabItem();
            }
            ImGui::EndTabBar();
        }
    }
    ImGui::End();
}

void EditorApp::BuildInspectorPanel() {
    if (!showInspector_ || selected_ < 0 ||
        selected_ >= static_cast<int>(entities_.size())) {
        return;
    }
    if (ImGui::Begin("属性", &showInspector_)) {
        SceneEntity& e = entities_[static_cast<size_t>(selected_)];
        char buf[160];
        std::snprintf(buf, sizeof(buf), "%s", e.name.c_str());
        // Every field edit routes through the undo/redo command stack; the old
        // value is captured before the widget mutates the entity.
        if (ImGui::InputText("名称", buf, sizeof(buf))) {
            const std::string oldName = e.name;
            e.name = buf;
            history_.Push(std::make_unique<EditPropertyCommand<std::string>>(
                &entities_, selected_, ApplyNameProp, oldName, e.name,
                /*mergeable=*/false)); // one undo step per keystroke
        }
        ImGui::TextDisabled("类型: %s", TypeLabel(e.meshKey).c_str());
        ImGui::Separator();
        const math::Vec3 oldPos = e.pos;
        if (ImGui::DragFloat3("位置", &e.pos.x, 0.1f)) {
            history_.Push(std::make_unique<EditTransformCommand>(
                &entities_, selected_, oldPos, e.rot, e.scale, e.pos, e.rot, e.scale,
                EditTransformCommand::kPos));
        }
        math::Vec3 euler = e.rot.ToMat4().TransformDir({0, 0, -1});
        float rotDeg = std::atan2(euler.x, euler.z) * math::kRadToDeg;
        const math::Quat oldRot = e.rot;
        if (ImGui::DragFloat("旋转 Y", &rotDeg, 0.5f, -180.0f, 180.0f)) {
            e.rot = math::Quat::FromEuler(0, rotDeg * math::kDegToRad, 0);
            history_.Push(std::make_unique<EditTransformCommand>(
                &entities_, selected_, e.pos, oldRot, e.scale, e.pos, e.rot, e.scale,
                EditTransformCommand::kRot));
        }
        const math::Vec3 oldScale = e.scale;
        if (ImGui::DragFloat3("缩放", &e.scale.x, 0.05f, 0.05f, 50.0f)) {
            history_.Push(std::make_unique<EditTransformCommand>(
                &entities_, selected_, e.pos, e.rot, oldScale, e.pos, e.rot, e.scale,
                EditTransformCommand::kScale));
        }
        const gfx::Color oldTint = e.tint;
        if (ImGui::ColorEdit3("颜色", &e.tint.r)) {
            e.material.tint = e.tint;
            history_.Push(std::make_unique<EditPropertyCommand<gfx::Color>>(
                &entities_, selected_, ApplyColorProp, oldTint, e.tint));
        }
        ImGui::Separator();
        ImGui::TextUnformatted("材质");
        const float oldMetallic = e.metallic;
        if (ImGui::DragFloat("金属度", &e.metallic, 0.01f, 0.0f, 1.0f)) {
            e.material.metallic = e.metallic;
            history_.Push(std::make_unique<EditPropertyCommand<float>>(
                &entities_, selected_, ApplyMetallicProp, oldMetallic, e.metallic));
        }
        const float oldRoughness = e.roughness;
        if (ImGui::DragFloat("粗糙度", &e.roughness, 0.01f, 0.0f, 1.0f)) {
            e.material.roughness = e.roughness;
            history_.Push(std::make_unique<EditPropertyCommand<float>>(
                &entities_, selected_, ApplyRoughnessProp, oldRoughness, e.roughness));
        }
        const float oldAO = e.ao;
        if (ImGui::DragFloat("环境光遮蔽", &e.ao, 0.01f, 0.0f, 1.0f)) {
            e.material.aoStrength = e.ao;
            history_.Push(std::make_unique<EditPropertyCommand<float>>(
                &entities_, selected_, ApplyAOProp, oldAO, e.ao));
        }
        const float oldEmissiveIntensity = e.emissiveIntensity;
        if (ImGui::DragFloat("自发光强度", &e.emissiveIntensity, 0.05f, 0.0f, 5.0f)) {
            e.material.emissiveIntensity = e.emissiveIntensity;
            history_.Push(std::make_unique<EditPropertyCommand<float>>(
                &entities_, selected_, ApplyEmissiveIntensityProp, oldEmissiveIntensity,
                e.emissiveIntensity));
        }
        ImGui::Separator();
        ImGui::TextUnformatted("纹理 (拖入资产面板的图片)");
        // One slot per PBR texture: thumbnail preview, editable path (Enter to
        // commit), clear button, and a drag-drop target from the asset panel.
        // Every change routes through the undo history as a texture-slot edit.
        auto textureSlot = [this, &e](const char* label, std::string& path,
                                      gfx::TextureHandle& handle,
                                      void (*apply)(SceneEntity&, const TextureSlotValue&)) {
            char rowId[128];
            std::snprintf(rowId, sizeof(rowId), "##slot_%s", label);
            ImGui::TextUnformatted(label);
            ImGui::SameLine();
            ImGui::BeginChild(rowId, ImVec2(-1.0f, 32.0f), ImGuiChildFlags_Borders);
            {
                ImTextureID tid = ImTextureID_Invalid;
                if (handle.Valid()) tid = gfx::ImGuiNeon_RegisterTexture(handle);
                const ImVec2 previewSize(26.0f, 26.0f);
                if (tid != ImTextureID_Invalid) {
                    ImGui::Image(tid, previewSize);
                } else {
                    ImGui::Dummy(previewSize);
                }
                ImGui::SameLine();
                char buf[2048];
                std::snprintf(buf, sizeof(buf), "%s", path.c_str());
                ImGui::SetNextItemWidth(-64.0f);
                if (ImGui::InputText((std::string("##path_") + label).c_str(), buf, sizeof(buf),
                                     ImGuiInputTextFlags_EnterReturnsTrue)) {
                    const std::string newPath(buf);
                    if (newPath != path) {
                        const TextureSlotValue oldVal{path, handle};
                        gfx::Texture tex =
                            newPath.empty() ? gfx::Texture{} : assetMgr_.LoadTexture(newPath);
                        if (newPath.empty() || tex.Valid()) {
                            const TextureSlotValue newVal{newPath, tex.Handle()};
                            history_.Push(std::make_unique<EditPropertyCommand<TextureSlotValue>>(
                                &entities_, selected_, apply, oldVal, newVal));
                        } else {
                            NEON_LOG_WARN("Editor: texture '%s' failed to load (slot '%s')",
                                          newPath.c_str(), label);
                        }
                    }
                }
                ImGui::SameLine();
                if (ImGui::Button((std::string("清除##") + label).c_str())) {
                    const TextureSlotValue oldVal{path, handle};
                    const TextureSlotValue newVal{"", gfx::TextureHandle{}};
                    history_.Push(std::make_unique<EditPropertyCommand<TextureSlotValue>>(
                        &entities_, selected_, apply, oldVal, newVal));
                }
            }
            ImGui::EndChild();
            if (ImGui::BeginDragDropTarget()) {
                if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("ASSET_TEXTURE")) {
                    std::string newPath(static_cast<const char*>(payload->Data),
                                        static_cast<size_t>(payload->DataSize));
                    if (!newPath.empty() && newPath.back() == '\0') newPath.pop_back();
                    if (!newPath.empty() && newPath != path) {
                        const TextureSlotValue oldVal{path, handle};
                        gfx::Texture tex = assetMgr_.LoadTexture(newPath);
                        if (tex.Valid()) {
                            const TextureSlotValue newVal{newPath, tex.Handle()};
                            history_.Push(std::make_unique<EditPropertyCommand<TextureSlotValue>>(
                                &entities_, selected_, apply, oldVal, newVal));
                        } else {
                            NEON_LOG_WARN("Editor: dropped texture '%s' failed to load", newPath.c_str());
                        }
                    }
                }
                ImGui::EndDragDropTarget();
            }
        };
        textureSlot("漫反射", e.albedoTex, e.material.albedo, ApplyAlbedoTexSlot);
        textureSlot("金属度/粗糙度", e.mrTex, e.material.metallicRoughness, ApplyMRTexSlot);
        textureSlot("环境光遮蔽图", e.aoTex, e.material.occlusion, ApplyAOTexSlot);
        textureSlot("自发光图", e.emissiveTex, e.material.emissive, ApplyEmissiveTexSlot);
        ImGui::Separator();
        ImGui::TextUnformatted("网格");
        ImGui::TextDisabled("来源: %s", e.meshKey.c_str());
        if (e.mesh.Valid()) {
            ImGui::TextDisabled("%u 三角形", e.mesh.TriangleCount());
            const math::AABB& b = e.mesh.Bounds();
            ImGui::TextDisabled("包围盒 (%.1f, %.1f, %.1f) ~ (%.1f, %.1f, %.1f)", b.min.x,
                                b.min.y, b.min.z, b.max.x, b.max.y, b.max.z);
        }
    }
    ImGui::End();
}

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

void EditorApp::BuildViewportPanel() {
    ImGuiWindowFlags vpFlags = ImGuiWindowFlags_NoScrollbar |
                               ImGuiWindowFlags_NoScrollWithMouse |
                               ImGuiWindowFlags_NoCollapse |
                               ImGuiWindowFlags_NoBackground |
                               ImGuiWindowFlags_NoInputs;
    if (ImGui::Begin("视口", nullptr, vpFlags)) {
        ImVec2 pos = ImGui::GetWindowPos();
        ImVec2 size = ImGui::GetWindowSize();
        math::Vec2 uiPos = renderer_.ScreenToUI({pos.x, pos.y});
        float scale = renderer_.UIScale();
        viewportRect_ = {uiPos.x, uiPos.y, size.x / scale, size.y / scale};

        ImGui::TextColored(ImVec4(0.65f, 0.85f, 1.0f, 1.0f),
                           "右键旋转 | 中键平移 | 滚轮缩放 | 左键拾取");
        if (playtestActive_) {
            ImGui::SameLine();
            ImGui::TextColored(ImVec4(0.4f, 1.0f, 0.5f, 1.0f), "试玩中 (F5 停止)");
        }
        ImGui::TextDisabled("实体 %zu | 目标 (%.1f, %.1f, %.1f) | 距离 %.1f",
                            entities_.size(), camTarget_.x, camTarget_.y, camTarget_.z,
                            camDist_);

        // Transform gizmo for the selected entity (drawn into this window's
        // draw list; interacts via ImGui's mouse state).
        DrawTransformGizmo();
    }
    ImGui::End();
}

// Re-scan <projectDir>/scripts/ and run a syntax check on every *.lua. Reuses
// one throwaway LuaHost across all checks (nothing ever runs, so a failed check
// leaves the host fully usable for the next one).
void EditorApp::RefreshScriptChecks() {
    scriptFiles_.clear();
    scriptChecks_.clear();
    if (!scriptCheckHost_) {
        scriptCheckHost_ = script::CreateLuaHost();
        if (scriptCheckHost_) scriptCheckHost_->Init();
    }
    std::vector<std::string> files;
    ListLuaFiles(ScriptsDir(projectDir_), "scripts", files);
    const std::string base = projectDir_.empty() ? "." : projectDir_;
    for (const std::string& rel : files) {
        if (scriptCheckHost_) {
            scriptChecks_.push_back(CheckScriptFile(*scriptCheckHost_, base, rel));
        } else {
            ScriptCheckResult failed;
            failed.path = rel;
            failed.ok = false;
            failed.message = "脚本宿主不可用";
            scriptChecks_.push_back(failed);
        }
        scriptFiles_.push_back(rel);
    }
    if (scriptAttachIndex_ >= static_cast<int>(scriptFiles_.size()))
        scriptAttachIndex_ = static_cast<int>(scriptFiles_.size()) - 1;
    if (scriptAttachIndex_ < 0 && !scriptFiles_.empty()) scriptAttachIndex_ = 0;
}

void EditorApp::BuildScriptPanel() {
    if (!showScripts_) return;
    if (ImGui::Begin("脚本", &showScripts_)) {
        // Throttle the scripts/ scan + syntax checks: run on panel open and
        // every ~1s of frames (60 @ 60fps), never per frame.
        const uint64_t now = TimeRef().frameIndex;
        if (now - scriptRefreshFrame_ >= 60 || scriptFiles_.empty()) {
            RefreshScriptChecks();
            scriptRefreshFrame_ = now;
        }
        if (ImGui::Button("刷新检查")) RefreshScriptChecks();
        ImGui::SameLine();
        ImGui::TextDisabled("项目: %s", projectDir_.c_str());
        ImGui::Separator();

        ImGui::BeginChild("##script_list", ImVec2(0, -150.0f), ImGuiChildFlags_Borders);
        if (scriptFiles_.empty()) {
            ImGui::TextDisabled("scripts/ 目录下没有 .lua 脚本");
        }
        for (size_t i = 0; i < scriptFiles_.size(); ++i) {
            const ScriptCheckResult& r = scriptChecks_[i];
            char label[320];
            std::snprintf(label, sizeof(label), "%s##script_%zu", scriptFiles_[i].c_str(), i);
            if (ImGui::Selectable(label, scriptAttachIndex_ == static_cast<int>(i)))
                scriptAttachIndex_ = static_cast<int>(i);
            if (r.ok) {
                ImGui::SameLine();
                ImGui::TextColored(ImVec4(0.4f, 0.9f, 0.5f, 1.0f), "✓ 语法通过");
            } else {
                ImGui::SameLine();
                ImGui::TextColored(ImVec4(1.0f, 0.45f, 0.4f, 1.0f), "✗ 错误 (行 %d): %s",
                                   r.line, r.message.c_str());
            }
        }
        ImGui::EndChild();
        ImGui::Separator();

        ImGui::TextUnformatted("附加到选中实体");
        if (selected_ < 0 || selected_ >= static_cast<int>(entities_.size())) {
            ImGui::TextDisabled("未选中实体");
            ImGui::End();
            return;
        }
        SceneEntity& e = entities_[static_cast<size_t>(selected_)];

        // Reload the vars editor from the entity's attached vars. Detects JSON
        // larger than the buffer so a silent truncation surfaces a warning.
        auto reloadVars = [&]() {
            const std::string json = e.scriptVars.IsObject()
                                         ? core::JsonWriter::Write(e.scriptVars)
                                         : "{}";
            const int n = std::snprintf(scriptVarsBuf_, sizeof(scriptVarsBuf_), "%s",
                                        json.c_str());
            if (static_cast<size_t>(n) >= sizeof(scriptVarsBuf_)) {
                char warn[128];
                std::snprintf(warn, sizeof(warn),
                              "变量 JSON 过大 (%.1f KB)，已截断到缓冲区上限",
                              static_cast<double>(sizeof(scriptVarsBuf_)) / 1024.0);
                scriptVarsError_ = warn;
            } else {
                scriptVarsError_.clear();
            }
        };

        // Sync the dropdown selection + vars buffer when the selected entity
        // changes (SetSelection / list mutations reset scriptSyncEntity_ so an
        // entity re-selected at the same index still re-syncs), so the panel
        // always reflects the attached script (if any).
        if (scriptSyncEntity_ != selected_) {
            scriptSyncEntity_ = selected_;
            scriptAttachIndex_ = -1;
            for (size_t i = 0; i < scriptFiles_.size(); ++i)
                if (scriptFiles_[i] == e.scriptPath) scriptAttachIndex_ = static_cast<int>(i);
            if (scriptAttachIndex_ < 0 && !scriptFiles_.empty()) scriptAttachIndex_ = 0;
            reloadVars();
        }

        if (scriptFiles_.empty()) {
            ImGui::TextDisabled("没有可附加的脚本");
        } else {
            std::vector<const char*> names;
            names.reserve(scriptFiles_.size());
            for (const auto& f : scriptFiles_) names.push_back(f.c_str());
            ImGui::SetNextItemWidth(-1.0f);
            ImGui::Combo("##script_attach", &scriptAttachIndex_, names.data(),
                         static_cast<int>(names.size()));
        }
        ImGui::TextUnformatted("变量 (JSON)");
        ImGui::SetNextItemWidth(-1.0f);
        ImGui::InputTextMultiline("##script_vars", scriptVarsBuf_, sizeof(scriptVarsBuf_),
                                  ImVec2(-1.0f, 88.0f));
        if (!scriptVarsError_.empty())
            ImGui::TextColored(ImVec4(1.0f, 0.45f, 0.4f, 1.0f), "%s", scriptVarsError_.c_str());
        if (!e.scriptPath.empty())
            ImGui::TextDisabled("已附加: %s", e.scriptPath.c_str());

        const bool haveScript = !scriptFiles_.empty() && scriptAttachIndex_ >= 0 &&
                                scriptAttachIndex_ < static_cast<int>(scriptFiles_.size());
        if (ImGui::Button("附加")) {
            if (!haveScript) {
                scriptVarsError_ = "没有可附加的脚本";
            } else {
                std::string perr;
                core::Json parsed = core::Json::Parse(scriptVarsBuf_, &perr);
                if (!perr.empty()) {
                    scriptVarsError_ = "变量 JSON 无效: " + perr;
                } else if (!parsed.IsNull() && !parsed.IsObject()) {
                    scriptVarsError_ = "变量必须是 JSON 对象";
                } else {
                    const SceneScriptFields oldV{e.scriptBackend, e.scriptPath, e.scriptVars};
                    const SceneScriptFields newV{
                        "lua", scriptFiles_[static_cast<size_t>(scriptAttachIndex_)],
                        parsed.IsNull() ? core::Json{} : parsed};
                    history_.Push(std::make_unique<EditPropertyCommand<SceneScriptFields>>(
                        &entities_, selected_, ApplyScriptFields, oldV, newV,
                        /*mergeable=*/false)); // one click = one undo step
                    reloadVars();
                }
            }
        }
        ImGui::SameLine();
        if (ImGui::Button("分离") && !e.scriptPath.empty()) {
            const SceneScriptFields oldV{e.scriptBackend, e.scriptPath, e.scriptVars};
            const SceneScriptFields emptyV;
            history_.Push(std::make_unique<EditPropertyCommand<SceneScriptFields>>(
                &entities_, selected_, ApplyScriptFields, oldV, emptyV,
                /*mergeable=*/false));
            reloadVars(); // e.scriptVars is now null -> buffer shows "{}"
        }
    }
    ImGui::End();
}

void EditorApp::RunPackage() {
    pack::PackConfig cfg;
    cfg.projectDir = projectDir_;
    cfg.outDir = packOutDirBuf_;
    cfg.playerSource = "build/neon_rush.exe";
    packReport_ = pack::PackProject(cfg);
    packRan_ = true;
    if (packReport_.ok) {
        NEON_LOG_INFO("Editor: packaged '%s' -> %s (%zu files, %zu bytes)",
                      projectDir_.c_str(), packReport_.packPath.c_str(),
                      packReport_.fileCount, packReport_.bytesWritten);
    } else {
        NEON_LOG_ERROR("Editor: package failed for '%s' (%zu errors)",
                       projectDir_.c_str(), packReport_.errors.size());
    }
}

void EditorApp::BuildPackagePanel() {
    if (!showPackage_) return;
    if (ImGui::Begin("打包", &showPackage_)) {
        ImGui::TextDisabled("项目目录 (game.json + scenes/ + prefabs/ + behaviors/ + scripts/ + assets/)");
        if (ImGui::InputText("##pack_proj", projectDirBuf_, sizeof(projectDirBuf_),
                             ImGuiInputTextFlags_EnterReturnsTrue)) {
            projectDir_ = projectDirBuf_;
            if (projectDir_.empty()) projectDir_ = ".";
            SaveEditorConfig();
        }
        ImGui::TextDisabled("输出目录");
        ImGui::SetNextItemWidth(-1.0f);
        ImGui::InputText("##pack_out", packOutDirBuf_, sizeof(packOutDirBuf_));
        if (ImGui::Button("一键打包")) RunPackage();
        ImGui::SameLine();
        ImGui::TextDisabled("生成 game.pack + run.bat + neon_game.exe");
        ImGui::Separator();

        if (packRan_) {
            if (packReport_.ok) {
                ImGui::TextColored(ImVec4(0.4f, 0.9f, 0.5f, 1.0f), "打包成功");
            } else {
                ImGui::TextColored(ImVec4(1.0f, 0.45f, 0.4f, 1.0f), "打包失败 (%zu 个错误)",
                                   packReport_.errors.size());
            }
            ImGui::TextDisabled("文件: %zu    字节: %zu", packReport_.fileCount,
                                packReport_.bytesWritten);
            if (!packReport_.packPath.empty()) ImGui::TextUnformatted(packReport_.packPath.c_str());
            if (!packReport_.runScriptPath.empty())
                ImGui::TextUnformatted(packReport_.runScriptPath.c_str());
            if (!packReport_.playerPath.empty())
                ImGui::TextUnformatted(packReport_.playerPath.c_str());
            if (!packReport_.errors.empty()) {
                ImGui::TextUnformatted("错误:");
                for (const std::string& e : packReport_.errors)
                    ImGui::TextColored(ImVec4(1.0f, 0.45f, 0.4f, 1.0f), "- %s", e.c_str());
            }
            if (!packReport_.warnings.empty()) {
                ImGui::TextUnformatted("警告:");
                for (const std::string& w : packReport_.warnings)
                    ImGui::TextColored(ImVec4(0.9f, 0.8f, 0.4f, 1.0f), "- %s", w.c_str());
            }
            ImGui::Separator();
        }
        ImGui::TextDisabled(
            "注意: neon_game.exe 目前为 neon_rush 演示占位，不读取 game.pack。\n"
            "T4.7 将提供真正的数据驱动播放器。");
    }
    ImGui::End();
}

} // namespace neon::editor
