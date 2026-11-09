// SPDX-License-Identifier: GPL-3.0-or-later
// The [nvn] settings of the NVN back end, typed and read in one place (nvn_settings.cpp). The ini keys and their NXRT_NVN_<KEY> environment
// variables are the ones docs/CONFIG.md lists; the defaults are here. Diagnostics switches (trace, shots, dumps, A/B windows) are in nvn_debug.h.
//
// When: nvn_settings() reads the configuration on first use; the back end reads it again when it comes up (gpu bring-up), and tests that change
// the configuration call nvn_settings_reload(). Neither runs while the back end's threads use the settings.
//
// Play mode does not write [nvn] keys: it passes the choices of its settings file (controls.ini) with nvn_set_play_defaults(), which take the
// place of the built-in defaults of pool_mirror and pass_barriers; a value in the ini or the environment still wins.
#pragma once
#include <cstdint>
#include <string>

namespace nxrt::nvn::be {

enum class NvnUpgrade : uint8_t { Off, Whole, Lto };   // [nvn] gpl_upgrade: off / whole (default) / lto

struct NvnSettings {
    int64_t log_first_calls = 3;        // log_first_calls: the first calls of each entry point that are logged
    bool unknown_null = false;          // unknown = null: unknown entry points resolve to null instead of a logging stub
    std::string shader_cache;           // shader_cache: the translated-shader folder (empty: ./shader_cache/spirv, then next to the exe)
    std::string pipeline_cache = "pipeline_cache.bin";
    bool prewarm = true;
    std::string pipeline_keys = "pipeline_keys.bin";
    int64_t prewarm_threads = 0;        // set by the reader: [nvn] prewarm_threads, else compile_threads_default()
    bool gpl = true, gpl_lto = false, gpl_cold = true;
    NvnUpgrade gpl_upgrade = NvnUpgrade::Whole;
    int64_t gpl_threads = 0;            // set by the reader: [nvn] gpl_threads, else compile_threads_default()
    int64_t gpl_upgrade_threads = 1;
    std::string compile_priority = "below";   // below / normal / idle
    bool compile_eco = true;
    bool prepare_cache = true;
    bool compile_note = true;           // the overlay's "compiling shaders" note (passed to the overlay by the pipeline libraries, nvn_gpl.cpp)
    bool pool_view = true, pool_prefetch = true;
    bool pool_mirror = true;            // pool_mirror: on / off (play default: controls.ini [display] fast_gpu_memory)
    int64_t pool_mirror_mb = -1;        // -1: not set (the cap follows the card's video memory)
    bool tracked_barriers = true;       // pass_barriers: tracked / full (play default: controls.ini [display] fewer_gpu_waits)
    std::string bc7_cache = "bc7_cache";
    bool bc7_quality_set = false;       // bc7_quality given (else nvn_texdata.cpp's default)
    int64_t bc7_quality = 0;
    bool tex_write_watch = true;
    bool tex_prep = true;
    int64_t tex_prep_mb = 512, tex_prep_min_kb = 8, tex_prep_early_kb = 1024, tex_prep_stage_kb = 1024;
    bool tex_prep_verify = false;
    int64_t tex_prep_threads = 0;       // 0: hardware threads / 3, 2 .. 8
    int64_t tex_edit_quality = 1;       // 0 .. 6
};

// Background compile threads when no count is configured (gpl_threads, prewarm_threads): a sixth of the hardware threads, 1 .. 4.
int64_t compile_threads_default();

const NvnSettings& nvn_settings();
void nvn_settings_reload();

// Play mode's choices for the two GPU paths (S38); see the header comment.
void nvn_set_play_defaults(bool pool_mirror, bool tracked_barriers);

}  // namespace nxrt::nvn::be
