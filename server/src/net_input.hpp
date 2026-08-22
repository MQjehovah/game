#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

#include "neon/math/vec2.hpp"
#include "neon/platform/input.hpp"

namespace neon::server {

// A platform::IInput fed by network input messages instead of OS events. The
// authoritative server owns one instance and wires it into the headless
// runtime's ScriptContext (GameRuntimeConfig::input), so data-driven scripts
// read the exact same InputAxis/InputKey/InputMouse bindings they would from a
// local player.
//
// Mapping (v1, documented here and reused by T6.4's client):
//   MsgInput.buttons bitmask  -> engine Key
//     bit 0 (0x01)  -> Space   (jump / action; the sample scripts'
//                               InputKey("space"))
//     bit 1 (0x02)  -> Shift   (sprint)
//     bit 2 (0x04)  -> Control (crouch)
//     bit 3 (0x08)  -> F       (interact)
//   MsgInput.moveY > +0.5 -> W  (InputAxis("forward")  -> +1)
//   MsgInput.moveY < -0.5 -> S  (InputAxis("forward")  -> -1)
//   MsgInput.moveX > +0.5 -> D  (InputAxis("strafe")   -> +1)
//   MsgInput.moveX < -0.5 -> A  (InputAxis("strafe")   -> -1)
//
// Held-state semantics: SetInput() snapshots the whole state, matching a
// button/axis report from the last MsgInput. IsDown is what the script
// bindings read; Pressed/Released edges are derived between SetInput calls
// (the server calls EndFrame after each fixed tick).
class NetInput : public platform::IInput {
public:
    static constexpr uint8_t kButtonJump = 0x01;     // -> Space
    static constexpr uint8_t kButtonSprint = 0x02;   // -> Shift
    static constexpr uint8_t kButtonCrouch = 0x04;   // -> Control
    static constexpr uint8_t kButtonInteract = 0x08; // -> F

    // Replaces the entire held input state from one network report.
    void SetInput(uint8_t buttons, float moveX, float moveY) {
        keys_.fill(false);
        SetButton(platform::Key::W, moveY > 0.5f);
        SetButton(platform::Key::S, moveY < -0.5f);
        SetButton(platform::Key::D, moveX > 0.5f);
        SetButton(platform::Key::A, moveX < -0.5f);
        SetButton(platform::Key::Space, (buttons & kButtonJump) != 0);
        SetButton(platform::Key::Shift, (buttons & kButtonSprint) != 0);
        SetButton(platform::Key::Control, (buttons & kButtonCrouch) != 0);
        SetButton(platform::Key::F, (buttons & kButtonInteract) != 0);
    }

    // ---- platform::IInput -------------------------------------------------
    void HandleEvent(const platform::InputEvent&) override {}

    bool IsDown(platform::Key key) const override {
        const size_t i = static_cast<size_t>(key);
        return i < keys_.size() && keys_[i];
    }
    bool Pressed(platform::Key key) const override {
        const size_t i = static_cast<size_t>(key);
        return i < keys_.size() && keys_[i] && !prev_[i];
    }
    bool Released(platform::Key key) const override {
        const size_t i = static_cast<size_t>(key);
        return i < keys_.size() && !keys_[i] && prev_[i];
    }

    bool MouseDown(platform::MouseButton) const override { return false; }
    bool MousePressed(platform::MouseButton) const override { return false; }
    bool MouseReleased(platform::MouseButton) const override { return false; }
    math::Vec2 MousePos() const override { return {}; }
    math::Vec2 MouseDelta() const override { return {}; }
    float WheelDelta() const override { return 0.0f; }

    void EndFrame() override { prev_ = keys_; }

private:
    void SetButton(platform::Key key, bool down) {
        const size_t i = static_cast<size_t>(key);
        if (i < keys_.size()) keys_[i] = down;
    }

    std::array<bool, static_cast<size_t>(platform::Key::Count)> keys_{};
    std::array<bool, static_cast<size_t>(platform::Key::Count)> prev_{};
};

} // namespace neon::server
