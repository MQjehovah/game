#include "neon/platform/input.hpp"

#include <array>

namespace neon::platform {

namespace {

constexpr size_t kKeyCount = static_cast<size_t>(Key::Count);

class InputState : public IInput {
public:
    void HandleEvent(const InputEvent& event) override {
        switch (event.type) {
            case InputEvent::Type::KeyDown:
                keys_[static_cast<size_t>(event.key)] = true;
                break;
            case InputEvent::Type::KeyUp:
                keys_[static_cast<size_t>(event.key)] = false;
                break;
            case InputEvent::Type::MouseMove:
                mousePos_ = {static_cast<float>(event.x), static_cast<float>(event.y)};
                mouseDelta_ += {static_cast<float>(event.dx), static_cast<float>(event.dy)};
                break;
            case InputEvent::Type::MouseDown:
                mouse_[static_cast<size_t>(event.button)] = true;
                break;
            case InputEvent::Type::MouseUp:
                mouse_[static_cast<size_t>(event.button)] = false;
                break;
            case InputEvent::Type::MouseWheel:
                wheel_ += static_cast<float>(event.wheel);
                break;
            case InputEvent::Type::Resize:
            case InputEvent::Type::Close:
                break;
        }
    }

    bool IsDown(Key key) const override {
        size_t i = static_cast<size_t>(key);
        return i < kKeyCount && keys_[i];
    }
    bool Pressed(Key key) const override {
        size_t i = static_cast<size_t>(key);
        return i < kKeyCount && keys_[i] && !prevKeys_[i];
    }
    bool Released(Key key) const override {
        size_t i = static_cast<size_t>(key);
        return i < kKeyCount && !keys_[i] && prevKeys_[i];
    }

    bool MouseDown(MouseButton button) const override {
        return mouse_[static_cast<size_t>(button)];
    }
    bool MousePressed(MouseButton button) const override {
        size_t i = static_cast<size_t>(button);
        return mouse_[i] && !prevMouse_[i];
    }
    bool MouseReleased(MouseButton button) const override {
        size_t i = static_cast<size_t>(button);
        return !mouse_[i] && prevMouse_[i];
    }
    math::Vec2 MousePos() const override { return mousePos_; }
    math::Vec2 MouseDelta() const override { return mouseDelta_; }
    float WheelDelta() const override { return wheel_; }

    void EndFrame() override {
        prevKeys_ = keys_;
        prevMouse_ = mouse_;
        wheel_ = 0.0f;
        mouseDelta_ = {0.0f, 0.0f};
    }

private:
    std::array<bool, kKeyCount> keys_{};
    std::array<bool, kKeyCount> prevKeys_{};
    std::array<bool, 3> mouse_{};
    std::array<bool, 3> prevMouse_{};
    math::Vec2 mousePos_;
    math::Vec2 mouseDelta_;
    float wheel_ = 0.0f;
};

} // namespace

std::unique_ptr<IInput> CreateInputState() {
    return std::make_unique<InputState>();
}

} // namespace neon::platform
