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
            if (id >= sparse_.size()) sparse_.resize(id + 1, 0);
            sparse_[id] = static_cast<int32_t>(dense_.size()) + 1;
            denseIds_.push_back(id);
            dense_.push_back(value);
        }

        T& Get(uint32_t id) { return dense_[sparse_[id] - 1]; }
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
    // that have BOTH T and U (U is looked up per entity; dense order of T
    // drives the visit order). Both offer ForEach (serial) and
    // ParallelForEach (deterministic split across worker threads). Archetype
    // storage (a future refactor) would give cache-friendly cross-component
    // iteration; these views deliver the same batch-iteration API on the
    // existing SparseSet pools.
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
        // iterating (Create/Destroy/Add/Remove) invalidates the pool.
        void ForEach(std::function<void(ecs::Entity, T&)> fn) {
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
        void ParallelForEach(std::function<void(ecs::Entity, T&)> fn) {
            const size_t n = pool_.Size();
            if (n == 0) return;
            ParallelIterationGuard guard(world_);
            World& w = world_;
            Pool<T>& p = pool_;
            parallel::ParallelFor(static_cast<uint32_t>(n),
                                  [&w, &p, fn](uint32_t s, uint32_t e) {
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
        explicit View(Pool<T>& pool, World& world) : pool_(pool), world_(world) {}
        size_t Size() const { return pool_.Size(); }
        T& operator[](size_t i) { return pool_.dense()[i]; }

        // Serial batch iteration over entities that have BOTH T and U. The
        // functor receives both components.
        void ForEach(std::function<void(ecs::Entity, T&, U&)> fn) {
            const size_t n = pool_.Size();
            for (size_t i = 0; i < n; ++i) {
                ecs::Entity e = world_.EntityFromPool(pool_, i);
                U* u = world_.template Get<U>(e);
                if (!u) continue;
                fn(e, pool_.dense_[i], *u);
            }
        }

        // Parallel version of the two-component view; same thread-safety
        // contract as View<T>::ParallelForEach. The U lookup is read-only.
        void ParallelForEach(std::function<void(ecs::Entity, T&, U&)> fn) {
            const size_t n = pool_.Size();
            if (n == 0) return;
            ParallelIterationGuard guard(world_);
            World& w = world_;
            Pool<T>& p = pool_;
            parallel::ParallelFor(static_cast<uint32_t>(n),
                                  [&w, &p, fn](uint32_t s, uint32_t e) {
                                      for (uint32_t i = s; i < e; ++i) {
                                          ecs::Entity ent = w.EntityFromPool(p, i);
                                          U* u = w.template Get<U>(ent);
                                          if (!u) continue;
                                          fn(ent, p.dense_[i], *u);
                                      }
                                  });
        }

    private:
        Pool<T>& pool_;
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
        assert(!inParallelIteration_ && "World::Add<T>() forbidden inside a parallel iteration");
        Pool<T>& pool = GetPool<T>();
        pool.Add(e.id, value);
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
        assert(!inParallelIteration_ && "World::Remove<T>() forbidden inside a parallel iteration");
        if (!Alive(e)) return;
        GetPool<T>().Remove(e.id);
    }

    template <class T>
    View<T> ViewAll() {
        return View<T>(GetPool<T>(), *this);
    }

    template <class T, class U>
    View<T, U> ViewAll() {
        GetPool<U>(); // ensure the U pool exists; read (never written) during iteration
        return View<T, U>(GetPool<T>(), *this);
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
    class ParallelIterationGuard {
    public:
        explicit ParallelIterationGuard(World& w) : world_(w) { world_.inParallelIteration_ = true; }
        ~ParallelIterationGuard() { world_.inParallelIteration_ = false; }

    private:
        World& world_;
    };

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
