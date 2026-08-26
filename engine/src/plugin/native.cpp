#include "neon/plugin/native.hpp"

#include <cstring>

#include "neon/core/log.hpp"

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <dlfcn.h>
#endif

namespace neon::plugin {
namespace {

#if defined(_WIN32)
struct HandleTraits {
    using Type = HMODULE;
    static Type Invalid() { return nullptr; }
    static Type Open(const std::string& path) { return LoadLibraryA(path.c_str()); }
    static void* Symbol(Type h, const char* name) { return (void*)GetProcAddress(h, name); }
    static void Close(Type h) { FreeLibrary(h); }
};
#else
struct HandleTraits {
    using Type = void*;
    static Type Invalid() { return nullptr; }
    static Type Open(const std::string& path) { return dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL); }
    static void* Symbol(Type h, const char* name) { return dlsym(h, name); }
    static void Close(Type h) { dlclose(h); }
};
#endif

using GetInfoFn = bool (*)(NativePluginInfo*);

bool IsInfoUsable(const NativePluginInfo& i) {
    return i.apiVersion == kNativeApiVersion && i.size == sizeof(NativePluginInfo) &&
           i.name != nullptr && i.create != nullptr && i.destroy != nullptr;
}

} // namespace

struct NativePlugin::Impl {
    typename HandleTraits::Type handle = HandleTraits::Invalid();
    ~Impl() {
        if (handle != HandleTraits::Invalid()) HandleTraits::Close(handle);
    }
};

std::unique_ptr<NativePlugin> NativePlugin::Load(const std::string& libPath, std::string* err) {
    auto fail = [&](const std::string& m) {
        if (err) *err = m;
        return std::unique_ptr<NativePlugin>{};
    };
    if (libPath.empty()) return fail("native plugin: empty library path");

    auto p = std::unique_ptr<NativePlugin>(new NativePlugin());
    p->impl_ = std::make_unique<Impl>();
    p->path_ = libPath;

    p->impl_->handle = HandleTraits::Open(libPath);
    if (p->impl_->handle == HandleTraits::Invalid())
        return fail("native plugin: cannot load '" + libPath + "'");

    GetInfoFn getInfo =
        reinterpret_cast<GetInfoFn>(HandleTraits::Symbol(p->impl_->handle, "NeonPlugin_GetInfo"));
    if (!getInfo)
        return fail("native plugin: '" + libPath + "' does not export NeonPlugin_GetInfo");

    NativePluginInfo info{};
    bool ok = false;
    try {
        ok = getInfo(&info);
    } catch (...) {
        return fail("native plugin: NeonPlugin_GetInfo raised from '" + libPath + "'");
    }
    if (!ok || !IsInfoUsable(info))
        return fail("native plugin: '" + libPath + "' ABI mismatch (api=" +
                    std::to_string(info.apiVersion) + ", size=" + std::to_string(info.size) +
                    ", expected api=" + std::to_string(kNativeApiVersion) +
                    ", size=" + std::to_string(sizeof(NativePluginInfo)) + ")");

    p->info_ = info;
    p->instance_ = info.create(kNativeApiVersion);
    if (!p->instance_)
        return fail("native plugin: '" + libPath + "' create() returned null");
    return p;
}

NativePlugin::~NativePlugin() {
    if (instance_ && info_.destroy) {
        try {
            info_.destroy(instance_);
        } catch (...) {
            // A misbehaving plugin must never crash the host during teardown.
        }
        instance_ = nullptr;
    }
    impl_.reset();
}

NativePlugin::NativePlugin(NativePlugin&& o) noexcept
    : impl_(std::move(o.impl_)), info_(o.info_), instance_(o.instance_), path_(std::move(o.path_)) {
    o.instance_ = nullptr;
    o.info_ = NativePluginInfo{};
}

NativePlugin& NativePlugin::operator=(NativePlugin&& o) noexcept {
    if (this != &o) {
        impl_ = std::move(o.impl_);
        info_ = o.info_;
        instance_ = o.instance_;
        path_ = std::move(o.path_);
        o.instance_ = nullptr;
        o.info_ = NativePluginInfo{};
    }
    return *this;
}

bool NativePlugin::Reload(const std::string& libPath, std::string* err) {
    // Destroy the current instance + free the library first, then load fresh.
    auto next = Load(libPath, err);
    if (!next) return false;
    *this = std::move(*next);
    return true;
}

void* NativePlugin::Symbol(const char* name) const {
    if (!impl_ || impl_->handle == HandleTraits::Invalid() || !name) return nullptr;
    return HandleTraits::Symbol(impl_->handle, name);
}

std::string NativeLibraryPath(const PluginManifest& m) {
    if (m.entry.empty()) return {};
#if !defined(_WIN32)
    // POSIX shared libraries conventionally end in .so; append it when the
    // manifest entry has no extension (authors may still name it explicitly).
    if (m.entry.find('.') == std::string::npos) return m.entry + ".so";
#endif
    return m.entry;
}

std::vector<std::unique_ptr<NativePlugin>> LoadNativePlugins(const std::string& baseDir) {
    std::vector<std::unique_ptr<NativePlugin>> out;
    for (const PluginManifest& m : DiscoverPlugins(baseDir)) {
        if (m.type != PluginType::Native) continue;
        const std::string libPath = m.dir + "/" + NativeLibraryPath(m);
        std::string err;
        std::unique_ptr<NativePlugin> p = NativePlugin::Load(libPath, &err);
        if (!p) {
            NEON_LOG_CAT(core::LogCategory::Core, core::LogLevel::Warn, "plugin: native '%s' (%s) %s",
                         m.id.c_str(), m.version.c_str(), err.c_str());
            continue;
        }
        NEON_LOG_CAT(core::LogCategory::Core, core::LogLevel::Info,
                     "plugin: native '%s' (%s) loaded from '%s'", m.id.c_str(),
                     p->Info().version ? p->Info().version : "?",
                     NativeLibraryPath(m).c_str());
        out.push_back(std::move(p));
    }
    return out;
}

} // namespace neon::plugin
