// SPDX-License-Identifier: GPL-3.0-or-later
// [nvn] settings (nvn_settings.h): the one reader of the section's ini keys and environment variables.
#include "nvn_settings.h"

#include <algorithm>
#include <atomic>
#include <mutex>
#include <thread>

#include "nxrt/config.h"
#include "nxrt/nvn_backend.h"

namespace nxrt::nvn::be {
namespace {

std::mutex g_mu;
NvnSettings g_settings;
std::atomic<bool> g_loaded{false};   // the fast path of nvn_settings() reads this without the lock
// play mode's defaults for pool_mirror / pass_barriers (nvn_set_play_defaults); unset: the built-in ones
bool g_play_set = false, g_play_mirror = true, g_play_tracked = true;

NvnSettings read() {
    NvnSettings s;
    const char* n = "nvn";
    s.log_first_calls = config_get_int(n, "log_first_calls", s.log_first_calls);
    const std::string unk = config_get_str(n, "unknown", "stub");
    s.unknown_null = unk == "null" || unk == "NULL";
    s.shader_cache = config_get_str(n, "shader_cache", "");

    s.pipeline_cache = config_get_str(n, "pipeline_cache", s.pipeline_cache.c_str());
    s.prewarm = config_get_bool(n, "prewarm", s.prewarm);
    s.pipeline_keys = config_get_str(n, "pipeline_keys", s.pipeline_keys.c_str());
    s.prewarm_threads = config_get_int(n, "prewarm_threads", compile_threads_default());
    s.gpl = config_get_bool(n, "gpl", s.gpl);
    s.gpl_lto = config_get_bool(n, "gpl_lto", s.gpl_lto);
    s.gpl_cold = config_get_bool(n, "gpl_cold", s.gpl_cold);
    const std::string up = config_get_str(n, "gpl_upgrade", "whole");
    s.gpl_upgrade = (up == "off" || up == "false" || up == "0") ? NvnUpgrade::Off : up == "lto" ? NvnUpgrade::Lto : NvnUpgrade::Whole;
    s.gpl_threads = config_get_int(n, "gpl_threads", compile_threads_default());
    s.gpl_upgrade_threads = config_get_int(n, "gpl_upgrade_threads", s.gpl_upgrade_threads);
    s.compile_priority = config_get_str(n, "compile_priority", s.compile_priority.c_str());
    s.compile_eco = config_get_bool(n, "compile_eco", s.compile_eco);
    s.prepare_cache = config_get_bool(n, "prepare_cache", s.prepare_cache);
    s.compile_note = config_get_bool(n, "compile_note", s.compile_note);

    s.pool_view = config_get_bool(n, "pool_view", s.pool_view);
    s.pool_prefetch = config_get_bool(n, "pool_prefetch", s.pool_prefetch);
    if (config_has(n, "pool_mirror")) {
        const std::string v = config_get_str(n, "pool_mirror", "on");
        s.pool_mirror = v == "on" || v == "1" || v == "true";
    } else {
        s.pool_mirror = g_play_set ? g_play_mirror : true;
    }
    s.pool_mirror_mb = config_has(n, "pool_mirror_mb") ? std::max<int64_t>(0, config_get_int(n, "pool_mirror_mb", 0)) : -1;
    if (config_has(n, "pass_barriers")) s.tracked_barriers = config_get_str(n, "pass_barriers", "tracked") != "full";
    else s.tracked_barriers = g_play_set ? g_play_tracked : true;

    s.bc7_cache = config_get_str(n, "bc7_cache", s.bc7_cache.c_str());
    s.bc7_quality_set = config_has(n, "bc7_quality");
    s.bc7_quality = config_get_int(n, "bc7_quality", 0);
    s.tex_write_watch = config_get_bool(n, "tex_write_watch", s.tex_write_watch);
    s.tex_prep = config_get_bool(n, "tex_prep", s.tex_prep);
    s.tex_prep_mb = config_get_int(n, "tex_prep_mb", s.tex_prep_mb);
    s.tex_prep_min_kb = config_get_int(n, "tex_prep_min_kb", s.tex_prep_min_kb);
    s.tex_prep_early_kb = config_get_int(n, "tex_prep_early_kb", s.tex_prep_early_kb);
    s.tex_prep_stage_kb = config_get_int(n, "tex_prep_stage_kb", s.tex_prep_stage_kb);
    s.tex_prep_verify = config_get_bool(n, "tex_prep_verify", s.tex_prep_verify);
    s.tex_prep_threads = config_get_int(n, "tex_prep_threads", s.tex_prep_threads);
    s.tex_edit_quality = std::clamp<int64_t>(config_get_int(n, "tex_edit_quality", s.tex_edit_quality), 0, 6);
    return s;
}

void load_locked() {
    g_settings = read();
    g_loaded.store(true, std::memory_order_release);
}

}  // namespace

int64_t compile_threads_default() {
    const unsigned hw = std::max(1u, std::thread::hardware_concurrency());
    return std::clamp<int64_t>(hw / 6, 1, 4);
}

const NvnSettings& nvn_settings() {
    if (!g_loaded.load(std::memory_order_acquire)) {
        std::lock_guard<std::mutex> lk(g_mu);
        if (!g_loaded.load(std::memory_order_relaxed)) load_locked();
    }
    return g_settings;
}

void nvn_settings_reload() {
    std::lock_guard<std::mutex> lk(g_mu);
    load_locked();
}

void nvn_set_play_defaults(bool pool_mirror, bool tracked_barriers) {
    std::lock_guard<std::mutex> lk(g_mu);
    g_play_set = true;
    g_play_mirror = pool_mirror;
    g_play_tracked = tracked_barriers;
    if (g_loaded.load(std::memory_order_relaxed)) {   // already read: only the two fields that follow these defaults change
        if (!config_has("nvn", "pool_mirror")) g_settings.pool_mirror = pool_mirror;
        if (!config_has("nvn", "pass_barriers")) g_settings.tracked_barriers = tracked_barriers;
    }
}

}  // namespace nxrt::nvn::be

namespace nxrt::nvn {
void backend_set_gpu_path_defaults(bool pool_mirror, bool tracked_barriers) { be::nvn_set_play_defaults(pool_mirror, tracked_barriers); }
}  // namespace nxrt::nvn
