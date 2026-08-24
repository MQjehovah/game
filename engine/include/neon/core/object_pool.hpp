#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <utility>

namespace neon::core {

// Fixed-capacity object pool (P0-3 memory-arena direction). Slots are
// constructed lazily on first Acquire and reused across releases, so hot paths
// (particles, network messages, entity handles) stop allocating from the heap
// per frame. Not thread-safe; call from one thread.
template <typename T, std::size_t N>
class ObjectPool {
public:
    using Index = std::uint32_t;
    static constexpr std::size_t kCapacity = N;

    ObjectPool() {
        for (Index i = 0; i < kCapacity; ++i) free_[i] = i;
        usedPos_.fill(kNotUsed);
        freeCount_ = kCapacity;
    }

    // Allocates a slot. On success `out` points at the (default-constructed)
    // slot and returns true; false when the pool is exhausted.
    bool Acquire(T*& out) {
        Index index = 0;
        return Acquire(index, out);
    }
    // Allocates a slot and reports its index (for Release later).
    bool Acquire(Index& index, T*& out) {
        if (freeCount_ == 0) return false;
        const Index slot = free_[--freeCount_];
        const std::size_t pos = count_;
        used_[pos] = slot;
        usedPos_[slot] = pos;
        out = &slots_[slot];
        index = slot;
        ++count_;
        return true;
    }

    // Returns a previously acquired slot to the free list (O(1)). The slot's
    // contents are left intact (reused on the next Acquire); callers overwrite.
    void Release(Index index) {
        if (index >= kCapacity || usedPos_[index] == kNotUsed) return;
        const std::size_t pos = usedPos_[index];
        const Index last = used_[count_ - 1];
        used_[pos] = last;
        usedPos_[last] = pos;
        usedPos_[index] = kNotUsed;
        --count_;
        free_[freeCount_++] = index;
    }

    T& At(Index index) { return slots_[index]; }
    const T& At(Index index) const { return slots_[index]; }
    Index UsedIndex(std::size_t i) const { return used_[i]; }
    std::size_t Count() const { return count_; }
    std::size_t Capacity() const { return kCapacity; }
    void Clear() {
        for (std::size_t i = 0; i < count_; ++i) usedPos_[used_[i]] = kNotUsed;
        count_ = 0;
        freeCount_ = kCapacity;
        for (Index i = 0; i < kCapacity; ++i) free_[i] = i;
        usedPos_.fill(kNotUsed);
    }

private:
    std::array<T, kCapacity> slots_{};
    std::array<Index, kCapacity> free_{};
    std::array<Index, kCapacity> used_{};
    static constexpr std::size_t kNotUsed = static_cast<std::size_t>(-1);
    std::array<std::size_t, kCapacity> usedPos_{};
    std::size_t count_ = 0;
    std::size_t freeCount_ = 0;
};

} // namespace neon::core
