// SPDX-License-Identifier: GPL-3.0-or-later
#include "nxrt/strutil.h"
#include "nxrt/extras.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <mutex>

#include "nxrt/config.h"
#include "nxrt/game.h"
#include "nxrt/game_render.h"
#include "nxrt/game/hdr_math.h"

namespace nxrt::extras {
namespace {

using nxrt::str::lower;

std::mutex g_mu;
Settings g_settings;
bool g_settings_set = false;
std::atomic<int> g_upscale_mode{-1};          // -1: not read yet
std::atomic<float> g_upscale_sharp{0.8f};
std::atomic<int> g_scene_scale{100};

// The live scene scale also goes to the game (GameRender::scene_scale), which applies it through its own resolution path.
void publish_scene_scale(int percent) {
    g_scene_scale.store(percent);
    if (const game::GameRender* r = game::info().render; r && r->scene_scale) r->scene_scale(percent);
}

// The state is a handful of floats read by the render thread, the guest threads (hooks) and the UI. A seqlock keeps readers lock-free
// and consistent without a mutex on the hot path.
std::atomic<uint32_t> g_seq{0};
HdrState g_state;

}  // namespace

const char* output_kind_name(OutputKind k) {
    switch (k) {
        case OutputKind::Sdr8: return "sdr8";
        case OutputKind::Hdr10: return "hdr10";
        case OutputKind::ScRgb: return "scrgb";
    }
    return "?";
}

Settings settings_from_config(const Settings& base) {
    Settings s = base;
    auto has = [](const char* sec, const char* key) { return config_has(sec, key); };
    if (has("hdr", "mode")) {
        const std::string hm = lower(config_get_str("hdr", "mode", "auto"));
        s.hdr.mode = hm == "off" || hm == "0" || hm == "false" ? HdrMode::Off : hm == "on" || hm == "1" || hm == "true" ? HdrMode::On : HdrMode::Auto;
    }
    if (has("hdr", "format")) {
        const std::string hf = lower(config_get_str("hdr", "format", "auto"));
        s.hdr.format = hf == "hdr10" ? HdrFormat::Hdr10 : hf == "scrgb" ? HdrFormat::ScRgb : HdrFormat::Auto;
    }
    if (has("hdr", "peak_nits")) {
        s.hdr.peak_nits = std::clamp(config_get_double("hdr", "peak_nits", 0.0), 0.0, 10000.0);
        if (s.hdr.peak_nits != 0 && s.hdr.peak_nits < 200) s.hdr.peak_nits = 200;
    }
    if (has("hdr", "paper_white_nits")) {
        s.hdr.paper_white_nits = std::clamp(config_get_double("hdr", "paper_white_nits", 0.0), 0.0, 500.0);
        if (s.hdr.paper_white_nits != 0 && s.hdr.paper_white_nits < 80) s.hdr.paper_white_nits = 80;
    }
    if (has("hdr", "highlight_boost")) s.hdr.highlight_boost = std::clamp(config_get_double("hdr", "highlight_boost", 1.0), 0.0, 2.0);

    if (has("upscale", "mode")) {
        const std::string um = lower(config_get_str("upscale", "mode", "off"));
        s.upscale.mode = um == "fsr1" || um == "spatial" ? UpscaleMode::Fsr1 : UpscaleMode::Off;
    }
    if (has("upscale", "sharpness")) s.upscale.sharpness = std::clamp(config_get_double("upscale", "sharpness", 0.8), 0.0, 1.0);
    if (has("upscale", "scene_scale")) s.upscale.scene_scale = static_cast<int>(std::clamp<int64_t>(config_get_int("upscale", "scene_scale", 100), 25, 100));

    if (has("latency", "mode")) {
        const std::string lm = lower(config_get_str("latency", "mode", "off"));
        s.latency.mode = lm == "auto" ? LatencyMode::Auto : lm == "on" || lm == "1" || lm == "true" ? LatencyMode::On : lm == "boost" ? LatencyMode::Boost : LatencyMode::Off;
    }
    if (has("latency", "frames_ahead")) s.latency.frames_ahead = static_cast<int>(std::clamp<int64_t>(config_get_int("latency", "frames_ahead", 1), -1, 3));
    if (has("latency", "log_timings")) s.latency.log_timings = config_get_bool("latency", "log_timings", false);
    return s;
}

Settings settings() {
    std::lock_guard<std::mutex> lk(g_mu);
    if (!g_settings_set) {
        g_settings = settings_from_config();
        g_settings_set = true;
        g_upscale_mode.store(static_cast<int>(g_settings.upscale.mode));
        g_upscale_sharp.store(static_cast<float>(g_settings.upscale.sharpness));
        publish_scene_scale(g_settings.upscale.scene_scale);
    }
    return g_settings;
}

void set_settings(const Settings& s) {
    std::lock_guard<std::mutex> lk(g_mu);
    g_settings = s;
    g_settings_set = true;
    g_upscale_mode.store(static_cast<int>(s.upscale.mode));
    g_upscale_sharp.store(static_cast<float>(s.upscale.sharpness));
    publish_scene_scale(s.upscale.scene_scale);
}

UpscaleMode upscale_mode() {
    int m = g_upscale_mode.load(std::memory_order_relaxed);
    if (m < 0) {
        settings();
        m = g_upscale_mode.load(std::memory_order_relaxed);
    }
    return static_cast<UpscaleMode>(m);
}

float upscale_sharpness() { return g_upscale_sharp.load(std::memory_order_relaxed); }

int upscale_scene_scale() {
    upscale_mode();
    return g_scene_scale.load(std::memory_order_relaxed);
}

HdrState hdr_state() {
    for (;;) {
        const uint32_t a = g_seq.load(std::memory_order_acquire);
        if (a & 1u) continue;
        const HdrState copy = g_state;
        std::atomic_thread_fence(std::memory_order_acquire);
        if (g_seq.load(std::memory_order_relaxed) == a) return copy;
    }
}

void hdr_set_state(const HdrState& s) {
    std::lock_guard<std::mutex> lk(g_mu);
    const uint32_t a = g_seq.load(std::memory_order_relaxed);
    g_seq.store(a + 1, std::memory_order_relaxed);
    std::atomic_thread_fence(std::memory_order_release);
    g_state = s;
    g_seq.store(a + 2, std::memory_order_release);
}

void hdr_refresh_levels() {
    HdrState hs = hdr_state();
    if (!hs.scene_hdr) return;
    const Settings es = settings();
    hdr_resolve_levels(es.hdr, hs.display_hdr, hs.display_peak_nits, hs.display_sdr_white_nits, &hs.paper_white_nits, &hs.peak_nits);
    hs.peak_ratio = hs.output == OutputKind::Sdr8 ? 1.0f : peak_ratio(hs.peak_nits, hs.paper_white_nits, static_cast<float>(es.hdr.highlight_boost));
    hdr_set_state(hs);
}

void hdr_resolve_levels(const HdrSettings& s, bool display_hdr, float display_peak_nits, float display_sdr_white_nits, float* paper, float* peak) {
    float pw = static_cast<float>(s.paper_white_nits);
    if (pw <= 0) pw = display_hdr && display_sdr_white_nits >= 80.0f ? std::clamp(display_sdr_white_nits, 80.0f, 500.0f) : 203.0f;
    float pk = static_cast<float>(s.peak_nits);
    if (pk <= 0) pk = display_peak_nits >= 200.0f ? std::min(display_peak_nits, 10000.0f) : 1000.0f;
    pk = std::max(pk, pw);
    if (paper) *paper = pw;
    if (peak) *peak = pk;
}

}  // namespace nxrt::extras
