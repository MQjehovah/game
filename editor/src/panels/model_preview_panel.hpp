#pragma once

// 模型查看器 —— 面板插件化（阶段 2，Task 7，沿用 Task 2-6 样板）：
// 原 EditorApp::BuildModelPreviewPanel / RenderModelPreviewPanel / OpenModelPreview /
// FrameModelPreview + ModelPreviewState（panels_preview.inc + editor.hpp）整体迁入本类。
//
// 与其它面板的差异（本面板有渲染 + 外部交互）：
// - Render() 由 EditorApp::RenderModelPreviewPanel 转发（editor_viewport 在主场景
//   之后调用，把模型画进离屏 RT）；
// - Tick(dt) 推进播放头（原 EditorApp::OnUpdate 的 preview 块）；
// - HandleViewportMouse(in) 接管悬停预览区的鼠标（右键拖拽旋转，消费时返回 true，
//   调用方提前 return 不让主视口相机驱动）；
// - OpenModelPreview(path) 是公共动作（editor_ui 右键菜单 / 调试覆盖层 / --preview
//   启动参数都调用，经 EditorApp 转发器到达这里）。
//
// 可见标志在过渡期指向 EditorApp::showModelPreview_（窗口菜单勾选 + ini 持久化 +
// 冒烟强制开启都读写它，阶段 3 收编到 PanelRegistry 后改为自有 bool）。
//
// ImGui 边界：Begin/End 由本面板自己的 Draw 负责（面板自治；注册表 ImGui-free）。

#include <memory>
#include <string>

#include "editor_context.hpp"
#include "imgui.h"
#include "neon/gfx/backend.hpp"
#include "neon/gfx/camera.hpp"
#include "neon/math/rect.hpp"

namespace neon::gfx {
class Renderer;
}
namespace neon::assets {
class AssetManager;
}
namespace neon::platform {
class IInput;
}

namespace neon::editor {

class ModelPreviewPanel : public IPanel {
public:
    ModelPreviewPanel(bool* visibleFlag, gfx::Renderer* renderer,
                      assets::AssetManager* assetMgr)
        : visible_(visibleFlag), renderer_(renderer), assetMgr_(assetMgr) {}

    const char* Title() const override { return "模型查看器"; }
    bool* VisibleFlag() override { return visible_; }
    void Draw(EditorContext& ctx) override;

    // --- 外部入口（经 EditorApp 转发器到达） --------------------------------
    void Open(const std::string& path);        // 原 EditorApp::OpenModelPreview
    void Render();                             // 原 EditorApp::RenderModelPreviewPanel
    void Tick(float dt);                       // 原 OnUpdate 的 preview 播放头推进
    // 鼠标悬停预览区时右键拖拽旋转预览相机；返回 true 表示消费了本次鼠标
    //（调用方应提前 return）。原 editor_viewport UpdateViewport 的 preview 块。
    bool HandleViewportMouse(const platform::IInput& in);

private:
    void FrameModelPreview(); // 自动初始视角（沿模型最小维度轴观察）

    bool* visible_ = nullptr;   // 不拥有；过渡期 = &EditorApp::showModelPreview_
    gfx::Renderer* renderer_ = nullptr;         // 不拥有
    assets::AssetManager* assetMgr_ = nullptr;  // 不拥有

    // 原 EditorApp::ModelPreviewState（editor.hpp:582-601）逐字段迁入。
    std::shared_ptr<scene::SkinnedModel> model;
    std::string path;
    char pathBuf[512] = "assets/models/wolf/Wolf-Blender-2.82a.gltf";
    bool playing = true;
    float time = 0.0f;
    int clip = 0;
    float yaw = 0.6f;
    float pitch = 0.3f;
    math::Rect2 screenRect{0, 0, 0, 0};
    gfx::RenderTargetHandle rt;
    gfx::TextureHandle rtColor;
    int rtW = 0;
    int rtH = 0;
    ImTextureID rtId = ImTextureID_Invalid;
};

} // namespace neon::editor
