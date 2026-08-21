#include "neon/core/app.hpp"

#include <chrono>
#include "neon/core/log.hpp"

namespace neon::core {

namespace {

double NowSeconds() {
    using namespace std::chrono;
    return duration<double>(steady_clock::now().time_since_epoch()).count();
}

constexpr float kFixedDt = 1.0f / 60.0f;

} // namespace

int Application::Run(const platform::WindowConfig& config) {
    window_ = platform::CreatePlatformWindow();
    if (!window_ || !window_->Create(config)) {
        NEON_LOG_ERROR("Application: failed to create platform window");
        return 1;
    }
    NEON_LOG_INFO("Application: window created (%dx%d)", window_->Width(), window_->Height());

    input_ = platform::CreateInputState();
    window_->onEvent = [this](const platform::InputEvent& event) {
        if (input_) input_->HandleEvent(event);
        OnEvent(event);
    };

    if (!OnCreate()) {
        NEON_LOG_ERROR("Application: OnCreate failed");
        window_->Destroy();
        return 2;
    }

    double previous = NowSeconds();
    float accumulator = 0.0f;

    while (!window_->ShouldClose()) {
        window_->PumpEvents();

        double now = NowSeconds();
        float frameTime = static_cast<float>(now - previous);
        previous = now;
        frameTime = frameTime > 0.25f ? 0.25f : frameTime;
        TimeRef().delta = frameTime;

        accumulator += frameTime;
        while (accumulator >= kFixedDt) {
            // Logs during OnUpdate carry the current tick as their frame number
            // (1-based tick counter). frameIndex itself is incremented after
            // OnUpdate so readers see the same value as before.
            SetLogFrame(TimeRef().frameIndex + 1);
            OnUpdate(kFixedDt);
            accumulator -= kFixedDt;
            TimeRef().elapsed += kFixedDt;
            TimeRef().frameIndex++;

            if (smokeFrames_ > 0 && TimeRef().frameIndex >= static_cast<uint64_t>(smokeFrames_)) {
                window_->RequestClose();
                break;
            }
        }

        OnRender();
        input_->EndFrame();
    }

    OnShutdown();
    window_->Destroy();
    return 0;
}

} // namespace neon::core
