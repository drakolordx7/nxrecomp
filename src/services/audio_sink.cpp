// SPDX-License-Identifier: GPL-3.0-or-later
// The active host audio sink (include/nxrt/audio_sink.h): a NullSink until replaced.
#include <mutex>

#include "nxrt/audio_sink.h"
#include "nxrt/config.h"
#include "nxrt/log.h"

namespace nxrt::audio {

namespace {
std::mutex g_mu;
std::shared_ptr<AudioSink> g_sink;
}  // namespace

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
        g_sink = chosen ? chosen : std::make_shared<NullSink>();
        NXLOG_I("audio", "host audio sink: %s", g_sink->name());
    }
    return g_sink;
}

}  // namespace nxrt::audio
