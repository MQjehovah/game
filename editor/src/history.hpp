#pragma once

#include <functional>
#include <memory>
#include <utility>
#include <vector>

// Generic undo/redo command stack for the editor.
//
// The stack is deliberately editor-independent: `Command` is type-erased and
// `HistoryManager` only moves `unique_ptr<Command>` around, so this header can
// be unit-tested without pulling in the editor (see tests/test_history.cpp).
// Concrete commands (entity add/delete/duplicate/reorder, transform and
// property edits) live in editor_history.hpp and hold a pointer to the
// editor's `std::vector<SceneEntity>`.
//
// Merge policy: HistoryManager::Push applies the command, pushes it onto the
// undo stack and clears the redo stack. If the top undo entry reports it can
// absorb the incoming command (Command::Merge), Push keeps that single entry
// and re-applies it with the merged CURRENT value, so one gizmo drag / slider
// drag becomes ONE undo step that reverts to the ORIGINAL value and re-applies
// the LAST value. Commands that report IsNoop() are discarded without pushing.

#include <cstddef>
#include <memory>
#include <vector>

namespace neon::editor {

// Type-erased reversible scene edit. Concrete commands capture enough state
// (entity index + original/new values) to apply, undo and reapply the change
// against the editor's entity list.
class Command {
public:
    virtual ~Command() = default;
    virtual void Apply() = 0; // execute the change
    virtual void Undo() = 0;  // revert the change

    // Re-apply a previously undone change. Most commands are symmetric with
    // Apply() and can rely on the default; a command with distinct state only
    // needs to override when Redo differs from Apply.
    virtual void Redo() { Apply(); }

    // Coalesce `incoming` into `this` (same operation, consecutive edits).
    // Returns true when merged; the receiver must adopt the incoming CURRENT
    // value while keeping its own ORIGINAL for undo. HistoryManager::Push
    // re-applies the receiver after a successful merge, so a merged receiver
    // must also leave the scene at the incoming value.
    virtual bool Merge(const Command& /*incoming*/) { return false; }

    // True when applying this command would not change the scene (e.g. a zero
    // drag delta); HistoryManager::Push discards such commands.
    virtual bool IsNoop() const { return false; }
};

// Undo/redo stacks with an optional depth cap (default 100) that bounds memory
// by evicting the oldest entries. Undo pops the top undo command, reverts it
// and pushes it onto the redo stack; Redo does the reverse. LIFO undo / FIFO
// redo guarantee that entity indices captured by commands are valid at each
// execution step.
class HistoryManager {
public:
    explicit HistoryManager(size_t maxDepth = 100);

    // Apply `cmd` and push it onto the undo stack (clears the redo stack).
    // Noops are discarded; commands that merge into the current top entry
    // collapse into it instead of pushing a new step.
    void Push(std::unique_ptr<Command> cmd);

    // Undo the most recent change. Returns false when there is nothing to undo.
    bool Undo();
    // Re-apply the most recently undone change. Returns false when nothing.
    bool Redo();

    void Clear();
    bool CanUndo() const { return !undo_.empty(); }
    bool CanRedo() const { return !redo_.empty(); }
    size_t UndoDepth() const { return undo_.size(); }
    size_t RedoDepth() const { return redo_.size(); }

    // Non-owning peek at the most recent undo entry (for callers that want to
    // tweak it, e.g. seal a finished gizmo drag so it stops merging). Null when
    // the undo stack is empty.
    Command* TopUndo() const { return undo_.empty() ? nullptr : undo_.back().get(); }

    // G5-4: invoked after a command applies (Push / Undo / Redo). The editor
    // uses it to keep the runtime sceneWorld_ in sync with the working model.
    std::function<void()> onChanged;

private:
    std::vector<std::unique_ptr<Command>> undo_;
    std::vector<std::unique_ptr<Command>> redo_;
    size_t maxDepth_;
};

} // namespace neon::editor
