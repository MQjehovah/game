#include "editor.hpp"

#include <algorithm>
#include <cerrno>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <direct.h>
#include <windows.h>
#else
#include <sys/stat.h>
#endif

#include "font_data.hpp"

#include "imgui_internal.h"
#include "neon/core/json.hpp"
#include "neon/gfx/imgui_neon.hpp"

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"

namespace neon::editor {
namespace {

std::string DirName(const std::string& path) {
    size_t pos = path.find_last_of("/\\");
    return pos == std::string::npos ? std::string(".") : path.substr(0, pos + 1);
}

// Map an editor mesh key to the runtime-loadable key written into an exported
// scene. File-backed built-ins resolve to their asset paths; procedural
// primitives ("terrain", "cube") and already-prefixed keys ("obj:", "gltf:")
// pass through verbatim.
std::string ExportMeshKey(const std::string& key) {
    if (key == "helmet") return "gltf:assets/models/DamagedHelmet/DamagedHelmet.gltf";
    if (key == "tree") return "obj:assets/kenney_nature/Models/OBJ format/tree_pineTallA.obj";
    return key;
}

bool MakeDir(const std::string& path) {
#if defined(_WIN32)
    return _mkdir(path.c_str()) == 0 || errno == EEXIST;
#else
    return ::mkdir(path.c_str(), 0777) == 0 || errno == EEXIST;
#endif
}

// Create every missing path component (accepts '/' and '\' separators).
bool EnsureDirs(const std::string& path) {
    std::string acc;
    size_t i = 0;
    while (i < path.size()) {
        size_t next = path.find_first_of("/\\", i);
        if (next == std::string::npos) next = path.size();
        std::string comp = path.substr(i, next - i);
        i = next + 1;
        if (!acc.empty() && !comp.empty()) {
            acc += "/" + comp;
        } else if (acc.empty()) {
            acc = comp;
        } else {
            continue; // duplicate separator; keep going
        }
        if (acc.empty() || acc == ".") continue;
        // A Windows drive root like "C:" already exists; skip creation.
        if (acc.size() == 2 && acc[1] == ':') continue;
        if (!MakeDir(acc)) return false;
    }
    return true;
}

std::string GetTempDir() {
#if defined(_WIN32)
    char buf[MAX_PATH];
    DWORD n = GetTempPathA(MAX_PATH, buf);
    if (n == 0 || n >= MAX_PATH) return ".";
    std::string dir(buf);
    while (!dir.empty() && (dir.back() == '\\' || dir.back() == '/')) dir.pop_back();
    return dir.empty() ? "." : dir;
#else
    const char* t = std::getenv("TMPDIR");
    if (t && *t) return t;
    return "/tmp";
#endif
}

gfx::Mesh MakeTerrain(gfx::Renderer& renderer) {
    const int segments = 48;
    const float size = 60.0f;
    std::vector<float> heights(static_cast<size_t>(segments + 1) * (segments + 1), 0.0f);
    float half = size * 0.5f;
    for (int row = 0; row <= segments; ++row) {
        for (int col = 0; col <= segments; ++col) {
            float x = -half + col * (size / segments);
            float z = -half + row * (size / segments);
            float h = std::sin(x * 0.11f) * std::cos(z * 0.13f) * 0.8f +
                      std::sin(x * 0.31f + z * 0.27f) * 0.35f;
            float d = std::sqrt(x * x + z * z);
            h *= math::Saturate((d - 6.0f) / 10.0f); // flatten the centre
            heights[static_cast<size_t>(row) * (segments + 1) + col] = h;
        }
    }
    return gfx::Mesh::CreateTerrain(renderer, segments, size, heights, 1.0f, "terrain");
}

math::Ray ScreenRay(const gfx::Camera& cam, float aspect, const math::Vec2& designPos) {
    float ndcX = designPos.x / static_cast<float>(gfx::Renderer::kDesignWidth) * 2.0f - 1.0f;
    float ndcY = 1.0f - designPos.y / static_cast<float>(gfx::Renderer::kDesignHeight) * 2.0f;
    math::Vec3 fwd = (cam.target - cam.position).Normalized();
    math::Vec3 right = math::Cross(fwd, cam.up).Normalized();
    math::Vec3 upv = math::Cross(right, fwd);
    float tanF = std::tan(cam.fovY * 0.5f);
    math::Vec3 dir = (fwd + right * ndcX * tanF * aspect + upv * ndcY * tanF).Normalized();
    return {cam.position, dir};
}

} // namespace

bool EditorApp::OnCreate() {
    if (!renderer_.Init(Window())) {
        NEON_LOG_ERROR("Editor: renderer init failed");
        return false;
    }
    assetMgr_.Init(&renderer_);

    pixelFont_ = renderer_.CreateFontFromMemory(neon_rush::kEmbeddedFontData,
                                                neon_rush::kEmbeddedFontSize, 24);
    const std::vector<std::string> cjkSamples = {
        "霓虹编辑器场景层级属性名称位置旋转缩放颜色保存加载添加删除播放停止网格头盔立方体树木地面",
        "工具栏视口实体选择变换材质渲染确定取消打开文件写入成功失败无法松树",
        "块测试引擎演示场景树表单滚动列表地形添加节点自研控件树拖动分隔条右侧面板第行",
        "文件视图退出图层属性菜单帮助关于版本相机旋转中键平移滚轮拾取"};
    cjkFont_ = assetMgr_.LoadSystemCJKFont(24, cjkSamples);
    ui_.Init(&renderer_, cjkFont_.Valid() ? cjkFont_ : pixelFont_);

    if (!gfx::ImGuiNeon_Init(&renderer_, gfx::ImGuiNeon_SystemCJKPath())) {
        NEON_LOG_ERROR("Editor: Dear ImGui init failed");
        return false;
    }

    SetupScene();
    BuildCustomUIDemo();
    InitToolPanels();

    LoadEditorConfig();
    NEON_LOG_INFO("NeonEditor ready (%zu entities), project dir '%s'", entities_.size(),
                  projectDir_.c_str());
    return true;
}

void EditorApp::OnShutdown() {
    SaveEditorConfig();
    gfx::ImGuiNeon_Shutdown();
    renderer_.Shutdown();
}

void EditorApp::SetupScene() {
    auto add = [&](const std::string& key, const std::string& name, const math::Vec3& pos,
                   const math::Vec3& scale, const gfx::Color& tint) {
        SceneEntity e;
        e.name = name;
        e.meshKey = key;
        e.pos = pos;
        e.scale = scale;
        e.tint = tint;
        if (ResolveMesh(e)) {
            ApplyMaterialParams(e);
            entities_.push_back(std::move(e));
        }
    };

    add("terrain", "地面", {0, 0, 0}, {1, 1, 1}, gfx::Color::White);
    add("helmet", "头盔", {3, 0.9f, -2}, {1, 1, 1}, gfx::Color::White);
    add("cube", "测试方块", {-3, 0.6f, 1}, {1, 1, 1}, gfx::Color{0.9f, 0.4f, 0.2f, 1.0f});
    add("tree", "松树A", {-5, 0, -3}, {1.6f, 1.6f, 1.6f}, gfx::Color::White);
    add("tree", "松树B", {5, 0, 4}, {1.2f, 1.2f, 1.2f}, gfx::Color::White);
    LoadScene("editor_scene.json");
    selected_ = entities_.empty() ? -1 : 0;
}

void EditorApp::OnUpdate(float dt) {
    UpdateViewport(dt);
    gfx::ImGuiNeon_NewFrame(*Input(), pendingText_, dt);
    pendingText_.clear();
    ImGui::NewFrame();
    BuildImGuiUI();
    ImGui::Render();

    // The custom UI demo only processes input when ImGui does not capture it.
    if (!ImGui::GetIO().WantCaptureMouse) ui_.Update(*Input());

    // Open every tool panel right before the UI smoke test.
    if (smokeMode_ && TimeRef().frameIndex == 29) {
        showCustomUIDemo_ = true;
        showHierarchy_ = true;
        showInspector_ = true;
        showAssets_ = true;
        showResources_ = true;
        showLog_ = true;
    }
    if (smokeMode_ && TimeRef().frameIndex == 30) RunUISmokeTest();
    if (playing_) {
        for (SceneEntity& e : entities_) {
            if (e.meshKey == "helmet") {
                e.rot = math::Quat::FromEuler(0, TimeRef().elapsed * 0.8f, 0);
            }
        }
    }
}

void EditorApp::OnRender() {
    renderer_.BeginFrame({0.06f, 0.08f, 0.13f, 1.0f});
    renderer_.SetSky({0.05f, 0.08f, 0.16f, 1.0f}, {0.35f, 0.45f, 0.6f, 1.0f});
    renderer_.SetFog({0.3f, 0.38f, 0.5f, 1.0f}, 60.0f, 140.0f);
    renderer_.DrawSky();

    float aspect = static_cast<float>(renderer_.ScreenWidth()) / renderer_.ScreenHeight();
    gfx::Camera cam;
    cam.position = camTarget_ + math::Vec3{std::sin(yaw_) * std::cos(pitch_),
                                           std::sin(pitch_),
                                           std::cos(yaw_) * std::cos(pitch_)} *
                                        camDist_;
    cam.target = camTarget_;
    renderer_.SetCamera(cam, aspect);
    renderer_.SetDirectionalLight({-0.4f, -1.0f, -0.3f}, {1.0f, 0.95f, 0.85f}, 0.3f);

    for (const SceneEntity& e : entities_) {
        math::Mat4 model = math::Mat4::Translation(e.pos) * e.rot.ToMat4() *
                           math::Mat4::Scale(e.scale);
        renderer_.DrawMesh(e.mesh, e.material, model);
    }
    static bool dbg = false;
    if (!dbg && smokeMode_) {
        dbg = true;
        NEON_LOG_INFO("EDITOR-DRAW: entities=%zu drawCalls=%u", entities_.size(),
                      renderer_.Stats().drawCalls);
    }
    if (selected_ >= 0 && selected_ < static_cast<int>(entities_.size())) {
        const SceneEntity& e = entities_[static_cast<size_t>(selected_)];
        math::Mat4 model = math::Mat4::Translation(e.pos) * e.rot.ToMat4() *
                           math::Mat4::Scale(e.scale);
        math::AABB world = math::TransformAABB(e.mesh.Bounds(), model);
        renderer_.DrawBox(world, gfx::Color{0.3f, 0.8f, 1.0f, 1.0f});
    }

    if (showCustomUIDemo_) ui_.Draw(renderer_);
    renderer_.Flush2D();
    gfx::ImGuiNeon_RenderDrawData(ImGui::GetDrawData());

    if (!screenshotPath_.empty() && TimeRef().frameIndex >= screenshotFrame_) {
        std::vector<uint8_t> pixels;
        if (renderer_.CaptureFrame(pixels)) {
            stbi_write_png(screenshotPath_.c_str(), renderer_.ScreenWidth(),
                           renderer_.ScreenHeight(), 4, pixels.data(),
                           renderer_.ScreenWidth() * 4);
            NEON_LOG_INFO("Editor screenshot: %s", screenshotPath_.c_str());
        }
        screenshotPath_.clear();
    }
    renderer_.EndFrame();
}

void EditorApp::OnEvent(const platform::InputEvent& event) {
    if (event.type == platform::InputEvent::Type::TextInput) {
        if (!gfx::ImGuiNeon_WantCaptureKeyboard()) ui_.TextInput(event.text);
        pendingText_ += event.text;
    } else if (event.type == platform::InputEvent::Type::KeyDown) {
        if (!gfx::ImGuiNeon_WantCaptureKeyboard()) ui_.Key(event.key, true);
    } else if (event.type == platform::InputEvent::Type::KeyUp) {
        if (!gfx::ImGuiNeon_WantCaptureKeyboard()) ui_.Key(event.key, false);
    }
}

void EditorApp::UpdateViewport(float dt) {
    platform::IInput* input = Input();
    math::Vec2 mp = renderer_.ScreenToUI(input->MousePos());
    // ImGui tool windows capture mouse when hovered/active; the 3D viewport
    // area itself has no ImGui window, so camera controls stay responsive.
    bool overPanel = ImGui::GetIO().WantCaptureMouse;
    bool inViewport = mp.x >= viewportRect_.x && mp.x <= viewportRect_.x + viewportRect_.w &&
                      mp.y >= viewportRect_.y && mp.y <= viewportRect_.y + viewportRect_.h;

    if (!overPanel && inViewport) {
        if (input->MouseDown(platform::MouseButton::Right)) {
            yaw_ += -input->MouseDelta().x * 0.005f;
            pitch_ = math::Clamp(pitch_ + -input->MouseDelta().y * 0.005f, 0.05f, 1.4f);
        }
        if (input->MouseDown(platform::MouseButton::Middle)) {
            math::Vec3 fwd = (camTarget_ + math::Vec3{0, 0, 0} -
                              (camTarget_ + math::Vec3{std::sin(yaw_) * std::cos(pitch_),
                                                       std::sin(pitch_),
                                                       std::cos(yaw_) * std::cos(pitch_)} *
                                   camDist_))
                                 .Normalized();
            math::Vec3 right = math::Cross(fwd, {0, 1, 0}).Normalized();
            math::Vec3 upv = math::Cross(right, fwd);
            camTarget_ -= right * input->MouseDelta().x * 0.02f;
            camTarget_ += upv * input->MouseDelta().y * 0.02f;
        }
        float wheel = input->WheelDelta();
        if (std::fabs(wheel) > 0.01f) camDist_ = math::Clamp(camDist_ - wheel * 1.2f, 3.0f, 60.0f);
        if (input->MousePressed(platform::MouseButton::Left)) {
            float aspect = static_cast<float>(renderer_.ScreenWidth()) / renderer_.ScreenHeight();
            gfx::Camera cam;
            cam.position = camTarget_ + math::Vec3{std::sin(yaw_) * std::cos(pitch_),
                                                   std::sin(pitch_),
                                                   std::cos(yaw_) * std::cos(pitch_)} *
                                            camDist_;
            cam.target = camTarget_;
            math::Ray ray = ScreenRay(cam, aspect, mp);
            float best = 1e30f;
            int picked = -1;
            for (size_t i = 0; i < entities_.size(); ++i) {
                const SceneEntity& e = entities_[i];
                math::Mat4 model = math::Mat4::Translation(e.pos) * e.rot.ToMat4() *
                                   math::Mat4::Scale(e.scale);
                math::AABB world = math::TransformAABB(e.mesh.Bounds(), model);
                float t = 0.0f;
                if (math::IntersectRayAABB(ray, world, t) && t < best) {
                    best = t;
                    picked = static_cast<int>(i);
                }
            }
            selected_ = picked;
        }
    }
    (void)dt;
}

void EditorApp::BuildCustomUIDemo() {
    ui::Element* root = ui_.Root();

    auto win = std::make_unique<ui::Window>();
    win->name = "ui_demo";
    win->title = "引擎 UI 演示";
    win->rect = {280, 480, 720, 236};
    auto* dock = new ui::DockLayout();
    dock->name = "demo_dock";
    win->Add(std::unique_ptr<ui::Element>(dock));

    // Left pane: TreeView.
    auto left = std::make_unique<ui::Panel>();
    left->fill = false;
    auto tree = std::make_unique<ui::TreeView>();
    tree->name = "demo_tree";
    demoTree_ = tree.get();
    ui::TreeNode sceneNode;
    sceneNode.text = "场景";
    sceneNode.expanded = true;
    ui::TreeNode entitiesNode;
    entitiesNode.text = "实体";
    entitiesNode.expanded = true;
    ui::TreeNode groundNode;
    groundNode.text = "地面";
    ui::TreeNode helmetNode;
    helmetNode.text = "头盔";
    entitiesNode.children.push_back(groundNode);
    entitiesNode.children.push_back(helmetNode);
    ui::TreeNode lightsNode;
    lightsNode.text = "光照";
    sceneNode.children.push_back(entitiesNode);
    sceneNode.children.push_back(lightsNode);
    tree->nodes.push_back(sceneNode);
    left->Add(std::move(tree));
    dock->Add(std::move(left));

    // Center pane: TabBar with form / scroll-list pages.
    auto center = std::make_unique<ui::Panel>();
    center->fill = false;
    auto tabs = std::make_unique<ui::TabBar>();
    tabs->name = "demo_tabs";
    tabs->tabs = {"表单", "滚动列表"};
    auto form = std::make_unique<ui::VBox>();
    form->name = "page_form";
    auto combo = std::make_unique<ui::ComboBox>();
    combo->name = "demo_combo";
    combo->options = {"地形", "头盔", "方块", "松树"};
    combo->selected = 0;
    combo->onChange = [this](int i) { demoComboChanged_ = i; };
    demoCombo_ = combo.get();
    form->Add(std::move(combo));
    auto addBtn = std::make_unique<ui::Button>();
    addBtn->name = "add_tree_node";
    addBtn->text = "+添加节点";
    addBtn->onClick = [this] {
        if (!demoTree_) return;
        ui::TreeNode n;
        n.text = "节点" + std::to_string(++demoAddClicks_);
        demoTree_->nodes[0].children[0].children.push_back(std::move(n));
    };
    demoAddButton_ = addBtn.get();
    form->Add(std::move(addBtn));
    auto info = std::make_unique<ui::Label>();
    info->text = "自研控件树：DockLayout + TabBar + ComboBox + TreeView";
    form->Add(std::move(info));
    tabs->Add(std::move(form));
    auto scrollPage = std::make_unique<ui::Panel>();
    scrollPage->fill = false;
    auto scroll = std::make_unique<ui::ScrollArea>();
    scroll->name = "demo_scroll";
    auto list = std::make_unique<ui::List>();
    for (int i = 1; i <= 20; ++i) list->items.push_back("第 " + std::to_string(i) + " 行");
    scroll->Add(std::move(list));
    scrollPage->Add(std::move(scroll));
    tabs->Add(std::move(scrollPage));
    center->Add(std::move(tabs));
    dock->Add(std::move(center));

    // Right pane: info labels.
    auto right = std::make_unique<ui::Panel>();
    right->fill = false;
    auto vbox = std::make_unique<ui::VBox>();
    auto lbl1 = std::make_unique<ui::Label>();
    lbl1->text = "右侧面板";
    auto lbl2 = std::make_unique<ui::Label>();
    lbl2->text = "拖动分隔条调整布局";
    vbox->Add(std::move(lbl1));
    vbox->Add(std::move(lbl2));
    right->Add(std::move(vbox));
    dock->Add(std::move(right));

    root->Add(std::move(win));
}

void EditorApp::BuildImGuiUI() {
    if (ImGui::BeginMainMenuBar()) {
        if (ImGui::BeginMenu("文件")) {
            if (ImGui::MenuItem("保存场景", "Ctrl+S")) SaveScene();
            if (ImGui::MenuItem("加载场景", "Ctrl+L")) LoadScene("editor_scene.json");
            ImGui::Separator();
            if (ImGui::MenuItem("退出")) {
                if (Window()) Window()->RequestClose();
            }
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("视图")) {
            ImGui::MenuItem("场景", nullptr, &showHierarchy_);
            ImGui::MenuItem("属性", nullptr, &showInspector_);
            ImGui::MenuItem("资产", nullptr, &showAssets_);
            ImGui::MenuItem("资源", nullptr, &showResources_);
            ImGui::MenuItem("日志", nullptr, &showLog_);
            ImGui::Separator();
            ImGui::MenuItem("引擎 UI 演示", nullptr, &showCustomUIDemo_);
            ImGui::MenuItem("ImGui Demo", nullptr, &showImGuiDemo_);
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("项目")) {
            ImGui::TextUnformatted("项目目录");
            if (ImGui::InputText("##project_dir", projectDirBuf_, sizeof(projectDirBuf_),
                                 ImGuiInputTextFlags_EnterReturnsTrue)) {
                projectDir_ = projectDirBuf_;
                if (projectDir_.empty()) projectDir_ = ".";
                SaveEditorConfig();
            }
            ImGui::TextDisabled("导出场景写入 %s/scenes/exported_scene.json",
                                projectDir_.c_str());
            ImGui::Separator();
            if (ImGui::MenuItem("导出游戏场景")) ExportScene();
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("帮助")) {
            ImGui::MenuItem("关于", nullptr, false, false);
            ImGui::EndMenu();
        }
        ImGui::EndMainMenuBar();
    }

    // Docking layout: full-workspace dock space below the menu bar.
    const float menuH = ImGui::GetFrameHeight();
    const float toolH = 36.0f;
    ImGuiViewport* mainVp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(ImVec2(0.0f, menuH + toolH), ImGuiCond_Always);
    ImGui::SetNextWindowSize(
        ImVec2(mainVp->WorkSize.x, mainVp->WorkSize.y - menuH - toolH),
                             ImGuiCond_Always);
    ImGui::SetNextWindowViewport(mainVp->ID);
    ImGuiWindowFlags dsFlags = ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoTitleBar |
                               ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoResize |
                               ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoBringToFrontOnFocus |
                               ImGuiWindowFlags_NoNavFocus | ImGuiWindowFlags_NoBackground |
                               ImGuiWindowFlags_NoSavedSettings;
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
    ImGui::Begin("##NeonDockSpace", nullptr, dsFlags);
    ImGui::PopStyleVar(3);
    ImGuiID dockId = ImGui::GetID("NeonDockSpace");
    ImGui::DockSpace(dockId, ImVec2(0.0f, 0.0f), ImGuiDockNodeFlags_PassthruCentralNode);
    ImGui::End();

    // First-run default docking layout (skipped when the ini already restores one).
    static bool layoutAttempted = false;
    if (!layoutAttempted) {
        layoutAttempted = true;
        ImGuiDockNode* node = ImGui::DockBuilderGetNode(dockId);
        if (node == nullptr || !node->IsSplitNode()) {
            ImGui::DockBuilderRemoveNode(dockId);
            ImGui::DockBuilderAddNode(dockId, ImGuiDockNodeFlags_DockSpace);
            ImGui::DockBuilderSetNodeSize(dockId,
                                          ImVec2(mainVp->WorkSize.x,
                                                 mainVp->WorkSize.y - menuH - toolH));
            ImGuiID right = ImGui::DockBuilderSplitNode(dockId, ImGuiDir_Right, 0.24f,
                                                        nullptr, &dockId);
            ImGuiID left = ImGui::DockBuilderSplitNode(dockId, ImGuiDir_Left, 0.22f,
                                                       nullptr, &dockId);
            ImGuiID bottom = ImGui::DockBuilderSplitNode(dockId, ImGuiDir_Down, 0.30f,
                                                         nullptr, &dockId);
            ImGui::DockBuilderDockWindow("场景", left);
            ImGui::DockBuilderDockWindow("资产", left);
            ImGui::DockBuilderDockWindow("资源", left);
            ImGui::DockBuilderDockWindow("属性", right);
            ImGui::DockBuilderDockWindow("日志", right);
            ImGui::DockBuilderDockWindow("视口", dockId);
            ImGui::DockBuilderFinish(dockId);
        }
    }

    // Toolbar row below the menu bar.
    ImGui::SetNextWindowPos(ImVec2(0.0f, menuH), ImGuiCond_Always);
    ImGui::SetNextWindowSize(ImVec2(1280.0f, 34.0f), ImGuiCond_Always);
    ImGuiWindowFlags tbFlags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                               ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoSavedSettings |
                               ImGuiWindowFlags_NoFocusOnAppearing |
                               ImGuiWindowFlags_NoDocking;
    if (ImGui::Begin("##toolbar", nullptr, tbFlags)) {
        if (ImGui::Button("保存")) SaveScene();
        ImGui::SameLine();
        if (ImGui::Button("加载")) LoadScene("editor_scene.json");
        ImGui::SameLine();
        if (ImGui::Button("+头盔")) AddEntity("helmet");
        ImGui::SameLine();
        if (ImGui::Button("+方块")) AddEntity("cube");
        ImGui::SameLine();
        if (ImGui::Button("+松树")) AddEntity("tree");
        ImGui::SameLine();
        if (ImGui::Button("删除")) {
            if (selected_ >= 0 && selected_ < static_cast<int>(entities_.size())) {
                entities_.erase(entities_.begin() + selected_);
                selected_ = -1;
            }
        }
        ImGui::SameLine();
        if (ImGui::Button(playing_ ? "停止" : "播放")) playing_ = !playing_;
        ImGui::SameLine();
        if (ImGui::Button("导出场景")) ExportScene();
        ImGui::SameLine();
        ImGui::Text("实体 %zu", entities_.size());
    }
    ImGui::End();

    BuildScenePanel();
    BuildAssetPanel();
    BuildResourcePanel();
    BuildInspectorPanel();
    BuildLogPanel();
    BuildViewportPanel();

    if (showImGuiDemo_) ImGui::ShowDemoWindow(&showImGuiDemo_);
}

void EditorApp::RunUISmokeTest() {
    auto check = [this](bool ok, const char* what) {
        NEON_LOG_INFO("EDITOR-UI-SMOKE: [%s] %s", ok ? "PASS" : "FAIL", what);
        if (!ok) smokeFailed_ = true;
    };

    // --- Dear ImGui tool layer ---
    check(ImGui::GetCurrentContext() != nullptr, "ImGui context created");
    check(ImGui::GetIO().Fonts->IsBuilt(), "ImGui font atlas built");
    check(ImGui::GetIO().Fonts->Fonts.Size >= 1, "ImGui has at least one font");
    ImDrawData* dd = ImGui::GetDrawData();
    check(dd != nullptr && dd->CmdListsCount > 0, "ImGui produced draw data");

    // --- Custom (engine widget) UI demo ---
    check(demoTree_ != nullptr && demoCombo_ != nullptr && demoAddButton_ != nullptr,
          "custom UI demo widgets exist");
    if (!demoTree_ || !demoCombo_ || !demoAddButton_) return;

    size_t leavesBefore = demoTree_->nodes[0].children[0].children.size();
    math::Vec2 addAbs = demoAddButton_->AbsolutePos();
    math::Vec2 addCenter{addAbs.x + demoAddButton_->rect.w * 0.5f,
                         addAbs.y + demoAddButton_->rect.h * 0.5f};
    check(ui_.HitTestAt(addCenter) == demoAddButton_, "hit test finds add button");
    demoAddButton_->MouseMove(addCenter);
    demoAddButton_->MouseDown(addCenter, platform::MouseButton::Left);
    demoAddButton_->MouseUp(addCenter, platform::MouseButton::Left);
    check(demoTree_->nodes[0].children[0].children.size() == leavesBefore + 1,
          "add button adds a tree node");

    math::Vec2 treeAbs = demoTree_->AbsolutePos();
    math::Vec2 arrow{treeAbs.x + 10.0f, treeAbs.y + 12.0f};
    check(ui_.HitTestAt(arrow) == demoTree_, "hit test finds tree view");
    demoTree_->MouseDown(arrow, platform::MouseButton::Left);
    check(!demoTree_->nodes[0].expanded, "tree arrow collapses root");
    demoTree_->MouseDown(arrow, platform::MouseButton::Left);
    check(demoTree_->nodes[0].expanded, "tree arrow expands root again");

    math::Vec2 comboAbs = demoCombo_->AbsolutePos();
    math::Vec2 boxCenter{comboAbs.x + 40.0f, comboAbs.y + 10.0f};
    check(ui_.HitTestAt(boxCenter) == demoCombo_, "hit test finds combo box");
    demoCombo_->MouseDown(boxCenter, platform::MouseButton::Left);
    check(demoCombo_->open, "combo opens on click");
    math::Vec2 row1{comboAbs.x + 40.0f, comboAbs.y + demoCombo_->rect.h + 30.0f};
    demoCombo_->MouseDown(row1, platform::MouseButton::Left);
    check(demoCombo_->selected == 1 && demoComboChanged_ == 1,
          "combo selects row and fires onChange");
    check(!demoCombo_->open, "combo closes after selection");

    // --- Tool panels ---
    check(!core::GetRecentLogs(16).empty(), "log panel has engine log entries");
    check(!assetEntries_.empty(), "asset panel enumerated files");

    assets::AssetStats stats = assetMgr_.Stats();
    check(stats.textures >= 4, "resource panel: PBR textures cached");
    check(stats.meshes >= 1, "resource panel: meshes cached");

    size_t beforeImport = entities_.size();
    ImportAssetPath("assets/models/DamagedHelmet/DamagedHelmet.gltf");
    check(entities_.size() == beforeImport + 1, "asset import adds glTF entity");
    if (entities_.size() > beforeImport) {
        const SceneEntity& last = entities_.back();
        check(last.meshKey.rfind("gltf:", 0) == 0 && last.mesh.Valid(),
              "imported entity resolves glTF mesh");
    }

    // --- Editor config round-trip: save then load the project dir ---
    {
        const std::string cfgDir = GetTempDir() + "/cfg_proj";
        const std::string cfgPrev = projectDir_;
        projectDir_ = cfgDir;
        SaveEditorConfig();
        LoadEditorConfig();
        check(projectDir_ == cfgDir, "editor config project dir round-trips");
        projectDir_ = cfgPrev;
    }

    // --- Export → load round-trip (temp project dir; no repo pollution) ---
    const size_t exportCount = entities_.size();
    const std::string oldProjectDir = projectDir_;
    projectDir_ = GetTempDir();
    core::Status exportStatus = ExportScene();
    projectDir_ = oldProjectDir;
    check(exportStatus.Ok(), "export scene writes componentized JSON");
    if (exportStatus.Ok()) {
        std::string exportedPath = GetTempDir() + "/scenes/exported_scene.json";
        std::ifstream fin(exportedPath);
        std::stringstream fss;
        fss << fin.rdbuf();
        auto parsed = scene::SceneFile::Parse(fss.str());
        check(parsed.Ok(), "exported scene parses with SceneFile::Parse");
        if (parsed.Ok()) {
            check(parsed.Value().entities.size() == exportCount,
                  "exported scene contains every editor entity");
            if (!parsed.Value().entities.empty()) {
                check(parsed.Value().entities[0].name == entities_[0].name,
                      "exported entity name matches editor entity");
            }
        }
    }

    NEON_LOG_INFO("EDITOR-UI-SMOKE: all checks done");
}

void EditorApp::AddEntity(const std::string& meshKey) {
    static int counter = 1;
    math::Vec3 pos = camTarget_ + math::Vec3{0, 1.0f, -3.0f};
    std::string name;
    if (meshKey.rfind("obj:", 0) == 0 || meshKey.rfind("gltf:", 0) == 0) {
        std::string path = meshKey.substr(meshKey.find(':') + 1);
        size_t slash = path.find_last_of("/\\");
        size_t dot = path.find_last_of('.');
        size_t begin = slash == std::string::npos ? 0 : slash + 1;
        size_t len = (dot == std::string::npos || dot < begin) ? std::string::npos : dot - begin;
        name = path.substr(begin, len) + std::to_string(counter++);
    } else {
        name = meshKey + std::to_string(counter++);
    }
    SceneEntity e;
    e.name = name;
    e.meshKey = meshKey;
    e.pos = pos;
    if (meshKey == "tree") {
        e.scale = {1.6f, 1.6f, 1.6f};
    }
    if (ResolveMesh(e)) {
        ApplyMaterialParams(e);
        entities_.push_back(std::move(e));
        selected_ = static_cast<int>(entities_.size()) - 1;
    }
}

core::Status EditorApp::ExportScene() {
    if (entities_.empty())
        return core::Status::Err("editor: nothing to export (scene is empty)");

    core::Json root;
    root.type_ = core::Json::Type::Object;
    core::Json arr;
    arr.type_ = core::Json::Type::Array;
    for (const SceneEntity& e : entities_) {
        auto res = scene::SceneFile::MakeEntity(e.name, e.pos, e.rot, e.scale,
                                                ExportMeshKey(e.meshKey), e.metallic,
                                                e.roughness, e.tint);
        if (!res.Ok()) {
            NEON_LOG_ERROR("Editor: export aborted: %s", res.Error().c_str());
            return core::Status::Err("editor: " + res.Error());
        }
        arr.array_.push_back(res.Value());
    }
    root.object_["entities"] = std::move(arr);

    std::string base = projectDir_.empty() ? "." : projectDir_;
    std::string scenesDir = base + "/scenes";
    if (!EnsureDirs(scenesDir)) {
        NEON_LOG_ERROR("Editor: cannot create export directory '%s'", scenesDir.c_str());
        return core::Status::Err("editor: cannot create export directory '" + scenesDir + "'");
    }
    std::string path = scenesDir + "/exported_scene.json";
    std::string json = core::JsonWriter::Write(root);
    if (std::ofstream out(path); out.is_open()) {
        out << json;
        NEON_LOG_INFO("Editor: exported scene (%zu entities) -> %s", entities_.size(),
                      path.c_str());
        return core::Status::Ok(true);
    }
    NEON_LOG_ERROR("Editor: cannot write '%s'", path.c_str());
    return core::Status::Err("editor: cannot write '" + path + "'");
}

void EditorApp::LoadEditorConfig() {
    projectDir_ = ".";
    std::ifstream in("neon_editor_config.json");
    if (in.is_open()) {
        std::stringstream ss;
        ss << in.rdbuf();
        std::string err;
        core::Json root = core::Json::Parse(ss.str(), &err);
        if (root.IsObject()) {
            if (const core::Json* p = root.Get("projectDir")) projectDir_ = p->GetString();
        }
    }
    if (projectDir_.empty()) projectDir_ = ".";
    std::strncpy(projectDirBuf_, projectDir_.c_str(), sizeof(projectDirBuf_) - 1);
    projectDirBuf_[sizeof(projectDirBuf_) - 1] = '\0';
}

void EditorApp::SaveEditorConfig() {
    if (projectDir_.empty()) projectDir_ = ".";
    core::Json root;
    root.type_ = core::Json::Type::Object;
    core::Json p;
    p.type_ = core::Json::Type::String;
    p.string_ = projectDir_;
    root.object_["projectDir"] = p;
    std::string json = core::JsonWriter::Write(root);
    if (std::ofstream out("neon_editor_config.json"); out.is_open()) {
        out << json;
        NEON_LOG_INFO("Editor: config saved (project dir '%s')", projectDir_.c_str());
    } else {
        NEON_LOG_WARN("Editor: cannot write editor config");
    }
}

bool EditorApp::ResolveMesh(SceneEntity& e) {
    const std::string& key = e.meshKey;
    if (key == "terrain") {
        e.mesh = MakeTerrain(renderer_);
        e.material = gfx::Material::Lit({}, e.tint, 4.0f);
    } else if (key == "helmet") {
        assets::GltfAsset gltf =
            assetMgr_.LoadGLTF("assets/models/DamagedHelmet/DamagedHelmet.gltf");
        if (!gltf.nodes.empty()) {
            e.mesh = gltf.nodes[0].mesh;
            e.material = gltf.nodes[0].material;
        }
    } else if (key == "cube") {
        e.mesh = gfx::Mesh::CreateCube(renderer_, 1, 1, 1, "cube");
        e.material = gfx::Material::Lit({}, e.tint, 12.0f);
    } else if (key == "tree") {
        e.mesh =
            assetMgr_.LoadMeshOBJ("assets/kenney_nature/Models/OBJ format/tree_pineTallA.obj");
        e.material = gfx::Material::Lit({}, e.tint, 8.0f);
    } else if (key.rfind("obj:", 0) == 0) {
        e.mesh = assetMgr_.LoadMeshOBJ(key.substr(4));
        e.material = gfx::Material::Lit({}, e.tint, 8.0f);
    } else if (key.rfind("gltf:", 0) == 0) {
        assets::GltfAsset gltf = assetMgr_.LoadGLTF(key.substr(5));
        if (!gltf.nodes.empty()) {
            e.mesh = gltf.nodes[0].mesh;
            e.material = gltf.nodes[0].material;
        }
    }
    return e.mesh.Valid();
}

void EditorApp::ApplyMaterialParams(SceneEntity& e) {
    e.material.tint = e.tint;
    e.material.metallic = e.metallic;
    e.material.roughness = e.roughness;
}

void EditorApp::SaveScene() {
    core::Json root;
    root.type_ = core::Json::Type::Object;
    core::Json arr;
    arr.type_ = core::Json::Type::Array;
    for (const SceneEntity& e : entities_) {
        core::Json obj;
        obj.type_ = core::Json::Type::Object;
        core::Json name;
        name.type_ = core::Json::Type::String;
        name.string_ = e.name;
        obj.object_["name"] = name;
        auto str = [](const std::string& s) {
            core::Json j;
            j.type_ = core::Json::Type::String;
            j.string_ = s;
            return j;
        };
        auto num = [](double v) {
            core::Json j;
            j.type_ = core::Json::Type::Number;
            j.number_ = v;
            return j;
        };
        auto vec3 = [&](const math::Vec3& v) {
            core::Json j;
            j.type_ = core::Json::Type::Array;
            j.array_ = {num(v.x), num(v.y), num(v.z)};
            return j;
        };
        obj.object_["mesh"] = str(e.meshKey);
        obj.object_["pos"] = vec3(e.pos);
        obj.object_["scale"] = vec3(e.scale);
        core::Json tint;
        tint.type_ = core::Json::Type::Array;
        tint.array_ = {num(e.tint.r), num(e.tint.g), num(e.tint.b)};
        obj.object_["tint"] = tint;
        obj.object_["metallic"] = num(e.metallic);
        obj.object_["roughness"] = num(e.roughness);
        arr.array_.push_back(obj);
    }
    root.object_["entities"] = arr;
    std::string json = core::JsonWriter::Write(root);
    if (std::ofstream out("editor_scene.json"); out.is_open()) {
        out << json;
        NEON_LOG_INFO("Scene saved (%zu entities)", entities_.size());
    }
}

void EditorApp::LoadScene(const std::string& path) {
    std::ifstream in(path);
    if (!in.is_open()) return;
    std::stringstream ss;
    ss << in.rdbuf();
    std::string err;
    core::Json root = core::Json::Parse(ss.str(), &err);
    const core::Json* arr = root.Get("entities");
    if (!arr) return;
    // Replace entity list, re-resolve meshes.
    std::vector<SceneEntity> loaded;
    for (size_t i = 0; i < arr->Size(); ++i) {
        const core::Json* j = arr->At(i);
        if (!j) continue;
        SceneEntity e;
        e.name = j->Get("name")->GetString("entity");
        e.meshKey = j->Get("mesh")->GetString("cube");
        if (const core::Json* p = j->Get("pos")) {
            e.pos = {static_cast<float>(p->At(0)->GetNumber()),
                     static_cast<float>(p->At(1)->GetNumber()),
                     static_cast<float>(p->At(2)->GetNumber())};
        }
        if (const core::Json* s = j->Get("scale")) {
            e.scale = {static_cast<float>(s->At(0)->GetNumber()),
                       static_cast<float>(s->At(1)->GetNumber()),
                       static_cast<float>(s->At(2)->GetNumber())};
        }
        if (const core::Json* t = j->Get("tint")) {
            e.tint = {static_cast<float>(t->At(0)->GetNumber()),
                      static_cast<float>(t->At(1)->GetNumber()),
                      static_cast<float>(t->At(2)->GetNumber()), 1.0f};
        }
        if (const core::Json* m = j->Get("metallic")) e.metallic = static_cast<float>(m->GetNumber());
        if (const core::Json* r = j->Get("roughness")) e.roughness = static_cast<float>(r->GetNumber());
        if (ResolveMesh(e)) {
            ApplyMaterialParams(e);
            loaded.push_back(std::move(e));
        }
    }
    if (!loaded.empty()) {
        entities_ = std::move(loaded);
        selected_ = -1;
        NEON_LOG_INFO("Scene loaded (%zu entities)", entities_.size());
    }
}

} // namespace neon::editor
