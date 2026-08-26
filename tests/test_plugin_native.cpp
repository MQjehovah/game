#include <fstream>
#include <string>
#include <vector>

#include "neon/neon.hpp"
#include "neon/plugin/backend.hpp"
#include "neon/plugin/native.hpp"
#include "helpers.hpp"
#include "physics_plugin/api.h"

// Set by CMake to the built plugin DLL/SO path ($<TARGET_FILE:...>).
#ifndef NEON_PLUGIN_PHYSICS_PATH
#define NEON_PLUGIN_PHYSICS_PATH ""
#endif

using namespace neon;

namespace {

#if defined(_WIN32)
#include <direct.h>
bool Mkdir(const std::string& p) { return ::_mkdir(p.c_str()) == 0; }
#else
#include <sys/stat.h>
bool Mkdir(const std::string& p) { return ::mkdir(p.c_str(), 0777) == 0; }
#endif

void WriteText(const std::string& path, const std::string& text) {
    std::ofstream out(path, std::ios::binary);
    out << text;
}

bool CopyFile(const std::string& src, const std::string& dst) {
    std::ifstream in(src, std::ios::binary);
    std::ofstream out(dst, std::ios::binary);
    if (!in.is_open() || !out.is_open()) return false;
    out << in.rdbuf();
    return true;
}

std::string DirOf(const std::string& p) {
    const size_t pos = p.find_last_of("/\\");
    return pos == std::string::npos ? std::string(".") : p.substr(0, pos + 1);
}

} // namespace

TEST(NativePluginLoadsAndDrivesPhysics) {
    std::string err;
    auto p = plugin::NativePlugin::Load(NEON_PLUGIN_PHYSICS_PATH, &err);
    CHECK(p != nullptr);
    if (!p) return;
    CHECK_EQ(p->Info().apiVersion, plugin::kNativeApiVersion);
    CHECK_EQ(std::string(p->Info().name), "physics_plugin");

    auto getApi = (const NeonPhysicsApi * (*)(void*))p->Symbol("NeonPhysics_GetApi");
    CHECK(getApi != nullptr);
    if (!getApi) return;
    const NeonPhysicsApi* api = getApi(p->Instance());
    CHECK(api != nullptr);
    if (!api) return;
    CHECK(api->add_sphere && api->step && api->body_count && api->get_y && api->reset);

    api->reset(p->Instance());
    const int id = api->add_sphere(p->Instance(), 0.0f, 10.0f, 0.0f, 1.0f, /*dynamic=*/1);
    CHECK_EQ(id, 1);
    CHECK_EQ(api->body_count(p->Instance()), 1);
    // Fall under gravity: 9 units from y=10 to the ground (y=radius) takes
    // ~1.35s, so step 2s to be safely resting.
    for (int i = 0; i < 120; ++i) api->step(p->Instance(), 1.0f / 60.0f, 0.0f, -9.81f, 0.0f);
    CHECK_NEAR(api->get_y(p->Instance(), id), 1.0f, 0.02f);
    // A static sphere does not fall.
    const int s = api->add_sphere(p->Instance(), 0.0f, 3.0f, 0.0f, 0.5f, /*dynamic=*/0);
    for (int i = 0; i < 60; ++i) api->step(p->Instance(), 1.0f / 60.0f, 0.0f, -9.81f, 0.0f);
    CHECK_NEAR(api->get_y(p->Instance(), s), 3.0f, 0.02f);
}

TEST(NativePluginRejectsMissingLibrary) {
    std::string err;
    auto p = plugin::NativePlugin::Load("definitely_missing_plugin_xyz", &err);
    CHECK(p == nullptr);
    CHECK(!err.empty());
}

TEST(NativePluginHotReload) {
    std::string err;
    auto p = plugin::NativePlugin::Load(NEON_PLUGIN_PHYSICS_PATH, &err);
    CHECK(p != nullptr);
    if (!p) return;
    auto api0 = (const NeonPhysicsApi * (*)(void*))p->Symbol("NeonPhysics_GetApi");
    CHECK(api0 != nullptr);
    const NeonPhysicsApi* a0 = api0(p->Instance());
    CHECK(a0 != nullptr);
    // Reload destroys the instance + frees the library, then reloads it: the
    // new instance must be a fresh world (0 bodies) with a working API.
    CHECK(p->Reload(NEON_PLUGIN_PHYSICS_PATH, &err));
    auto api1 = (const NeonPhysicsApi * (*)(void*))p->Symbol("NeonPhysics_GetApi");
    CHECK(api1 != nullptr);
    const NeonPhysicsApi* a1 = api1(p->Instance());
    CHECK(a1 != nullptr);
    if (!a1) return;
    CHECK_EQ(a1->body_count(p->Instance()), 0);
    a1->add_sphere(p->Instance(), 0.0f, 4.0f, 0.0f, 1.0f, 1);
    CHECK_EQ(a1->body_count(p->Instance()), 1);
}

TEST(NativePluginDiscoversViaManifest) {
    // A temp <base>/plugins/physics_plugin/ with a native manifest + the DLL:
    // LoadNativePlugins must discover + load it from the manifest alone.
    test::TempDir tmp;
    const std::string plugins = tmp.Str() + "/plugins";
    const std::string dir = plugins + "/physics_plugin";
    CHECK(Mkdir(plugins));
    CHECK(Mkdir(dir));
    WriteText(dir + "/plugin.json",
              "{\"id\":\"physics_plugin\",\"name\":\"示例物理\",\"version\":\"1.0.0\","
              "\"type\":\"native\",\"backend\":\"native\",\"entry\":\"neon_plugin_physics.dll\"}");
    CHECK(CopyFile(NEON_PLUGIN_PHYSICS_PATH, dir + "/neon_plugin_physics.dll"));

    std::vector<std::unique_ptr<plugin::NativePlugin>> loaded =
        plugin::LoadNativePlugins(tmp.Str());
    CHECK_EQ(loaded.size(), 1u);
    if (loaded.empty()) return;
    CHECK_EQ(std::string(loaded[0]->Info().name), "physics_plugin");
    auto getApi =
        (const NeonPhysicsApi * (*)(void*))loaded[0]->Symbol("NeonPhysics_GetApi");
    CHECK(getApi != nullptr);
    if (!getApi) return;
    const NeonPhysicsApi* api = getApi(loaded[0]->Instance());
    CHECK(api != nullptr);
    api->add_sphere(loaded[0]->Instance(), 0.0f, 2.0f, 0.0f, 1.0f, 1);
    CHECK_EQ(api->body_count(loaded[0]->Instance()), 1);
}

// G5-1: the same DLL also ships a full neon::physics::World via the C factory.
// The host discovers the provider through LoadNativePhysicsBackend and drives
// the real solver through the plugin, exactly like a middleware physics SDK.
TEST(NativeBackendLoadsPhysicsProvider) {
    test::TempDir tmp;
    const std::string plugins = tmp.Str() + "/plugins";
    const std::string dir = plugins + "/physics_plugin";
    CHECK(Mkdir(plugins));
    CHECK(Mkdir(dir));
    WriteText(dir + "/plugin.json",
              "{\"id\":\"physics_plugin\",\"name\":\"示例物理\",\"version\":\"1.0.0\","
              "\"type\":\"native\",\"backend\":\"native\",\"entry\":\"neon_plugin_physics.dll\"}");
    CHECK(CopyFile(NEON_PLUGIN_PHYSICS_PATH, dir + "/neon_plugin_physics.dll"));

    // Discovery by name ("physics" matches the reported backend name "custom"?
    // no — it matches the library stem neon_plugin_physics / info name
    // "physics_plugin"); the generic "*" accepts any provider.
    std::unique_ptr<plugin::PhysicsBackend> backend =
        plugin::LoadNativePhysicsBackend("*", tmp.Str());
    CHECK(backend != nullptr);
    if (!backend) return;
    CHECK_EQ(backend->Name(), "custom");

    std::unique_ptr<physics::World, std::function<void(physics::World*)>> world =
        backend->CreateWorld();
    CHECK(world != nullptr);
    if (!world) return;

    // The world is the real engine solver: the dynamic sphere falls onto the
    // static box (box top at y=1.0, so the sphere rests at top + radius = 2.0);
    // the static box itself never moves.
    const physics::World::BodyId dyn =
        world->AddSphere(/*owner=*/1, math::Vec3{0.0f, 10.0f, 0.0f}, /*radius=*/1.0f,
                         /*dynamic=*/true);
    CHECK(dyn.Valid());
    const physics::World::BodyId st =
        world->AddBox(/*owner=*/2, math::Vec3{0.0f, 0.5f, 0.0f}, math::Vec3{0.5f, 0.5f, 0.5f},
                      /*dynamic=*/false);
    CHECK(st.Valid());
    for (int i = 0; i < 240; ++i) world->Step(1.0f / 60.0f, math::Vec3{0.0f, -9.81f, 0.0f});
    CHECK_NEAR(world->GetPosition(dyn).y, 2.0f, 0.02f);
    CHECK_NEAR(world->GetPosition(st).y, 0.5f, 0.02f);
}

// G5-1: provider lookup by explicit name (matches the info name / library stem,
// case-insensitive); a wrong name finds nothing and is not fatal.
TEST(NativeBackendByName) {
    test::TempDir tmp;
    const std::string plugins = tmp.Str() + "/plugins";
    const std::string dir = plugins + "/physics_plugin";
    CHECK(Mkdir(plugins));
    CHECK(Mkdir(dir));
    WriteText(dir + "/plugin.json",
              "{\"id\":\"physics_plugin\",\"name\":\"示例物理\",\"version\":\"1.0.0\","
              "\"type\":\"native\",\"backend\":\"native\",\"entry\":\"neon_plugin_physics.dll\"}");
    CHECK(CopyFile(NEON_PLUGIN_PHYSICS_PATH, dir + "/neon_plugin_physics.dll"));

    std::unique_ptr<plugin::PhysicsBackend> found =
        plugin::LoadNativePhysicsBackend("PHYSICS", tmp.Str());
    CHECK(found != nullptr);
    CHECK(found->plugin->Info().name != nullptr);
    CHECK(found->plugin->Info().name[0] != '\0');

    std::unique_ptr<plugin::PhysicsBackend> missing =
        plugin::LoadNativePhysicsBackend("nonexistent", tmp.Str());
    CHECK(missing == nullptr);
}
