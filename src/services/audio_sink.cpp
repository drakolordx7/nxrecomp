// SPDX-License-Identifier: GPL-3.0-or-later
// The active host audio sink (include/nxrt/audio_sink.h): a NullSink until replaced. Also the process-wide master volume and the
// stats line that each sink logs when it closes.
#include <atomic>
#include <cstdio>
#include <mutex>

#include "nxrt/audio_sink.h"
#include "nxrt/config.h"
#include "nxrt/log.h"

namespace nxrt::audio {

namespace {
std::mutex g_mu;
std::shared_ptr<AudioSink> g_sink;

std::atomic<float> g_volume{1.0f};
std::once_flag g_volume_once;

float clamp01(float v) { return v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v); }

// Reads [audio] volume the first time the volume is used. set_master_volume and master_volume both call this first.
void load_volume_config() {
    std::call_once(g_volume_once, [] {
        double v = config_get_double("audio", "volume", 1.0);
        if (!(v == v)) v = 1.0;   // NaN
        g_volume.store(clamp01(static_cast<float>(v)));
    });
}
}  // namespace

void NullSink::close() { NXLOG_I("audio", "%s", format_stats(name(), stats()).c_str()); }

void set_sink(std::shared_ptr<AudioSink> s) {
    std::shared_ptr<AudioSink> old;
    {
        std::lock_guard<std::mutex> lk(g_mu);
        old = std::move(g_sink);
        g_sink = std::move(s);
    }
    if (old) old->close();
}

std::shared_ptr<AudioSink> sink() {
    std::lock_guard<std::mutex> lk(g_mu);
    if (!g_sink) {
        std::shared_ptr<AudioSink> chosen;
        if (config_get_str("audio", "sink", "null") == "xaudio2") {
            chosen = make_xaudio2_sink();
            if (!chosen) NXLOG_W("audio", "[audio] sink = xaudio2: XAudio2 is not available, using the null sink");
        }
        if (!chosen) chosen = std::make_shared<NullSink>();
        const std::string wav = config_get_str("audio", "wav_dump", "");
        if (!wav.empty()) chosen = make_wav_tee(chosen, wav);
        g_sink = chosen;
        NXLOG_I("audio", "host audio sink: %s", g_sink->name());
    }
    return g_sink;
}

void set_master_volume(float volume) {
    load_volume_config();
    if (!(volume == volume)) return;   // NaN: keep the current volume
    g_volume.store(clamp01(volume));
}

float master_volume() {
    load_volume_config();
    return g_volume.load();
}

std::string format_stats(const char* sink_name, const SinkStats& s) {
    char buf[256];
    std::snprintf(buf, sizeof buf,
                  "sink %s: written %llu, accepted %llu, dropped %llu, underruns %llu, queued peak %u",
                  sink_name ? sink_name : "?", static_cast<unsigned long long>(s.frames_written),
                  static_cast<unsigned long long>(s.frames_accepted), static_cast<unsigned long long>(s.frames_dropped),
                  static_cast<unsigned long long>(s.underruns), static_cast<unsigned>(s.queued_peak));
    return buf;
}

std::string stats_report() {
    std::shared_ptr<AudioSink> s;
    {
        std::lock_guard<std::mutex> lk(g_mu);
        s = g_sink;
    }
    if (!s) return "no audio sink";
    return format_stats(s->name(), s->stats());
}

}  // namespace nxrt::audio
