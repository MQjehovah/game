#pragma once

// Client-side input bridge (T6.4): forwards the real platform input into both
// the local prediction runtime (scripts read InputAxis/InputKey) and the
// MsgInput builder, so the same input drives local prediction and the
// authoritative server. In smoke/headless runs SetForceMove(true) synthesizes
// W (moveY = +1) so the controlled entity moves without a human at the
// keyboard.
//
// The MsgInput.buttons bitmask MUST match the server's NetInput mapping
// (server/src/net_input.hpp): bit0 -> Space, bit1 -> Shift, bit2 -> Control,
// bit3 -> F. moveX/moveY are the strafe/forward axes (D-A, W-S).

#include <cstdint>

#include "neon/math/vec2.hpp"
#include "neon/platform/input.hpp"

namespace neon::client {

// MsgInput.buttons bit assignments (mirrors server::NetInput).
inline constexpr uint8_t kButtonJump = 0x01;     // -> Space
inline constexpr uint8_t kButtonSprint = 0x02;   // -> Shift
inline constexpr uint8_t kButtonControl = 0x04;  // -> Control
inline constexpr uint8_t kButtonInteract = 0x08; // -> F

// Bridges real input to the prediction runtime + the wire. All methods
// forward to `base`; SetForceMove(true) overrides W (used by --ticks smoke).
class ClientInput : public platform::IInput {
public:
    ClientInput() = default;
    explicit ClientInput(platform::IInput* base) : base_(base) {}

    void SetBase(platform::IInput* base) { base_ = base; }
    void SetForceMove(bool on) { forceMove_ = on; }

    // ---- platform::IInput ------------------------------------------------
    void HandleEvent(const platform::InputEvent& event) override {
        if (base_) base_->HandleEvent(event);
    }

    bool IsDown(platform::Key key) const override {
        if (forceMove_ && key == platform::Key::W) return true;
        return base_ ? base_->IsDown(key) : false;
    }
    bool Pressed(platform::Key key) const override {
        return base_ ? base_->Pressed(key) : false;
    }
    bool Released(platform::Key key) const override {
        return base_ ? base_->Released(key) : false;
    }

    bool MouseDown(platform::MouseButton button) const override {
        return base_ ? base_->MouseDown(button) : false;
    }
    bool MousePressed(platform::MouseButton button) const override {
        return base_ ? base_->MousePressed(button) : false;
    }
    bool MouseReleased(platform::MouseButton button) const override {
        return base_ ? base_->MouseReleased(button) : false;
    }
    math::Vec2 MousePos() const override { return base_ ? base_->MousePos() : math::Vec2{}; }
    math::Vec2 MouseDelta() const override { return base_ ? base_->MouseDelta() : math::Vec2{}; }
    float WheelDelta() const override { return base_ ? base_->WheelDelta() : 0.0f; }

    void EndFrame() override {
        if (base_) base_->EndFrame();
    }

private:
    platform::IInput* base_ = nullptr;
    bool forceMove_ = false;
};

} // namespace neon::client
