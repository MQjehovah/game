#pragma once

#include <memory>

#include "neon/audio/audio.hpp"
#include "neon/gfx/renderer.hpp"
#include "neon/kernel/module.hpp"
#include "neon/physics/physics.hpp"
#include "neon/script/script.hpp"

namespace neon::modules {

// Thin IModule adapters over the engine's already-interface'd subsystems, so
// the microkernel can load, wire and swap them uniformly. Each module OWNS its
// subsystem instance and registers the underlying interface on the
// ServiceRegistry in Init(), so consumers fetch it by type and never link the
// concrete backend. Replacing a subsystem = constructing the module with a
// different implementation.

class GfxModule : public kernel::IModule {
public:
    explicit GfxModule(std::unique_ptr<gfx::Renderer> renderer)
        : renderer_(std::move(renderer)) {}
    kernel::ModuleInfo Info() const override {
        kernel::ModuleInfo m;
        m.id = "gfx";
        m.version = "1.0.0";
        return m;
    }
    bool Init(kernel::ServiceRegistry& reg) override {
        if (!renderer_) return false;
        reg.Register<gfx::Renderer>(renderer_.get());
        return true;
    }
    void Shutdown() override {}
    gfx::Renderer* Renderer() { return renderer_.get(); }

private:
    std::unique_ptr<gfx::Renderer> renderer_;
};

class PhysicsModule : public kernel::IModule {
public:
    explicit PhysicsModule(std::unique_ptr<physics::World> world)
        : world_(std::move(world)) {}
    kernel::ModuleInfo Info() const override {
        kernel::ModuleInfo m;
        m.id = "physics";
        m.version = "1.0.0";
        return m;
    }
    bool Init(kernel::ServiceRegistry& reg) override {
        if (!world_) return false;
        reg.Register<physics::World>(world_.get());
        return true;
    }
    void Shutdown() override {}
    physics::World* World() { return world_.get(); }

private:
    std::unique_ptr<physics::World> world_;
};

class AudioModule : public kernel::IModule {
public:
    explicit AudioModule(std::unique_ptr<audio::IAudioBackend> backend)
        : backend_(std::move(backend)) {}
    kernel::ModuleInfo Info() const override {
        kernel::ModuleInfo m;
        m.id = "audio";
        m.version = "1.0.0";
        return m;
    }
    bool Init(kernel::ServiceRegistry& reg) override {
        if (!backend_) return false;
        if (!backend_->Init()) return false;
        reg.Register<audio::IAudioBackend>(backend_.get());
        return true;
    }
    void Shutdown() override {
        if (backend_) backend_->Shutdown();
    }
    audio::IAudioBackend* Backend() { return backend_.get(); }

private:
    std::unique_ptr<audio::IAudioBackend> backend_;
};

class ScriptModule : public kernel::IModule {
public:
    explicit ScriptModule(std::unique_ptr<script::IScriptHost> host)
        : host_(std::move(host)) {}
    kernel::ModuleInfo Info() const override {
        kernel::ModuleInfo m;
        m.id = "script";
        m.version = "1.0.0";
        return m;
    }
    bool Init(kernel::ServiceRegistry& reg) override {
        if (!host_) return false;
        if (!host_->Init()) return false;
        reg.Register<script::IScriptHost>(host_.get());
        return true;
    }
    void Shutdown() override {
        if (host_) host_->Shutdown();
    }
    script::IScriptHost* Host() { return host_.get(); }

private:
    std::unique_ptr<script::IScriptHost> host_;
};

} // namespace neon::modules
