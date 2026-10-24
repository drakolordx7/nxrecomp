// SPDX-License-Identifier: GPL-3.0-or-later
// The shader program list of the player's game: every NVN program (a vertex + fragment [+ geometry] triple of code blocks) found in
// the RomFS, written as shader_cache/programs.bin for the runtime to compile ahead of play.
//
// Where the programs are: the SNVN records of MaterialArchive.arc, and the program sets embedded (zlib-wrapped) in the META footer
// of every MTRL asset of every .pak. Format knowledge: tools/survey/extract_shaders.py of this project (re-implemented, not copied).
//
// Hash of a stage: the first 8 bytes of the SHA-1 of its code block (the code bytes only), read as a big-endian u64. This is the
// same value as the first 16 hex digits used for the shader cache file names (shadercache.h).
#pragma once
#include <compare>
#include <functional>
#include <string>
#include <vector>

#include "common.h"

namespace mprs {

// One shader program. gs = 0 when the program has no geometry stage.
struct ProgramHashes {
    u64 vs = 0, fs = 0, gs = 0;
    auto operator<=>(const ProgramHashes&) const = default;
};

// Per source counts from one scan (for the log and for checking a scan against a reference).
struct ProgramSourceStats {
    u32 units = 0;      // program sets looked at: SNVN records (arc) or embedded sets found (paks)
    u32 invalid = 0;    // set that inflated but did not split, or whose control sections did not match
    u32 skipped = 0;    // compute, tessellation, two blocks of one stage, or no vertex/fragment block
    u32 programs = 0;   // sets that gave a program (before the merge removes duplicates)
};
struct ProgramScanStats {
    ProgramSourceStats arc, pak;
    u32 arc_inflate_failed = 0;   // SNVN records whose zlib stream did not inflate
    u32 mtrl_assets = 0;          // MTRL assets read from the paks
    u32 mtrl_failed = 0;          // MTRL assets that could not be read or whose footer did not inflate
    u32 pak_files_skipped = 0;    // .pak files that could not be opened
    u32 unknown_stage_blocks = 0; // code blocks whose stage field is not vertex, tessellation, geometry or fragment (ignored)
    u32 unique_programs = 0;
    std::string first_asset_error;   // reason of the first MTRL asset that failed (empty when none failed)
};

// Scans the RomFS folder (MaterialArchive.arc and every .pak under it). threads: 0 = hardware threads. Returns the programs sorted
// ascending by (vs, fs, gs), unique. progress(done, total) counts MTRL assets; it is called from worker threads, one call per asset.
// cancel() is polled between assets; when it returns true the scan throws Error(Fail::Cancelled).
// A pak that cannot be opened is skipped (counted in stats). Throws Error(Fail::Cancelled) on cancel only.
std::vector<ProgramHashes> scan_shader_programs(const std::string& romfs_dir_utf8, int threads,
                                                const std::function<void(u64 done, u64 total)>& progress,
                                                const std::function<bool()>& cancel, ProgramScanStats* stats = nullptr);

// File "programs.bin": magic "MPRPRG01", u32 count, u32 0, then count records of three u64 (vs, fs, gs), little-endian.
// Written atomically (AtomicFileWriter). Throws Error(Disk) on write failure.
void write_programs_file(const std::string& path_utf8, const std::vector<ProgramHashes>& programs);
// Reads and checks the file: magic, reserved word, exact size for the count, records strictly ascending. Throws Error(Input) if it is
// not a valid file (a count of 0 is valid), Error(Generic) if it cannot be read.
std::vector<ProgramHashes> read_programs_file(const std::string& path_utf8);

// Result of one raw (inflated) program set.
enum class SetResult { Invalid, Skipped, Program };
// One inflated program set (code blocks then control sections, as in the NVN output): splits it, hashes the stages, and returns
// the program when it has a vertex and a fragment block (and no compute, tessellation or repeated stage). A block whose stage field
// is none of the known values is ignored; unknown_stage_blocks (if given) is increased for each such block.
SetResult program_of_raw_set(const std::vector<u8>& raw, ProgramHashes& out, u32* unknown_stage_blocks = nullptr);
// The program sets embedded in a decompressed pak MTRL blob: finds each "00 xx xx xx xx | 11|19|08 00 00 00 | u32 len | 78 .." record
// the same way the Python extractor does and returns the inflated bodies whose zlib stream ends exactly at its declared length and
// starts with a code magic. Not split yet (use program_of_raw_set).
std::vector<std::vector<u8>> embedded_raw_sets(const std::vector<u8>& blob);

}  // namespace mprs
