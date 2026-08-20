#pragma once
#include <cstdint>
#include <memory>
#include <string>
#include "neon/math/vec2.hpp"

namespace neon::platform {

enum class Key : uint16_t {
    Unknown = 0,
    A, B, C, D, E, F, G, H, I, J, K, L, M,
    N, O, P, Q, R, S, T, U, V, W, X, Y, Z,
    D0, D1, D2, D3, D4, D5, D6, D7, D8, D9,
    Space,
    Enter,
    Escape,
    Tab,
    Backspace,
    Shift,
    Control,
    Alt,
    ArrowUp,
    ArrowDown,
    ArrowLeft,
    ArrowRight,
    F1, F2, F3, F4, F5, F6, F7, F8, F9, F10, F11, F12,
    Count
};

enum class MouseButton : uint8_t { Left = 0, Right, Middle };

struct InputEvent {
    enum class Type : uint8_t {
        KeyDown,
        KeyUp,
        MouseMove,
        MouseDown,
        MouseUp,
        MouseWheel,
        Resize,
        Close,
        TextInput
    };

    Type type = Type::KeyDown;
    Key key = Key::Unknown;
    MouseButton button = MouseButton::Left;
    int x = 0;
    int y = 0;
    int wheel = 0;
    int dx = 0; // mouse delta since last event (relative look)
    int dy = 0;
    int width = 0;
    int height = 0;
    std::string text; // TextInput: one UTF-8 character
};

// Engine-side input state machine fed by platform events. One shared
// implementation for every platform backend.
class IInput {
public:
    virtual ~IInput() = default;

    virtual void HandleEvent(const InputEvent& event) = 0;

    virtual bool IsDown(Key key) const = 0;
    virtual bool Pressed(Key key) const = 0;
    virtual bool Released(Key key) const = 0;

    virtual bool MouseDown(MouseButton button) const = 0;
    virtual bool MousePressed(MouseButton button) const = 0;
    virtual bool MouseReleased(MouseButton button) const = 0;
    virtual math::Vec2 MousePos() const = 0;
    virtual math::Vec2 MouseDelta() const = 0;
    virtual float WheelDelta() const = 0;

    // Call once per rendered frame; clears edge flags.
    virtual void EndFrame() = 0;
};

std::unique_ptr<IInput> CreateInputState();

} // namespace neon::platform
