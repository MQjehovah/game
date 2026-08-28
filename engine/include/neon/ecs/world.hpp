#pragma once
#include <algorithm>
#include <cassert>
#include <cstdint>
#include <functional>
#include <memory>
#include <typeindex>
#include <type_traits>
#include <unordered_map>
#include <vector>

#include "neon/core/log.hpp"
#include "neon/ecs/parallel.hpp"

namespace neon::ecs {

struct Entity {
    uint32_t id = 0;
    uint32_t generation = 0;

    bool IsValid() const { return id != 0; }
    bool operator==(const Entity& o) const { return id == o.id && generation == o.generation; }
    bool operator!=(const Entity& o) const { return !(*this == o); }
};

class World {
public:
    // --- Internal storage (exposed only so View can reference it) ---
    struct IPool {
        virtual ~IPool() = default;
        virtual void Remove(uint32_t id) = 0;
        virtual bool Has(uint32_t id) const = 0;
        virtual void Grow(size_t n) = 0;
        virtual size_t Size() const = 0;
        virtual void Clear() = 0;
    };

    template <class T>
    struct Pool : IPool {
        void Add(uint32_t id, const T& value) {
            // C11: a duplicate Add (already-present id) used to clobber the
            // sparse slot, leaving an orphaned dense entry that iteration
            // visited twice. Guard it here instead of trusting every caller.
            if (id < sparse_.size() && sparse_[id] > 0) {
                dense_[sparse_[id] - 1] = value;
                return;
            }
            if (id >= sparse_.size()) sparse_.resize(id + 1, 0);
            sparse_[id] = static_cast<int32_t>(dense_.size()) + 1;
            denseIds_.push_back(id);
            dense_.push_back(value);
        }

        T& Get(uint32_t id) {
            // C11: bounds-checked -- a stale id used to index dense_[-1] (UB)
            // when the sparse slot was 0. Returns a static dummy to keep the
            // call sites valid (callers guard with TryGet/Alive in practice).
            static T kEmpty{};
            if (id >= sparse_.size() || sparse_[id] <= 0) return kEmpty;
            return dense_[sparse_[id] - 1];
        }
        T* TryGet(uint32_t id) {
            if (id >= sparse_.size() || sparse_[id] <= 0) return nullptr;
            return &dense_[sparse_[id] - 1];
        }
        const T* TryGet(uint32_t id) const {
            if (id >= sparse_.size() || sparse_[id] <= 0) return nullptr;
            return &dense_[sparse_[id] - 1];
        }

        void Remove(uint32_t id) override {
            if (id >= sparse_.size() || sparse_[id] <= 0) return;
            int32_t s = sparse_[id] - 1;
            size_t last = dense_.size() - 1;
            if (static_cast<size_t>(s) != last) {
                dense_[s] = dense_[last];
                denseIds_[s] = denseIds_[last];
                sparse_[denseIds_[s]] = s + 1;
            }
            dense_.pop_back();
            denseIds_.pop_back();
            sparse_[id] = 0;
        }

        bool Has(uint32_t id) const override { return id < sparse_.size() && sparse_[id] > 0; }
        void Grow(size_t n) override {
            if (sparse_.size() < n) sparse_.resize(n, 0);
        }
        size_t Size() const override { return dense_.size(); }
        void Clear() override {
            dense_.clear();
            denseIds_.clear();
            std::fill(sparse_.begin(), sparse_.end(), 0);
        }
        std::vector<T>& dense() { return dense_; }

        std::vector<T> dense_;
        std::vector<uint32_t> denseIds_;
        std::vector<int32_t> sparse_;
    };

    // --- Batch iteration views ---
    // View<T> iterates every entity that has T; View<T,U> iterates entities
    // that have BOTH T and U (the U pool is pre-created and held directly, so
    // the per-entity U lookup is a pool-local sparse-index probe instead of a
    // world type-table + virtual call; dense order of T drives the visit
    // order). Both offer ForEach (serial) and ParallelForEach (deterministic
    // split across worker threads). Archetype storage (a future refactor)
    // would give cache-friendly cross-component iteration; these views
    // deliver the same batch-iteration API on the existing SparseSet pools.
    template <class T, class... Us>
    class View;

    template <class T>
    class View<T> {
    public:
        explicit View(Pool<T>& pool, World& world) : pool_(pool), world_(world) {}
        size_t Size() const { return pool_.Size(); }
        T& operator[](size_t i) { return pool_.dense()[i]; }

        // Serial batch iteration: calls fn(entity, component) once per entity
        // holding T. fn may write its own component. Mutating the world while
        // iterating (Create/Destroy/Add/Remove) invalidates the pool. The
        // callback is templated (no std::function allocation in hot paths).
        template <class Fn>
        void ForEach(Fn&& fn) {
            const size_t n = pool_.Size();
            for (size_t i = 0; i < n; ++i) {
                ecs::Entity e = world_.EntityFromPool(pool_, i);
                fn(e, pool_.dense_[i]);
            }
        }

        // Parallel batch iteration: splits the pool's dense range across
        // worker threads. The functor MUST touch ONLY the (entity, component)
        // pair it is given - no shared mutable state, no world mutation. For
        // such independent-item workloads the result is bit-identical to
        // ForEach and identical across runs.
        template <class Fn>
        void ParallelForEach(Fn&& fn) {
            const size_t n = pool_.Size();
            if (n == 0) return;
            ParallelIterationGuard guard(world_);
            World& w = world_;
            Pool<T>& p = pool_;
            parallel::ParallelFor(static_cast<uint32_t>(n),
                                  [&w, &p, fn = std::forward<Fn>(fn)](
                                      uint32_t s, uint32_t e) {
                                      for (uint32_t i = s; i < e; ++i) {
                                          ecs::Entity ent = w.EntityFromPool(p, i);
                                          fn(ent, p.dense_[i]);
                                      }
                                  });
        }

    private:
        Pool<T>& pool_;
        World& world_;
    };

    template <class T, class U>
    class View<T, U> {
    public:
        // Pre-creates the U pool at construction (not lazily during iteration),
        // so workers in ParallelForEach only ever read an existing pool and
        // never race to insert into the world's pool table. The U pool is
        // held directly: per-entity lookups skip the world's type table.
        explicit View(Pool<T>& pool, World& world)
            : pool_(pool), poolU_(world.template GetPool<U>()), world_(world) {}
        size_t Size() const { return pool_.Size(); }
        T& operator[](size_t i) { return pool_.dense()[i]; }

        // Serial batch iteration over entities that have BOTH T and U. The
        // functor receives both components. The callback is templated (no
        // std::function allocation per call).
        template <class Fn>
        void ForEach(Fn&& fn) {
            const size_t n = pool_.Size();
            for (size_t i = 0; i < n; ++i) {
                ecs::Entity e = world_.EntityFromPool(pool_, i);
                U* u = poolU_.TryGet(e.id);
                if (!u) continue;
                fn(e, pool_.dense_[i], *u);
            }
        }

        // Parallel version of the two-component view; same thread-safety
        // contract as View<T>::ParallelForEach. The U lookup is read-only.
        template <class Fn>
        void ParallelForEach(Fn&& fn) {
            const size_t n = pool_.Size();
            if (n == 0) return;
            ParallelIterationGuard guard(world_);
            World& w = world_;
            Pool<T>& p = pool_;
            Pool<U>& pu = poolU_;
            parallel::ParallelFor(static_cast<uint32_t>(n),
                                  [&w, &p, &pu, fn = std::forward<Fn>(fn)](
                                      uint32_t s, uint32_t e) {
                                      for (uint32_t i = s; i < e; ++i) {
                                          ecs::Entity ent = w.EntityFromPool(p, i);
                                          U* u = pu.TryGet(ent.id);
                                          if (!u) continue;
                                          fn(ent, p.dense_[i], *u);
                                      }
                                  });
        }

    private:
        Pool<T>& pool_;
        Pool<U>& poolU_;
        World& world_;
    };

    // --- Entity lifecycle ---
    World();
    ~World();

    Entity Create();
    void Destroy(Entity e);
    bool Alive(Entity e) const;
    void Clear();
    size_t EntityCount() const { return aliveCount_; }

    // --- Components ---
    template <class T>
    T& Add(Entity e, const T& value = T{}) {
        if (inParallelIteration_) {
            // Refused: no-op (no pool access - GetPool may insert). The returned
            // reference is a throwaway never stored; treat it as void.
            // C11: const so a caller cannot write the shared dummy and race.
            RejectParallelMutation("Add");
            static const T kRejected{};
            return const_cast<T&>(kRejected);
        }
        Pool<T>& pool = GetPool<T>();
        // Idempotent: a second Add for the same entity leaves the existing
        // component in place (overwriting sparse would orphan the old dense
        // entry and corrupt pool iteration).
        if (!pool.Has(e.id)) pool.Add(e.id, value);
        return pool.Get(e.id);
    }

    template <class T>
    T* Get(Entity e) {
        if (!Alive(e)) return nullptr;
        return GetPool<T>().TryGet(e.id);
    }

    template <class T>
    const T* Get(Entity e) const {
        if (!Alive(e)) return nullptr;
        return GetPool<T>().TryGet(e.id);
    }

    template <class T>
    bool Has(Entity e) const {
        return Alive(e) && GetPool<T>().Has(e.id);
    }

    template <class T>
    void Remove(Entity e) {
        if (inParallelIteration_) {
            RejectParallelMutation("Remove"); // no-op, no pool access
            return;
        }
        if (!Alive(e)) return;
        GetPool<T>().Remove(e.id);
    }

    template <class T>
    View<T> ViewAll() {
        return View<T>(GetPool<T>(), *this);
    }

    template <class T, class U>
    View<T, U> ViewAll() {
        // The View<T,U> constructor pre-creates the U pool.
        return View<T, U>(GetPool<T>(), *this);
    }

    // Direct access to a component pool (advanced / batch-iteration use).
    // Ensures the pool exists, like Get<T>/ViewAll<T>. Enables callers to
    // construct views directly, e.g. World::View<T,U>(world.PoolOf<T>(), world).
    template <class T>
    Pool<T>& PoolOf() {
        return GetPool<T>();
    }

    // Entity handle for the index-th element of a component pool view.
    template <class T>
    Entity EntityAt(size_t index) {
        Pool<T>& pool = GetPool<T>();
        if (index >= pool.Size()) return {};
        uint32_t id = pool.denseIds_[index];
        return {id, generations_[id]};
    }

    // Entity handle for the index-th element of the given pool. O(1), no type
    // lookup - used by views to build handles during batch iteration.
    template <class T>
    Entity EntityFromPool(Pool<T>& pool, size_t index) {
        if (index >= pool.Size()) return {};
        uint32_t id = pool.denseIds_[index];
        return {id, generations_[id]};
    }

    // True only while a ParallelForEach is running on this world. Used by the
    // debug mutation guard (assert in Create/Destroy/Add/Remove) and by tests
    // to observe the guard. Read-only from worker threads; written only by the
    // owning (main) thread between the submit/join of a parallel pass.
    bool InParallelIteration() const { return inParallelIteration_; }

private:
    template <class T>
    Pool<T>& GetPool() {
        std::type_index ti = std::type_index(typeid(T));
        auto it = poolIndex_.find(ti);
        if (it != poolIndex_.end()) {
            return *static_cast<Pool<T>*>(pools_[it->second].get());
        }
        auto pool = std::make_unique<Pool<T>>();
        pool->Grow(generations_.size());
        poolIndex_[ti] = pools_.size();
        pools_.push_back(std::move(pool));
        return *static_cast<Pool<T>*>(pools_.back().get());
    }

    template <class T>
    const Pool<T>& GetPool() const {
        return const_cast<World*>(this)->GetPool<T>();
    }

    // RAII guard raised around a ParallelForEach pass. Cleared on the owning
    // thread after the pass joins, so worker threads only ever see it as true.
    // Also verifies the pass created no component pools (a read during the pass
    // must never insert into the pool table): logs an error when it did.
    class ParallelIterationGuard {
    public:
        explicit ParallelIterationGuard(World& w)
            : world_(w), poolsBefore_(w.poolIndex_.size()) {
            world_.inParallelIteration_ = true;
        }
        ~ParallelIterationGuard() {
            world_.inParallelIteration_ = false;
            if (world_.poolIndex_.size() != poolsBefore_) {
                assert(false && "parallel iteration created a component pool (world mutation)");
                NEON_LOG_CAT(neon::core::LogCategory::Ecs, neon::core::LogLevel::Error,
                             "parallel iteration grew the component pool table (%zu -> %zu): "
                             "views must pre-create every pool they read",
                             poolsBefore_, world_.poolIndex_.size());
            }
        }

    private:
        World& world_;
        size_t poolsBefore_;
    };

    // Logs an error and returns - called when a world mutation is attempted
    // inside a parallel iteration. The caller then refuses the mutation.
    void RejectParallelMutation(const char* op);

    bool inParallelIteration_ = false;
    std::vector<uint32_t> generations_{1}; // entity ids start at 1
    std::vector<uint32_t> freeIds_;
    size_t aliveCount_ = 0;
    std::vector<std::unique_ptr<IPool>> pools_;
    std::unordered_map<std::type_index, size_t> poolIndex_;
};

// Systems operate on the world; the game registers them in order.
struct System {
    virtual ~System() = default;
    virtual void Update(float dt, World& world) = 0;
};

} // namespace neon::ecs
