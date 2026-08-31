#include "neon/neon.hpp"
#include "neon/kernel/module.hpp"
#include "neon/kernel/registry.hpp"
#include "helpers.hpp"

#include <memory>
#include <string>
#include <vector>

using namespace neon;
using namespace neon::kernel;

namespace {

// --- Dummy service interfaces (what a module publishes/consumes) -----------
struct ILog {
    virtual ~ILog() = default;
    virtual void Log(const char*) = 0;
};
struct IRender {
    virtual ~IRender() = default;
    virtual bool HasLog() const = 0;
};

// --- Concrete implementations ----------------------------------------------
struct NullLog : ILog {
    int calls = 0;
    void Log(const char*) override { ++calls; }
};
struct RenderWithLog : IRender {
    ILog* log = nullptr;
    bool HasLog() const override { return log != nullptr; }
};

// --- Modules -----------------------------------------------------------------
class LogModule : public IModule {
public:
    explicit LogModule(ILog* log) : log_(log) {}
    ModuleInfo Info() const override {
        ModuleInfo m;
        m.id = "log";
        m.version = "1.0.0";
        return m;
    }
    bool Init(ServiceRegistry& reg) override {
        reg.Register<ILog>(log_);
        return true;
    }
    void Shutdown() override {}

private:
    ILog* log_;
};

class RenderModule : public IModule {
public:
    ModuleInfo Info() const override {
        ModuleInfo m;
        m.id = "render";
        m.version = "1.0.0";
        m.requires = {"log"};
        return m;
    }
    bool Init(ServiceRegistry& reg) override {
        ILog* log = reg.Get<ILog>();
        if (!log) return false;  // missing dependency
        render_.log = log;
        reg.Register<IRender>(&render_);
        return true;
    }
    void Shutdown() override {}

    RenderWithLog render_;
};

// Records init order for the topological-order test.
std::vector<std::string> g_initOrder;

class OrderModule : public IModule {
public:
    OrderModule(const char* id, std::vector<const char*> reqs = {})
        : id_(id), reqs_(std::move(reqs)) {}
    ModuleInfo Info() const override {
        ModuleInfo m;
        m.id = id_;
        m.version = "1.0.0";
        m.requires = reqs_;
        return m;
    }
    bool Init(ServiceRegistry&) override {
        g_initOrder.push_back(id_);
        return true;
    }
    void Shutdown() override {}

private:
    const char* id_;
    std::vector<const char*> reqs_;
};

} // namespace

TEST(ServiceRegistryRegisterAndGet) {
    ServiceRegistry reg;
    NullLog log;
    reg.Register<ILog>(&log);
    CHECK(reg.Get<ILog>() == &log);
    CHECK(reg.Get<IRender>() == nullptr);  // absent -> nullptr

    // Swap: re-registering the same interface replaces the implementation.
    NullLog log2;
    reg.Register<ILog>(&log2);
    CHECK(reg.Get<ILog>() == &log2);
}

TEST(ModuleRegistryInitDependencyOrder) {
    g_initOrder.clear();
    ModuleRegistry mgr;
    mgr.Add(std::make_unique<OrderModule>("c", std::vector<const char*>{"a", "b"}));
    mgr.Add(std::make_unique<OrderModule>("b", std::vector<const char*>{"a"}));
    mgr.Add(std::make_unique<OrderModule>("a"));
    ServiceRegistry reg;
    CHECK(mgr.InitAll(reg));
    CHECK_EQ(g_initOrder.size(), 3u);
    CHECK_EQ(g_initOrder[0], std::string("a"));
    CHECK_EQ(g_initOrder[1], std::string("b"));
    CHECK_EQ(g_initOrder[2], std::string("c"));
    mgr.ShutdownAll();
}

TEST(ModuleRegistryDependencyWiring) {
    ModuleRegistry mgr;
    NullLog log;
    mgr.Add(std::make_unique<LogModule>(&log));
    mgr.Add(std::make_unique<RenderModule>());
    ServiceRegistry reg;
    CHECK(mgr.InitAll(reg));
    IRender* render = reg.Get<IRender>();
    CHECK(render != nullptr);
    CHECK(render->HasLog());  // wired to the log service
    mgr.ShutdownAll();
}

TEST(ModuleRegistryMissingDependencyFails) {
    ModuleRegistry mgr;
    mgr.Add(std::make_unique<RenderModule>());  // needs "log", which is absent
    ServiceRegistry reg;
    CHECK(!mgr.InitAll(reg));
}

TEST(ModuleRegistryCycleFails) {
    ModuleRegistry mgr;
    mgr.Add(std::make_unique<OrderModule>("a", std::vector<const char*>{"b"}));
    mgr.Add(std::make_unique<OrderModule>("b", std::vector<const char*>{"a"}));
    ServiceRegistry reg;
    CHECK(!mgr.InitAll(reg));
}
