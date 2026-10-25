// SPDX-License-Identifier: GPL-3.0-or-later
// The active host audio sink (include/nxrt/audio_sink.h): a NullSink until replaced. Also the process-wide master volume and the
// stats line that each sink logs when it closes.
#include <atomic>
#include <cstdio>
#include <functional>
#include <mutex>

#include "nxrt/audio_sink.h"
#include "nxrt/config.h"
#include "nxrt/gauge.h"
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

// The current sink's counters (a copy of the pointer keeps the sink alive while they are read), or zeros with no sink.
SinkStats current_stats() {
    std::shared_ptr<AudioSink> s;
    {
        std::lock_guard<std::mutex> lk(g_mu);
        s = g_sink;
    }
    return s ? s->stats() : SinkStats{};
}

// ---- gauges (include/nxrt/gauge.h): the sink's own counters, same numbers as stats_report() ----
// Removes its gauge when this file's statics are torn down (declared after g_sink, so it goes first).
struct GaugeHook {
    const char* name;
    GaugeHook(const char* n, std::function<int64_t()> f) : name(n) { nxrt::gauge_register(n, std::move(f)); }
    ~GaugeHook() { nxrt::gauge_unregister(name); }
};
GaugeHook gauge_frames_written("audio.sink_frames_written", [] { return static_cast<int64_t>(current_stats().frames_written); });
GaugeHook gauge_frames_accepted("audio.sink_frames_accepted", [] { return static_cast<int64_t>(current_stats().frames_accepted); });
GaugeHook gauge_frames_dropped("audio.sink_frames_dropped", [] { return static_cast<int64_t>(current_stats().frames_dropped); });   // overruns: device queue full
GaugeHook gauge_underruns("audio.underruns", [] { return static_cast<int64_t>(current_stats().underruns); });
GaugeHook gauge_queued_buffers_peak("audio.queued_buffers_peak", [] { return static_cast<int64_t>(current_stats().queued_peak); });
GaugeHook gauge_queued_frames_peak("audio.queued_frames_peak", [] { return static_cast<int64_t>(current_stats().queued_peak_frames); });
GaugeHook gauge_queued_frames_low("audio.queued_frames_low", [] { return static_cast<int64_t>(current_stats().queued_low_frames); });
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
        const std::string want = config_get_str("audio", "sink", "auto");
        if (want == "paced") {
            chosen = make_paced_sink(config_get_double("audio", "paced_ppm", 0.0));
        } else if (want == "xaudio2" || want == "auto") {
            chosen = make_xaudio2_sink();
            if (!chosen) {
                NXLOG_W("audio", "XAudio2 is not available: using the null sink (no sound)");
            } else if (!chosen->open(48000, 2)) {   // the real open comes with the first frames; this only finds out whether a device is there
                NXLOG_W("audio", "XAudio2 cannot open an output device: using the null sink (no sound)");
                chosen.reset();
            } else {
                chosen->close();
            }
        } else if (want != "null") {
            NXLOG_W("audio", "[audio] sink = %s is not a sink name (auto, xaudio2, paced, null): using the null sink", want.c_str());
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
                  "sink %s: written %llu, accepted %llu, dropped %llu, underruns %llu, queued peak %u buffers, queue %u..%u frames",
                  sink_name ? sink_name : "?", static_cast<unsigned long long>(s.frames_written),
                  static_cast<unsigned long long>(s.frames_accepted), static_cast<unsigned long long>(s.frames_dropped),
                  static_cast<unsigned long long>(s.underruns), static_cast<unsigned>(s.queued_peak),
                  static_cast<unsigned>(s.queued_low_frames), static_cast<unsigned>(s.queued_peak_frames));
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
