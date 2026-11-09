// SPDX-License-Identifier: GPL-3.0-or-later
// NVN back-end diagnostics (nvn_debug.h): the one reader of the diagnostics switches.
#include "nvn_debug.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <mutex>

#include "nxrt/config.h"
#include "nvn_core.h"

namespace nxrt::nvn::be {
namespace {

// Splits "a,b,c" into its non-empty items.
std::vector<std::string> items(const std::string& list) {
    std::vector<std::string> out;
    size_t i = 0;
    while (i < list.size()) {
        size_t j = list.find(',', i);
        if (j == std::string::npos) j = list.size();
        if (j > i) out.push_back(list.substr(i, j - i));
        i = j + 1;
    }
    return out;
}

NvnDebug read() {
    NvnDebug d;
    const char* n = "nvn";
    for (const std::string& it : items(config_get_str(n, "trace_frame", ""))) d.trace_frames.push_back(std::atoll(it.c_str()));
    d.trace_after_fs = config_get_str(n, "trace_after_fs", "");
    d.trace_min_sec = static_cast<double>(config_get_int(n, "trace_min_sec", 0));
    d.trace_sec = static_cast<double>(config_get_int(n, "trace_sec", 0));
    for (const std::string& it : items(config_get_str(n, "trace_snap", ""))) d.trace_snap_draws.push_back(std::atoi(it.c_str()));
    d.trace_snap_fs = items(config_get_str(n, "trace_snap_fs", ""));
    d.trace_snap_sampled = config_get_int(n, "trace_snap_sampled", 0) != 0;
    d.trace_dump_tex = config_get_str(n, "trace_dump_tex", "");
    {
        const std::string te = config_get_str(n, "trace_tex_events", "0");
        unsigned w = 0, l = 0, m = 0;
        if (std::sscanf(te.c_str(), "%ux%ux%u", &w, &l, &m) == 3) {
            d.tex_events = true;
            d.tex_events_w = w, d.tex_events_layers = l, d.tex_events_levels = m;
        } else {
            d.tex_events = std::atoll(te.c_str()) != 0;
        }
    }
    d.skip_fs_file = config_get_str(n, "skip_fs_file", "");
    d.shot_dir = config_get_str(n, "shot_dir", d.shot_dir.c_str());
    for (const std::string& it : items(config_get_str(n, "shot_frames", ""))) {
        long long a = 0, b = 0, c = 1;
        const int k = std::sscanf(it.c_str(), "%lld:%lld:%lld", &a, &b, &c);
        if (k == 1) d.shots.push_back({a, a, 1});
        else if (k >= 2 && c > 0) d.shots.push_back({a, b, c});
    }
    d.dump_missing = config_get_str(n, "dump_missing", "");
    d.dump_astc = config_get_str(n, "dump_astc", "");
    d.dump_astc_w = config_get_int(n, "dump_astc_w", 0);
    d.tex_prep_log = config_get_bool(n, "tex_prep_log", false);
    d.gpl_ab_pause = config_get_int(n, "gpl_ab_pause", 0);
    d.gpuperf_ab = config_get_int(n, "gpuperf_ab", 0);
    d.latency_ab_toggle_s = config_get_double("latency", "ab_toggle_s", 0.0);
    d.any = !d.trace_frames.empty() || !d.trace_after_fs.empty() || d.trace_sec > 0 || !d.trace_dump_tex.empty() || d.tex_events || !d.skip_fs_file.empty() ||
            !d.shots.empty() || !d.dump_missing.empty() || !d.dump_astc.empty() || d.tex_prep_log || d.gpl_ab_pause > 0 || d.gpuperf_ab > 0 || d.latency_ab_toggle_s > 0;
    return d;
}

}  // namespace

const NvnDebug& nvn_debug() {
    static const NvnDebug d = read();
    return d;
}

bool debug_skip_draw(const std::string& fs_hash, int draw_index) {
    const NvnDebug& d = nvn_debug();
    if (d.skip_fs_file.empty()) return false;
    static std::mutex mu;
    static std::vector<std::string> list;
    static std::chrono::steady_clock::time_point last{};
    std::lock_guard<std::mutex> lk(mu);
    const auto now = std::chrono::steady_clock::now();
    if (last.time_since_epoch().count() == 0 || now - last > std::chrono::milliseconds(500)) {
        last = now;
        list.clear();
        std::ifstream f(d.skip_fs_file);
        std::string w;
        while (f >> w) list.push_back(w);
    }
    for (const std::string& p : list) {
        if (!p.empty() && p[0] == '#') {   // "#198" or "#198-210"
            const int a = std::atoi(p.c_str() + 1);
            const size_t dash = p.find('-');
            const int b = dash == std::string::npos ? a : std::atoi(p.c_str() + dash + 1);
            if (draw_index >= a && draw_index <= b) return true;
        } else if (fs_hash.compare(0, p.size(), p) == 0) {
            return true;
        }
    }
    return false;
}

bool debug_shot_wanted(int64_t n) {
    for (const NvnDebug::Shot& s : nvn_debug().shots)
        if (n >= s.from && n <= s.to && (n - s.from) % s.step == 0) return true;
    return false;
}

bool debug_tex_events(const Texture* t) {
    const NvnDebug& d = nvn_debug();
    if (!d.tex_events || !t || !t->fmt) return false;
    return (!d.tex_events_w || t->b.width == static_cast<int>(d.tex_events_w)) && (!d.tex_events_layers || t->layers == d.tex_events_layers) &&
           (!d.tex_events_levels || t->levels == d.tex_events_levels);
}

}  // namespace nxrt::nvn::be
