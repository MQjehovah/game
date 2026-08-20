#pragma once
#include <algorithm>
#include <cstdint>
#include <memory>
#include <typeindex>
#include <type_traits>
#include <unordered_map>
#include <vector>

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

    template <class T>
    class View {
    public:
        explicit View(Pool<T>& pool) : pool_(pool) {}
        size_t Size() const { return pool_.Size(); }
        T& operator[](size_t i) { return pool_.dense()[i]; }

    private:
        Pool<T>& pool_;
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
        if (!Alive(e)) return;
        GetPool<T>().Remove(e.id);
    }

    template <class T>
    View<T> ViewAll() {
        return View<T>(GetPool<T>());
    }

    // Entity handle for the index-th element of a component pool view.
    template <class T>
    Entity EntityAt(size_t index) {
        Pool<T>& pool = GetPool<T>();
        if (index >= pool.Size()) return {};
        uint32_t id = pool.denseIds_[index];
        return {id, generations_[id]};
    }

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
