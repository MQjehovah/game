#include <cstdint>

#include "neon/neon.hpp"
#include "neon/gfx/backend.hpp"
#include "helpers.hpp"
#include "test_backend.hpp"

using namespace neon;

// G6-1: the GPU memory query API reports zeros when the driver/backend cannot
// provide a budget (NullBackend has no driver) — the contract callers rely on.
TEST(RendererGpuMemoryUnavailable) {
    test::HeadlessAssetFixture fx;
    const gfx::IRenderBackend::GpuMemStats gpu = fx.renderer.GpuMemory();
    CHECK_EQ(gpu.totalBytes, 0u);
    CHECK_EQ(gpu.usedBytes, 0u);
}
