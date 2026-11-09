// SPDX-License-Identifier: GPL-3.0-or-later
// The whole setup as one call, used by both the CLI and the GUI.
//
// Output layout (all under SetupOptions::out_dir; the game is started with this folder as its working directory):
//   exefs/            the NSOs and main.npdm (rtld, main, sdk, subsdk0, main.npdm), byte-identical to the dump
//   romfs/            the RomFS as loose files (with the update applied when one is given)
//   aoc/<title id>/   the RomFS of each DLC (16 upper-case hex digits), for the runtime's add-on content service
//   bc7_cache/        ASTC textures transcoded to BC7, one file per texture, in the runtime's cache format (only for a game whose hook lists textures)
//   shader_cache/spirv/   translated shaders from the shader pack (setup stops without a pack, unless --no-shaders or an earlier run installed them)
//   <game ini>        (game.h; <id>.ini) [fs] romfs / [nvn] bc7_cache, bc7_quality, shader_cache for the runtime, then the hook's ini_lines
//   setup_state.tsv   what was done, hashes of every output file, quality used; makes re-runs cheap and resumable
//   setup.log         the log of every run (appended); run_setup opens it itself (log_open) as soon as out_dir exists
#pragma once
#include <string>
#include <vector>

#include "common.h"
#include "game.h"
#include "game_setup.h"
#include "keys.h"
#include "log.h"

namespace nxs {

enum class Stage { Inspect = 0, Space, Exefs, Romfs, Textures, ShaderCache, Finish, Count };
const char* stage_name(Stage s);

struct SetupOptions {
    std::string input_path;                 // .nsp .xci .nsz .xcz
    std::vector<std::string> input_paths;   // more inputs (base game, update, DLC in any order); used together with input_path
    std::vector<std::string> key_files;     // prod.keys (and optionally title.keys); empty = look in the usual places
    std::string out_dir;
    int bc7_quality = 3;                    // 0..6; the runtime reads [nvn] bc7_quality and its cache keys include it. Default = runtime default.
    int threads = 0;                        // 0 = all hardware threads for texture conversion (RomFS copy uses 3)
    std::string shader_cache_src;           // shader pack file or folder with the translated shaders; empty = the game's shader pack (game.h, shader_pack) or shader_cache\ next to the setup program
    bool allow_no_shaders = false;          // developer escape hatch (--no-shaders): with no pack, install without shaders. The game then draws nothing.
    bool verify_input = true;               // check the dump's own hashes (PFS0 hash table, RomFS integrity tree) before writing
    bool verify_output = false;             // re-hash files that are already present instead of trusting their size
    bool skip_textures = false;             // developer option (--skip-textures): leave the texture cache out even when the game's hook has one
    bool inspect_only = false;              // open, validate and size up; write nothing
    // The game: its title id, accepted builds and file names (the shipped tool always uses game(), the one it is built for; tests pass
    // their own), and its setup hook (nullptr: extraction only, no texture cache, no program list).
    GameProfile profile = game();
    const GameSetup* hook = game_setup();
};

// input_path (if set) followed by input_paths.
std::vector<std::string> setup_inputs(const SetupOptions& o);

struct InputSummary {
    std::string container;                  // "NSP", "XCI", "NSZ", "XCZ"
    std::string title_id;                   // 16 hex digits, upper case
    std::string version;                    // "1.0.0" style display version, or "" if unknown
    std::string build_id;                   // main NSO build id (40 hex digits) or ""
    std::string update;                     // the update that is applied ("3.0.3" style, or "v<number>"), "" for none
    u32 dlc_count = 0;                      // DLC found for the game
    u64 dlc_bytes = 0;
    std::vector<std::string> notes;         // things the player should see: an input that was not used, a DLC that cannot be read
    u64 romfs_bytes = 0;                    // total size of the RomFS files
    u64 exefs_bytes = 0;
    u32 romfs_files = 0;
    u32 astc_textures = 0;                  // 0 until the RomFS has been scanned (inspect_only after extraction)
    u64 bc7_bytes_estimate = 0;
    u64 disk_needed = 0;                    // bytes still to be written for this options set (existing valid files not counted)
    u64 disk_free = 0;
    bool supported = false;
    std::string problem;                    // plain-language reason when !supported
};

class Reporter {
public:
    virtual ~Reporter() = default;
    // A stage starts. May be called again for the same stage to refine the title or the total (for example once the texture count is known).
    // total = number of units (bytes for Exefs/Romfs, textures for Textures; 0 = unknown); unit = "bytes" or "textures".
    virtual void stage_begin(Stage, const std::string& title, u64 total, const char* unit) {}
    virtual void stage_progress(u64 done) {}               // done of total in the current stage (may be called from any thread)
    virtual void stage_end(Stage, double seconds) {}
    virtual bool cancel_requested() { return false; }      // polled often; true = stop soon, keep progress
};

struct SetupReport {
    double seconds[static_cast<int>(Stage::Count)] = {};
    u64 files_written = 0, files_skipped = 0, bytes_written = 0;
    u32 textures_total = 0, textures_converted = 0, textures_skipped = 0, textures_failed = 0;
    double total_seconds = 0;
};

// Quick check: opens the dump with the keys and checks what can be checked without reading the whole file (container, title id,
// version, build id, disk space for out_dir). The dump's own hash tree is checked by run_setup (verify_input), not here. Never throws for "not supported": it fills supported/problem.
// Throws Error only for things like an unreadable file.
InputSummary inspect_input(const SetupOptions& o);

// Does the whole setup. Idempotent: running it again on a finished folder only checks sizes (or hashes with verify_output) and
// returns quickly. Stops with Error(Fail::Cancelled) when the reporter asks; everything finished so far stays valid.
SetupReport run_setup(const SetupOptions& o, Reporter& r);

}  // namespace nxs
