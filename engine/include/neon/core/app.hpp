#pragma once
#include <memory>
#include "neon/core/time.hpp"
#include "neon/platform/input.hpp"
#include "neon/platform/window.hpp"

namespace neon::core {

// Application owns the platform window/input and runs the game loop.
// The renderer/audio live in the game layer so this core module stays
// platform- and graphics-agnostic.
class Application {
public:
    virtual ~Application() = default;

    int Run(const platform::WindowConfig& config);

    virtual bool OnCreate() = 0;
    virtual void OnShutdown() = 0;
    virtual void OnUpdate(float dt) = 0;
    virtual void OnRender() = 0;
    virtual void OnEvent(const platform::InputEvent&) {}

    // Smoke-test mode: exit successfully after N fixed simulation steps.
    void SetSmokeTestFrames(int frames) { smokeFrames_ = frames; }
    int SmokeTestFrames() const { return smokeFrames_; }

protected:
    platform::IWindow* Window() { return window_.get(); }
    platform::IInput* Input() { return input_.get(); }
    Time& TimeRef() { return time_; }
    const Time& TimeRef() const { return time_; }

private:
    std::unique_ptr<platform::IWindow> window_;
    std::unique_ptr<platform::IInput> input_;
    Time time_;
    int smokeFrames_ = 0;
};

} // namespace neon::core
