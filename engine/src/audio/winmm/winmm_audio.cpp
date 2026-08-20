#include "neon/audio/audio.hpp"

#include <windows.h>
#include <mmsystem.h>

#include <algorithm>
#include <atomic>
#include <cstring>
#include <vector>

#include "neon/core/log.hpp"

namespace neon::audio {
namespace {

constexpr uint32_t kSampleRate = 44100;
constexpr size_t kBufferFrames = 4096;
constexpr int kBufferCount = 4;
constexpr size_t kMaxVoices = 48;

struct Voice {
    const SoundFx* fx = nullptr;
    size_t pos = 0;
    float volume = 1.0f;
    bool active = false;
};

// Simple software mixer on a background thread. One mono 16-bit stream
// is rendered into a small ring of waveOut buffers.
class WinMMAudio : public IAudioBackend {
public:
    ~WinMMAudio() override { Shutdown(); }

    bool Init() override {
        InitializeCriticalSection(&criticalSection_);
        WAVEFORMATEX format{};
        format.wFormatTag = WAVE_FORMAT_PCM;
        format.nChannels = 1;
        format.nSamplesPerSec = kSampleRate;
        format.wBitsPerSample = 16;
        format.nBlockAlign = 2;
        format.nAvgBytesPerSec = kSampleRate * 2;
        MMRESULT result = waveOutOpen(&device_, WAVE_MAPPER, &format, 0, 0, CALLBACK_NULL);
        if (result != MMSYSERR_NOERROR) {
            NEON_LOG_WARN("WinMM: waveOutOpen failed (%u)", result);
            return false;
        }
        buffers_.resize(kBufferCount);
        for (WAVEHDR& hdr : buffers_) {
            std::memset(&hdr, 0, sizeof(hdr));
            hdr.lpData = new char[kBufferFrames * 2];
            hdr.dwBufferLength = static_cast<DWORD>(kBufferFrames * 2);
        }
        running_ = true;
        thread_ = CreateThread(nullptr, 0, &WinMMAudio::ThreadEntry, this, 0, nullptr);
        available_ = true;
        NEON_LOG_INFO("WinMM: audio mixer started (%u Hz)", kSampleRate);
        return true;
    }

    void Shutdown() override {
        if (!available_ && !running_) return;
        running_ = false;
        if (thread_) {
            WaitForSingleObject(thread_, 3000);
            CloseHandle(thread_);
            thread_ = nullptr;
        }
        if (device_) {
            waveOutReset(device_);
            for (WAVEHDR& hdr : buffers_) {
                if (hdr.dwFlags & WHDR_PREPARED) waveOutUnprepareHeader(device_, &hdr, sizeof(hdr));
                delete[] hdr.lpData;
            }
            waveOutClose(device_);
            device_ = nullptr;
        }
        buffers_.clear();
        DeleteCriticalSection(&criticalSection_);
        available_ = false;
    }

    void Play(const SoundFx& sound, float volume) override {
        if (!available_ || sound.samples.empty()) return;
        EnterCriticalSection(&criticalSection_);
        if (voices_.size() >= kMaxVoices) voices_.erase(voices_.begin());
        Voice voice;
        voice.fx = &sound;
        voice.volume = volume * sound.volume;
        voice.active = true;
        voices_.push_back(voice);
        LeaveCriticalSection(&criticalSection_);
    }

    void StopAll() override {
        EnterCriticalSection(&criticalSection_);
        voices_.clear();
        LeaveCriticalSection(&criticalSection_);
    }

    bool Available() const override { return available_; }

private:
    static DWORD WINAPI ThreadEntry(LPVOID param) {
        static_cast<WinMMAudio*>(param)->MixLoop();
        return 0;
    }

    void MixLoop() {
        int next = 0;
        while (running_) {
            WAVEHDR& hdr = buffers_[next];
            if ((hdr.dwFlags & WHDR_PREPARED) && !(hdr.dwFlags & WHDR_DONE)) {
                Sleep(1);
                continue;
            }

            {
                EnterCriticalSection(&criticalSection_);
                auto* data = reinterpret_cast<int16_t*>(hdr.lpData);
                std::memset(hdr.lpData, 0, kBufferFrames * 2);
                for (Voice& voice : voices_) {
                    if (!voice.active || !voice.fx) continue;
                    const std::vector<int16_t>& samples = voice.fx->samples;
                    for (size_t i = 0; i < kBufferFrames && voice.pos < samples.size(); ++i, ++voice.pos) {
                        int mixed = static_cast<int>(data[i]) +
                                    static_cast<int>(static_cast<float>(samples[voice.pos]) * voice.volume);
                        data[i] = static_cast<int16_t>(std::max(-32768, std::min(32767, mixed)));
                    }
                    if (voice.pos >= samples.size()) {
                        voice.pos = 0;
                        if (!voice.fx->loop) voice.active = false;
                    }
                }
                voices_.erase(std::remove_if(voices_.begin(), voices_.end(),
                                             [](const Voice& v) { return !v.active; }),
                              voices_.end());
                LeaveCriticalSection(&criticalSection_);
            }

            if (hdr.dwFlags & WHDR_PREPARED) waveOutUnprepareHeader(device_, &hdr, sizeof(hdr));
            hdr.dwFlags = 0;
            hdr.dwBufferLength = static_cast<DWORD>(kBufferFrames * 2);
            if (waveOutPrepareHeader(device_, &hdr, sizeof(hdr)) == MMSYSERR_NOERROR) {
                waveOutWrite(device_, &hdr, sizeof(hdr));
            }
            next = (next + 1) % kBufferCount;
        }
    }

    HWAVEOUT device_ = nullptr;
    std::vector<WAVEHDR> buffers_;
    HANDLE thread_ = nullptr;
    std::atomic<bool> running_{false};
    CRITICAL_SECTION criticalSection_{};
    std::vector<Voice> voices_;
    bool available_ = false;
};

} // namespace

std::unique_ptr<IAudioBackend> CreateWinMMAudioBackend() {
    return std::make_unique<WinMMAudio>();
}

} // namespace neon::audio
