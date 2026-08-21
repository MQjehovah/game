// Undo/redo command stack (generic HistoryManager from editor/src/history.hpp).
// The generic manager is editor-independent, so these tests need no editor
// sources: they exercise push/undo/redo/clear, the undo-depth cap, merge
// coalescing, no-op skipping and the "fresh push clears redo" rule with small
// local commands.
#include <memory>

#include "history.hpp"
#include "helpers.hpp"

namespace {
using neon::editor::Command;
using neon::editor::HistoryManager;

// A command that adds a constant delta to a shared int.
struct AddCommand : public Command {
    int* value;
    int delta;
    AddCommand(int* v, int d) : value(v), delta(d) {}
    void Apply() override { *value += delta; }
    void Undo() override { *value -= delta; }
};

// A value-chain-mergeable command (models EditPropertyCommand / gizmo drags):
// consecutive edits of the same target coalesce into one undo step that
// reverts to the ORIGINAL value and re-applies the LAST value.
struct ValueCommand : public Command {
    int* value;
    int old_;
    int cur_;
    bool mergeable;
    ValueCommand(int* v, int old, int cur, bool m = true)
        : value(v), old_(old), cur_(cur), mergeable(m) {}
    void Apply() override { *value = cur_; }
    void Undo() override { *value = old_; }
    bool Merge(const Command& incoming) override {
        const ValueCommand* other = dynamic_cast<const ValueCommand*>(&incoming);
        if (!other || !mergeable) return false;
        if (other->old_ != cur_) return false; // not a consecutive edit chain
        cur_ = other->cur_;
        return true;
    }
    bool IsNoop() const override { return old_ == cur_; }
};
} // namespace

TEST(HistoryPushApplyUndoRedo) {
    int v = 0;
    HistoryManager h;
    CHECK(!h.CanUndo());
    CHECK(!h.CanRedo());
    h.Push(std::make_unique<AddCommand>(&v, 5));
    CHECK_EQ(v, 5); // Push applies immediately
    CHECK(h.CanUndo());
    CHECK(!h.CanRedo());
    CHECK_EQ(h.UndoDepth(), 1u);
    CHECK(h.Undo());
    CHECK_EQ(v, 0);
    CHECK(!h.CanUndo());
    CHECK(h.CanRedo());
    CHECK_EQ(h.RedoDepth(), 1u);
    CHECK(h.Redo());
    CHECK_EQ(v, 5);
    CHECK(!h.CanRedo());
    CHECK(h.CanUndo());
    HistoryManager empty;
    CHECK(!empty.Undo());
    CHECK(!empty.Redo());
}

TEST(HistoryClear) {
    int v = 0;
    HistoryManager h;
    h.Push(std::make_unique<AddCommand>(&v, 1));
    h.Push(std::make_unique<AddCommand>(&v, 2));
    h.Undo();
    h.Clear();
    CHECK(!h.CanUndo());
    CHECK(!h.CanRedo());
    CHECK_EQ(h.UndoDepth(), 0u);
    CHECK_EQ(h.RedoDepth(), 0u);
    CHECK_EQ(v, 1); // Clear drops the stacks without mutating state
}

TEST(HistoryUndoDepthCap) {
    int v = 0;
    HistoryManager h(2);
    h.Push(std::make_unique<AddCommand>(&v, 1));
    h.Push(std::make_unique<AddCommand>(&v, 2));
    h.Push(std::make_unique<AddCommand>(&v, 4));
    CHECK_EQ(v, 7);
    CHECK_EQ(h.UndoDepth(), 2u); // oldest entry evicted
    CHECK(h.Undo());
    CHECK_EQ(v, 3); // -4
    CHECK(h.Undo());
    CHECK_EQ(v, 1); // -2
    CHECK(!h.CanUndo());
}

TEST(HistoryMergeCoalescesToSingleStep) {
    int v = 0;
    HistoryManager h;
    h.Push(std::make_unique<ValueCommand>(&v, 0, 1));
    h.Push(std::make_unique<ValueCommand>(&v, 1, 2));
    h.Push(std::make_unique<ValueCommand>(&v, 2, 3));
    CHECK_EQ(v, 3);
    CHECK_EQ(h.UndoDepth(), 1u); // all merged into one undo step
    CHECK(h.Undo());
    CHECK_EQ(v, 0); // reverts to the ORIGINAL value
    CHECK(h.Redo());
    CHECK_EQ(v, 3); // re-applies the LAST value
}

TEST(HistoryMergeRespectsChainAndType) {
    int v = 0;
    HistoryManager h;
    // A gap in the value chain must not merge (separate edit on same target).
    h.Push(std::make_unique<ValueCommand>(&v, 0, 1));
    h.Push(std::make_unique<ValueCommand>(&v, 3, 4)); // old != 1: not consecutive
    CHECK_EQ(h.UndoDepth(), 2u);
    // A non-mergeable command must not merge.
    HistoryManager h2;
    h2.Push(std::make_unique<ValueCommand>(&v, 0, 1, false));
    h2.Push(std::make_unique<ValueCommand>(&v, 1, 2, true));
    CHECK_EQ(h2.UndoDepth(), 2u);
}

TEST(HistoryFreshPushClearsRedo) {
    int v = 0;
    HistoryManager h;
    h.Push(std::make_unique<AddCommand>(&v, 1)); // v = 1
    h.Push(std::make_unique<AddCommand>(&v, 2)); // v = 3
    h.Undo(); // v = 1, redo = [+2]
    CHECK(h.CanRedo());
    h.Push(std::make_unique<AddCommand>(&v, 10)); // v = 11, redo cleared
    CHECK(!h.CanRedo());
    CHECK_EQ(h.RedoDepth(), 0u);
    CHECK(h.Undo()); // v = 1
    CHECK_EQ(v, 1);
    CHECK(h.Undo()); // v = 0
    CHECK_EQ(v, 0);
    CHECK(!h.CanUndo());
}

TEST(HistoryNoopIsSkipped) {
    int v = 5;
    HistoryManager h;
    h.Push(std::make_unique<ValueCommand>(&v, 5, 5)); // unchanged
    CHECK_EQ(h.UndoDepth(), 0u);
    CHECK_EQ(v, 5);
    CHECK(!h.CanUndo());
}
