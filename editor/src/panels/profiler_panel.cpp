#include "panels/profiler_panel.hpp"

// 性能面板实现 = 原 EditorApp::BuildProfilerPanel（panels_debug.inc:7-62）方法体
// 逐行迁移：EditorApp 成员（showProfiler_/renderer_/profiler_/profilerDrawn_/
// playActive_/play_/entities_/assetMgr_）改本类 visible_ / ctx 指针 / ctx 回调。
// 行为零变化。kProfilerSamples 随 ProfilerState 提升为 ProfilerState::kSamples。

#include "editor.hpp"
#include "imgui.h"
#include "neon/core/mem_stats.hpp"

namespace neon::editor {

void ProfilerPanel::Draw(EditorContext& ctx) {
    if (!visible_ || !*visible_) return;
    if (ImGui::Begin("性能", visible_)) {
        const float ms = ctx.time->delta * 1000.0f;
        const gfx::Renderer::RenderStats& st = ctx.renderer->Stats();
        ctx.profiler->ms[ctx.profiler->msHead] = ms;
        ctx.profiler->msHead = (ctx.profiler->msHead + 1) % ProfilerState::kSamples;

        ImGui::Text("帧时间 %.2f ms | %.1f FPS", ms, ctx.time->Fps());
        ImGui::Text("Draw 调用 %u | 三角形 %u | 实例 %u", st.drawCalls, st.triangles,
                    st.instances);

        const bool playing = ctx.playActive && *ctx.playActive;
        const size_t sceneEnts = ctx.entities->size();
        const size_t playEnts = playing ? ctx.playEntityCount() : 0;
        const size_t bodies = playing ? ctx.playBodyCount() : 0;
        const size_t trees = playing ? ctx.playBtCount() : 0;
        const size_t scripts = playing ? ctx.playScriptCount() : 0;
        ImGui::Text("实体 %zu (播放 %zu) | 物理刚体 %zu", sceneEnts, playEnts, bodies);
        ImGui::Text("行为树 %zu | 脚本 %zu", trees, scripts);

        const assets::AssetStats a = ctx.assetMgr->Stats();
        ImGui::Text("资产 %zu | 网格 %zu | 三角形 %zu", a.textures, a.meshes, a.meshTriangles);
        ImGui::Text("纹理内存 %.2f MB",
                    static_cast<double>(a.textureBytes) / (1024.0 * 1024.0));

        // G6-3: global heap tracking (operator new/delete overrides).
        const core::MemStats::Snapshot heap = core::MemStats::SnapshotNow();
        ImGui::Text("堆: 存活 %.2f MB | 峰值 %.2f MB | 分配 %llu 次",
                    static_cast<double>(heap.liveBytes) / (1024.0 * 1024.0),
                    static_cast<double>(heap.peakLiveBytes) / (1024.0 * 1024.0),
                    static_cast<unsigned long long>(heap.allocCount));

        // G6-1: driver-reported GPU memory budget/usage (0 = not reported).
        const gfx::IRenderBackend::GpuMemStats gpu = ctx.renderer->GpuMemory();
        if (gpu.totalBytes > 0) {
            ImGui::Text("GPU 显存: 总 %.2f GB | 已用 %.2f MB",
                        static_cast<double>(gpu.totalBytes) / (1024.0 * 1024.0 * 1024.0),
                        static_cast<double>(gpu.usedBytes) / (1024.0 * 1024.0));
        } else {
            ImGui::Text("GPU 显存: 驱动未报告 (GL_NVX_gpu_memory_info / GL_ATI_meminfo 不可用)");
        }

        ImGui::Separator();
        // Plot the ring buffer in chronological order (oldest = profiler.msHead)
        // so the graph reads left-to-right instead of jumping when the head
        // wraps around the fixed array.
        float wrapped[ProfilerState::kSamples];
        for (int i = 0; i < ProfilerState::kSamples; ++i)
            wrapped[i] = ctx.profiler->ms[(ctx.profiler->msHead + i) % ProfilerState::kSamples];
        ImGui::PlotLines("##frame_ms", wrapped, ProfilerState::kSamples, 0, "帧时间 (ms)",
                         0.0f, 40.0f, ImVec2(-1.0f, 88.0f));
        *ctx.profilerDrawn = true;
    }
    ImGui::End();
}

} // namespace neon::editor
