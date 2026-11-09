// SPDX-License-Identifier: GPL-3.0-or-later
// Diagnostics of the NVN back end: the switches that exist for one investigation (frame traces, snapshots, screenshots of chosen frames,
// skipped draws, data dumps, A/B measurement windows), read once in one place (nvn_debug.cpp). None is a player setting; with none of them set
// debug_on() is false and the replay pays one flag test. Keys and environment variables (NXRT_NVN_<KEY>) are in docs/CONFIG.md.
#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace nxrt::nvn::be {

struct Texture;

struct NvnDebug {
    std::vector<int64_t> trace_frames;        // [nvn] trace_frame = "6050,9300": presented frames to trace (the first is the main one)
    std::string trace_after_fs;               // trace_after_fs = hash prefix: trace the frame after the first draw with this fragment shader
    double trace_min_sec = 0;                 // trace_min_sec: trace_after_fs only counts that long after the first draw
    double trace_sec = 0;                     // trace_sec: trace the first frame presented this long after the first draw
    std::vector<int> trace_snap_draws;        // trace_snap = "253,300": after these draws of a traced frame the targets are copied
    std::vector<std::string> trace_snap_fs;   // trace_snap_fs = "f77f9d,e72811": ... and after every draw with these fragment shaders
    bool trace_snap_sampled = false;          // trace_snap_sampled = 1: a snapshot also copies the textures that draw samples
    std::string trace_dump_tex;               // trace_dump_tex = DIR: the sampled textures of a traced frame, prepared and as the GPU holds them
    bool tex_events = false;
    uint32_t tex_events_w = 0, tex_events_layers = 0, tex_events_levels = 0;   // 0: any
    std::string skip_fs_file;                 // skip_fs_file = FILE: draws to leave out (debug_skip_draw)
    std::string shot_dir = "shots";           // shot_frames = "300,1000:2000:50": presented frames written as <shot_dir>/<n, 6 digits>.png
    struct Shot { int64_t from, to, step; };
    std::vector<Shot> shots;
    std::string dump_missing;                 // dump_missing = DIR: shader blocks the cache has no translation for (nvn_shaders.cpp)
    std::string dump_astc;                    // dump_astc = DIR: level 0 of ASTC textures as tight blocks (nvn_texdata.cpp) ...
    int64_t dump_astc_w = 0;                  // dump_astc_w: ... of this width only (0: any)
    bool tex_prep_log = false;                // tex_prep_log = 1: one line per texture upload with where its data came from
    int64_t gpl_ab_pause = 0;                 // [nvn] gpl_ab_pause = N presents: the pipeline compile threads rest in even windows
    int64_t gpuperf_ab = 0;                   // [nvn] gpuperf_ab = N presents: pool copies and tracked barriers off in even windows
    double latency_ab_toggle_s = 0;           // [latency] ab_toggle_s = seconds: the low-latency mode flips every window

    bool any = false;                         // some switch above is set
};

const NvnDebug& nvn_debug();                  // read on first use
inline bool debug_on() { return nvn_debug().any; }

// [nvn] skip_fs_file: FILE holds fragment shader hash prefixes and "#N" / "#N-M" draw numbers of the frame (the first draw is #1, as in the
// trace), separated by blanks; matching draws are not drawn. The file is read again twice a second, so it can change while the game runs.
bool debug_skip_draw(const std::string& fs_hash, int draw_index);
// [nvn] shot_frames: whether presented frame n is one to write.
bool debug_shot_wanted(int64_t n);
// [nvn] trace_tex_events: whether events of this texture are logged.
bool debug_tex_events(const Texture* t);

}  // namespace nxrt::nvn::be
