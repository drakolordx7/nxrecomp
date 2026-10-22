// SPDX-License-Identifier: GPL-3.0-or-later
// Host audio output for the audio services (stream S3c). The audren / audout services produce interleaved 16-bit PCM and hand it to
// the active AudioSink; the sink is pulled by the host device clock. The default sink discards samples (silence), which is also
// what runs under tests. src/services/audio_xaudio2.cpp provides the XAudio2 sink (selected with [audio] sink = xaudio2).
#pragma once
#include <cstdint>
#include <memory>

namespace nxrt::audio {

class AudioSink {
public:
    virtual ~AudioSink() = default;
    // Opens the device for `channels` (2 or 6) interleaved int16 channels at `sample_rate` Hz. False on failure.
    virtual bool open(uint32_t sample_rate, uint32_t channels) = 0;
    virtual void close() = 0;
    // Queues `frames` frames (frames * channels int16 values). Returns the number of frames accepted (the rest is dropped when the
    // device queue is full). Never blocks for long.
    virtual uint32_t write(const int16_t* pcm, uint32_t frames) = 0;
    // Frames queued but not yet played.
    virtual uint32_t queued_frames() = 0;
    virtual const char* name() const = 0;
};

// The null sink: accepts everything, plays nothing, reports an empty queue. Counts frames for tests.
class NullSink : public AudioSink {
public:
    bool open(uint32_t sr, uint32_t ch) override { rate = sr; channels = ch; return true; }
    void close() override {}
    uint32_t write(const int16_t*, uint32_t frames) override { total_frames += frames; return frames; }
    uint32_t queued_frames() override { return 0; }
    const char* name() const override { return "null"; }
    uint32_t rate = 0, channels = 0;
    uint64_t total_frames = 0;
};

void set_sink(std::shared_ptr<AudioSink> s);          // replaces the active sink (closing the old one)
std::shared_ptr<AudioSink> sink();                    // never null: a NullSink until [audio] sink selects another or set_sink is called
std::shared_ptr<AudioSink> make_xaudio2_sink();       // src/services/audio_xaudio2.cpp; nullptr when XAudio2 is unavailable

}  // namespace nxrt::audio
