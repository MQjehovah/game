#include <fstream>
#include <string>

#include "neon/neon.hpp"
#include "neon/plugin/backend.hpp"
#include "helpers.hpp"
#include "audio_plugin/api.h"

// Set by CMake (Windows) to the built audio plugin DLL path.
#ifndef NEON_PLUGIN_AUDIO_PATH
#define NEON_PLUGIN_AUDIO_PATH ""
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

bool CopyFile(const std::string& src, const std::string& dst) {
    std::ifstream in(src, std::ios::binary);
    std::ofstream out(dst, std::ios::binary);
    if (!in.is_open() || !out.is_open()) return false;
    out << in.rdbuf();
    return true;
}

} // namespace

// G5-1: the audio module can be taken from a native plugin — the same
// kind-scoped backend discovery as physics, driven through the C++ IAudioBackend
// interface. The WinMM mixer may fail to open a device (headless), but the
// backend object must still be created, queried and safely driven.
TEST(NativeAudioBackendLoadsProvider) {
#ifdef NEON_PLUGIN_AUDIO_PATH
    if (std::string(NEON_PLUGIN_AUDIO_PATH).empty()) return; // no audio plugin built
#endif
    test::TempDir tmp;
    const std::string plugins = tmp.Str() + "/plugins";
    const std::string dir = plugins + "/audio_plugin";
    CHECK(Mkdir(plugins));
    CHECK(Mkdir(dir));
    {
        std::ofstream m(dir + "/plugin.json", std::ios::binary);
        m << "{\"id\":\"audio_plugin\",\"name\":\"示例音频\",\"version\":\"1.0.0\","
             "\"type\":\"native\",\"backend\":\"native\",\"entry\":\"neon_plugin_audio.dll\"}";
    }
    CHECK(CopyFile(NEON_PLUGIN_AUDIO_PATH, dir + "/neon_plugin_audio.dll"));

    std::unique_ptr<plugin::AudioBackend> backend =
        plugin::LoadNativeAudioBackend("*", tmp.Str());
    CHECK(backend != nullptr);
    if (!backend) return;
    CHECK_EQ(backend->Name(), "winmm");

    std::unique_ptr<audio::IAudioBackend, std::function<void(audio::IAudioBackend*)>> b =
        backend->CreateBackend();
    CHECK(b != nullptr);
    if (!b) return;

    // The WinMM mixer sets up its critical section in Init(), so Init must be
    // called before driving the interface. On a headless machine it fails —
    // the backend must then stay safe to call and report unavailable.
    const bool ok = b->Init();
    CHECK_EQ(b->Available(), ok);

    audio::SoundFx fx;
    fx.samples = {0, 100, -100, 200};
    fx.sampleRate = 44100;
    b->Play(fx, 0.7f);
    b->PlayMusic(fx, 0.4f);
    b->Play3D(fx, math::Vec3{1, 2, 3}, math::Vec3{0, 0, 0}, math::Vec3{0, 0, -1}, 0.5f);
    b->SetBusVolume(audio::AudioBus::Sfx, 0.8f);
    b->StopAll(); // always safe after Init (successful or not)

    if (ok) { // real device present -> fully exercise the mixer
        b->Play(fx, 1.0f);
        b->Shutdown();
        CHECK(!b->Available());
    }
}

// G5-1: explicit name lookup matches the library stem / reported name
// (case-insensitive); a wrong name finds nothing without crashing.
TEST(NativeAudioBackendByName) {
#ifdef NEON_PLUGIN_AUDIO_PATH
    if (std::string(NEON_PLUGIN_AUDIO_PATH).empty()) return; // no audio plugin built
#endif
    test::TempDir tmp;
    const std::string plugins = tmp.Str() + "/plugins";
    const std::string dir = plugins + "/audio_plugin";
    CHECK(Mkdir(plugins));
    CHECK(Mkdir(dir));
    {
        std::ofstream m(dir + "/plugin.json", std::ios::binary);
        m << "{\"id\":\"audio_plugin\",\"name\":\"示例音频\",\"version\":\"1.0.0\","
             "\"type\":\"native\",\"backend\":\"native\",\"entry\":\"neon_plugin_audio.dll\"}";
    }
    CHECK(CopyFile(NEON_PLUGIN_AUDIO_PATH, dir + "/neon_plugin_audio.dll"));

    std::unique_ptr<plugin::AudioBackend> found =
        plugin::LoadNativeAudioBackend("AUDIO", tmp.Str());
    CHECK(found != nullptr);
    CHECK(found->plugin->Info().name != nullptr);

    std::unique_ptr<plugin::AudioBackend> missing =
        plugin::LoadNativeAudioBackend("nonexistent", tmp.Str());
    CHECK(missing == nullptr);
}
