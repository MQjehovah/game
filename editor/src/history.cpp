#include "history.hpp"

namespace neon::editor {

HistoryManager::HistoryManager(size_t maxDepth) : maxDepth_(maxDepth) {}

void HistoryManager::Push(std::unique_ptr<Command> cmd) {
    if (!cmd || cmd->IsNoop()) return;
    if (!undo_.empty() && undo_.back()->Merge(*cmd)) {
        // The top entry adopted the incoming CURRENT value; re-apply it so the
        // scene reflects the merged result. A merge invalidates the redo stack
        // just like a fresh push.
        undo_.back()->Apply();
        redo_.clear();
        return;
    }
    cmd->Apply();
    undo_.push_back(std::move(cmd));
    redo_.clear();
    if (maxDepth_ > 0 && undo_.size() > maxDepth_) undo_.erase(undo_.begin());
}

bool HistoryManager::Undo() {
    if (undo_.empty()) return false;
    std::unique_ptr<Command> cmd = std::move(undo_.back());
    undo_.pop_back();
    cmd->Undo();
    redo_.push_back(std::move(cmd));
    return true;
}

bool HistoryManager::Redo() {
    if (redo_.empty()) return false;
    std::unique_ptr<Command> cmd = std::move(redo_.back());
    redo_.pop_back();
    cmd->Redo();
    undo_.push_back(std::move(cmd));
    return true;
}

void HistoryManager::Clear() {
    undo_.clear();
    redo_.clear();
}

} // namespace neon::editor
