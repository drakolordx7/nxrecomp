// SPDX-License-Identifier: GPL-3.0-or-later
// XAudio2 host sink (stream S3c): an AudioSink that plays the guest's interleaved int16 PCM on the default output device.
// The engine, a mastering voice on the default device and one source voice are created in open(); write() copies the caller's
// frames into a small pool of buffers and submits them; the voice callback (OnBufferEnd, on XAudio2's audio thread) returns the
// buffer to the pool. When the device already holds kMaxQueued buffers, or the pool is empty, write() drops the rest and reports
// how many frames it accepted, so the caller never blocks.
#include <windows.h>
#include <objbase.h>
#include <mmreg.h>
#include <xaudio2.h>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <vector>

#include "nxrt/audio_sink.h"
#include "nxrt/log.h"

#pragma comment(lib, "xaudio2.lib")

namespace nxrt::audio {
namespace {

constexpr uint32_t kSlots = 16;          // buffers in the pool
constexpr uint32_t kSlotFrames = 1024;   // frames per buffer (about 21 ms at 48 kHz)
constexpr UINT32 kMaxQueued = 12;        // beyond this many queued buffers write() drops the new frames

// KSDATAFORMAT_SUBTYPE_PCM (ksmedia.h), spelled out so the link needs no ksguid.lib.
constexpr GUID kSubtypePcm = {0x00000001, 0x0000, 0x0010, {0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71}};
// SPEAKER_5POINT1: FRONT_LEFT | FRONT_RIGHT | FRONT_CENTER | LOW_FREQUENCY | BACK_LEFT | BACK_RIGHT.
constexpr DWORD kChannelMask51 = 0x3F;

class XAudio2Sink final : public AudioSink {
public:
    ~XAudio2Sink() override { close(); }

    bool open(uint32_t sample_rate, uint32_t channels) override;
    void close() override;
    uint32_t write(const int16_t* pcm, uint32_t frames) override;
    uint32_t queued_frames() override;
    const char* name() const override { return "xaudio2"; }

private:
    struct Slot {
        std::vector<int16_t> pcm;   // kSlotFrames * channels values
        uint32_t frames = 0;
        bool busy = false;          // submitted and not yet finished by the voice
    };

    // The voice callback. XAudio2 calls it on its own thread; only OnBufferEnd does anything.
    struct Callback final : IXAudio2VoiceCallback {
        XAudio2Sink* owner = nullptr;
        void STDMETHODCALLTYPE OnVoiceProcessingPassStart(UINT32) noexcept override {}
        void STDMETHODCALLTYPE OnVoiceProcessingPassEnd() noexcept override {}
        void STDMETHODCALLTYPE OnStreamEnd() noexcept override {}
        void STDMETHODCALLTYPE OnBufferStart(void*) noexcept override {}
        void STDMETHODCALLTYPE OnBufferEnd(void* ctx) noexcept override {
            owner->release_slot(static_cast<uint32_t>(reinterpret_cast<uintptr_t>(ctx)));
        }
        void STDMETHODCALLTYPE OnLoopEnd(void*) noexcept override {}
        void STDMETHODCALLTYPE OnVoiceError(void*, HRESULT error) noexcept override {
            NXLOG_W("audio", "xaudio2: voice error 0x%08lx", static_cast<unsigned long>(error));
        }
    };

    void release_slot(uint32_t idx);
    void teardown();   // destroys the voices and the engine and undoes our COM initialisation; safe when nothing is open

    IXAudio2* engine_ = nullptr;
    IXAudio2MasteringVoice* master_ = nullptr;
    IXAudio2SourceVoice* source_ = nullptr;
    Callback callback_;
    bool com_owned_ = false;          // this sink called CoInitializeEx successfully and must balance it
    DWORD com_thread_ = 0;            // the thread that did, so CoUninitialize runs there
    uint32_t channels_ = 0;

    std::mutex mu_;                   // guards slots_ (write, queued_frames and the callback)
    std::vector<Slot> slots_ = std::vector<Slot>(kSlots);
};

bool XAudio2Sink::open(uint32_t sample_rate, uint32_t channels) {
    close();
    if (channels != 2 && channels != 6) {
        NXLOG_W("audio", "xaudio2: %u channels not supported (2 or 6)", channels);
        return false;
    }
    if (sample_rate < XAUDIO2_MIN_SAMPLE_RATE || sample_rate > XAUDIO2_MAX_SAMPLE_RATE) {
        NXLOG_W("audio", "xaudio2: sample rate %u out of range", sample_rate);
        return false;
    }

    // XAudio2 wants COM. Initialise it only when this thread has no mode yet; an existing mode (RPC_E_CHANGED_MODE) is kept.
    const HRESULT com = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (com == S_OK || com == S_FALSE) {
        com_owned_ = true;
        com_thread_ = GetCurrentThreadId();
    } else if (com != RPC_E_CHANGED_MODE) {
        NXLOG_W("audio", "xaudio2: CoInitializeEx failed (0x%08lx)", static_cast<unsigned long>(com));
        return false;
    }

    HRESULT hr = XAudio2Create(&engine_, 0, XAUDIO2_DEFAULT_PROCESSOR);
    if (FAILED(hr)) {
        NXLOG_W("audio", "xaudio2: XAudio2Create failed (0x%08lx)", static_cast<unsigned long>(hr));
        teardown();
        return false;
    }

    hr = engine_->CreateMasteringVoice(&master_, XAUDIO2_DEFAULT_CHANNELS, XAUDIO2_DEFAULT_SAMPLERATE, 0, nullptr, nullptr,
                                       AudioCategory_GameEffects);
    if (FAILED(hr)) {
        NXLOG_W("audio", "xaudio2: no audio output device (CreateMasteringVoice 0x%08lx)", static_cast<unsigned long>(hr));
        teardown();
        return false;
    }

    // Stereo is plain PCM; 5.1 needs WAVEFORMATEXTENSIBLE with the channel mask.
    WAVEFORMATEXTENSIBLE wfx{};
    WAVEFORMATEX& f = wfx.Format;
    f.wFormatTag = channels == 2 ? WAVE_FORMAT_PCM : WAVE_FORMAT_EXTENSIBLE;
    f.nChannels = static_cast<WORD>(channels);
    f.nSamplesPerSec = sample_rate;
    f.wBitsPerSample = 16;
    f.nBlockAlign = static_cast<WORD>(channels * 2);
    f.nAvgBytesPerSec = sample_rate * f.nBlockAlign;
    if (channels == 6) {
        f.cbSize = sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX);
        wfx.Samples.wValidBitsPerSample = 16;
        wfx.dwChannelMask = kChannelMask51;
        wfx.SubFormat = kSubtypePcm;
    }

    callback_.owner = this;
    hr = engine_->CreateSourceVoice(&source_, &wfx.Format, 0, XAUDIO2_DEFAULT_FREQ_RATIO, &callback_, nullptr, nullptr);
    if (FAILED(hr)) {
        NXLOG_W("audio", "xaudio2: CreateSourceVoice(%u Hz, %u ch) failed (0x%08lx)", sample_rate, channels,
                static_cast<unsigned long>(hr));
        teardown();
        return false;
    }
    hr = source_->Start(0);
    if (FAILED(hr)) {
        NXLOG_W("audio", "xaudio2: source voice Start failed (0x%08lx)", static_cast<unsigned long>(hr));
        teardown();
        return false;
    }

    {
        std::lock_guard<std::mutex> lk(mu_);
        for (Slot& s : slots_) {
            s.pcm.assign(static_cast<size_t>(kSlotFrames) * channels, 0);
            s.frames = 0;
            s.busy = false;
        }
    }
    channels_ = channels;
    NXLOG_I("audio", "xaudio2: opened %u Hz, %u channels", sample_rate, channels);
    return true;
}

void XAudio2Sink::close() {
    if (!engine_ && !com_owned_) return;
    if (source_) {
        source_->Stop(0);
        source_->FlushSourceBuffers();
        // DestroyVoice waits for the audio thread to leave the callbacks.
        source_->DestroyVoice();
        source_ = nullptr;
    }
    teardown();
    std::lock_guard<std::mutex> lk(mu_);
    for (Slot& s : slots_) {
        s.frames = 0;
        s.busy = false;
    }
}

void XAudio2Sink::teardown() {
    if (master_) {
        master_->DestroyVoice();
        master_ = nullptr;
    }
    if (engine_) {
        engine_->Release();
        engine_ = nullptr;
    }
    if (com_owned_) {
        com_owned_ = false;
        if (GetCurrentThreadId() == com_thread_) CoUninitialize();
    }
    channels_ = 0;
}

uint32_t XAudio2Sink::write(const int16_t* pcm, uint32_t frames) {
    if (!source_ || !pcm || frames == 0) return 0;
    const size_t ch = channels_;
    uint32_t accepted = 0;
    while (accepted < frames) {
        XAUDIO2_VOICE_STATE state{};
        source_->GetState(&state);
        if (state.BuffersQueued >= kMaxQueued) break;

        const uint32_t n = std::min<uint32_t>(frames - accepted, kSlotFrames);
        uint32_t idx = kSlots;
        {
            std::lock_guard<std::mutex> lk(mu_);
            for (uint32_t i = 0; i < kSlots; i++) {
                if (!slots_[i].busy) {
                    idx = i;
                    break;
                }
            }
            if (idx == kSlots) break;   // pool empty: the voice has not returned buffers yet
            Slot& s = slots_[idx];
            s.busy = true;
            s.frames = n;
            std::memcpy(s.pcm.data(), pcm + static_cast<size_t>(accepted) * ch, static_cast<size_t>(n) * ch * sizeof(int16_t));
        }

        XAUDIO2_BUFFER buf{};
        buf.AudioBytes = n * static_cast<UINT32>(ch) * 2;
        buf.pAudioData = reinterpret_cast<const BYTE*>(slots_[idx].pcm.data());
        buf.pContext = reinterpret_cast<void*>(static_cast<uintptr_t>(idx));
        const HRESULT hr = source_->SubmitSourceBuffer(&buf);
        if (FAILED(hr)) {
            release_slot(idx);
            break;
        }
        accepted += n;
    }
    return accepted;
}

uint32_t XAudio2Sink::queued_frames() {
    std::lock_guard<std::mutex> lk(mu_);
    uint32_t total = 0;
    for (const Slot& s : slots_) {
        if (s.busy) total += s.frames;
    }
    return total;
}

void XAudio2Sink::release_slot(uint32_t idx) {
    std::lock_guard<std::mutex> lk(mu_);
    if (idx >= kSlots) return;
    slots_[idx].busy = false;
    slots_[idx].frames = 0;
}

}  // namespace

std::shared_ptr<AudioSink> make_xaudio2_sink() {
    // The sink is only useful when the system XAudio2 DLL is there; the device is opened later in open().
    HMODULE lib = LoadLibraryExW(L"xaudio2_9.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!lib) return nullptr;
    FreeLibrary(lib);
    return std::make_shared<XAudio2Sink>();
}

}  // namespace nxrt::audio
