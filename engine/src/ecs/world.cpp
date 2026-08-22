#include "neon/ecs/world.hpp"

namespace neon::ecs {

World::World() = default;
World::~World() = default;

void World::RejectParallelMutation(const char* op) {
    NEON_LOG_CAT(neon::core::LogCategory::Ecs, neon::core::LogLevel::Error,
                 "World::%s() rejected: world mutation is forbidden inside a parallel iteration",
                 op);
}

Entity World::Create() {
    assert(!inParallelIteration_ && "World::Create() forbidden inside a parallel iteration");
    if (inParallelIteration_) {
        RejectParallelMutation("Create");
        return {}; // no-op: invalid entity
    }
    uint32_t id = 0;
    if (freeIds_.empty()) {
        id = static_cast<uint32_t>(generations_.size());
        generations_.push_back(1);
    } else {
        id = freeIds_.back();
        freeIds_.pop_back();
    }
    ++aliveCount_;
    return {id, generations_[id]};
}

void World::Destroy(Entity e) {
    assert(!inParallelIteration_ && "World::Destroy() forbidden inside a parallel iteration");
    if (inParallelIteration_) {
        RejectParallelMutation("Destroy");
        return;
    }
    if (!Alive(e)) return;
    ++generations_[e.id];
    freeIds_.push_back(e.id);
    --aliveCount_;
    for (auto& pool : pools_) pool->Remove(e.id);
}

bool World::Alive(Entity e) const {
    return e.id != 0 && e.id < generations_.size() && generations_[e.id] == e.generation;
}

void World::Clear() {
    generations_.assign(1, 1);
    freeIds_.clear();
    aliveCount_ = 0;
    for (auto& pool : pools_) pool->Clear();
}

} // namespace neon::ecs
