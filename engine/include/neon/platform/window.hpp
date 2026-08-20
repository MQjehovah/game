#pragma once
#include <functional>
#include <memory>
#include <string>
#include "neon/math/vec2.hpp"
#include "neon/platform/input.hpp"

namespace neon::platform {

struct WindowConfig {
    std::string title = "NeonEngine";
    int width = 1280;
    int height = 720;
    bool resizable = true;
    bool vsync = true;
    int glMajor = 3;
    int glMinor = 3;
};

// Pure abstraction over one native window + its GL context.
// One implementation per platform (Win32 / X11 / Cocoa).
class IWindow {
public:
    virtual ~IWindow() = default;

    virtual bool Create(const WindowConfig& config) = 0;
    virtual void Destroy() = 0;

    // Pump the native message/event queue and forward events to onEvent.
    virtual void PumpEvents() = 0;

    virtual void SwapBuffers() = 0;
    virtual bool MakeGLContextCurrent() = 0;

    virtual bool ShouldClose() const = 0;
    virtual void RequestClose() = 0;

    virtual int Width() const = 0;
    virtual int Height() const = 0;

    // Mouse capture (relative look): hides the cursor and reports deltas.
    virtual void SetCaptureMouse(bool capture) = 0;

    std::function<void(const InputEvent&)> onEvent;
};

std::unique_ptr<IWindow> CreatePlatformWindow();

} // namespace neon::platform
