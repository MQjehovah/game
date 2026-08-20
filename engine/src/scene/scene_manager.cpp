#include "neon/scene/scene.hpp"

namespace neon::scene {

void SceneManager::Change(std::unique_ptr<Scene> next) {
    pending_ = std::move(next);
    hasPending_ = true;
}

void SceneManager::ApplyPending() {
    if (!hasPending_) return;
    hasPending_ = false;
    if (current_) current_->OnExit();
    current_ = std::move(pending_);
    if (current_) current_->OnEnter();
}

void SceneManager::Update(float dt) {
    ApplyPending();
    if (current_) current_->Update(dt);
}

void SceneManager::Draw(gfx::Renderer& renderer) {
    ApplyPending();
    if (current_) current_->Draw(renderer);
}

void SceneManager::OnEvent(const platform::InputEvent& event) {
    ApplyPending();
    if (current_) current_->OnEvent(event);
}

} // namespace neon::scene
