// SPDX-License-Identifier: GPL-3.0-or-later
// SDL3 host sink (include/nxrt/audio_sink.h): an AudioSink that plays the guest's interleaved int16 PCM through an SDL audio stream on the
// output device chosen by [audio] device (the default device unless it names another one). The same code runs on Windows (WASAPI under SDL)
// and Linux (PipeWire, PulseAudio, ALSA under SDL). It also provides list_output_devices().
//
// Push model: write() puts frames into the stream (SDL converts format, rate and channel count to what the device wants, 5.1 is mixed down on
// a stereo device) and the device thread drains it at its own clock, so queued_frames() is the SDL queue level and
// has_device_clock() is true. The stream is capped at kMaxQueuedFrames: beyond that write() drops the rest and reports what it accepted, so
// the caller never blocks. A "get" callback on the stream runs on SDL's audio thread each time the device wants data; when the stream cannot
// fill the request it counts an underrun (once per starved stretch), and it samples the queue level for the counters. The stream carries the
// process master volume ([audio] volume) as its gain, re-applied whenever it changes.
#include <SDL3/SDL.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <string>
#include <vector>

#include "nxrt/audio_sink.h"
#include "nxrt/config.h"
#include "nxrt/log.h"

namespace nxrt::audio {
namespace {

constexpr uint32_t kMaxQueuedFrames = 12 * 1024;   // about 256 ms at 48 kHz, as many frames as the XAudio2 sink queued
constexpr uint32_t kDeviceFrames = 512;            // the device buffer asked for (about 11 ms at 48 kHz); SDL_AUDIO_DEVICE_SAMPLE_FRAMES overrides it

// Holds the SDL audio subsystem (reference counted by SDL) while it lives.
class AudioSubsystem {
public:
    AudioSubsystem() : ok_(SDL_InitSubSystem(SDL_INIT_AUDIO)) {
        if (!ok_) NXLOG_W("audio", "sdl: audio subsystem failed to start: %s", SDL_GetError());
    }
    ~AudioSubsystem() {
        if (ok_) SDL_QuitSubSystem(SDL_INIT_AUDIO);
    }
    AudioSubsystem(const AudioSubsystem&) = delete;
    AudioSubsystem& operator=(const AudioSubsystem&) = delete;
    explicit operator bool() const { return ok_; }
    // Hands the reference over to the caller (who calls SDL_QuitSubSystem(SDL_INIT_AUDIO) itself).
    void release() { ok_ = false; }

private:
    bool ok_;
};

std::string lower_ascii(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

void log_devices(const std::vector<OutputDevice>& devices) {
    if (devices.empty()) NXLOG_I("audio", "sdl: no output devices");
    for (const OutputDevice& d : devices) NXLOG_I("audio", "sdl: output device%s: %s [%s]", d.is_default ? " (default)" : "", d.name.c_str(), d.id.c_str());
}

// The device [audio] device asks for: 0 (SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, which follows the system default) for "default" or empty. Otherwise an
// exact device id (as listed) wins, then the first device whose name contains the text (ASCII case-insensitive). No match: a warning and the default.
SDL_AudioDeviceID pick_device(const std::vector<OutputDevice>& devices, const std::string& want) {
    if (want.empty() || lower_ascii(want) == "default") return 0;
    for (const OutputDevice& d : devices) {
        if (d.id == want) return static_cast<SDL_AudioDeviceID>(std::strtoul(d.id.c_str(), nullptr, 10));
    }
    const std::string needle = lower_ascii(want);
    for (const OutputDevice& d : devices) {
        if (lower_ascii(d.name).find(needle) != std::string::npos) return static_cast<SDL_AudioDeviceID>(std::strtoul(d.id.c_str(), nullptr, 10));
    }
    NXLOG_W("audio", "sdl: [audio] device = %s matches no output device; using the default device", want.c_str());
    return 0;
}

class SdlSink final : public AudioSink {
public:
    ~SdlSink() override { close(); }

    bool open(uint32_t sample_rate, uint32_t channels) override;
    void close() override;
    uint32_t write(const int16_t* pcm, uint32_t frames) override;
    uint32_t queued_frames() override { return stream_ ? queued_now() : 0; }
    bool has_device_clock() const override { return true; }   // the stream drains at the device's rate
    const char* name() const override { return "sdl"; }
    SinkStats stats() override;

private:
    static void SDLCALL on_pull(void* self, SDL_AudioStream*, int additional_amount, int total_amount) {
        static_cast<SdlSink*>(self)->note_pull(additional_amount, total_amount);
    }
    void note_pull(int additional_bytes, int total_bytes) noexcept;
    uint32_t queued_now() noexcept;
    void note_peak(uint32_t frames) noexcept;

    SDL_AudioStream* stream_ = nullptr;
    bool subsystem_ = false;          // this sink holds a reference to the SDL audio subsystem
    uint32_t channels_ = 0;
    uint32_t rate_ = 48000;
    float applied_volume_ = 1.0f;

    std::atomic<uint64_t> frames_written_{0};
    std::atomic<uint64_t> frames_accepted_{0};
    std::atomic<uint64_t> frames_dropped_{0};
    std::atomic<uint64_t> underruns_{0};
    std::atomic<uint32_t> queue_peak_frames_{0};
    std::atomic<uint32_t> queue_low_frames_{0xffffffffu};
    std::atomic<uint64_t> submitted_frames_{0};
    std::atomic<bool> started_{false};   // frames have been written: the device running dry is an underrun from here on
    std::atomic<bool> starved_{false};   // the device found the stream short now; an underrun is counted once per such stretch
};

bool SdlSink::open(uint32_t sample_rate, uint32_t channels) {
    close();
    if (channels != 2 && channels != 6) {
        NXLOG_W("audio", "sdl: %u channels not supported (2 or 6)", channels);
        return false;
    }
    if (sample_rate < 8000 || sample_rate > 192000) {
        NXLOG_W("audio", "sdl: sample rate %u out of range", sample_rate);
        return false;
    }

    AudioSubsystem audio;
    if (!audio) return false;

    SDL_SetHintWithPriority(SDL_HINT_AUDIO_DEVICE_SAMPLE_FRAMES, std::to_string(kDeviceFrames).c_str(), SDL_HINT_DEFAULT);
    const std::vector<OutputDevice> devices = list_output_devices();
    log_devices(devices);
    const SDL_AudioDeviceID device_id = pick_device(devices, config_get_str("audio", "device", "default"));

    // 5.1 is FL FR FC LFE BL BR in SDL as in the Switch's layout; SDL mixes it down for a stereo device.
    SDL_AudioSpec spec{};
    spec.format = SDL_AUDIO_S16;
    spec.channels = static_cast<int>(channels);
    spec.freq = static_cast<int>(sample_rate);
    SDL_AudioStream* stream = SDL_OpenAudioDeviceStream(device_id ? device_id : SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, nullptr, nullptr);
    if (!stream && device_id) {
        NXLOG_W("audio", "sdl: the chosen output device failed (%s); using the default device", SDL_GetError());
        stream = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, nullptr, nullptr);
    }
    if (!stream) {
        NXLOG_W("audio", "sdl: no audio output device (%u Hz, %u ch): %s", sample_rate, channels, SDL_GetError());
        return false;
    }

    started_.store(false);
    starved_.store(false);
    submitted_frames_.store(0);
    channels_ = channels;
    rate_ = sample_rate;
    applied_volume_ = master_volume();
    SDL_SetAudioStreamGain(stream, applied_volume_);
    SDL_SetAudioStreamGetCallback(stream, &SdlSink::on_pull, this);
    if (!SDL_ResumeAudioStreamDevice(stream)) {   // the stream starts paused
        NXLOG_W("audio", "sdl: starting the output device failed: %s", SDL_GetError());
        SDL_DestroyAudioStream(stream);
        return false;
    }
    stream_ = stream;
    subsystem_ = true;
    audio.release();   // kept until close()

    SDL_AudioSpec dev{};
    int dev_frames = 0;
    const SDL_AudioDeviceID dev_id = SDL_GetAudioStreamDevice(stream_);
    SDL_GetAudioDeviceFormat(dev_id, &dev, &dev_frames);
    const char* dev_name = SDL_GetAudioDeviceName(dev_id);
    NXLOG_I("audio", "sdl: opened %u Hz, %u channels, volume %.2f on %s (device %d Hz, %d ch, %d frames per buffer, driver %s)", sample_rate, channels,
            applied_volume_, dev_name ? dev_name : "?", dev.freq, dev.channels, dev_frames, SDL_GetCurrentAudioDriver());
    return true;
}

void SdlSink::close() {
    if (stream_) {
        NXLOG_I("audio", "%s", format_stats(name(), stats()).c_str());
        started_.store(false);
        // Closes the device; waits for the audio thread to leave the callback. Not callable from the callback.
        SDL_DestroyAudioStream(stream_);
        stream_ = nullptr;
    }
    if (subsystem_) {
        subsystem_ = false;
        SDL_QuitSubSystem(SDL_INIT_AUDIO);
    }
    channels_ = 0;
}

uint32_t SdlSink::write(const int16_t* pcm, uint32_t frames) {
    uint32_t accepted = 0;
    if (pcm && frames && stream_) {
        const float volume = master_volume();
        if (volume != applied_volume_) {
            SDL_SetAudioStreamGain(stream_, volume);
            applied_volume_ = volume;
        }
        const uint32_t queued = queued_now();
        const uint32_t room = queued >= kMaxQueuedFrames ? 0 : kMaxQueuedFrames - queued;
        const uint32_t n = std::min(frames, room);
        if (n && SDL_PutAudioStreamData(stream_, pcm, static_cast<int>(n * channels_ * sizeof(int16_t)))) {
            accepted = n;
            started_.store(true);
            starved_.store(false);
            submitted_frames_.fetch_add(n, std::memory_order_relaxed);
            note_peak(queued + n);
        }
    }
    frames_written_.fetch_add(frames, std::memory_order_relaxed);
    frames_accepted_.fetch_add(accepted, std::memory_order_relaxed);
    frames_dropped_.fetch_add(frames - accepted, std::memory_order_relaxed);
    return accepted;
}

uint32_t SdlSink::queued_now() noexcept {
    const int bytes = stream_ ? SDL_GetAudioStreamQueued(stream_) : 0;
    return bytes > 0 && channels_ ? static_cast<uint32_t>(bytes) / (channels_ * static_cast<uint32_t>(sizeof(int16_t))) : 0u;
}

void SdlSink::note_peak(uint32_t frames) noexcept {
    uint32_t peak = queue_peak_frames_.load(std::memory_order_relaxed);
    while (frames > peak && !queue_peak_frames_.compare_exchange_weak(peak, frames, std::memory_order_relaxed)) {
    }
}

// Runs on SDL's audio thread, with the stream locked, each time the device wants data: `additional` is how much the stream is short of the
// request. Cheap: one queue-level read and a few atomic updates.
void SdlSink::note_pull(int additional_bytes, int total_bytes) noexcept {
    (void)total_bytes;
    if (!started_.load(std::memory_order_relaxed) || !stream_) return;
    const uint32_t q = queued_now();
    note_peak(q);
    if (additional_bytes > 0) {
        if (!starved_.exchange(true)) underruns_.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    starved_.store(false);
    // The margin against an underrun: the fewest frames queued when the device came for more (not counted during the start-up fill)
    if (q > 0 && submitted_frames_.load(std::memory_order_relaxed) > rate_ / 2 + q) {
        uint32_t low = queue_low_frames_.load(std::memory_order_relaxed);
        while (q < low && !queue_low_frames_.compare_exchange_weak(low, q, std::memory_order_relaxed)) {
        }
    }
}

SinkStats SdlSink::stats() {
    SinkStats s;
    s.frames_written = frames_written_.load(std::memory_order_relaxed);
    s.frames_accepted = frames_accepted_.load(std::memory_order_relaxed);
    s.frames_dropped = frames_dropped_.load(std::memory_order_relaxed);
    s.underruns = underruns_.load(std::memory_order_relaxed);
    s.queued_peak_frames = queue_peak_frames_.load(std::memory_order_relaxed);
    s.queued_peak = (s.queued_peak_frames + 511) / 512;   // in device-buffer sized units, what the XAudio2 sink counted in buffers
    const uint32_t low = queue_low_frames_.load(std::memory_order_relaxed);
    s.queued_low_frames = low == 0xffffffffu ? 0 : low;
    return s;
}

}  // namespace

std::shared_ptr<AudioSink> make_sdl_sink() { return std::make_shared<SdlSink>(); }

std::vector<OutputDevice> list_output_devices() {
    std::vector<OutputDevice> out;
    AudioSubsystem audio;
    if (!audio) return out;
    int count = 0;
    SDL_AudioDeviceID* ids = SDL_GetAudioPlaybackDevices(&count);
    if (!ids) return out;
    const char* def = SDL_GetAudioDeviceName(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK);   // the name of the physical device that is the default now
    const std::string default_name = def ? def : "";
    for (int i = 0; i < count; i++) {
        const char* name = SDL_GetAudioDeviceName(ids[i]);
        OutputDevice d;
        d.id = std::to_string(static_cast<unsigned long>(ids[i]));
        d.name = name ? name : "";
        d.is_default = !default_name.empty() && d.name == default_name;
        out.push_back(std::move(d));
    }
    SDL_free(ids);
    return out;
}

}  // namespace nxrt::audio
