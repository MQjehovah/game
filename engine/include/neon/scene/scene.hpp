#pragma once
#include <memory>
#include "neon/gfx/renderer.hpp"
#include "neon/platform/input.hpp"

namespace neon::scene {

class Scene {
public:
    virtual ~Scene() = default;
    virtual void OnEnter() {}
    virtual void OnExit() {}
    virtual void Update(float dt) = 0;
    virtual void Draw(gfx::Renderer& renderer) = 0;
    virtual void OnEvent(const platform::InputEvent&) {}
};

class SceneManager {
public:
    void Change(std::unique_ptr<Scene> next);
    Scene* Current() { return current_.get(); }

    void Update(float dt);
    void Draw(gfx::Renderer& renderer);
    void OnEvent(const platform::InputEvent& event);

private:
    void ApplyPending();
    std::unique_ptr<Scene> current_;
    std::unique_ptr<Scene> pending_;
    bool hasPending_ = false;
};

} // namespace neon::scene
